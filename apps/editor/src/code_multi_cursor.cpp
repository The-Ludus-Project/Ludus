#include "internal/code_multi_cursor.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/sort.hpp>

#include "internal/code_document_controller.h"
#include "internal/code_editor.h"

#include <QColor>
#include <QKeyEvent>
#include <QPainter>
#include <QPalette>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

// Multiple cursors (CE06, design section 5). The primary cursor stays Qt's own
// QTextCursor so accessibility, IME and the clipboard see one cursor; the
// secondary cursors are this overlay's own UTF-16 regions, painted by the view.
// Every text operation is turned into one CodeEditSet and applied through the
// controller, so a hundred cursors cost one pass over the text: the controller
// maps every region with AdvanceCodeByteOffset, never GetCodeByteOffset per
// cursor. Movement uses Qt's own QTextCursor logic so platform word and line
// behavior is preserved.
namespace ludus::editor
{
namespace
{
using namespace foundation;

/// Width of a painted secondary caret in device-independent pixels.
constexpr int32 SECONDARY_CARET_WIDTH = 2;
/// Opacity of a painted secondary caret (alpha, 0-255).
constexpr int32 SECONDARY_CARET_OPACITY = 200;

} // namespace

CodeMultiCursor::CodeMultiCursor(CodeEditor& inoutEditor) noexcept : mEditor(inoutEditor)
{
}

int32 CodeMultiCursor::GetCursorCount() const noexcept
{
    return 1 + static_cast<int32>(mSecondary.size());
}

bool CodeMultiCursor::Collapse()
{
    mColumnAnchorPosition = -1;
    if (mSecondary.isEmpty() == true)
    {
        return false;
    }
    mSecondary.clear();
    mEditor.NotifyMultiCursorChanged();
    return true;
}

void CodeMultiCursor::AdoptSecondaryCursors(const QList<CodeSecondaryCursor>& secondaryCursors)
{
    mSecondary = secondaryCursors;
    mColumnAnchorPosition = -1;
    sortAndMerge();
    mEditor.NotifyMultiCursorChanged();
}

QList<CodeRegionEdit> CodeMultiCursor::collectRegions() const
{
    QList<CodeRegionEdit> regions;
    regions.reserve(mSecondary.size() + 1);
    const QTextCursor primary = mEditor.textCursor();
    regions.append(CodeRegionEdit{ .Start = primary.selectionStart(),
                                   .End = primary.selectionEnd(),
                                   .Inserted = QString{},
                                   .IsPrimary = true });
    for (const CodeSecondaryCursor& secondary : mSecondary)
    {
        regions.append(CodeRegionEdit{ .Start = secondary.Start(),
                                       .End = secondary.End(),
                                       .Inserted = QString{},
                                       .IsPrimary = false });
    }
    foundation::Sort(regions.data(),
                     static_cast<usize>(regions.size()),
                     [](const CodeRegionEdit& left, const CodeRegionEdit& right) noexcept {
                         return left.Start < right.Start;
                     });
    return regions;
}

void CodeMultiCursor::sortAndMerge()
{
    const QTextCursor primary = mEditor.textCursor();
    const int32 primaryStart = primary.selectionStart();
    const int32 primaryEnd = primary.selectionEnd();
    foundation::Sort(mSecondary.data(),
                     static_cast<usize>(mSecondary.size()),
                     [](const CodeSecondaryCursor& left, const CodeSecondaryCursor& right) noexcept {
                         return left.Start() < right.Start();
                     });
    QList<CodeSecondaryCursor> merged;
    for (const CodeSecondaryCursor& secondary : mSecondary)
    {
        // The primary owns its span: drop a secondary that touches it. A bare
        // caret touches the primary when it falls within [start, end]; a secondary
        // with its own selection touches when the ranges intersect or abut.
        const bool touchesPrimary = secondary.Start() <= primaryEnd && secondary.End() >= primaryStart;
        if (touchesPrimary == true)
        {
            continue;
        }
        // Merge a secondary that touches or overlaps the previous one, so no two
        // cursors ever share text. The merged range keeps the forward direction.
        if (merged.isEmpty() == false && secondary.Start() <= merged.last().End())
        {
            const int32 start = qMin(merged.last().Start(), secondary.Start());
            const int32 end = qMax(merged.last().End(), secondary.End());
            merged.last() = CodeSecondaryCursor{ .Anchor = start, .Caret = end };
            continue;
        }
        merged.append(secondary);
    }
    mSecondary = merged;
}

void CodeMultiCursor::AddCursorInDirection(bool shouldGoUp)
{
    QTextDocument* document = mEditor.document();
    // The reference cursor is the topmost caret when adding above, the bottommost
    // when adding below, so a column of cursors grows away from the primary.
    QTextCursor reference = mEditor.textCursor();
    for (const CodeSecondaryCursor& secondary : mSecondary)
    {
        if ((shouldGoUp == true && secondary.Caret < reference.position()) ||
            (shouldGoUp == false && secondary.Caret > reference.position()))
        {
            reference = QTextCursor(document);
            reference.setPosition(secondary.Anchor);
            reference.setPosition(secondary.Caret, QTextCursor::KeepAnchor);
        }
    }
    const int32 column = reference.positionInBlock();
    QTextCursor moved = reference;
    moved.clearSelection();
    if (moved.movePosition(shouldGoUp == true ? QTextCursor::Up : QTextCursor::Down) == false)
    {
        return; // At the document edge; nothing to add.
    }
    const QTextBlock block = moved.block();
    const int32 clampedColumn = qMin(column, block.length() - 1);
    const int32 caret = block.position() + clampedColumn;
    mSecondary.append(CodeSecondaryCursor{ .Anchor = caret, .Caret = caret });
    sortAndMerge();
    mEditor.NotifyMultiCursorChanged();
}

void CodeMultiCursor::AddNextOccurrence()
{
    QTextCursor primary = mEditor.textCursor();
    if (primary.hasSelection() == false)
    {
        // First press selects the word under the caret; it becomes the search term.
        primary.select(QTextCursor::WordUnderCursor);
        if (primary.hasSelection() == false)
        {
            return;
        }
        mEditor.setTextCursor(primary);
        mEditor.NotifyMultiCursorChanged();
        return;
    }
    const QString needle = primary.selectedText();
    if (needle.isEmpty() == true || needle.contains(QChar::ParagraphSeparator) == true)
    {
        return;
    }
    const QString text = mEditor.toPlainText();
    // Collect the primary and every secondary range so the search skips cursors
    // that already cover a match, and wraps once past the last one.
    QList<int32> occupiedStart;
    occupiedStart.append(primary.selectionStart());
    for (const CodeSecondaryCursor& secondary : mSecondary)
    {
        occupiedStart.append(secondary.Start());
    }
    int32 searchFrom = primary.selectionEnd();
    for (const CodeSecondaryCursor& secondary : mSecondary)
    {
        searchFrom = qMax(searchFrom, secondary.End());
    }
    int32 at = static_cast<int32>(text.indexOf(needle, searchFrom, Qt::CaseSensitive));
    if (at < 0)
    {
        at = static_cast<int32>(text.indexOf(needle, 0, Qt::CaseSensitive));
    }
    while (at >= 0 && occupiedStart.contains(at) == true)
    {
        at = static_cast<int32>(text.indexOf(needle, at + 1, Qt::CaseSensitive));
    }
    if (at < 0)
    {
        return; // Every occurrence already has a cursor.
    }
    // The old primary selection becomes a secondary; the new match is the primary.
    mSecondary.append(CodeSecondaryCursor{ .Anchor = primary.selectionStart(), .Caret = primary.selectionEnd() });
    QTextCursor next(mEditor.document());
    next.setPosition(at);
    next.setPosition(at + static_cast<int32>(needle.size()), QTextCursor::KeepAnchor);
    mEditor.setTextCursor(next);
    sortAndMerge();
    mEditor.NotifyMultiCursorChanged();
}

void CodeMultiCursor::AddAllOccurrences()
{
    QTextCursor primary = mEditor.textCursor();
    if (primary.hasSelection() == false)
    {
        primary.select(QTextCursor::WordUnderCursor);
        if (primary.hasSelection() == false)
        {
            return;
        }
        mEditor.setTextCursor(primary);
    }
    const QString needle = primary.selectedText();
    if (needle.isEmpty() == true || needle.contains(QChar::ParagraphSeparator) == true)
    {
        return;
    }
    const QString text = mEditor.toPlainText();
    const int32 primaryStart = primary.selectionStart();
    // One pass over the decoded text with QString::indexOf (never QTextDocument::find
    // in a loop): every occurrence but the primary's becomes a secondary cursor.
    mSecondary.clear();
    int32 at = static_cast<int32>(text.indexOf(needle, 0, Qt::CaseSensitive));
    while (at >= 0)
    {
        if (at != primaryStart)
        {
            const int32 end = at + static_cast<int32>(needle.size());
            mSecondary.append(CodeSecondaryCursor{ .Anchor = at, .Caret = end });
        }
        at = static_cast<int32>(text.indexOf(needle, at + static_cast<int32>(needle.size()), Qt::CaseSensitive));
    }
    sortAndMerge();
    mEditor.NotifyMultiCursorChanged();
}

void CodeMultiCursor::ColumnSelectTo(const QPoint& viewportPoint)
{
    QTextCursor primary = mEditor.textCursor();
    const int32 anchorPosition = mColumnAnchorPosition >= 0 ? mColumnAnchorPosition : primary.anchor();
    mColumnAnchorPosition = anchorPosition;
    QTextDocument* document = mEditor.document();
    QTextCursor anchorCursor(document);
    anchorCursor.setPosition(anchorPosition);
    const int32 anchorColumn = anchorCursor.positionInBlock();
    const int32 anchorBlock = anchorCursor.blockNumber();
    const QTextCursor pointCursor = mEditor.cursorForPosition(viewportPoint);
    const int32 pointColumn = pointCursor.positionInBlock();
    const int32 pointBlock = pointCursor.blockNumber();
    const int32 firstBlock = qMin(anchorBlock, pointBlock);
    const int32 lastBlock = qMax(anchorBlock, pointBlock);
    const int32 fromColumn = qMin(anchorColumn, pointColumn);
    const int32 toColumn = qMax(anchorColumn, pointColumn);
    // One box cursor per spanned line, clamped to each line's length. The line the
    // primary ends on carries the Qt cursor; the others become secondaries.
    mSecondary.clear();
    QList<CodeSecondaryCursor> boxed;
    for (int32 blockNumber = firstBlock; blockNumber <= lastBlock; ++blockNumber)
    {
        const QTextBlock block = document->findBlockByNumber(blockNumber);
        if (block.isValid() == false)
        {
            continue;
        }
        const int32 lineLength = block.length() - 1;
        const int32 start = block.position() + qMin(fromColumn, lineLength);
        const int32 end = block.position() + qMin(toColumn, lineLength);
        boxed.append(CodeSecondaryCursor{ .Anchor = start, .Caret = end });
    }
    if (boxed.isEmpty() == true)
    {
        return;
    }
    // Make the caret line the primary; the rest are secondaries. The caret keeps
    // its real direction, so the box anchor is the from-column, the caret end the
    // to-column, flipped when the pointer is to the left of the anchor column.
    const int32 caretBlock = pointBlock;
    const bool isRightward = pointColumn >= anchorColumn;
    bool hasPrimary = false;
    for (const CodeSecondaryCursor& region : boxed)
    {
        if (hasPrimary == false && document->findBlock(region.Anchor).blockNumber() == caretBlock)
        {
            QTextCursor newPrimary(document);
            newPrimary.setPosition(isRightward == true ? region.Start() : region.End());
            newPrimary.setPosition(isRightward == true ? region.End() : region.Start(), QTextCursor::KeepAnchor);
            mEditor.setTextCursor(newPrimary);
            hasPrimary = true;
        }
        else
        {
            mSecondary.append(region);
        }
    }
    mEditor.NotifyMultiCursorChanged();
}

bool CodeMultiCursor::moveEveryCursor(int32 moveOperation, bool shouldKeepAnchor)
{
    QTextDocument* document = mEditor.document();
    const QTextCursor::MoveOperation operation = static_cast<QTextCursor::MoveOperation>(moveOperation);
    const QTextCursor::MoveMode mode = shouldKeepAnchor == true ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;
    QTextCursor primary = mEditor.textCursor();
    primary.movePosition(operation, mode);
    mEditor.setTextCursor(primary);
    for (CodeSecondaryCursor& secondary : mSecondary)
    {
        QTextCursor cursor(document);
        cursor.setPosition(secondary.Anchor);
        cursor.setPosition(secondary.Caret, QTextCursor::KeepAnchor);
        cursor.movePosition(operation, mode);
        secondary = CodeSecondaryCursor{ .Anchor = cursor.anchor(), .Caret = cursor.position() };
    }
    sortAndMerge();
    mEditor.NotifyMultiCursorChanged();
    return true;
}

bool CodeMultiCursor::HandleKeyPress(QKeyEvent* event)
{
    if (mSecondary.isEmpty() == true)
    {
        return false;
    }
    // Movement for every cursor using Qt's own operations (design section 5), so
    // word and line moves match the platform. Shift extends every selection.
    const bool shouldKeepAnchor = event->modifiers().testFlag(Qt::ShiftModifier) == true;
    const Qt::KeyboardModifiers moveModifiers = event->modifiers() & ~Qt::KeyboardModifiers(Qt::ShiftModifier);
    if (moveModifiers == Qt::NoModifier)
    {
        switch (event->key())
        {
            case Qt::Key_Left:
                return moveEveryCursor(static_cast<int32>(QTextCursor::Left), shouldKeepAnchor);
            case Qt::Key_Right:
                return moveEveryCursor(static_cast<int32>(QTextCursor::Right), shouldKeepAnchor);
            case Qt::Key_Up:
                return moveEveryCursor(static_cast<int32>(QTextCursor::Up), shouldKeepAnchor);
            case Qt::Key_Down:
                return moveEveryCursor(static_cast<int32>(QTextCursor::Down), shouldKeepAnchor);
            case Qt::Key_Home:
                return moveEveryCursor(static_cast<int32>(QTextCursor::StartOfLine), shouldKeepAnchor);
            case Qt::Key_End:
                return moveEveryCursor(static_cast<int32>(QTextCursor::EndOfLine), shouldKeepAnchor);
            default:
                break;
        }
    }
    const bool isWordModifier =
#if defined(Q_OS_MACOS)
        moveModifiers == Qt::AltModifier;
#else
        moveModifiers == Qt::ControlModifier;
#endif
    if (isWordModifier == true)
    {
        if (event->key() == Qt::Key_Left)
        {
            return moveEveryCursor(static_cast<int32>(QTextCursor::WordLeft), shouldKeepAnchor);
        }
        if (event->key() == Qt::Key_Right)
        {
            return moveEveryCursor(static_cast<int32>(QTextCursor::WordRight), shouldKeepAnchor);
        }
    }

    // Text operations. Each becomes one CodeEditSet across every cursor. The
    // controller maps and applies them in one pass and returns the new carets.
    QString inserted;
    bool removesBefore = false;
    bool removesAfter = false;
    if (event->key() == Qt::Key_Backspace && moveModifiers == Qt::NoModifier)
    {
        removesBefore = true;
    }
    else if (event->key() == Qt::Key_Delete && moveModifiers == Qt::NoModifier)
    {
        removesAfter = true;
    }
    else if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && moveModifiers == Qt::NoModifier)
    {
        inserted = QStringLiteral("\n");
    }
    else if (event->key() == Qt::Key_Tab && moveModifiers == Qt::NoModifier)
    {
        inserted = QStringLiteral("    ");
    }
    else if (event->text().isEmpty() == false && event->text().at(0).isPrint() == true &&
             (moveModifiers == Qt::NoModifier))
    {
        inserted = event->text();
    }
    else
    {
        return false; // Not a key this layer distributes; leave it to the view.
    }

