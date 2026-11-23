#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/code_document.h"
#include "internal/code_workspace_edit.h"
#include "internal/language_client.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

namespace ludus::editor
{
class CodeDocumentController;
class CodeEditor;

/// What a code actions request is asked against: the draft identity, the
/// server's virtual unit and the authored source span within it.
struct CodeActionScope final
{
    CodeDocumentIdentity Identity;
    CodeWorkspaceEditContext Edit;
    /// The selection or caret, as UTF-16 offsets in the authored source.
    foundation::int32 Start = 0;
    foundation::int32 End = 0;
    /// The analysis diagnostics there, as the server published them.
    QJsonArray Diagnostics;
};

/// Code actions for one editor (code editor M4, CE10): `textDocument/codeAction`
/// at the caret or selection, the light bulb when any exist, and applying the
/// chosen action's edit through the same validated workspace edits as rename,
/// as one undo step. An action without an edit is resolved first when the server
/// supports `codeAction/resolve`. Actions that only run a server command are
/// listed disabled with the reason: the client does not run commands. GUI thread
/// only; the editor, document and client outlive this object.
class CodeActions final : public QObject
{
    Q_OBJECT
public:
    /// Actions kept per request; a longer reply keeps its first actions.
    static constexpr foundation::int32 MAX_CODE_ACTIONS = 64;
    CodeActions(CodeEditor& inoutEditor, CodeDocumentController& inoutDocument, LanguageClient& inoutClient);
    /// Apply multi-file actions through `hostOrNull`; see CodeAssistance::SetWorkspaceEditHost.
    void SetWorkspaceEditHost(CodeWorkspaceEditHost* hostOrNull) noexcept;
    /// Ask for the actions in `scope`, whose unit the caller has just passed to
    /// LanguageClient::SetDocument. An explicit request (Ctrl+.) opens the list
    /// with the reply, or says there are none.
    void Request(const CodeActionScope& scope, bool isExplicit);
    /// Apply a reply. False when `kind` is not a code action kind.
    bool Receive(foundation::int32 request, LanguageRequestKind kind, const QJsonValue& value);
    /// Cancel pending requests and remove the light bulb.
    void Clear();

Q_SIGNALS:
    void StatusChanged(const QString& message);

private:
    void choose(foundation::int32 index);
    void apply(const QJsonObject& action);
    [[nodiscard]] bool isCurrent() const;
    CodeEditor& mEditor;
    CodeDocumentController& mDocument;
    LanguageClient& mClient;
    CodeWorkspaceEditHost* mWorkspaceEditHostOrNull = nullptr;
    CodeActionScope mScope;
    /// The actions shown, in list order, as the server sent them.
    QList<QJsonObject> mActions;
    foundation::int32 mRequest = 0;
    foundation::int32 mResolveRequest = 0;
    bool mIsExplicit = false;
};
} // namespace ludus::editor
