#include "internal/code_assistance.h"

#include "internal/code_text.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QTextDocument>
#include <QUrl>

namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 MAX_COMPLETIONS = 256;
constexpr int32 REFRESH_DELAY_MILLISECONDS = 100;
constexpr int32 MAX_SIGNATURES = 32;
constexpr int32 MAX_COMPLETION_CANDIDATES = 8192;
constexpr int32 MAX_ADDITIONAL_EDITS = 32;
constexpr int32 MAX_LABEL_UNITS = 2048;
constexpr int32 MAX_DOCUMENTATION_UNITS = 4096;

QString GetDocumentation(const QJsonValue& value)
{
    QString text =
        (value.isString() == true ? value.toString() : value.toObject().value(QStringLiteral("value")).toString())
            .left(MAX_DOCUMENTATION_UNITS);
    // Slang appends a generated translation-unit location. Keep authored API
    // documentation; temporary implementation paths are not useful navigation.
    const isize provenance = text.lastIndexOf(QStringLiteral("\n\nDefined in "));
    if (provenance >= 0)
    {
        text.truncate(provenance);
    }
    return text.trimmed();
}

bool IsWordCharacter(QChar character)
{
    return character.isLetterOrNumber() == true || character == QLatin1Char('_');
}

} // namespace

CodeAssistance::CodeAssistance(CodeEditor& inoutEditor, CodeDocumentController& inoutDocument, QObject* parentOrNull)
    : QObject(parentOrNull), mEditor(inoutEditor), mDocument(inoutDocument), mService(this)
{
    connect(&mEditor, &CodeEditor::AssistanceRequested, this, &CodeAssistance::Request);
    connect(&mEditor.GetNavigation(), &CodeNavigation::Requested, this, &CodeAssistance::RequestNavigation);
    connect(&mEditor.GetNavigation(), &CodeNavigation::ResultChosen, this, &CodeAssistance::acceptNavigation);
    connect(&mEditor.GetNavigation(), &CodeNavigation::Dismissed, this, &CodeAssistance::Cancel);
    connect(&mEditor, &CodeEditor::CompletionChosen, this, &CodeAssistance::acceptCompletion);
    connect(&mEditor, &CodeEditor::SignatureOverloadRequested, this, [this](int32 direction) {
        if (isCurrent() == true && mSignatures.isEmpty() == false)
        {
            mActiveSignature = (mActiveSignature + direction + static_cast<int32>(mSignatures.size())) %
                               static_cast<int32>(mSignatures.size());
            showSignatures();
        }
    });
    mRefresh.setSingleShot(true);
    mRefresh.setInterval(REFRESH_DELAY_MILLISECONDS);
    connect(&mRefresh, &QTimer::timeout, this, [this]() {
        if (mEditor.hasFocus() == true &&
            (mShouldTrackSignature == true ||
             (mShouldTrackCompletion == true && mDocument.Capture().Identity != mCapture.Identity)))
        {
            const QString text = mEditor.toPlainText();
            const int32 caret = mEditor.textCursor().position();
            if (mShouldTrackSignature == true || (caret > 0 && IsWordCharacter(text.at(caret - 1)) == true))
            {
                Request(mShouldTrackSignature);
            }
        }
    });
    const auto invalidate = [this]() {
        cancelPending();
        if (mShouldTrackSignature == true || mShouldTrackCompletion == true)
        {
            mRefresh.start();
        }
    };
    connect(&mDocument, &CodeDocumentController::TextAccepted, this, invalidate);
    connect(&mEditor, &CodeEditor::AssistanceDismissed, this, [this]() {
        mShouldTrackSignature = false;
        mShouldTrackCompletion = false;
        Cancel();
    });
    connect(&mDocument, &CodeDocumentController::BindingChanged, this, &CodeAssistance::ClearContext);
    connect(&mEditor, &QPlainTextEdit::cursorPositionChanged, this, invalidate);
    connect(&mService, &SlangLanguageService::Replied, this, &CodeAssistance::receive);
    connect(&mService, &SlangLanguageService::Ready, this, [this]() {
        mEditor.GetNavigation().SetReferencesAvailable(mService.CanRequest(QStringLiteral("textDocument/references")));
        requestCurrent();
    });
    connect(&mService, &SlangLanguageService::RequestUnavailable, this, [this](const QString& message) {
        Cancel();
        Q_EMIT StatusChanged(message);
    });
    connect(&mService, &SlangLanguageService::Failed, this, [this](const QString& message) {
        Cancel();
        mServer.clear();
        Q_EMIT StatusChanged(message);
    });
}