    QList<CodeRegionEdit> regions = collectRegions();
    for (CodeRegionEdit& region : regions)
    {
        if (region.Start == region.End)
        {
            // A bare caret: Backspace removes the char before, Delete the char after.
            if (removesBefore == true && region.Start > 0)
            {
                region.Start -= 1;
            }
            else if (removesAfter == true && region.End < mEditor.document()->characterCount() - 1)
            {
                region.End += 1;
            }
        }
        // A selection plus Backspace/Delete just removes the selection (Start..End).
        region.Inserted = inserted;
    }
    return applyRegionEdits(regions);
}

bool CodeMultiCursor::HandlePaste(const QString& text)
{
    if (mSecondary.isEmpty() == true)
    {
        return false;
    }
    QList<CodeRegionEdit> regions = collectRegions();
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = normalized.split(QLatin1Char('\n'));
    // One line per cursor when the counts match, else the whole text at each
    // cursor (design section 5). split() yields a trailing empty element for a
    // terminating newline, so a clipboard of N lines with a final newline matches
    // N cursors by dropping that empty element.
    QStringList perCursor = lines;
    if (perCursor.size() > 1 && perCursor.last().isEmpty() == true)
    {
        perCursor.removeLast();
    }
    const bool oneLineEach = perCursor.size() == regions.size();
    for (int32 index = 0; index < regions.size(); ++index)
    {
        regions[index].Inserted = oneLineEach == true ? perCursor.at(index) : normalized;
    }
    return applyRegionEdits(regions);
}

