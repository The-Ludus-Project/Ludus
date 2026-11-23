#pragma once

#include "internal/code_actions.h"
#include "internal/code_document_controller.h"
#include "internal/code_navigation_results.h"
#include "internal/code_semantic_layers.h"
#include "internal/code_workspace_edit.h"
#include "internal/language_client.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>

namespace ludus::editor
{
/// Private editing controller for semantic assistance. All result publication is
/// guarded by the document identity, context generation, request and caret.
/// The editor/adapter outlive this object. Never saves authored files.
class CodeAssistance final : public QObject
{
    Q_OBJECT
public:
    CodeAssistance(CodeEditor& inoutEditor, CodeDocumentController& inoutDocument, QObject* parentOrNull = nullptr);
    /// Invalidate an incompatible context and all pending replies/popups.
    void ClearContext();
    /// Supply compiler-owned generated surroundings and the server's virtual URI.
    /// Source text is always taken from the current authoritative draft.
    void SetContext(const QString& server,
                    const QString& workspace,
                    const QString& uri,
                    const QString& prefix,
                    const QString& suffix,
                    const QStringList& serverArguments = {},
                    const CodeNavigationSources& navigationSources = {},
                    const LanguageServerProfile& profile = GetSlangServerProfile());
    /// The connected server's retained standard error, for Show Log.
    [[nodiscard]] const QString& GetServerLog() const noexcept;
    /// Apply renames that reach other files through `hostOrNull`, which must
    /// outlive this object (code editor M4, CE11). Without a host, a rename that
    /// would change another file is refused and only the edited source changes.
    void SetWorkspaceEditHost(CodeWorkspaceEditHost* hostOrNull) noexcept;
    /// Rename Symbol at the caret: prepare the context, ask the server where the
    /// symbol is, then show the editor's rename box.
    void RequestRename();
    /// Show Code Actions at the caret or selection: prepare the context, ask the
    /// server, then open the list (code editor M4, CE10).
    void RequestCodeActions();
    void Request(bool isSignature);
    /// Read-only semantic navigation through the same versioned compiler context.
    void RequestNavigation(CodeNavigationKind kind);
    void Cancel();

Q_SIGNALS:
    void ContextRequested();
    void ContextCancelled();
    void StatusChanged(const QString& message);

private:
    struct Completion final
    {
        QString Label;
        QString Detail;
        QJsonObject Item;
        /// The item as the server sent it, for completionItem/resolve.
        QJsonObject Original;
        bool IsResolved = false;
    };
    /// The completionItem/resolve in flight and the suggestion it describes.
    struct ResolveRequest final
    {
        foundation::int32 Request = 0;
        foundation::int32 Row = -1;
    };
    void cancelPending();
    void requestCurrent();
    void receive(foundation::int32 request, const QJsonValue& value);
    void applyAvailability();
    void requestHover(foundation::int32 position);
    void receiveHover(foundation::int32 request, const QJsonValue& value);
    /// The rename in progress: where the symbol is, its old and new name, and the
    /// draft and context it was found in.
    struct RenameRequest final
    {
        foundation::int32 Request = 0;
        CodeDocumentIdentity Identity;
        foundation::uint64 Context = 0;
        foundation::int32 Start = 0;
        foundation::int32 End = 0;
        QString OldName;
        QString NewName;
        bool ShouldPreview = false;
    };
    void receivePrepareRename(const QJsonValue& value);
    void showRenameBox(foundation::int32 start, foundation::int32 end, const QString& name);
    void acceptRename(const QString& name, bool shouldPreview);
    void receiveRename(foundation::int32 request, const QJsonValue& value);
    void applyRename(const RenameRequest& rename, const QList<CodeWorkspaceFileChange>& changes);
    [[nodiscard]] bool isRenameCurrent(const RenameRequest& rename) const;
    /// Ask for code actions at the caret now; the light bulb shows the result.
    void requestCodeActions(bool isExplicit);
    void scheduleCodeActions();
    /// The text of every open editor of this language, this one first, for word completion.
    [[nodiscard]] QList<QString> collectOpenBuffers() const;
    /// The context a workspace edit from the server is read against.
    [[nodiscard]] CodeWorkspaceEditContext makeWorkspaceEditContext(const QString& unit, const QString& source) const;
    void resolveCompletion(foundation::int32 row);
    void receiveResolve(foundation::int32 request, const QJsonValue& value);
    /// Send the current draft and refresh the background layers for it.
    void synchronizeAnalysis();
    void receiveDiagnostics(const QString& uri, foundation::int32 version, const QJsonArray& diagnostics);
    void clearAnalysis();
    /// The current draft as the server's virtual unit, or false when it is not valid text.
    [[nodiscard]] bool buildUnit(CodeTextCapture& outCapture, QString& outSource, QString& outUnit) const;
    void resetAvailability();
    void showSignatures();
    void acceptCompletion(foundation::int32 row);
    void acceptNavigation(foundation::int32 row);
    void beginRequest();
    [[nodiscard]] bool isCurrent() const;
    [[nodiscard]] bool mapPosition(const QJsonObject& position, foundation::usize& outBytes) const;
    CodeEditor& mEditor;
    CodeDocumentController& mDocument;
    LanguageClient mClient;
    CodeSemanticLayers mLayers;
    CodeActions mActions;
    /// A published analysis diagnostic as the server sent it, with its range in the
    /// authored source, for code action requests.
    struct AnalysisRecord final
    {
        QJsonObject Diagnostic;
        foundation::int32 Start = 0;
        foundation::int32 End = 0;
    };
    QList<AnalysisRecord> mAnalysisRecords;
    /// The draft the analysis records describe.
    CodeDocumentIdentity mAnalysisIdentity;
    QTimer mCodeActionDelay;
    const LanguageServerProfile* mProfile = &GetSlangServerProfile();
    LanguageRequestKind mRequestKind = LanguageRequestKind::Completion;
    /// The hover in flight: its request, position and the identities it was asked for.
    struct HoverRequest final
    {
        foundation::int32 Request = 0;
        foundation::int32 Position = 0;
        CodeDocumentIdentity Identity;
        foundation::uint64 Context = 0;
    };
    HoverRequest mHover;
    ResolveRequest mResolve;
    RenameRequest mRename;
    CodeWorkspaceEditHost* mWorkspaceEditHostOrNull = nullptr;
    QTimer mAnalysisSync;
    QTimer mRefresh;
    CodeTextCapture mCapture;
    QString mServer;
    QString mServerStamp;
    QStringList mServerArguments;
    QString mUri;
    QString mPrefix;
    QString mSuffix;
    QString mUnit;
    CodeNavigationSources mNavigationSources;
    QList<CodeNavigationTarget> mNavigationTargets;
    QList<Completion> mCompletions;
    QJsonArray mSignatures;
    foundation::uint64 mContext = 0;
    foundation::uint64 mRequestedContext = 0;
    foundation::int32 mCaret = 0;
    foundation::int32 mAnchor = 0;
    foundation::int32 mRequest = 0;
    foundation::int32 mWordStart = 0;
    foundation::int32 mWordEnd = 0;
    foundation::int32 mActiveSignature = 0;
    foundation::int32 mActiveParameter = 0;
    bool mIsSignature = false;
    bool mIsNavigation = false;
    bool mIsRename = false;
    bool mIsCodeActions = false;
    CodeNavigationKind mNavigationKind = CodeNavigationKind::Symbols;
    bool mHasIntent = false;
    bool mShouldTrackSignature = false;
    bool mShouldTrackCompletion = false;
};
} // namespace ludus::editor