void CodeAssistance::Cancel()
{
    mShouldTrackSignature = false;
    mShouldTrackCompletion = false;
    cancelPending();
    Q_EMIT ContextCancelled();
}

void CodeAssistance::cancelPending()
{
    mRefresh.stop();
    mService.Cancel();
    mHasIntent = false;
    mRequest = 0;
    mCompletions.clear();
    mSignatures = {};
    mNavigationTargets.clear();
    mEditor.GetNavigation().Dismiss();
    mEditor.HideAssistance();
}

void CodeAssistance::ClearContext()
{
    mEditor.GetNavigation().SetReferencesAvailable(true);
    mShouldTrackSignature = false;
    mShouldTrackCompletion = false;
    Cancel();
    ++mContext;
    mUri.clear();
    mPrefix.clear();
    mSuffix.clear();
    mNavigationSources.clear();
}

void CodeAssistance::SetContext(const QString& server,
                                const QString& workspace,
                                const QString& uri,
                                const QString& prefix,
                                const QString& suffix,
                                const QStringList& serverArguments,
                                const CodeNavigationSources& navigationSources)
{
    if (isCurrent() == false)
    {
        return;
    }
    const QUrl documentUri(uri);
    const QString canonical =
        documentUri.isLocalFile() == true ? QFileInfo(documentUri.toLocalFile()).canonicalFilePath() : QString{};
    mUri = canonical.isEmpty() == true ? uri : QUrl::fromLocalFile(canonical).toString();
    mPrefix = prefix;
    mSuffix = suffix;
    mNavigationSources = navigationSources;
    ++mContext;
    mRequestedContext = mContext;
    const QFileInfo executable(server);
    const QString stamp =
        executable.canonicalFilePath() +
        QStringLiteral(":%1:%2").arg(executable.size()).arg(executable.lastModified().toMSecsSinceEpoch());
    if (mServer != server || mServerStamp != stamp || mServerArguments != serverArguments)
    {
        mServer = server;
        mServerStamp = stamp;
        mServerArguments = serverArguments;
        mService.Start(server, workspace, serverArguments);
    }
    mEditor.GetNavigation().SetReferencesAvailable(mService.CanRequest(QStringLiteral("textDocument/references")));
    requestCurrent();
}

void CodeAssistance::Request(bool isSignature)
{
    Cancel();
    mShouldTrackSignature = false;
    mShouldTrackCompletion = false;
    if (mEditor.GetLanguage() != CodeLanguage::Slang || mDocument.IsBound() == false || mEditor.isReadOnly() == true)
    {
        return;
    }
#if defined(Q_OS_WASM)
    static_cast<void>(isSignature);
    Q_EMIT StatusChanged(QStringLiteral("Shader assistance needs native Slang; unavailable in the browser editor."));
#else
    mIsNavigation = false;
    mShouldTrackSignature = isSignature;
    mShouldTrackCompletion = isSignature == false;
    mIsSignature = isSignature;
    beginRequest();
#endif
}

void CodeAssistance::RequestNavigation(CodeNavigationKind kind)
{
    Cancel();
    if (mEditor.GetLanguage() != CodeLanguage::Slang || mDocument.IsBound() == false)
    {
        return;
    }
#if defined(Q_OS_WASM)
    static_cast<void>(kind);
    Q_EMIT StatusChanged(QStringLiteral(
        "Semantic navigation needs native Slang. Quick Open, line navigation and history remain available."));
#else
    mIsNavigation = true;
    mNavigationKind = kind;
    beginRequest();
#endif
}

void CodeAssistance::beginRequest()
{
    mCapture = mDocument.Capture();
    mCaret = mEditor.textCursor().position();
    mAnchor = mEditor.textCursor().anchor();
    mHasIntent = true;
    mRequestedContext = mContext;
    // The workspace revalidates its package/manifest/tool context on every intent;
    // ordinary text changes can reuse a prepared context with incremental sync.
    Q_EMIT ContextRequested();
}