bool CodeMultiCursor::applyRegionEdits(const QList<CodeRegionEdit>& regionsInDocumentOrder)
{
    if (mControllerOrNull == nullptr)
    {
        return false; // Unbound view: multi-cursor edits are unavailable.
    }
    QList<CodeSecondaryCursor> secondary;
    const CodeEditStatus status = mControllerOrNull->ApplyMultiCursorEdit(regionsInDocumentOrder, secondary);
    if (status != CodeEditStatus::Ok)
    {
        return false;
    }
    mSecondary = secondary;
    mColumnAnchorPosition = -1;
    sortAndMerge();
    mEditor.NotifyMultiCursorChanged();
    return true;
}

void CodeMultiCursor::Paint(QPainter& inoutPainter) const
{
    QColor caretColor = mEditor.palette().color(QPalette::Text);
    caretColor.setAlpha(SECONDARY_CARET_OPACITY);
    inoutPainter.setPen(Qt::NoPen);
    inoutPainter.setBrush(caretColor);
    for (const CodeSecondaryCursor& secondary : mSecondary)
    {
        QTextCursor cursor(mEditor.document());
        cursor.setPosition(secondary.Caret);
        const QRect caret = mEditor.cursorRect(cursor);
        inoutPainter.drawRect(QRect(caret.left(), caret.top(), SECONDARY_CARET_WIDTH, caret.height()));
    }
}
} // namespace ludus::editor
