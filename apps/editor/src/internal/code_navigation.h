#pragma once

#include <ludus/foundation/base/types.h>

#include <QList>
#include <QObject>
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
};

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

/// Source navigation presentation and bounded view history, independent of authored Undo.
/// Borrows the editor, which owns this object. GUI thread only; no disk or process access.
class CodeNavigation final : public QObject
{
    Q_OBJECT
public:
    explicit CodeNavigation(CodeEditor& inoutEditor);
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
    void Dismiss();
    /// Reveal a validated destination, or inspect generated text read-only. Missing files
    /// preserve the current selection/history and report failure in the chooser.
    [[nodiscard]] bool Navigate(const CodeNavigationTarget& target);
    void ReportFailure(const QString& message);
    void RefreshActions();
    /// Provider capability, independent of language selection. Unknown providers start enabled.
    void SetReferencesAvailable(bool canFindReferences);
    [[nodiscard]] bool IsChoosing() const;
    [[nodiscard]] const QString& GetSource() const noexcept;
    [[nodiscard]] QList<QAction*> GetActions() const;

Q_SIGNALS:
    /// Synchronous host request; the host validates its inventory and completes Begin/EndSource.
    void FileRequested(const QString& file);
    void Requested(CodeNavigationKind kind);
    void ResultChosen(foundation::int32 row);
    void Dismissed();

private:
    struct Location final
    {
        QString File;
        QTextCursor Anchor;
        foundation::int32 Block = 0;
        foundation::int32 Column = 0;
    };
    bool eventFilter(QObject* watched, QEvent* event) override;
    void filterResults();
    void acceptResult();
    void showPicker();
    void openFile();
    void traverse(bool shouldGoForward);
    void appendLocation(QList<Location>& inoutLocations, const Location& location);
    [[nodiscard]] Location capture(const QTextCursor& cursor) const;
    [[nodiscard]] QTextCursor resolve(const Location& location) const;
    void freezeLocations(QList<Location>& inoutLocations);
    void anchorLocations(QList<Location>& inoutLocations);
    CodeEditor& mEditor;
    QFrame* mPopup;
    QLineEdit* mFilter;
    QListWidget* mResults;
    QLabel* mMessage;
    QList<QAction*> mActions;
    QAction* mBack;
    QAction* mForward;
    QAction* mFiles;
    QAction* mReferences;
    QString mScope;
    QString mFile;
    QStringList mFilesAvailable;
    QList<Location> mPrevious;
    QList<Location> mNext;
    QList<CodeNavigationTarget> mTargets;
    bool mIsBinding = false;
    bool mIsTraversing = false;
    bool mIsFilePicker = false;
    bool mCanFindReferences = true;
};
} // namespace ludus::editor