bool CodeAssistance::isCurrent() const
{
    return mHasIntent == true && mRequestedContext == mContext && mCapture.Identity == mDocument.Capture().Identity &&
           mEditor.textCursor().position() == mCaret && mEditor.textCursor().anchor() == mAnchor &&
           mEditor.GetLanguage() == CodeLanguage::Slang;
}

void CodeAssistance::requestCurrent()
{
    if (isCurrent() == false || mUri.isEmpty() == true)
    {
        return;
    }
    QString source;
    if (DecodeCodeText(mCapture.Text, source) == false)
    {
        return;
    }
    mUnit = mPrefix + source + mSuffix;
    mWordStart = mCaret;
    while (mWordStart > 0 && IsWordCharacter(source.at(mWordStart - 1)) == true)
    {
        --mWordStart;
    }
    mWordEnd = mCaret;
    while (mWordEnd < source.size() && IsWordCharacter(source.at(mWordEnd)) == true)
    {
        ++mWordEnd;
    }
    const int32 position = static_cast<int32>(mPrefix.size()) + mCaret;
    const int32 previous = position == 0 ? -1 : static_cast<int32>(mUnit.lastIndexOf(QLatin1Char('\n'), position - 1));
    const QJsonObject location{
        {QStringLiteral("line"), static_cast<int32>(mUnit.left(position).count(QLatin1Char('\n')))},
        {QStringLiteral("character"), position - previous - 1}};
    mService.SetDocument(mUri, mUnit);
    QString method =
        mIsSignature == true ? QStringLiteral("textDocument/signatureHelp") : QStringLiteral("textDocument/completion");
    if (mIsNavigation == true)
    {
        method = mNavigationKind == CodeNavigationKind::Symbols      ? QStringLiteral("textDocument/documentSymbol")
                 : mNavigationKind == CodeNavigationKind::Definition ? QStringLiteral("textDocument/definition")
                                                                     : QStringLiteral("textDocument/references");
    }
    mRequest = mService.Request(method, location);
    if (mRequest == 0 && mHasIntent == true)
    {
        Q_EMIT StatusChanged(QStringLiteral("Starting Slang shader assistance…"));
    }
}

bool CodeAssistance::mapPosition(const QJsonObject& position, usize& outBytes) const
{
    int32 offset = 0;
    int32 sourceLength = 0;
    if (GetCodeProtocolOffset(mUnit, position, offset) == false ||
        GetCodeCursorPosition(mCapture.Text, static_cast<usize>(mCapture.Text.size()), sourceLength) == false)
    {
        return false;
    }
    offset -= static_cast<int32>(mPrefix.size());
    return offset >= 0 && offset <= sourceLength && GetCodeByteOffset(mCapture.Text, offset, outBytes) == true;
}

