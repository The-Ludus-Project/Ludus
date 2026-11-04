#pragma once

#include <QFrame>
#include <QString>

class QLabel;
class QPushButton;

namespace ludus::editor
{
/// A dismissible bar shown above a code editor when an open document's file
/// changed on disk while the buffer is dirty (CE03). It never overwrites the
/// draft: it offers Compare, Keep Mine and Reload, and leaves editing usable.
/// It also presents a read-only/degraded-mode explanation (invalid UTF-8, large
/// file, long lines) with no actions when the host opened the file that way.
class CodeConflictBanner final : public QFrame
{
    Q_OBJECT
public:
    explicit CodeConflictBanner(QWidget* parentOrNull = nullptr);

    /// Show an external-change conflict with the three resolution actions.
    void ShowConflict(const QString& fileName);
    /// Show a read-only or degraded-mode explanation with no resolution actions.
    void ShowExplanation(const QString& explanation);
    /// Hide the bar; called after a resolution or when the document is clean.
    void Dismiss();

Q_SIGNALS:
    /// The user asked to see the difference between the draft and the disk file.
    void CompareRequested();
    /// The user chose to keep the draft and overwrite the external change on save.
    void KeepMineRequested();
    /// The user chose to discard the draft and reload the external change.
    void ReloadRequested();

private:
    void setActionsVisible(bool isVisible);

    // Assigned in the constructor; nonnull for the widget's lifetime.
    QLabel* mMessage;
    QPushButton* mCompare;
    QPushButton* mKeepMine;
    QPushButton* mReload;
};
} // namespace ludus::editor
