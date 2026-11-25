#include "internal/code_actions.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_document_controller.h"
#include "internal/code_editor.h"
#include "internal/language_positions.h"

// Thanks to Microsoft, Language Server Protocol 3.17, "Code Action Request" and
// "Code Action Resolve Request": a reply mixes Command and CodeAction literals,
// which may be preferred, disabled with a reason, or resolved for their edit.
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#textDocument_codeAction
namespace ludus::editor
{
namespace
{
using namespace foundation;
// LSP 3.17 CodeActionTriggerKind.
constexpr int32 TRIGGER_INVOKED = 1;
constexpr int32 TRIGGER_AUTOMATIC = 2;
constexpr qsizetype MAX_TITLE_UNITS = 200;

/// The reason `action` cannot be applied here, or empty when it can be.
QString GetDisabledReason(const QJsonObject& action, bool canResolve)
{
    const QString disabled =
        action.value(QStringLiteral("disabled")).toObject().value(QStringLiteral("reason")).toString();
    if (action.contains(QStringLiteral("disabled")) == true)
    {
        return disabled.isEmpty() == true ? QStringLiteral("The server says this action does not apply here.")
                                          : disabled;
    }
    if (action.contains(QStringLiteral("edit")) == true ||
        (canResolve == true && action.contains(QStringLiteral("data")) == true))
    {
        return {};
    }
    return QStringLiteral("This action runs a server command, which the editor does not run. Make the change by "
                          "hand.");
}
} // namespace

CodeActions::CodeActions(CodeEditor& inoutEditor, CodeDocumentController& inoutDocument, LanguageClient& inoutClient)
    : mEditor(inoutEditor), mDocument(inoutDocument), mClient(inoutClient)
{
    connect(&mEditor, &CodeEditor::CodeActionChosen, this, &CodeActions::choose);
}

void CodeActions::SetWorkspaceEditHost(CodeWorkspaceEditHost* hostOrNull) noexcept
{
    mWorkspaceEditHostOrNull = hostOrNull;
}

void CodeActions::Request(const CodeActionScope& scope, bool isExplicit)
{
    if (mClient.CanRequest(LanguageRequestKind::CodeActions) == false)
    {
        if (isExplicit == true)
        {
            Q_EMIT StatusChanged(mClient.GetUnavailableReason(LanguageRequestKind::CodeActions));
        }
        return;
    }
    // An automatic request never replaces the author's own pending Ctrl+.
    if (isExplicit == false && mRequest != 0 && mIsExplicit == true)
    {
        return;
    }
    const LanguagePositionEncoding encoding = mClient.GetPositionEncoding();
    const QString& unit = scope.Edit.Unit;
    mScope = scope;
    mIsExplicit = isExplicit;
    mRequest = mClient.Request(
        LanguageRequestKind::CodeActions,
        {{QStringLiteral("range"),
          QJsonObject{
              {QStringLiteral("start"), MakeLanguagePosition(unit, scope.Edit.SourceStart + scope.Start, encoding)},
              {QStringLiteral("end"), MakeLanguagePosition(unit, scope.Edit.SourceStart + scope.End, encoding)}}},
         {QStringLiteral("context"),
          QJsonObject{{QStringLiteral("diagnostics"), scope.Diagnostics},
                      {QStringLiteral("triggerKind"), isExplicit == true ? TRIGGER_INVOKED : TRIGGER_AUTOMATIC}}}});
}

bool CodeActions::isCurrent() const
{
    return mScope.Identity == mDocument.Capture().Identity;
}

bool CodeActions::Receive(int32 request, LanguageRequestKind kind, const QJsonValue& value)
{
    if (kind == LanguageRequestKind::CodeActionResolve)
    {
        if (request != 0 && request == mResolveRequest)
        {
            mResolveRequest = 0;
            apply(value.toObject());
        }
        return true;
    }
    if (kind != LanguageRequestKind::CodeActions)
    {
        return false;
    }
    if (request == 0 || request != mRequest)
    {
        return true;
    }
    mRequest = 0;
    if (isCurrent() == false)
    {
        return true;
    }
    const bool canResolve = mClient.CanRequest(LanguageRequestKind::CodeActionResolve);
    // Preferred fixes first, otherwise in the server's order.
    QList<QJsonObject> preferred;
    QList<QJsonObject> others;
    for (const QJsonValue entry : value.toArray())
    {
        const QJsonObject action = entry.toObject();
        if (action.value(QStringLiteral("title")).toString().isEmpty() == true)
        {
            continue;
        }
        if (preferred.size() + others.size() == MAX_CODE_ACTIONS)
        {
            break;
        }
        (action.value(QStringLiteral("isPreferred")).toBool(false) == true ? preferred : others).append(action);
    }
    mActions = preferred + others;
    QList<CodeActionEntry> entries;
    for (const QJsonObject& action : mActions)
    {
        entries.append(
        {
            .Title = action.value(QStringLiteral("title")).toString().left(MAX_TITLE_UNITS),
            .DisabledReason = GetDisabledReason(action, canResolve),
            .IsPreferred = action.value(QStringLiteral("isPreferred")).toBool(false),
        });
    }
    mEditor.SetCodeActions(mScope.Start, entries);
    if (mIsExplicit == true)
    {
        if (entries.isEmpty() == true)
        {
            Q_EMIT StatusChanged(QStringLiteral("No code actions at the caret."));
            return true;
        }
        mEditor.ShowCodeActionMenu();
    }
    return true;
}

void CodeActions::choose(int32 index)
{
    if (index < 0 || index >= mActions.size())
    {
        return;
    }
    if (isCurrent() == false)
    {
        Q_EMIT StatusChanged(QStringLiteral("The source changed after these actions were found. Press Ctrl+. again; "
                                            "nothing was changed."));
        return;
    }
    const QJsonObject action = mActions.at(index);
    const QString reason = GetDisabledReason(action, mClient.CanRequest(LanguageRequestKind::CodeActionResolve));
    if (reason.isEmpty() == false)
    {
        Q_EMIT StatusChanged(reason);
        return;
    }
    if (action.contains(QStringLiteral("edit")) == true)
    {
        apply(action);
        return;
    }
    mResolveRequest = mClient.Request(LanguageRequestKind::CodeActionResolve, action);
}

void CodeActions::apply(const QJsonObject& action)
{
    const QString title = action.value(QStringLiteral("title")).toString().left(MAX_TITLE_UNITS);
    if (isCurrent() == false)
    {
        Q_EMIT StatusChanged(QStringLiteral("The source changed before '%1' was applied. Press Ctrl+. again; "
                                            "nothing was changed.")
                                 .arg(title));
        return;
    }
    if (action.contains(QStringLiteral("edit")) == false)
    {
        Q_EMIT StatusChanged(QStringLiteral("The server sent no change for '%1'. Make the change by hand.").arg(title));
        return;
    }
    CodeWorkspaceEditContext context = mScope.Edit;
    context.CanEditOtherFiles = mWorkspaceEditHostOrNull != nullptr;
    QList<CodeWorkspaceFileChange> changes;
    QString message;
    if (ReadCodeWorkspaceEdit(action.value(QStringLiteral("edit")).toObject(), context, changes, message) == false)
    {
        Q_EMIT StatusChanged(message);
        return;
    }
    if (changes.isEmpty() == true)
    {
        Q_EMIT StatusChanged(QStringLiteral("'%1' changes nothing here.").arg(title));
        return;
    }
    const CodeEditStatus status = ApplyCodeWorkspaceChanges(mDocument,
                                                            mWorkspaceEditHostOrNull,
                                                            changes,
                                                            mEditor.textCursor().position(),
                                                            "Apply Code Action");
    mEditor.SetCodeActions(0, {});
    switch (status)
    {
        case CodeEditStatus::Ok:
            Q_EMIT StatusChanged(QStringLiteral("Applied '%1'. Undo reverts it in every file.").arg(title));
            break;
        case CodeEditStatus::Conflict:
            Q_EMIT StatusChanged(QStringLiteral("A file changed before '%1' was applied. Press Ctrl+. again; nothing "
                                                "was changed.")
                                     .arg(title));
            break;
        case CodeEditStatus::Capacity:
            Q_EMIT StatusChanged(QStringLiteral("'%1' would exceed a source or package size limit; nothing was "
                                                "changed.")
                                     .arg(title));
            break;
        case CodeEditStatus::InvalidText:
        case CodeEditStatus::Unavailable:
            Q_EMIT StatusChanged(
                QStringLiteral("'%1' could not be applied to these sources; nothing was changed.").arg(title));
            break;
    }
}

void CodeActions::Clear()
{
    if (mRequest != 0)
    {
        mClient.Cancel(LanguageRequestKind::CodeActions);
    }
    if (mResolveRequest != 0)
    {
        mClient.Cancel(LanguageRequestKind::CodeActionResolve);
    }
    mRequest = 0;
    mResolveRequest = 0;
    mActions.clear();
    mEditor.SetCodeActions(0, {});
}
} // namespace ludus::editor