void CodeAssistance::receive(int32 request, const QJsonValue& value)
{
    if (request != mRequest || isCurrent() == false)
    {
        return;
    }
    if (mIsNavigation == true)
    {
        QString source;
        if (DecodeCodeText(mCapture.Text, source) == false)
        {
            return;
        }
        QString message;
        mNavigationTargets = ReadCodeNavigationResults(mNavigationKind,
                                                       value,
                                                       mUri,
                                                       mUnit,
                                                       mPrefix,
                                                       source,
                                                       mEditor.GetNavigation().GetSource(),
                                                       mNavigationSources,
                                                       message);
        Q_EMIT StatusChanged(message);
        if (mNavigationKind == CodeNavigationKind::Definition && mNavigationTargets.size() == 1)
        {
            acceptNavigation(0);
        }
        else
        {
            mEditor.GetNavigation().ShowResults(mNavigationTargets, message);
        }
        return;
    }
    if (mIsSignature == true)
    {
        const QJsonObject result = value.toObject();
        const QJsonArray signatures = result.value(QStringLiteral("signatures")).toArray();
        mSignatures = {};
        const int32 count = qMin(MAX_SIGNATURES, static_cast<int32>(signatures.size()));
        for (int32 index = 0; index < count; ++index)
        {
            mSignatures.append(signatures.at(index));
        }
        mActiveSignature = qBound(0,
                                  result.value(QStringLiteral("activeSignature")).toInt(),
                                  qMax(0, static_cast<int32>(mSignatures.size()) - 1));
        mActiveParameter = result.value(QStringLiteral("activeParameter")).toInt();
        mShouldTrackSignature = mSignatures.isEmpty() == false;
        showSignatures();
        Q_EMIT StatusChanged(mSignatures.isEmpty() == true ? QStringLiteral("No signature at this position.")
                                                           : QString());
        return;
    }
    const QJsonObject list = value.toObject();
    const QJsonArray items = value.isArray() == true ? value.toArray() : list.value(QStringLiteral("items")).toArray();
    const QJsonObject defaults = list.value(QStringLiteral("itemDefaults")).toObject();
    const QString word = mEditor.toPlainText().mid(mWordStart, mCaret - mWordStart);
    QStringList labels;
    QStringList details;
    int32 examined = 0;
    for (const QJsonValue& entry : items)
    {
        if (++examined > MAX_COMPLETION_CANDIDATES)
        {
            break;
        }
        QJsonObject item = entry.toObject();
        const QString label = item.value(QStringLiteral("label")).toString();
        const QString filter = item.value(QStringLiteral("filterText")).toString(label);
        if (label.isEmpty() == true || label.size() > MAX_LABEL_UNITS ||
            filter.startsWith(word, Qt::CaseInsensitive) == false)
        {
            continue;
        }
        if (mCompletions.size() >= MAX_COMPLETIONS)
        {
            break;
        }
        if (item.contains(QStringLiteral("insertTextFormat")) == false)
        {
            item.insert(QStringLiteral("insertTextFormat"), defaults.value(QStringLiteral("insertTextFormat")));
        }
        const QJsonObject defaultRange = defaults.value(QStringLiteral("editRange")).toObject();
        if (item.contains(QStringLiteral("textEdit")) == false && defaultRange.isEmpty() == false)
        {
            QJsonObject edit = defaultRange.contains(QStringLiteral("start")) == true
                                   ? QJsonObject{{QStringLiteral("range"), defaultRange}}
                                   : defaultRange;
            edit.insert(QStringLiteral("newText"),
                        item.value(QStringLiteral("textEditText"))
                            .toString(item.value(QStringLiteral("insertText")).toString(label)));
            item.insert(QStringLiteral("textEdit"), edit);
        }
        const QString detail = item.value(QStringLiteral("detail")).toString().left(MAX_DOCUMENTATION_UNITS);
        mCompletions.append({label, detail, item});
        labels.append(detail.isEmpty() == true ? label : label + QStringLiteral("  —  ") + detail);
        details.append(GetDocumentation(item.value(QStringLiteral("documentation"))));
    }
    mShouldTrackCompletion = mCompletions.isEmpty() == false;
    mEditor.ShowCompletions(labels, details);
    Q_EMIT StatusChanged(
        mCompletions.isEmpty() == true ? QStringLiteral("No matching shader symbols.")
        : mCompletions.size() == MAX_COMPLETIONS
            ? QStringLiteral("Showing the first %1 symbols. Type a prefix and complete again.").arg(MAX_COMPLETIONS)
            : QString());
}

void CodeAssistance::acceptNavigation(int32 row)
{
    if (isCurrent() == false || row < 0 || row >= mNavigationTargets.size())
    {
        mEditor.GetNavigation().ReportFailure(
            QStringLiteral("The source or shader context changed. Request navigation again; current view retained."));
        return;
    }
    const CodeNavigationTarget target = mNavigationTargets.at(row);
    static_cast<void>(mEditor.GetNavigation().Navigate(target));
}

