#pragma once

// Undo and redo feedback (docs/development/editor-workspace.md#undo-history).
//
// Every document history entry carries a readable command name (DocumentHistory). This header turns a history
// into what the editor shows: the Undo and Redo labels, the brief "Undid: Rename Entity" notice and the History
// panel, where clicking an entry returns the document to that state. Everything here runs only when a person
// asks for Undo, Redo or History, or when the History panel is visible; committing an edit only stores a name.

#include <ludus/foundation/base/types.h>

#include "internal/document_history.h"

#include <QAbstractListModel>
#include <QString>
#include <QStringList>
#include <QWidget>

class QLabel;
class QListView;
class QPropertyAnimation;
class QTimer;

namespace ludus::editor
{
/// One document's undo history as the editor presents it. A copy: it never aliases the history it describes.
struct EditHistoryState final
{
    /// Document the history belongs to, such as "World"; empty when the active document keeps no history.
    QString DocumentName;
    /// Retained entry names, oldest first.
    QStringList Names;
    /// Entries currently applied: 0 when every retained entry is undone, Names.size() when none is.
    foundation::int32 Cursor = 0;
    /// True when entries can be chosen to jump to them; text fields keep their own history and cannot.
    bool CanJump = false;

    [[nodiscard]] bool operator==(const EditHistoryState& other) const = default;
};

/// Snapshot of a document history for presentation, in O(entries).
template <typename T, foundation::usize CAPACITY>
[[nodiscard]] EditHistoryState MakeEditHistoryState(const QString& documentName,
                                                    const DocumentHistory<T, CAPACITY>& history)
{
    EditHistoryState state;
    state.DocumentName = documentName;
    state.Names.reserve(static_cast<qsizetype>(history.GetCount()));
    for (foundation::usize index = 0; index < history.GetCount(); ++index)
    {
        state.Names.append(QString::fromUtf8(history.GetName(index)));
    }
    state.Cursor = static_cast<foundation::int32>(history.GetCursor());
    state.CanJump = true;
    return state;
}

/// Name shown for an edit typed into a text field or code editor, whose own history has no command names.
inline constexpr const char* TYPING_EDIT_NAME = "Typing";

/// Edit menu text for Undo: "&Undo Rename Entity", or "&Undo" when name is empty (nothing to undo).
[[nodiscard]] QString MakeUndoMenuText(const QString& name);
/// Edit menu text for Redo: "&Redo Rename Entity", or "&Redo" when name is empty (nothing to redo).
[[nodiscard]] QString MakeRedoMenuText(const QString& name);
/// Notice after Undo: "Undid: Rename Entity".
[[nodiscard]] QString MakeUndoFeedbackText(const QString& name);
/// Notice after Redo: "Redid: Rename Entity".
[[nodiscard]] QString MakeRedoFeedbackText(const QString& name);
/// Notice after jumping from before.Cursor to cursor in the History panel. One step reads like Undo or Redo;
/// several steps name how many and the state reached ("Undid 3 edits, back to: Move Entity"). Empty when
/// cursor equals before.Cursor or lies outside [0, before.Names.size()].
[[nodiscard]] QString MakeHistoryJumpFeedbackText(const EditHistoryState& before, foundation::int32 cursor);

/// List model of one EditHistoryState: row 0 is the state before the oldest retained edit and row r the state
/// after edit r, so a row number is the cursor a jump to it reaches. GUI thread only.
class EditHistoryModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    /// Item data roles beyond Qt's own.
    enum Role : foundation::uint16
    {
        /// bool: this row is the document's current state.
        IsCurrentRole = Qt::UserRole + 1,
        /// bool: this row lies after the current state, so reaching it redoes edits.
        IsUndoneRole,
    };
    /// Label of row 0, the state before the oldest retained edit.
    static constexpr const char* INITIAL_STATE_NAME = "Initial state";

    explicit EditHistoryModel(QObject* parentOrNull = nullptr);
    /// Replace the presented history; resets the model only when the state differs.
    void SetState(const EditHistoryState& state);
    /// The presented history.
    [[nodiscard]] const EditHistoryState& GetState() const noexcept
    {
        return mState;
    }
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

private:
    EditHistoryState mState;
};

/// History tool window: the active document's edits, oldest first, with the current state selected and undone
/// edits dimmed. Choosing a row (click, Enter or Space) asks to jump there. GUI thread only.
class EditHistoryPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit EditHistoryPanel(QWidget* parentOrNull = nullptr);
    /// Present state, keeping the current row in view. Cheap when the state is unchanged.
    void SetState(const EditHistoryState& state);
    /// The model behind the list, for tests and accessibility checks.
    [[nodiscard]] const EditHistoryModel& GetModel() const noexcept
    {
        return *mModel;
    }
    /// The list itself, for focus and tests.
    [[nodiscard]] QListView& GetList() const noexcept
    {
        return *mList;
    }

Q_SIGNALS:
    /// The person chose the state after `cursor` entries; the owner validates and performs the jump.
    void JumpRequested(foundation::int32 cursor);

private:
    void requestJump(const QModelIndex& index);
    void renderCaption();
    EditHistoryModel* mModel;
    QListView* mList;
    QLabel* mCaption;
    QLabel* mEmpty;
};

/// Brief notice such as "Undid: Rename Entity", shown near the bottom of an anchor widget and dismissed on its
/// own. A frameless tool window, so it stays visible above native viewport surfaces; it never takes focus or
/// mouse input. Fades in and out unless reduced motion is requested, in which case it appears and disappears at
/// once. Colours come from the editor palette (surface.raised, border.control, text tokens).
class UndoFeedbackToast final : public QWidget
{
    Q_OBJECT
public:
    /// How long a notice stays fully visible.
    static constexpr foundation::int32 VISIBLE_MILLISECONDS = 1600;
    /// Fade duration when motion is allowed.
    static constexpr foundation::int32 FADE_MILLISECONDS = 150;

    explicit UndoFeedbackToast(QWidget* parentOrNull = nullptr);
    /// Show text over the bottom centre of anchor, replacing any notice already shown and restarting its timer.
    /// The text before the first ": " is drawn as a quieter verb.
    void ShowNotice(const QString& text, const QWidget& anchor);
    /// The text of the notice being shown, or of the last one.
    [[nodiscard]] const QString& GetText() const noexcept
    {
        return mText;
    }
    /// Hide at once, without a fade.
    void Dismiss();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void beginFadeOut();
    QString mText;
    QTimer* mTimer;
    QPropertyAnimation* mFadeOrNull = nullptr;
};
} // namespace ludus::editor
