#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTextCursor>

QT_BEGIN_NAMESPACE
class QAction;
class QFrame;
class QLineEdit;
class QListWidget;
class QLabel;
QT_END_NAMESPACE

namespace ludus::editor
{
class CodeEditor;

/// Semantic navigation intents; interpreted by the current language provider.
enum class CodeNavigationKind : foundation::uint8
{
    Symbols,
    Definition,
    References,
    Declaration,
    TypeDefinition,
    Implementation,
    /// Symbols across the server's workspace, queried with the word at the caret.
    WorkspaceSymbols,
};

/// The number of CodeNavigationKind values, for per-kind tables.
inline constexpr foundation::usize CODE_NAVIGATION_KIND_COUNT = 7;

/// True for the kinds that list symbols rather than locations of one name.
[[nodiscard]] constexpr bool IsCodeSymbolNavigation(CodeNavigationKind kind) noexcept
{
    return kind == CodeNavigationKind::Symbols || kind == CodeNavigationKind::WorkspaceSymbols;
}

/// True for the Go to kinds that jump straight to a single result.
[[nodiscard]] constexpr bool IsCodeGoToNavigation(CodeNavigationKind kind) noexcept
{
    return kind == CodeNavigationKind::Definition || kind == CodeNavigationKind::Declaration ||
           kind == CodeNavigationKind::TypeDefinition || kind == CodeNavigationKind::Implementation;
}

/// Copied semantic destination. Offsets are validated UTF-16 boundaries in Text.
/// An empty File means the current buffer. Read-only targets never rebind authored text.
struct CodeNavigationTarget final
{
    QString Label;
    QString File;
    QString Text;
    foundation::int32 Start = 0;
    foundation::int32 End = 0;
    bool IsReadOnly = false;
};

/// A Find References result for a references tool window (code editor M4,
/// CE10): the symbol at the request caret and the copied, validated locations.
struct CodeReferenceResults final
{
    QString Symbol;
    QList<CodeNavigationTarget> Targets;
    /// The provider's summary, such as how many locations were rejected.
    QString Message;
};

class CodeNavigation;

/// Presents package sources in the document area and restores live views for history.
class CodeNavigationViewHost
{
public:
    virtual ~CodeNavigationViewHost() = default;
    /// Present `results` in a references tool window and return true, or return
    /// false (the default) for the chooser beside the editor. `inoutOrigin` found them.
    [[nodiscard]] virtual bool ShowReferences(CodeNavigation& inoutOrigin, const CodeReferenceResults& results)
    {
        static_cast<void>(inoutOrigin);
        static_cast<void>(results);
        return false;
    }
    /// Open a validated location, returning null if its source is unavailable.
    [[nodiscard]] virtual CodeNavigation* OpenNavigationTarget(const CodeNavigationTarget& target) = 0;
    /// Restore an existing view or reopen its authored file, returning null on failure.
    [[nodiscard]] virtual CodeNavigation* ActivateNavigationView(CodeNavigation* viewOrNull, const QString& file) = 0;
};

/// Source navigation presentation and bounded view history, independent of authored Undo.
/// Borrows the editor, which owns this object. GUI thread only; no disk or process access.
class CodeNavigation final : public QObject
{
    Q_OBJECT
public:
    explicit CodeNavigation(CodeEditor& inoutEditor);
    /// Use the package's existing history across the Source pane and its code tabs.
    void SetViewHost(CodeNavigationViewHost* hostOrNull, CodeNavigation* historyOwnerOrNull = nullptr);
    /// Pair around the host's buffer rebinding. Scope changes clear history/results;
    /// file changes freeze anchored positions before Qt replaces the text.
    void BeginSource(const QString& scope, const QString& file, const QStringList& files);
    void EndSource();
    /// Drop all locations on document replacement, even when its package path is unchanged.
    void Clear();
    /// Remember a successful nonlocal jump's origin; duplicate origins are coalesced.
    void Remember(const QTextCursor& cursor);
    void RememberCurrent();
    void Back();
    void Forward();
    /// Show copied result labels without previewing/mutating source. The provider validates
    /// the captured identity again on ResultChosen before calling Navigate.
    void ShowResults(const QList<CodeNavigationTarget>& targets, const QString& message);
    /// Present Find References results in the view host's references tool window,
    /// or in the chooser when the host has none.
    void ShowReferences(const CodeReferenceResults& results);
    /// Request Find References at the caret, as the command does. False, without
    /// emitting Requested, when the command is disabled.
    bool RequestReferences();
    /// Bring this view forward through its host, as Back and Forward do. False when
    /// the host cannot show it; true without a host.
    bool RevealView();
    /// The editor this navigation presents; it owns this object.
    [[nodiscard]] LUDUS_INLINE CodeEditor& GetEditor() const noexcept
    {
        return mEditor;
    }
    /// Why Find References is disabled, or empty when it is available.
    [[nodiscard]] QString GetReferencesUnavailableReason() const;
    void Dismiss();
    /// Reveal a validated destination, or inspect generated text read-only. Missing files
    /// preserve the current selection/history and report failure in the chooser.
    [[nodiscard]] bool Navigate(const CodeNavigationTarget& target);
    void ReportFailure(const QString& message);
    void RefreshActions();
    /// Provider capability, independent of language selection. Unknown providers start enabled.
    /// Disable the command for `kind` with `unavailableReason` in its tooltip, or
    /// enable it when the reason is empty (capability dispatch, CE09).
    void SetSemanticAvailability(CodeNavigationKind kind, const QString& unavailableReason);
    /// Enable every semantic command again, before a server reports its capabilities.
    void ResetSemanticAvailability();
    /// Request the definition at the caret, as Go to Definition does. Returns false, without
    /// emitting Requested, when the current language provides no semantic navigation.
    bool RequestDefinition();
    [[nodiscard]] bool IsChoosing() const;
    [[nodiscard]] const QString& GetSource() const noexcept;
    [[nodiscard]] QList<QAction*> GetActions() const;

Q_SIGNALS:
    /// Synchronous host request; the host validates its inventory and completes Begin/EndSource.
    void FileRequested(const QString& file);
    void Requested(CodeNavigationKind kind);
    void ResultChosen(foundation::int32 row);
    void Dismissed();
    void HistoryChanged();

private:
    struct Location final
    {
        QString File;
        QTextCursor Anchor;
        QPointer<CodeNavigation> View;
        foundation::int32 Scroll = 0;
        foundation::int32 Block = 0;
        foundation::int32 Column = 0;
    };
    bool eventFilter(QObject* watched, QEvent* event) override;
    void filterResults();
    void acceptResult();
    void showPicker();
    void openFile();
    void traverse(bool shouldGoForward);
    void traverseViews(bool shouldGoForward);
    void notifyHistoryChanged();
    [[nodiscard]] CodeNavigation& historyOwner() noexcept;
    void appendLocation(QList<Location>& inoutLocations, const Location& location);
    [[nodiscard]] Location capture(const QTextCursor& cursor);
    [[nodiscard]] QTextCursor resolve(const Location& location) const;
    void freezeLocations(QList<Location>& inoutLocations);
    void anchorLocations(QList<Location>& inoutLocations);
    CodeEditor& mEditor;
    CodeNavigationViewHost* mViewHostOrNull = nullptr;
    QPointer<CodeNavigation> mHistoryOwner;
    QFrame* mPopup;
    QLineEdit* mFilter;
    QListWidget* mResults;
    QLabel* mMessage;
    QList<QAction*> mActions;
    QAction* mBack;
    QAction* mForward;
    QAction* mFiles;
    QAction* mSymbols;
    QAction* mDefinition;
    QAction* mReferences;
    QAction* mDeclaration;
    QAction* mTypeDefinition;
    QAction* mImplementation;
    QAction* mWorkspaceSymbols;
    foundation::StaticArray<QString, CODE_NAVIGATION_KIND_COUNT> mUnavailableReasons{};
    QString mScope;
    QString mFile;
    QStringList mFilesAvailable;
    QList<Location> mPrevious;
    QList<Location> mNext;
    QList<CodeNavigationTarget> mTargets;
    bool mIsBinding = false;
    bool mIsTraversing = false;
    bool mIsFilePicker = false;
};
} // namespace ludus::editor
