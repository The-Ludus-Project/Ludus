#include "internal/code_workspace_edit.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/stable_sort.hpp>

#include "internal/code_file_limits.h"
#include "internal/code_text.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QUrl>

// Thanks to Microsoft, Language Server Protocol 3.17, "WorkspaceEdit" and
// "TextDocumentEdit": edits arrive per URI, optionally versioned, and resource
// operations (create, rename, delete) may be mixed in.
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#workspaceEdit
namespace ludus::editor
{
namespace
{
using namespace foundation;

struct PendingFile final
{
    CodeWorkspaceFileChange Change;
    QList<int32> LineStarts;
    /// Unit offsets for the edited source, file text offsets otherwise.
    int32 Offset = 0;
    int32 Limit = 0;
};
} // namespace

bool ReadCodeWorkspaceEdit(const QJsonObject& edit,
                           const CodeWorkspaceEditContext& context,
                           QList<CodeWorkspaceFileChange>& outChanges,
                           QString& outMessage)
{
    QList<QPair<QString, QJsonArray>> documents;
    const QJsonValue documentChanges = edit.value(QStringLiteral("documentChanges"));
    if (documentChanges.isArray() == true)
    {
        for (const QJsonValue value : documentChanges.toArray())
        {
            const QJsonObject change = value.toObject();
            if (change.contains(QStringLiteral("kind")) == true)
            {
                outMessage = QStringLiteral("The server would also create, move or delete files, which the editor "
                                            "does not do. Make that change by hand; nothing was changed.");
                return false;
            }
            const QJsonObject document = change.value(QStringLiteral("textDocument")).toObject();
            const QString uri = document.value(QStringLiteral("uri")).toString();
            const QJsonValue version = document.value(QStringLiteral("version"));
            if (QUrl(uri) == QUrl(context.Uri) && version.isDouble() == true && version.toInt() != context.SentVersion)
            {
                outMessage = QStringLiteral("The server answered for an earlier version of the source. Try again; "
                                            "nothing was changed.");
                return false;
            }
            documents.append({uri, change.value(QStringLiteral("edits")).toArray()});
        }
    }
    else
    {
        const QJsonObject changes = edit.value(QStringLiteral("changes")).toObject();
        for (QJsonObject::const_iterator change = changes.constBegin(); change != changes.constEnd(); ++change)
        {
            documents.append({change.key(), change.value().toArray()});
        }
    }
    QMap<QString, PendingFile> files;
    qsizetype editCount = 0;
    for (const QPair<QString, QJsonArray>& document : documents)
    {
        const QString& uri = document.first;
        const bool isSource = QUrl(uri) == QUrl(context.Uri);
        const QString key = isSource == true ? QString() : uri;
        if (files.contains(key) == false)
        {
            PendingFile pending;
            if (isSource == true)
            {
                pending.Change.Text = context.Unit.mid(context.SourceStart, context.SourceEnd - context.SourceStart);
                pending.LineStarts = IndexLanguageLines(context.Unit);
                pending.Offset = context.SourceStart;
                pending.Limit = context.SourceEnd;
            }
            else
            {
                const QMap<QString, CodeNavigationSource>::const_iterator source = context.Sources.constFind(uri);
                const QString name = QFileInfo(QUrl(uri).toLocalFile()).fileName();
                if (source == context.Sources.cend())
                {
                    outMessage = QStringLiteral("The server would change %1, which is not part of this shader's "
                                                "context. Check the selected shader context; nothing was changed.")
                                     .arg(name.isEmpty() == true ? uri : name);
                    return false;
                }
                if (source->File.isEmpty() == true)
                {
                    outMessage = QStringLiteral("The server would change generated or read-only code in %1. Change "
                                                "the symbol where it is authored; nothing was changed.")
                                     .arg(name.isEmpty() == true ? QStringLiteral("the shader context") : name);
                    return false;
                }
                if (context.CanEditOtherFiles == false)
                {
                    outMessage = QStringLiteral("The server would also change %1. Open this shader from its package "
                                                "to change several files at once; nothing was changed.")
                                     .arg(source->File);
                    return false;
                }
                pending.Change.File = source->File;
                pending.Change.Text = source->Text;
                pending.LineStarts = IndexLanguageLines(source->Text);
                pending.Limit = static_cast<int32>(source->Text.size());
            }
            if (files.size() == CODE_MAX_WORKSPACE_EDIT_FILES)
            {
                outMessage = QStringLiteral("The change spans more than %1 files, more than the editor applies at "
                                            "once. Make it in smaller steps; nothing was changed.")
                                 .arg(CODE_MAX_WORKSPACE_EDIT_FILES);
                return false;
            }
            files.insert(key, pending);
        }
        PendingFile& pending = files[key];
        const QString& text = isSource == true ? context.Unit : pending.Change.Text;
        for (const QJsonValue value : document.second)
        {
            if (++editCount > CODE_MAX_WORKSPACE_EDITS)
            {
                outMessage = QStringLiteral("The change has more than %1 edits, more than the editor applies at "
                                            "once. Make it in smaller steps; nothing was changed.")
                                 .arg(CODE_MAX_WORKSPACE_EDITS);
                return false;
            }
            const QJsonObject textEdit = value.toObject();
            const QJsonObject range = textEdit.value(QStringLiteral("range")).toObject();
            int32 start = 0;
            int32 end = 0;
            if (textEdit.value(QStringLiteral("newText")).isString() == false ||
                GetLanguageOffset(text,
                                  pending.LineStarts,
                                  range.value(QStringLiteral("start")).toObject(),
                                  context.Encoding,
                                  start) == false ||
                GetLanguageOffset(text,
                                  pending.LineStarts,
                                  range.value(QStringLiteral("end")).toObject(),
                                  context.Encoding,
                                  end) == false ||
                end < start)
            {
                outMessage = QStringLiteral("The server sent an invalid edit range. Try again or restart assistance; "
                                            "nothing was changed.");
                return false;
            }
            if (start < pending.Offset || end > pending.Limit)
            {
                outMessage = QStringLiteral("The server would change code generated around this shader. Change the "
                                            "symbol where it is authored; nothing was changed.");
                return false;
            }
            QString newText = textEdit.value(QStringLiteral("newText")).toString();
            newText.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            pending.Change.Edits.append(
                { .Start = start - pending.Offset, .End = end - pending.Offset, .NewText = newText });
        }
    }
    QList<CodeWorkspaceFileChange> changes;
    for (PendingFile& pending : files)
    {
        QList<CodeWorkspaceTextEdit>& edits = pending.Change.Edits;
        QList<CodeWorkspaceTextEdit> scratch = edits;
        [[maybe_unused]] const bool isSorted =
            StableSort(edits.data(),
                       scratch.data(),
                       static_cast<usize>(edits.size()),
                       [](const CodeWorkspaceTextEdit& left, const CodeWorkspaceTextEdit& right) noexcept {
                           return left.Start < right.Start;
                       });
        LUDUS_ASSERT(isSorted == true || edits.isEmpty() == true);
        for (qsizetype index = 1; index < edits.size(); ++index)
        {
            if (edits.at(index).Start < edits.at(index - 1).End)
            {
                outMessage = QStringLiteral("The server sent overlapping edits. Try again or restart assistance; "
                                            "nothing was changed.");
                return false;
            }
        }
        if (edits.isEmpty() == false)
        {
            changes.append(Move(pending.Change));
        }
    }
    outChanges = Move(changes);
    return true;
}

QString ApplyCodeWorkspaceTextEdits(const QString& text, const QList<CodeWorkspaceTextEdit>& edits)
{
    QString result;
    int32 copied = 0;
    for (const CodeWorkspaceTextEdit& edit : edits)
    {
        result += QStringView(text).sliced(copied, edit.Start - copied);
        result += edit.NewText;
        copied = edit.End;
    }
    result += QStringView(text).sliced(copied);
    return result;
}

bool ApplyCodeWorkspaceEditBytes(const QByteArray& draft,
                                 const QList<CodeWorkspaceTextEdit>& edits,
                                 QByteArray& outResult)
{
    const bool hasCrlf = draft.contains("\r\n");
    QByteArray result;
    CodePositionOffset from;
    usize copied = 0;
    for (const CodeWorkspaceTextEdit& edit : edits)
    {
        usize start = 0;
        usize end = 0;
        if (AdvanceCodeByteOffset(draft, edit.Start, from, start) == false ||
            AdvanceCodeByteOffset(draft, edit.End, from, end) == false)
        {
            return false;
        }
        QString inserted = edit.NewText;
        if (hasCrlf == true)
        {
            inserted.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
        }
        result += draft.mid(static_cast<isize>(copied), static_cast<isize>(start - copied));
        result += inserted.toUtf8();
        copied = end;
    }
    result += draft.mid(static_cast<isize>(copied));
    outResult = Move(result);
    return true;
}

int32 MapCodeWorkspacePosition(const QList<CodeWorkspaceTextEdit>& edits, int32 position)
{
    int32 delta = 0;
    for (const CodeWorkspaceTextEdit& edit : edits)
    {
        if (edit.Start >= position && (edit.Start > position || edit.End > position))
        {
            break;
        }
        if (edit.End > position)
        {
            return edit.Start + delta;
        }
        delta += static_cast<int32>(edit.NewText.size()) - (edit.End - edit.Start);
    }
    return position + delta;
}
} // namespace ludus::editor
