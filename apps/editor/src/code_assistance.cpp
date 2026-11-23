#include "internal/code_assistance.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/stable_sort.hpp>

#include "internal/code_completion_sources.h"
#include "internal/code_language_registry.h"
#include "internal/code_snippet.h"
#include "internal/code_text.h"
#include "internal/code_workspace_edit_preview.h"
#include "internal/fuzzy_match.h"
#include "internal/language_positions.h"

#include <QApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
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
// Analysis follows edits after this pause, so typing never sends per keystroke.
constexpr int32 ANALYSIS_DELAY_MILLISECONDS = 100;
// Code actions follow the caret after it rests this long, so moving through text sends nothing.
constexpr int32 CODE_ACTION_DELAY_MILLISECONDS = 250;
constexpr int32 MAX_ANALYSIS_MESSAGE_UNITS = 2048;
// LSP DiagnosticSeverity: 1 error, 2 warning, 3 information, 4 hint.
constexpr int32 SEVERITY_ERROR = 1;
constexpr int32 SEVERITY_INFORMATION = 3;

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

// Hover contents are MarkupContent, a MarkedString or an array of them. Show them
// as plain text: fences are dropped and nothing is rendered as markup.
QString ReadHoverText(const QJsonValue& contents)
{
    QStringList parts;
    const QJsonArray values = contents.isArray() == true ? contents.toArray() : QJsonArray{contents};
    for (const QJsonValue& value : values)
    {
        QString part = GetDocumentation(value);
        QStringList lines = part.split(QLatin1Char('\n'));
        lines.removeIf([](const QString& line) { return line.trimmed().startsWith(QStringLiteral("```")) == true; });
        part = lines.join(QLatin1Char('\n')).trimmed();
        if (part.isEmpty() == false)
        {
            parts.append(part);
        }
    }
    return parts.join(QStringLiteral("\n\n")).left(MAX_DOCUMENTATION_UNITS);
}

LanguageRequestKind GetNavigationRequest(CodeNavigationKind kind)
{
    switch (kind)
    {
        case CodeNavigationKind::Symbols:
            return LanguageRequestKind::DocumentSymbols;
        case CodeNavigationKind::Definition:
            return LanguageRequestKind::Definition;
        case CodeNavigationKind::References:
            return LanguageRequestKind::References;
        case CodeNavigationKind::Declaration:
            return LanguageRequestKind::Declaration;
        case CodeNavigationKind::TypeDefinition:
            return LanguageRequestKind::TypeDefinition;
        case CodeNavigationKind::Implementation:
            return LanguageRequestKind::Implementation;
        case CodeNavigationKind::WorkspaceSymbols:
            return LanguageRequestKind::WorkspaceSymbols;
    }
    LUDUS_UNREACHABLE();
}

bool IsWordCharacter(QChar character)
{
    return character.isLetterOrNumber() == true || character == QLatin1Char('_');
}

} // namespace