void CodeAssistance::acceptCompletion(int32 row)
{
    if (isCurrent() == false || row < 0 || row >= mCompletions.size())
    {
        Cancel();
        Q_EMIT StatusChanged(
            QStringLiteral("The source or selection changed. Request completion again; no edit was applied."));
        return;
    }
    const Completion completion = mCompletions.at(row);
    constexpr int32 SNIPPET_FORMAT = 2;
    if (completion.Item.value(QStringLiteral("insertTextFormat")).toInt() == SNIPPET_FORMAT)
    {
        Cancel();
        Q_EMIT StatusChanged(QStringLiteral(
            "This completion requires unsupported snippet placeholders. Choose another symbol; no edit was applied."));
        return;
    }
    struct Replacement final
    {
        usize Start = 0;
        usize End = 0;
        QByteArray Text;
        bool IsPrimary = false;
    };
    QList<Replacement> replacements;
    const auto appendEdit = [this, &replacements](const QJsonObject& edit, bool isPrimary) {
        QJsonObject range = edit.value(QStringLiteral("range")).toObject();
        if (range.isEmpty() == true)
        {
            range = edit.value(QStringLiteral("replace")).toObject();
        }
        Replacement replacement;
        replacement.IsPrimary = isPrimary;
        if (edit.value(QStringLiteral("newText")).isString() == false)
        {
            return false;
        }
        if (range.isEmpty() == true ||
            mapPosition(range.value(QStringLiteral("start")).toObject(), replacement.Start) == false ||
            mapPosition(range.value(QStringLiteral("end")).toObject(), replacement.End) == false ||
            replacement.End < replacement.Start)
        {
            return false;
        }
        QString text = edit.value(QStringLiteral("newText")).toString();
        if (mCapture.Text.contains("\r\n") == true)
        {
            text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
        }
        replacement.Text = text.toUtf8();
        usize caretBytes = 0;
        if (isPrimary == true && (GetCodeByteOffset(mCapture.Text, mCaret, caretBytes) == false ||
                                  caretBytes < replacement.Start || caretBytes > replacement.End))
        {
            return false;
        }
        replacements.append(replacement);
        return true;
    };
    QJsonObject primary = completion.Item.value(QStringLiteral("textEdit")).toObject();
    bool isValid = true;
    if (primary.isEmpty() == true)
    {
        Replacement replacement;
        isValid = GetCodeByteOffset(mCapture.Text, mWordStart, replacement.Start) == true &&
                  GetCodeByteOffset(mCapture.Text, mWordEnd, replacement.End) == true;
        replacement.Text = completion.Item.value(QStringLiteral("insertText")).toString(completion.Label).toUtf8();
        replacement.IsPrimary = true;
        replacements.append(replacement);
    }
    else
    {
        isValid = appendEdit(primary, true);
    }
    const QJsonArray additional = completion.Item.value(QStringLiteral("additionalTextEdits")).toArray();
    isValid = isValid == true && additional.size() <= MAX_ADDITIONAL_EDITS;
    for (const QJsonValue& edit : additional)
    {
        isValid = isValid == true && appendEdit(edit.toObject(), false) == true;
    }
    // Bounded insertion ordering; reject overlapping edits instead of guessing.
    QList<Replacement> ordered;
    for (const Replacement& replacement : replacements)
    {
        int32 index = 0;
        while (index < ordered.size() && ordered.at(index).Start < replacement.Start)
        {
            ++index;
        }
        ordered.insert(index, replacement);
    }
    QByteArray result;
    usize original = 0;
    usize caret = 0;
    bool hasPrior = false;
    bool hasCapacityFailure = false;
    usize priorEnd = 0;
    usize priorStart = 0;
    for (const Replacement& replacement : ordered)
    {
        if (replacement.Start < original ||
            (hasPrior == true && (replacement.Start == priorStart ||
                                  (replacement.Start == priorEnd && replacement.Start == replacement.End))))
        {
            isValid = false;
            break;
        }
        result += mCapture.Text.mid(static_cast<isize>(original), static_cast<isize>(replacement.Start - original));
        result += replacement.Text;
        if (replacement.IsPrimary == true)
        {
            caret = static_cast<usize>(result.size());
        }
        original = replacement.End;
        priorEnd = replacement.End;
        priorStart = replacement.Start;
        hasPrior = true;
        if (static_cast<usize>(result.size()) > mCapture.LimitBytes)
        {
            hasCapacityFailure = true;
            isValid = false;
            break;
        }
    }
    result += mCapture.Text.mid(static_cast<isize>(original));
    const CodeDocumentIdentity identity = mCapture.Identity;
    mShouldTrackCompletion = false;
    mShouldTrackSignature = false;
    Cancel();
    if (hasCapacityFailure == true || static_cast<usize>(result.size()) > mCapture.LimitBytes)
    {
        Q_EMIT StatusChanged(QStringLiteral(
            "Completion exceeds this source's byte limit. Shorten the file and try again; no edit was applied."));
        return;
    }
    if (isValid == false)
    {
        Q_EMIT StatusChanged(QStringLiteral("Slang returned overlapping edits or edits outside this authored file. "
                                            "Choose another program context or symbol; no edit was applied."));
        return;
    }
    const CodeEditStatus status = mDocument.ApplyServiceText(identity, result, {caret, caret});
    switch (status)
    {
        case CodeEditStatus::Ok:
            Q_EMIT StatusChanged(QString());
            break;
        case CodeEditStatus::Conflict:
            Q_EMIT StatusChanged(QStringLiteral("The source changed or has a disk conflict. Resolve or reload the "
                                                "source, then request completion again."));
            break;
        case CodeEditStatus::Capacity:
            Q_EMIT StatusChanged(QStringLiteral("Completion exceeds a source or package size limit. Reduce its size "
                                                "and try again; no edit was applied."));
            break;
        case CodeEditStatus::InvalidText:
            Q_EMIT StatusChanged(QStringLiteral("Completion returned invalid source text. Repair the shader tool "
                                                "selection and retry; no edit was applied."));
            break;
        case CodeEditStatus::Unavailable:
            Q_EMIT StatusChanged(QStringLiteral(
                "This source is unavailable or read-only. Open an editable source and request completion again."));
            break;
    }
}

