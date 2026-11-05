#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <QList>
#include <QString>

QT_BEGIN_NAMESPACE
class QKeyEvent;
class QPainter;
class QPoint;
QT_END_NAMESPACE

namespace ludus::editor
{
class CodeEditor;
class CodeDocumentController;

/// One secondary cursor in the view's own UTF-16 coordinates, carrying its
/// selection direction through its anchor and caret exactly like Qt's cursor.
/// The primary cursor is always Qt's own `QTextCursor`, so Qt's accessibility,
/// IME and clipboard see one cursor; the secondary cursors are this layer's
/// overlay (CE06, design section 5).
struct CodeSecondaryCursor final
{
    foundation::int32 Anchor = 0;
    foundation::int32 Caret = 0;
    [[nodiscard]] LUDUS_INLINE foundation::int32 Start() const noexcept
    {
        return Anchor < Caret ? Anchor : Caret;
    }
    [[nodiscard]] LUDUS_INLINE foundation::int32 End() const noexcept
    {
        return Anchor < Caret ? Caret : Anchor;
    }
};

/// One region's text operation when the cursor set types, deletes or pastes, in
/// the view's UTF-16 coordinates (design section 5). The controller maps every
/// region's positions to UTF-8 byte offsets in one ascending pass and publishes
/// the whole set as one CodeEditSet, so the per-region cost stays amortized over
/// a single walk of the text, never one walk per region.
struct CodeRegionEdit final
{
    foundation::int32 Start = 0; ///< Removed-range start (UTF-16), ascending across the set.
    foundation::int32 End = 0;   ///< Removed-range end (UTF-16); End >= Start.
    QString Inserted;            ///< Replacement text with LF newlines; the host owns CRLF re-insertion.
    bool IsPrimary = false;      ///< The one region whose caret the single Qt cursor follows afterwards.
};

/// Secondary-cursor state and input distribution for one CodeEditor (CE06).
/// Owned by the editor and used only on the GUI thread. The secondary cursors
/// are kept sorted by Start in document order and never overlap the primary or
/// each other; merging is applied after every edit or movement. Every text
/// operation is turned into a single CodeEditSet through the editor's bound
/// controller, so a hundred cursors cost one pass over the text, not a hundred.
class CodeMultiCursor final
{
public:
    explicit CodeMultiCursor(CodeEditor& inoutEditor) noexcept;

    /// Bind the controller that publishes this view's edits, or clear it with
    /// null. The controller sets itself on construction and clears itself on
    /// destruction, so an unbound view (no controller) keeps the overlay inert
    /// and every multi-cursor text operation is a no-op.
    LUDUS_INLINE void SetController(CodeDocumentController* controllerOrNull) noexcept
    {
        mControllerOrNull = controllerOrNull;
    }

    /// The secondary cursors in document order, sorted by Start. The primary
    /// cursor is the editor's own QTextCursor and is not included here.
    [[nodiscard]] LUDUS_INLINE const QList<CodeSecondaryCursor>& GetSecondaryCursors() const noexcept
    {
        return mSecondary;
    }
    /// 1 (the primary) plus every secondary cursor: the number the status bar shows.
    [[nodiscard]] foundation::int32 GetCursorCount() const noexcept;
    [[nodiscard]] LUDUS_INLINE bool HasSecondaryCursors() const noexcept
    {
        return mSecondary.isEmpty() == false;
    }

    /// Drop every secondary cursor, leaving only the primary. Escape collapses to
    /// the primary this way. Returns true when a secondary cursor was removed, so
    /// the caller can accept the key only when it changed the set.
    bool Collapse();

    /// Add a collapsed cursor one visual line above or below the topmost or
    /// bottommost cursor, at the same column (design section 5). A no-op at the
    /// document edge. The added cursor becomes a secondary; the primary is kept.
    void AddCursorInDirection(bool shouldGoUp);

    /// Select the word under the primary caret if nothing is selected; otherwise
    /// add the next occurrence of the primary selection as a new secondary cursor
    /// and make it the primary, wrapping at the end (Ctrl+D). The search runs once
    /// over the document text with QString::indexOf, never QTextDocument::find.
    void AddNextOccurrence();

    /// Select the primary's word or selection at every occurrence in the document
    /// in one pass (Ctrl+Shift+L). The first match stays primary.
    void AddAllOccurrences();

    /// Column (box) selection from the primary's anchor to a viewport point: one
    /// cursor per spanned line, each from the anchor column to the point column
    /// (design section 5). Replaces the current secondary set.
    void ColumnSelectTo(const QPoint& viewportPoint);

    /// Distribute a printable key, Backspace, Delete, Return/Enter or Tab across
    /// every cursor as one CodeEditSet, when there is at least one secondary
    /// cursor. Returns true when it handled the key (the caller then accepts it);
    /// false leaves the key to the single-cursor path.
    [[nodiscard]] bool HandleKeyPress(QKeyEvent* event);

    /// Paste `text` across the cursor set: one line per cursor when the line
    /// count matches the cursor count, otherwise the whole text at each cursor
    /// (design section 5). Returns true when it handled the paste.
    [[nodiscard]] bool HandlePaste(const QString& text);

    /// Paint the secondary carets over the viewport, after the base
    /// QPlainTextEdit paint and before the latency probe marks the frame. The
    /// secondary selections are drawn by the view as Qt extra selections, since
    /// extra selections paint no caret of their own.
    void Paint(QPainter& inoutPainter) const;

    /// Replace the secondary cursors with `secondaryCursors` (document order, in
    /// UTF-16 positions) and repaint. The controller calls this when it restores
    /// a multi-cursor selection set from an undo step or a multi-cursor edit, so
    /// the overlay mirrors the recorded set. Cursors equal to the primary and
    /// touching duplicates are merged.
    void AdoptSecondaryCursors(const QList<CodeSecondaryCursor>& secondaryCursors);

private:
    /// Build the ordered region set (primary first in list order is not required;
    /// regions are sorted by Start), apply it through the controller, and adopt
    /// the returned after-selection set as the new primary and secondaries.
    [[nodiscard]] bool applyRegionEdits(const QList<CodeRegionEdit>& regionsInDocumentOrder);
    /// Move every cursor (primary and secondaries) with Qt's own movement on a
    /// temporary QTextCursor, so platform word/line behavior is preserved, then
    /// normalize. The operation is a QTextCursor::MoveOperation (kept as an int so
    /// the header needs no Qt text includes). Returns true to consume the key.
    [[nodiscard]] bool moveEveryCursor(foundation::int32 moveOperation, bool shouldKeepAnchor);
    /// Every cursor as a region in document order (primary included), sorted by
    /// Start, with the primary flagged. Overlaps are already prevented by the set
    /// invariant, so adjacent regions never share an offset.
    [[nodiscard]] QList<CodeRegionEdit> collectRegions() const;
    void sortAndMerge();
    CodeEditor& mEditor;
    CodeDocumentController* mControllerOrNull = nullptr;
    QList<CodeSecondaryCursor> mSecondary;
    /// The anchor of a column selection, kept so dragging extends the same box.
    foundation::int32 mColumnAnchorPosition = -1;
};
} // namespace ludus::editor