CodeAssistance::CodeAssistance(CodeEditor& inoutEditor, CodeDocumentController& inoutDocument, QObject* parentOrNull)
    : QObject(parentOrNull), mEditor(inoutEditor), mDocument(inoutDocument), mClient(this),
      mLayers(inoutEditor, mClient), mActions(inoutEditor, inoutDocument, mClient)
{
    connect(&mEditor, &CodeEditor::AssistanceRequested, this, &CodeAssistance::Request);
    connect(&mEditor.GetNavigation(), &CodeNavigation::Requested, this, &CodeAssistance::RequestNavigation);
    connect(&mEditor.GetNavigation(), &CodeNavigation::ResultChosen, this, &CodeAssistance::acceptNavigation);
    connect(&mEditor.GetNavigation(), &CodeNavigation::Dismissed, this, &CodeAssistance::Cancel);
    connect(&mEditor, &CodeEditor::CompletionChosen, this, &CodeAssistance::acceptCompletion);
    connect(&mEditor, &CodeEditor::CompletionHighlighted, this, &CodeAssistance::resolveCompletion);
    connect(&mEditor, &CodeEditor::RenameRequested, this, &CodeAssistance::RequestRename);
    connect(&mEditor, &CodeEditor::RenameAccepted, this, &CodeAssistance::acceptRename);
    connect(&mEditor, &CodeEditor::RenameCancelled, this, [this]() { mRename = {}; });
    connect(&mEditor, &CodeEditor::CodeActionsRequested, this, &CodeAssistance::RequestCodeActions);
    connect(&mActions, &CodeActions::StatusChanged, this, &CodeAssistance::StatusChanged);
    mCodeActionDelay.setSingleShot(true);
    mCodeActionDelay.setInterval(CODE_ACTION_DELAY_MILLISECONDS);
    connect(&mCodeActionDelay, &QTimer::timeout, this, [this]() { requestCodeActions(false); });
    connect(&mEditor, &QPlainTextEdit::cursorPositionChanged, this, &CodeAssistance::scheduleCodeActions);
    connect(&mDocument, &CodeDocumentController::TextAccepted, this, [this]() {
        // The light bulb describes the text it was asked for.
        mEditor.SetCodeActions(0, {});
        scheduleCodeActions();
    });
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
    connect(&mClient,
            &LanguageClient::Replied,
            this,
            [this](int32 request, LanguageRequestKind kind, const QJsonValue& value) {
                if (mLayers.Receive(request, kind, value, mDocument.Capture().Identity, mContext) == true ||
                    mActions.Receive(request, kind, value) == true)
                {
                    return;
                }
                if (kind == LanguageRequestKind::Hover)
                {
                    receiveHover(request, value);
                    return;
                }
                if (kind == LanguageRequestKind::CompletionResolve)
                {
                    receiveResolve(request, value);
                    return;
                }
                if (kind == LanguageRequestKind::Rename)
                {
                    receiveRename(request, value);
                    return;
                }
                receive(request, value);
            });
    connect(&mClient, &LanguageClient::DiagnosticsPublished, this, &CodeAssistance::receiveDiagnostics);
    // A server refresh re-asks for the layers after the same pause as an edit.
    connect(&mClient, &LanguageClient::RefreshRequested, this, [this]() {
        if (mUri.isEmpty() == false)
        {
            mAnalysisSync.start();
        }
    });
    connect(&mEditor, &CodeEditor::HoverRequested, this, &CodeAssistance::requestHover);
    mAnalysisSync.setSingleShot(true);
    mAnalysisSync.setInterval(ANALYSIS_DELAY_MILLISECONDS);
    connect(&mAnalysisSync, &QTimer::timeout, this, &CodeAssistance::synchronizeAnalysis);
    connect(&mDocument, &CodeDocumentController::TextAccepted, this, [this]() {
        mEditor.HideHover();
        if (mUri.isEmpty() == false && mClient.IsReady() == true)
        {
            mAnalysisSync.start();
        }
    });
    connect(&mClient, &LanguageClient::Ready, this, [this]() {
        applyAvailability();
        requestCurrent();
        synchronizeAnalysis();
    });
    connect(&mClient,
            &LanguageClient::RequestUnavailable,
            this,
            [this](LanguageRequestKind kind, const QString& message) {
                static_cast<void>(kind);
                Cancel();
                Q_EMIT StatusChanged(message);
            });
    connect(&mClient,
            &LanguageClient::RequestFailed,
            this,
            [this](int32 request, LanguageRequestKind kind, const QString& message) {
                static_cast<void>(kind);
                if (request != 0 && request == mRename.Request)
                {
                    mRename = {};
                    Q_EMIT StatusChanged(message);
                    return;
                }
                if (request == mRequest)
                {
                    Cancel();
                    Q_EMIT StatusChanged(message);
                }
            });
    // A restart keeps the intent: the next Ready re-sends it if it is still current.
    connect(&mClient, &LanguageClient::Restarting, this, &CodeAssistance::StatusChanged);
    connect(&mClient, &LanguageClient::Failed, this, [this](const QString& message) {
        Cancel();
        mServer.clear();
        resetAvailability();
        clearAnalysis();
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
    if (mRequest != 0)
    {
        mClient.Cancel(mRequestKind);
    }
    mHasIntent = false;
    mRequest = 0;
    if (mResolve.Request != 0)
    {
        mClient.Cancel(LanguageRequestKind::CompletionResolve);
    }
    mResolve = {};
    mCompletions.clear();
    mSignatures = {};
    mNavigationTargets.clear();
    mEditor.GetNavigation().Dismiss();
    mEditor.HideAssistance();
}

void CodeAssistance::applyAvailability()
{
    // Capability dispatch (CE09): a command the server does not serve is disabled
    // with its reason, never sent and left to fail.
    CodeNavigation& navigation = mEditor.GetNavigation();
    for (usize index = 0; index < CODE_NAVIGATION_KIND_COUNT; ++index)
    {
        const CodeNavigationKind kind = static_cast<CodeNavigationKind>(index);
        navigation.SetSemanticAvailability(kind, mClient.GetUnavailableReason(GetNavigationRequest(kind)));
    }
    CodeAssistanceAvailability availability;
    availability.CompletionUnavailableReason = mClient.GetUnavailableReason(LanguageRequestKind::Completion);
    availability.SignatureUnavailableReason = mClient.GetUnavailableReason(LanguageRequestKind::SignatureHelp);
    availability.HoverUnavailableReason = mClient.GetUnavailableReason(LanguageRequestKind::Hover);
    availability.RenameUnavailableReason = mClient.GetUnavailableReason(LanguageRequestKind::Rename);
    availability.CodeActionsUnavailableReason = mClient.GetUnavailableReason(LanguageRequestKind::CodeActions);
    const auto readTriggers = [this](const QString& capability, QString& outTriggers) {
        const QJsonObject options = mClient.GetCapabilities().value(capability).toObject();
        if (options.contains(QStringLiteral("triggerCharacters")) == false)
        {
            return;
        }
        outTriggers.clear();
        for (const QString& key : {QStringLiteral("triggerCharacters"), QStringLiteral("retriggerCharacters")})
        {
            for (const QJsonValue& value : options.value(key).toArray())
            {
                // One UTF-16 unit each: the view compares the typed character.
                if (value.toString().size() == 1)
                {
                    outTriggers += value.toString();
                }
            }
        }
    };
    readTriggers(QStringLiteral("completionProvider"), availability.CompletionTriggers);
    readTriggers(QStringLiteral("signatureHelpProvider"), availability.SignatureTriggers);
    mEditor.SetAssistanceAvailability(availability);
}

void CodeAssistance::resetAvailability()
{
    mEditor.GetNavigation().ResetSemanticAvailability();
    mEditor.SetAssistanceAvailability({});
}

const QString& CodeAssistance::GetServerLog() const noexcept
{
    return mClient.GetLog();
}

void CodeAssistance::clearAnalysis()
{
    mAnalysisSync.stop();
    mHover = {};
    mEditor.HideHover();
    mEditor.SetAnalysisDiagnostics({}, {});
    mLayers.Clear();
    mCodeActionDelay.stop();
    mAnalysisRecords.clear();
    mAnalysisIdentity = {};
    mActions.Clear();
}

bool CodeAssistance::buildUnit(CodeTextCapture& outCapture, QString& outSource, QString& outUnit) const
{
    CodeTextCapture capture = mDocument.Capture();
    QString source;
    if (DecodeCodeText(capture.Text, source) == false)
    {
        return false;
    }
    outUnit = mPrefix + source + mSuffix;
    outSource = source;
    outCapture = capture;
    return true;
}

void CodeAssistance::synchronizeAnalysis()
{
    CodeTextCapture capture;
    QString source;
    QString unit;
    if (mUri.isEmpty() == false && mDocument.IsBound() == true && mClient.IsReady() == true &&
        buildUnit(capture, source, unit) == true)
    {
        mClient.SetDocument(mUri, unit);
        const int32 start = static_cast<int32>(mPrefix.size());
        mLayers.Refresh(capture.Identity,
                        mContext,
                        unit,
                        {
                            .Start = start,
                            .End = start + static_cast<int32>(source.size()),
                            .FirstLine = static_cast<int32>(mPrefix.count(QLatin1Char('\n'))),
                        });
    }
}

void CodeAssistance::receiveDiagnostics(const QString& uri, int32 version, const QJsonArray& diagnostics)
{
    // Stale-result rules: the report must name this context's document, describe
    // the version the server last received, and that text must still be the
    // current draft. Anything else waits for the next report.
    if (mUri.isEmpty() == true || QUrl(uri) != QUrl(mUri) || (version >= 0 && version != mClient.GetSentVersion()) ||
        QUrl(mClient.GetSentUri()) != QUrl(mUri))
    {
        return;
    }
    CodeTextCapture capture;
    QString source;
    QString unit;
    if (buildUnit(capture, source, unit) == false || unit != mClient.GetSentText())
    {
        return;
    }
    const QList<int32> lines = IndexLanguageLines(unit);
    const int32 begin = static_cast<int32>(mPrefix.size());
    const int32 sourceEnd = begin + static_cast<int32>(source.size());
    QList<CodeAnalysisDiagnostic> markers;
    mAnalysisRecords.clear();
    mAnalysisIdentity = capture.Identity;
    for (const QJsonValue& value : diagnostics)
    {
        const QJsonObject diagnostic = value.toObject();
        const int32 severity = diagnostic.value(QStringLiteral("severity")).toInt(SEVERITY_ERROR);
        const QJsonObject range = diagnostic.value(QStringLiteral("range")).toObject();
        int32 start = 0;
        int32 end = 0;
        // Hints are suggestions, not problems; generated surroundings are not authored text.
        if (severity > SEVERITY_INFORMATION ||
            GetLanguageOffset(unit,
                              lines,
                              range.value(QStringLiteral("start")).toObject(),
                              mClient.GetPositionEncoding(),
                              start) == false ||
            GetLanguageOffset(unit,
                              lines,
                              range.value(QStringLiteral("end")).toObject(),
                              mClient.GetPositionEncoding(),
                              end) == false ||
            start < begin || end > sourceEnd || end < start)
        {
            continue;
        }
        mAnalysisRecords.append({ .Diagnostic = diagnostic, .Start = start - begin, .End = end - begin });
        markers.append(
        {
            .Start = start - begin,
            .End = end - begin,
            .Severity = severity == SEVERITY_ERROR ? CodeDiagnosticSeverity::Error : CodeDiagnosticSeverity::Warning,
            .Message = diagnostic.value(QStringLiteral("message")).toString().left(MAX_ANALYSIS_MESSAGE_UNITS),
        });
    }
    mEditor.SetAnalysisDiagnostics(markers, mProfile->ServerName);
}

void CodeAssistance::requestHover(int32 position)
{
    // Hover never prepares a context: a resting pointer must not start tools.
    mHover = {};
    if (mUri.isEmpty() == true || mClient.CanRequest(LanguageRequestKind::Hover) == false ||
        HasCodeLanguageService(mEditor.GetLanguage()) == false || mDocument.IsBound() == false)
    {
        // Without a server hover the editor still describes diagnostics there.
        mEditor.ShowHover(position, {});
        return;
    }
    CodeTextCapture capture;
    QString source;
    QString unit;
    if (buildUnit(capture, source, unit) == false || position < 0 || position > source.size())
    {
        return;
    }
    mClient.SetDocument(mUri, unit);
    const int32 request = mClient.Request(
        LanguageRequestKind::Hover,
        {{QStringLiteral("position"),
          MakeLanguagePosition(unit, static_cast<int32>(mPrefix.size()) + position, mClient.GetPositionEncoding())}});
    mHover = { .Request = request, .Position = position, .Identity = capture.Identity, .Context = mContext };
}

void CodeAssistance::receiveHover(int32 request, const QJsonValue& value)
{
    if (request == 0 || request != mHover.Request || mHover.Context != mContext ||
        mHover.Identity != mDocument.Capture().Identity)
    {
        return;
    }
    const int32 position = mHover.Position;
    mHover = {};
    mEditor.ShowHover(position, ReadHoverText(value.toObject().value(QStringLiteral("contents"))));
}

void CodeAssistance::ClearContext()
{
    resetAvailability();
    clearAnalysis();
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
                                const CodeNavigationSources& navigationSources,
                                const LanguageServerProfile& profile)
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
    if (mServer != server || mServerStamp != stamp || mServerArguments != serverArguments || mProfile != &profile)
    {
        mServer = server;
        mServerStamp = stamp;
        mServerArguments = serverArguments;
        mProfile = &profile;
        resetAvailability();
        mClient.Start(profile, server, workspace, serverArguments);
    }
    if (mClient.IsReady() == true)
    {
        applyAvailability();
    }
    requestCurrent();
    synchronizeAnalysis();
}

void CodeAssistance::Request(bool isSignature)
{
    Cancel();
    mShouldTrackSignature = false;
    mShouldTrackCompletion = false;
    if (HasCodeLanguageService(mEditor.GetLanguage()) == false || mDocument.IsBound() == false ||
        mEditor.isReadOnly() == true)
    {
        return;
    }
#if defined(Q_OS_WASM)
    static_cast<void>(isSignature);
    Q_EMIT StatusChanged(QStringLiteral("Shader assistance needs native Slang; unavailable in the browser editor."));
#else
    mIsNavigation = false;
    mIsRename = false;
    mIsCodeActions = false;
    mShouldTrackSignature = isSignature;
    mShouldTrackCompletion = isSignature == false;
    mIsSignature = isSignature;
    beginRequest();
#endif
}

void CodeAssistance::RequestNavigation(CodeNavigationKind kind)
{
    Cancel();
    if (HasCodeLanguageService(mEditor.GetLanguage()) == false || mDocument.IsBound() == false)
    {
        return;
    }
#if defined(Q_OS_WASM)
    static_cast<void>(kind);
    Q_EMIT StatusChanged(QStringLiteral(
        "Semantic navigation needs native Slang. Quick Open, line navigation and history remain available."));
#else
    mIsNavigation = true;
    mIsRename = false;
    mIsCodeActions = false;
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
           HasCodeLanguageService(mEditor.GetLanguage()) == true;
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
    if (mIsCodeActions == true)
    {
        mHasIntent = false;
        requestCodeActions(true);
        return;
    }
    const int32 position = static_cast<int32>(mPrefix.size()) + mCaret;
    mClient.SetDocument(mUri, mUnit);
    LanguageRequestKind kind =
        mIsSignature == true ? LanguageRequestKind::SignatureHelp : LanguageRequestKind::Completion;
    if (mIsNavigation == true)
    {
        kind = GetNavigationRequest(mNavigationKind);
    }
    if (mIsRename == true)
    {
        kind = LanguageRequestKind::PrepareRename;
        if (mClient.IsReady() == true && mClient.CanRequest(LanguageRequestKind::Rename) == false)
        {
            mHasIntent = false;
            Q_EMIT StatusChanged(mClient.GetUnavailableReason(LanguageRequestKind::Rename));
            return;
        }
        // A server without prepareRename renames the word at the caret.
        if (mClient.IsReady() == true && mClient.CanRequest(LanguageRequestKind::PrepareRename) == false)
        {
            mHasIntent = false;
            if (mWordStart == mWordEnd)
            {
                Q_EMIT StatusChanged(QStringLiteral("Place the caret on a name to rename it."));
                return;
            }
            showRenameBox(mWordStart, mWordEnd, source.mid(mWordStart, mWordEnd - mWordStart));
            return;
        }
    }
    QJsonObject parameters;
    if (kind == LanguageRequestKind::WorkspaceSymbols)
    {
        parameters.insert(QStringLiteral("query"), source.mid(mWordStart, mWordEnd - mWordStart));
    }
    else if (kind != LanguageRequestKind::DocumentSymbols)
    {
        parameters.insert(QStringLiteral("position"),
                          MakeLanguagePosition(mUnit, position, mClient.GetPositionEncoding()));
    }
    if (kind == LanguageRequestKind::References)
    {
        parameters.insert(QStringLiteral("context"), QJsonObject{{QStringLiteral("includeDeclaration"), true}});
    }
    mRequestKind = kind;
    mRequest = mClient.Request(kind, parameters);
    if (mRequest == 0 && mHasIntent == true)
    {
        Q_EMIT StatusChanged(QStringLiteral("Starting Slang shader assistance…"));
    }
}

bool CodeAssistance::mapPosition(const QJsonObject& position, usize& outBytes) const
{
    int32 offset = 0;
    int32 sourceLength = 0;
    if (GetLanguageOffset(mUnit, position, mClient.GetPositionEncoding(), offset) == false ||
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
    if (mIsRename == true)
    {
        receivePrepareRename(value);
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
                                                       message,
                                                       mClient.GetPositionEncoding());
        Q_EMIT StatusChanged(message);
        if (IsCodeGoToNavigation(mNavigationKind) == true && mNavigationTargets.size() == 1)
        {
            acceptNavigation(0);
        }
        else if (mNavigationKind == CodeNavigationKind::References)
        {
            mEditor.GetNavigation().ShowReferences(
            {
                .Symbol = source.mid(mWordStart, mWordEnd - mWordStart),
                .Targets = mNavigationTargets,
                .Message = message,
            });
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
    const QString text = mEditor.toPlainText();
    const QString word = text.mid(mWordStart, mCaret - mWordStart);
    // Server items, built-in snippets and words from open buffers are merged and
    // ranked by the command palette's fuzzy matcher (CE10). After a `.` only the
    // server knows the members, so the other sources stay out.
    struct Ranked final
    {
        int32 Index = 0;
        int32 Score = 0;
        CodeCompletionSource Source = CodeCompletionSource::Server;
    };
    QList<Completion> candidates;
    QList<Ranked> ranked;
    int32 examined = 0;
    for (const QJsonValue& entry : items)
    {
        if (++examined > MAX_COMPLETION_CANDIDATES)
        {
            break;
        }
        QJsonObject item = entry.toObject();
        const QString label = item.value(QStringLiteral("label")).toString();
        const int32 score = ScoreFuzzyMatch(word, item.value(QStringLiteral("filterText")).toString(label));
        if (label.isEmpty() == true || label.size() > MAX_LABEL_UNITS || score < 0)
        {
            continue;
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
        ranked.append(
            { .Index = static_cast<int32>(candidates.size()), .Score = score, .Source = CodeCompletionSource::Server });
        candidates.append(
        {
            .Label = label,
            .Detail = item.value(QStringLiteral("detail")).toString().left(MAX_DOCUMENTATION_UNITS),
            .Item = item,
            .Original = entry.toObject(),
            .IsResolved = false,
        });
    }
    const bool isMemberAccess = mWordStart > 0 && text.at(mWordStart - 1) == QLatin1Char('.');
    if (isMemberAccess == false)
    {
        constexpr int32 SNIPPET_FORMAT = 2;
        const qsizetype lineStart = text.lastIndexOf(QLatin1Char('\n'), qMax(0, mWordStart - 1)) + 1;
        qsizetype indentEnd = lineStart;
        while (indentEnd < mWordStart &&
               (text.at(indentEnd) == QLatin1Char(' ') || text.at(indentEnd) == QLatin1Char('\t')))
        {
            ++indentEnd;
        }
        const QStringView indentation = QStringView(text).sliced(lineStart, indentEnd - lineStart);
        for (const CodeSnippetTemplate& snippet : GetCodeSnippetTemplates(mEditor.GetLanguage()))
        {
            const QString prefix(snippet.Prefix);
            const int32 score = ScoreFuzzyMatch(word, prefix);
            const QString body = IndentCodeSnippetBody(QString(snippet.Body), indentation);
            CodeSnippet expanded;
            if (score < 0 || ParseCodeSnippet(body, expanded) == false)
            {
                continue;
            }
            ranked.append(
            {
                .Index = static_cast<int32>(candidates.size()),
                .Score = score,
                .Source = CodeCompletionSource::Snippet,
            });
            candidates.append(
            {
                .Label = prefix,
                .Detail = QString(snippet.Description) + QStringLiteral(" (snippet)"),
                .Item = {{QStringLiteral("label"), prefix},
                         {QStringLiteral("insertText"), body},
                         {QStringLiteral("insertTextFormat"), SNIPPET_FORMAT},
                         {QStringLiteral("documentation"), expanded.Text}},
                .Original = {},
                .IsResolved = true,
            });
        }
        if (word.isEmpty() == false)
        {
            QSet<QString> serverLabels;
            for (const Completion& candidate : candidates)
            {
                serverLabels.insert(candidate.Label);
            }
            for (const QString& found : CollectCodeCompletionWords(collectOpenBuffers(), word))
            {
                if (serverLabels.contains(found) == true)
                {
                    continue;
                }
                ranked.append(
                {
                    .Index = static_cast<int32>(candidates.size()),
                    .Score = ScoreFuzzyMatch(word, found),
                    .Source = CodeCompletionSource::Word,
                });
                candidates.append(
                {
                    .Label = found,
                    .Detail = QStringLiteral("word in an open file"),
                    .Item = {{QStringLiteral("label"), found}, {QStringLiteral("insertText"), found}},
                    .Original = {},
                    .IsResolved = true,
                });
            }
        }
    }
    // Higher scores first; equal scores keep server, then snippet, then word order.
    QList<Ranked> scratch = ranked;
    [[maybe_unused]] const bool isSorted =
        StableSort(ranked.data(),
                   scratch.data(),
                   static_cast<usize>(ranked.size()),
                   [](const Ranked& left, const Ranked& right) noexcept {
                       return left.Score != right.Score ? left.Score > right.Score : left.Source < right.Source;
                   });
    LUDUS_ASSERT(isSorted == true || ranked.isEmpty() == true);
    QStringList labels;
    QStringList details;
    for (const Ranked& entry : ranked)
    {
        if (mCompletions.size() == MAX_COMPLETIONS)
        {
            break;
        }
        const Completion& candidate = candidates.at(entry.Index);
        mCompletions.append(candidate);
        labels.append(candidate.Detail.isEmpty() == true
                          ? candidate.Label
                          : candidate.Label + QStringLiteral("  —  ") + candidate.Detail);
        details.append(GetDocumentation(candidate.Item.value(QStringLiteral("documentation"))));
    }
    mShouldTrackCompletion = mCompletions.isEmpty() == false;
    mEditor.ShowCompletions(labels, details);
    Q_EMIT StatusChanged(mCompletions.isEmpty() == true ? QStringLiteral("No matching shader symbols.")
                         : ranked.size() > MAX_COMPLETIONS
                             ? QStringLiteral("Showing the best %1 suggestions. Type more of the name to narrow them.")
                                   .arg(MAX_COMPLETIONS)
                             : QString());
}

QList<QString> CodeAssistance::collectOpenBuffers() const
{
    // The buffer being edited first, then every other open editor of the same language.
    QList<QString> texts{mEditor.toPlainText()};
    for (QWidget* widget : QApplication::allWidgets())
    {
        const CodeEditor* editorOrNull = qobject_cast<const CodeEditor*>(widget);
        if (editorOrNull != nullptr && editorOrNull != &mEditor &&
            editorOrNull->GetLanguage() == mEditor.GetLanguage() &&
            editorOrNull->document()->characterCount() <= CODE_MAX_COMPLETION_WORD_SCAN_UNITS)
        {
            texts.append(editorOrNull->toPlainText());
        }
    }
    return texts;
}

void CodeAssistance::SetWorkspaceEditHost(CodeWorkspaceEditHost* hostOrNull) noexcept
{
    mWorkspaceEditHostOrNull = hostOrNull;
    mActions.SetWorkspaceEditHost(hostOrNull);
}

CodeWorkspaceEditContext CodeAssistance::makeWorkspaceEditContext(const QString& unit, const QString& source) const
{
    const int32 begin = static_cast<int32>(mPrefix.size());
    return
    {
        .Uri = mUri,
        .Unit = unit,
        .SourceStart = begin,
        .SourceEnd = begin + static_cast<int32>(source.size()),
        .SentVersion = mClient.GetSentVersion(),
        .Sources = mNavigationSources,
        .Encoding = mClient.GetPositionEncoding(),
        .CanEditOtherFiles = mWorkspaceEditHostOrNull != nullptr,
    };
}

void CodeAssistance::RequestCodeActions()
{
    Cancel();
    if (HasCodeLanguageService(mEditor.GetLanguage()) == false || mDocument.IsBound() == false ||
        mEditor.isReadOnly() == true)
    {
        return;
    }
#if defined(Q_OS_WASM)
    Q_EMIT StatusChanged(QStringLiteral("Code actions need native shader assistance; unavailable in the browser "
                                        "editor."));
#else
    mIsNavigation = false;
    mIsSignature = false;
    mIsRename = false;
    mIsCodeActions = true;
    beginRequest();
#endif
}

void CodeAssistance::scheduleCodeActions()
{
    if (mUri.isEmpty() == false && mClient.CanRequest(LanguageRequestKind::CodeActions) == true &&
        HasCodeLanguageService(mEditor.GetLanguage()) == true && mEditor.isReadOnly() == false)
    {
        mCodeActionDelay.start();
    }
}

void CodeAssistance::requestCodeActions(bool isExplicit)
{
    CodeTextCapture capture;
    QString source;
    QString unit;
    if (isExplicit == true)
    {
        mCodeActionDelay.stop();
    }
    if (mUri.isEmpty() == true || mDocument.IsBound() == false || buildUnit(capture, source, unit) == false)
    {
        return;
    }
    mClient.SetDocument(mUri, unit);
    const QTextCursor cursor = mEditor.textCursor();
    CodeActionScope scope
    {
        .Identity = capture.Identity,
        .Edit = makeWorkspaceEditContext(unit, source),
        .Start = qBound(0, cursor.selectionStart(), static_cast<int32>(source.size())),
        .End = qBound(0, cursor.selectionEnd(), static_cast<int32>(source.size())),
        .Diagnostics = {},
    };
    // The diagnostics there are sent as the server published them, while they still describe this draft.
    if (mAnalysisIdentity == capture.Identity)
    {
        for (const AnalysisRecord& record : mAnalysisRecords)
        {
            if (record.Start <= scope.End && record.End >= scope.Start)
            {
                scope.Diagnostics.append(record.Diagnostic);
            }
        }
    }
    mActions.Request(scope, isExplicit);
}

void CodeAssistance::RequestRename()
{
    Cancel();
    mRename = {};
    if (HasCodeLanguageService(mEditor.GetLanguage()) == false || mDocument.IsBound() == false ||
        mEditor.isReadOnly() == true)
    {
        return;
    }
#if defined(Q_OS_WASM)
    Q_EMIT StatusChanged(QStringLiteral("Rename needs native shader assistance; unavailable in the browser editor."));
#else
    mIsNavigation = false;
    mIsSignature = false;
    mIsRename = true;
    mIsCodeActions = false;
    beginRequest();
#endif
}

void CodeAssistance::receivePrepareRename(const QJsonValue& value)
{
    mHasIntent = false;
    mRequest = 0;
    QString source;
    if (DecodeCodeText(mCapture.Text, source) == false)
    {
        return;
    }
    // LSP 3.17: a Range, { range, placeholder }, { defaultBehavior } or null.
    const QJsonObject result = value.toObject();
    if (value.isObject() == false || result.isEmpty() == true)
    {
        Q_EMIT StatusChanged(QStringLiteral("Nothing at the caret can be renamed. Place the caret on a name you "
                                            "authored and press F2 again."));
        return;
    }
    if (result.value(QStringLiteral("defaultBehavior")).toBool(false) == true)
    {
        if (mWordStart == mWordEnd)
        {
            Q_EMIT StatusChanged(QStringLiteral("Place the caret on a name to rename it."));
            return;
        }
        showRenameBox(mWordStart, mWordEnd, source.mid(mWordStart, mWordEnd - mWordStart));
        return;
    }
    const QJsonObject range =
        result.contains(QStringLiteral("range")) == true ? result.value(QStringLiteral("range")).toObject() : result;
    const int32 begin = static_cast<int32>(mPrefix.size());
    int32 start = 0;
    int32 end = 0;
    if (GetLanguageOffset(mUnit,
                          range.value(QStringLiteral("start")).toObject(),
                          mClient.GetPositionEncoding(),
                          start) == false ||
        GetLanguageOffset(mUnit, range.value(QStringLiteral("end")).toObject(), mClient.GetPositionEncoding(), end) ==
            false ||
        start < begin || end > begin + source.size() || end <= start)
    {
        Q_EMIT StatusChanged(QStringLiteral("That name is declared in generated code, so it cannot be renamed here. "
                                            "Rename it where it is authored."));
        return;
    }
    const QString placeholder =
        result.value(QStringLiteral("placeholder")).toString(source.mid(start - begin, end - start));
    showRenameBox(start - begin, end - begin, placeholder);
}

void CodeAssistance::showRenameBox(int32 start, int32 end, const QString& name)
{
    mRename =
    {
        .Request = 0,
        .Identity = mCapture.Identity,
        .Context = mContext,
        .Start = start,
        .End = end,
        .OldName = name,
        .NewName = {},
        .ShouldPreview = false,
    };
    mEditor.ShowRenameBox(start, end, name);
}

bool CodeAssistance::isRenameCurrent(const RenameRequest& rename) const
{
    return rename.Context != 0 && rename.Context == mContext && rename.Identity == mDocument.Capture().Identity;
}

void CodeAssistance::acceptRename(const QString& name, bool shouldPreview)
{
    if (isRenameCurrent(mRename) == false)
    {
        mRename = {};
        Q_EMIT StatusChanged(QStringLiteral("The source changed while you were renaming. Press F2 again; nothing "
                                            "was changed."));
        return;
    }
    CodeTextCapture capture;
    QString source;
    QString unit;
    if (name.isEmpty() == true || name == mRename.OldName || buildUnit(capture, source, unit) == false)
    {
        mRename = {};
        return;
    }
    mClient.SetDocument(mUri, unit);
    mRename.NewName = name;
    mRename.ShouldPreview = shouldPreview;
    mRename.Request = mClient.Request(LanguageRequestKind::Rename,
                                      {{QStringLiteral("position"),
                                        MakeLanguagePosition(unit,
                                                             static_cast<int32>(mPrefix.size()) + mRename.Start,
                                                             mClient.GetPositionEncoding())},
                                       {QStringLiteral("newName"), name}});
    if (mRename.Request == 0)
    {
        mRename = {};
        Q_EMIT StatusChanged(QStringLiteral("Shader assistance is not ready. Wait for it to start, then press F2 "
                                            "again; nothing was changed."));
    }
}

void CodeAssistance::receiveRename(int32 request, const QJsonValue& value)
{
    if (request == 0 || request != mRename.Request)
    {
        return;
    }
    const RenameRequest rename = mRename;
    mRename = {};
    CodeTextCapture capture;
    QString source;
    QString unit;
    if (isRenameCurrent(rename) == false || buildUnit(capture, source, unit) == false)
    {
        Q_EMIT StatusChanged(QStringLiteral("The source changed before the rename arrived. Press F2 again; nothing "
                                            "was changed."));
        return;
    }
    const CodeWorkspaceEditContext context = makeWorkspaceEditContext(unit, source);
    QList<CodeWorkspaceFileChange> changes;
    QString message;
    if (ReadCodeWorkspaceEdit(value.toObject(), context, changes, message) == false)
    {
        Q_EMIT StatusChanged(message);
        return;
    }
    if (changes.isEmpty() == true)
    {
        Q_EMIT StatusChanged(QStringLiteral("The server found nothing to rename at that name."));
        return;
    }
    const bool isSpanningFiles = changes.size() > 1 || changes.constFirst().File.isEmpty() == false;
    if (rename.ShouldPreview == false && isSpanningFiles == false)
    {
        applyRename(rename, changes);
        return;
    }
    // A change beyond the edited source is shown first, as is any rename asked for with Shift+Enter.
    const QString sourceName = mEditor.GetNavigation().GetSource().isEmpty() == true
                                   ? QStringLiteral("Current source")
                                   : mEditor.GetNavigation().GetSource();
    CodeWorkspaceEditPreview* preview =
        new CodeWorkspaceEditPreview(QStringLiteral("Rename '%1' to '%2'").arg(rename.OldName, rename.NewName),
                                     sourceName,
                                     changes,
                                     &mEditor);
    preview->setAttribute(Qt::WA_DeleteOnClose);
    connect(preview, &QDialog::accepted, this, [this, rename, changes]() { applyRename(rename, changes); });
    connect(preview, &QDialog::rejected, this, [this]() {
        Q_EMIT StatusChanged(QStringLiteral("Rename cancelled; nothing was changed."));
    });
    preview->open();
}

void CodeAssistance::applyRename(const RenameRequest& rename, const QList<CodeWorkspaceFileChange>& changes)
{
    if (isRenameCurrent(rename) == false)
    {
        Q_EMIT StatusChanged(QStringLiteral("The source changed before the rename was applied. Press F2 again; "
                                            "nothing was changed."));
        return;
    }
    qsizetype editCount = 0;
    for (const CodeWorkspaceFileChange& change : changes)
    {
        editCount += change.Edits.size();
    }
    const CodeEditStatus status =
        ApplyCodeWorkspaceChanges(mDocument, mWorkspaceEditHostOrNull, changes, rename.Start, "Rename Symbol");
    switch (status)
    {
        case CodeEditStatus::Ok:
            Q_EMIT StatusChanged(QStringLiteral("Renamed '%1' to '%2': %3 changes in %4 files. Undo reverts all of "
                                                "them.")
                                     .arg(rename.OldName, rename.NewName)
                                     .arg(editCount)
                                     .arg(changes.size()));
            break;
        case CodeEditStatus::Conflict:
            Q_EMIT StatusChanged(QStringLiteral("A file changed before the rename was applied. Press F2 again; "
                                                "nothing was changed."));
            break;
        case CodeEditStatus::Capacity:
            Q_EMIT StatusChanged(QStringLiteral("The rename would exceed a source or package size limit. Choose a "
                                                "shorter name; nothing was changed."));
            break;
        case CodeEditStatus::InvalidText:
        case CodeEditStatus::Unavailable:
            Q_EMIT StatusChanged(QStringLiteral("The rename could not be applied to these sources. Press F2 again; "
                                                "nothing was changed."));
            break;
    }
}

void CodeAssistance::resolveCompletion(int32 row)
{
    // Documentation and details arrive lazily for the highlighted suggestion only
    // (CE10); a server without resolveProvider is never asked.
    if (isCurrent() == false || row < 0 || row >= mCompletions.size() || mCompletions.at(row).IsResolved == true ||
        mClient.CanRequest(LanguageRequestKind::CompletionResolve) == false)
    {
        return;
    }
    mResolve =
    {
        .Request = mClient.Request(LanguageRequestKind::CompletionResolve, mCompletions.at(row).Original),
        .Row = row,
    };
}

void CodeAssistance::receiveResolve(int32 request, const QJsonValue& value)
{
    if (request == 0 || request != mResolve.Request || isCurrent() == false || mResolve.Row >= mCompletions.size())
    {
        return;
    }
    Completion& completion = mCompletions[mResolve.Row];
    const int32 row = mResolve.Row;
    mResolve = {};
    completion.IsResolved = true;
    // The resolved item adds fields, such as documentation, detail and
    // additional edits; the edits computed from the list defaults stay unless it
    // replaces them.
    const QJsonObject resolved = value.toObject();
    for (QJsonObject::const_iterator field = resolved.constBegin(); field != resolved.constEnd(); ++field)
    {
        completion.Item.insert(field.key(), field.value());
    }
    completion.Detail = completion.Item.value(QStringLiteral("detail")).toString().left(MAX_DOCUMENTATION_UNITS);
    mEditor.UpdateCompletion(row,
                             completion.Detail.isEmpty() == true
                                 ? completion.Label
                                 : completion.Label + QStringLiteral("  —  ") + completion.Detail,
                             GetDocumentation(completion.Item.value(QStringLiteral("documentation"))));
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
    const bool isSnippet = completion.Item.value(QStringLiteral("insertTextFormat")).toInt() == SNIPPET_FORMAT;
    QJsonObject primary = completion.Item.value(QStringLiteral("textEdit")).toObject();
    QString insertText = completion.Item.value(QStringLiteral("insertText")).toString(completion.Label);
    CodeSnippet snippet;
    if (isSnippet == true)
    {
        // Snippets (CE10) expand to their text; the editor then visits their tab stops.
        const QString source =
            primary.isEmpty() == true ? insertText : primary.value(QStringLiteral("newText")).toString();
        if (ParseCodeSnippet(source, snippet) == false)
        {
            Cancel();
            Q_EMIT StatusChanged(QStringLiteral("This completion uses a snippet transform or more placeholders than "
                                                "the editor supports. Choose another symbol; no edit was applied."));
            return;
        }
        if (primary.isEmpty() == true)
        {
            insertText = snippet.Text;
        }
        else
        {
            primary.insert(QStringLiteral("newText"), snippet.Text);
        }
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
    bool isValid = true;
    if (primary.isEmpty() == true)
    {
        Replacement replacement;
        isValid = GetCodeByteOffset(mCapture.Text, mWordStart, replacement.Start) == true &&
                  GetCodeByteOffset(mCapture.Text, mWordEnd, replacement.End) == true;
        replacement.Text = insertText.toUtf8();
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
    usize primaryStart = 0;
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
        if (replacement.IsPrimary == true)
        {
            primaryStart = static_cast<usize>(result.size());
        }
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
        case CodeEditStatus::Ok: {
            Q_EMIT StatusChanged(QString());
            int32 snippetStart = 0;
            if (isSnippet == true && GetCodeCursorPosition(result, primaryStart, snippetStart) == true)
            {
                mEditor.StartSnippet(snippetStart, snippet);
            }
            break;
        }
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