void CodeAssistance::showSignatures()
{
    if (mSignatures.isEmpty() == true || isCurrent() == false)
    {
        mEditor.ShowSignature({});
        return;
    }
    const QJsonObject signature = mSignatures.at(mActiveSignature).toObject();
    const QString label = signature.value(QStringLiteral("label")).toString().left(MAX_LABEL_UNITS);
    const QJsonArray parameters = signature.value(QStringLiteral("parameters")).toArray();
    const int32 active = signature.value(QStringLiteral("activeParameter")).toInt(mActiveParameter);
    int32 start = -1;
    int32 end = -1;
    QString documentation = GetDocumentation(signature.value(QStringLiteral("documentation")));
    if (active >= 0 && active < parameters.size())
    {
        const QJsonObject parameter = parameters.at(active).toObject();
        const QJsonValue parameterLabel = parameter.value(QStringLiteral("label"));
        if (parameterLabel.isArray() == true && parameterLabel.toArray().size() == 2)
        {
            start = parameterLabel.toArray().at(0).toInt(-1);
            end = parameterLabel.toArray().at(1).toInt(-1);
        }
        else if (parameterLabel.isString() == true)
        {
            start = static_cast<int32>(label.indexOf(parameterLabel.toString()));
            end = start + static_cast<int32>(parameterLabel.toString().size());
        }
        const QString parameterDocumentation = GetDocumentation(parameter.value(QStringLiteral("documentation")));
        if (parameterDocumentation.isEmpty() == false)
        {
            documentation += QLatin1Char('\n') + parameterDocumentation;
        }
    }
    const QString formatted = start >= 0 && end >= start && end <= label.size()
                                  ? label.left(start).toHtmlEscaped() + QStringLiteral("<b><u>") +
                                        label.mid(start, end - start).toHtmlEscaped() + QStringLiteral("</u></b>") +
                                        label.mid(end).toHtmlEscaped()
                                  : label.toHtmlEscaped();
    QString html = QStringLiteral("<code>%1</code>").arg(formatted);
    if (documentation.isEmpty() == false)
    {
        html += QStringLiteral("<p>%1</p>")
                    .arg(documentation.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    }
    if (mSignatures.size() > 1)
    {
        html += QStringLiteral("<p>%1 / %2 · Alt+PageUp/PageDown: overload · Esc: close</p>")
                    .arg(mActiveSignature + 1)
                    .arg(mSignatures.size());
    }
    mEditor.ShowSignature(html);
}
} // namespace ludus::editor
