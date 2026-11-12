#include "internal/code_structure_view.h"

#include "internal/code_navigation.h"

#include <ludus/foundation/base/core.h>

#include <QAccessible>
#include <QAction>
#include <QCryptographicHash>
#include <QDialog>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 MAXIMUM_OUTLINE_ROWS = 4096;
constexpr int32 MAXIMUM_STORED_FOLDS = 12000;

QString HashFoldHeader(const QTextBlock& block)
{
    return QString::fromLatin1(QCryptographicHash::hash(block.text().toUtf8(), QCryptographicHash::Sha256).toHex());
}
} // namespace

CodeStructureView::CodeStructureView(CodeEditor* editor, const CodeSyntaxService& syntax)
    : QObject(editor), mEditor(editor), mSyntax(syntax), mHeader(new QWidget(editor)),
      mOutline(new QToolButton(mHeader)), mBreadcrumbs(new QToolButton(mHeader)), mSticky(new QToolButton(mHeader))
{
    QVBoxLayout* layout = new QVBoxLayout(mHeader);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    QHBoxLayout* navigation = new QHBoxLayout;
    navigation->setSpacing(0);
    navigation->addWidget(mOutline);
    navigation->addWidget(mBreadcrumbs, 1);
    layout->addLayout(navigation);
    layout->addWidget(mSticky, 0, Qt::AlignLeft);
    mOutline->setText(tr("Outline"));
    mOutline->setObjectName(QStringLiteral("codeSyntaxOutline"));
    mBreadcrumbs->setObjectName(QStringLiteral("codeSyntaxBreadcrumbs"));
    mSticky->setObjectName(QStringLiteral("codeSyntaxSticky"));
    mBreadcrumbs->setAccessibleName(tr("Current syntax scopes"));
    mSticky->setAccessibleName(tr("Pinned syntax scope"));
    mHeader->hide();
    connect(mOutline, &QToolButton::clicked, this, &CodeStructureView::showOutline);
    connect(mBreadcrumbs, &QToolButton::clicked, this, &CodeStructureView::showBreadcrumbs);
    connect(mSticky, &QToolButton::clicked, this, [this]() { goToPosition(mStickyPosition); });

    connect(editor, &CodeEditor::ToggleFoldRequested, this, &CodeStructureView::toggleFoldAtCaret);
    connect(editor, &CodeEditor::FoldAllRequested, this, &CodeStructureView::FoldAll);
    connect(editor, &CodeEditor::UnfoldAllRequested, this, &CodeStructureView::UnfoldAll);
    connect(&syntax, &CodeSyntaxService::ResultChanged, this, &CodeStructureView::acceptResult);
    connect(editor, &CodeEditor::cursorPositionChanged, this, [this]() {
        if (mIsApplyingVisibility == false)
        {
            const QTextCursor cursor = mEditor->textCursor();
            if (cursor.block().isVisible() == false)
            {
                Reveal(cursor);
            }
            UpdateHeader();
        }
    });
    mRepair.setSingleShot(true);
    connect(&mRepair, &QTimer::timeout, this, &CodeStructureView::refreshVisibility);
    mDocumentRevision = editor->document()->revision();
    connect(editor->document(), &QTextDocument::contentsChange, this, [this](int32 position, int32 removed, int32) {
        const int32 revision = mEditor->document()->revision();
        if (mDocumentRevision != revision)
        {
            mDocumentRevision = revision;
            mSelectionHistory.clear();
            mLastExpandedSelection = {};
            if (mCollapsed.isEmpty() == false)
            {
                mCollapsed.removeIf([position, removed](const CollapsedFold& fold) {
                    return position <= fold.OpeningPosition &&
                           removed >= fold.OpeningPosition - position + fold.OpeningLength;
                });
                for (CollapsedFold& fold : mCollapsed)
                {
                    const QTextBlock opening = fold.First.block();
                    fold.OpeningPosition = opening.position();
                    fold.OpeningLength = opening.length();
                }
                mRepair.start(0);
            }
        }
    });

    const auto addAction = [this](const QString& label, QKeySequence shortcut, void (CodeStructureView::*operation)()) {
        QAction* action = new QAction(label, mEditor);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(action, &QAction::triggered, this, operation);
        mEditor->addAction(action);
    };
    addAction(tr("Outline"), QKeySequence(), &CodeStructureView::showOutline);
    addAction(tr("Expand Selection"),
              QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Right),
              &CodeStructureView::expandSelection);
    addAction(tr("Shrink Selection"),
              QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Left),
              &CodeStructureView::shrinkSelection);
    addAction(tr("Fold Selection"), QKeySequence(), &CodeStructureView::foldSelection);
}

void CodeStructureView::acceptResult()
{
    const CodeSyntaxResult* resultOrNull = mSyntax.GetCurrentResultOrNull();
    if (resultOrNull == nullptr)
    {
        UpdateHeader();
        return;
    }
    mAvailable.clear();
    for (const CodeSyntaxFold& fold : resultOrNull->Folds)
    {
        const QMap<int32, CodeSyntaxFold>::const_iterator found = mAvailable.constFind(fold.FirstLine);
        if (found == mAvailable.cend() || fold.LastLine > found->LastLine ||
            (fold.LastLine == found->LastLine && fold.Kind == CodeFoldKind::Region))
        {
            mAvailable.insert(fold.FirstLine, fold);
        }
    }
    mCollapsed.removeIf([this](const CollapsedFold& fold) {
        const int32 line = fold.First.blockNumber();
        if (fold.Kind == CodeFoldKind::Manual)
        {
            return line >= fold.Last.blockNumber();
        }
        const QMap<int32, CodeSyntaxFold>::const_iterator found = mAvailable.constFind(line);
        return found == mAvailable.cend() || fold.Kind != found->Kind;
    });
    for (CollapsedFold& fold : mCollapsed)
    {
        if (fold.Kind != CodeFoldKind::Manual)
        {
            const CodeSyntaxFold& available = *mAvailable.constFind(fold.First.blockNumber());
            fold.Last = QTextCursor(mEditor->document()->findBlockByNumber(available.LastLine));
            fold.Label = available.Label;
        }
    }
    for (const QJsonValue& entry : mRestore)
    {
        const QJsonObject stored = entry.toObject();
        const int32 line = stored.value(QStringLiteral("line")).toInt(-1);
        const QTextBlock block = mEditor->document()->findBlockByNumber(line);
        const QMap<int32, CodeSyntaxFold>::const_iterator found = mAvailable.constFind(line);
        const int32 kind = stored.value(QStringLiteral("kind")).toInt(-1);
        if (block.isValid() == false || HashFoldHeader(block) != stored.value(QStringLiteral("header")).toString())
        {
            continue;
        }
        if (found != mAvailable.cend() && kind == static_cast<int32>(found->Kind))
        {
            addCollapsed(*found);
        }
        else if (kind == static_cast<int32>(CodeFoldKind::Manual))
        {
            const int32 last = stored.value(QStringLiteral("last")).toInt(-1);
            addCollapsed({line, last, CodeFoldKind::Manual, {}});
        }
    }
    mRestore = {};
    if (mCollapsed.isEmpty() == false || mPreviousLastHiddenLine > 0)
    {
        refreshVisibility();
    }
    mOutline->setToolTip(resultOrNull->HasPartialFolds == true
                             ? tr("Outline. This document has more fold ranges than the editor can display.")
                             : tr("Show document outline"));
    UpdateHeader();
    mEditor->updateFoldPresentation();
}

void CodeStructureView::addCollapsed(const CodeSyntaxFold& fold)
{
    const QTextBlock first = mEditor->document()->findBlockByNumber(fold.FirstLine);
    const QTextBlock last = mEditor->document()->findBlockByNumber(fold.LastLine);
    if (first.isValid() == false || last.isValid() == false || first.blockNumber() >= last.blockNumber())
    {
        return;
    }
    QTextCursor opening(first);
    opening.setKeepPositionOnInsert(true);
    mCollapsed.append({opening, QTextCursor(last), first.position(), first.length(), fold.Kind, fold.Label});
}

void CodeStructureView::refreshVisibility()
{
    if (mIsApplyingVisibility == true)
    {
        return;
    }
    mIsApplyingVisibility = true;
    mHiddenThrough.clear();
    mPlaceholderLabels.clear();
    mCollapsed.removeIf([](const CollapsedFold& fold) {
        return fold.First.block().isValid() == false || fold.First.blockNumber() >= fold.Last.blockNumber();
    });
    int32 lastHiddenLine = 0;
    for (const CollapsedFold& fold : mCollapsed)
    {
        const int32 first = fold.First.blockNumber();
        const int32 last = fold.Last.blockNumber();
        if (first < last)
        {
            mHiddenThrough.insert(first, qMax(last, mHiddenThrough.value(first, first)));
            if (fold.Label.isEmpty() == false)
            {
                mPlaceholderLabels.insert(first, fold.Label);
            }
            lastHiddenLine = qMax(lastHiddenLine, last);
        }
    }
    int32 coveredThrough = -1;
    int32 firstChanged = -1;
    int32 lastChanged = 0;
    const int32 scanThrough = qMax(lastHiddenLine, mPreviousLastHiddenLine);
    const int32 firstHiddenLine = mHiddenThrough.isEmpty() == true ? scanThrough : mHiddenThrough.firstKey();
    const int32 scanFrom =
        mPreviousLastHiddenLine == 0 ? firstHiddenLine : qMin(firstHiddenLine, mPreviousFirstHiddenLine);
    mPreviousFirstHiddenLine = firstHiddenLine;
    mPreviousLastHiddenLine = lastHiddenLine;
    QMap<int32, int32>::const_iterator next = mHiddenThrough.cbegin();
    QTextDocument* document = mEditor->document();
    int32 line = scanFrom;
    for (QTextBlock block = document->findBlockByNumber(scanFrom); block.isValid() == true && line <= scanThrough;
         block = block.next(), ++line)
    {
        const bool isVisible = line > coveredThrough;
        if (block.isVisible() != isVisible)
        {
            block.setVisible(isVisible);
            block.setLineCount(isVisible == true ? 1 : 0);
            if (firstChanged < 0)
            {
                firstChanged = block.position();
            }
            lastChanged = block.position() + block.length();
        }
        if (next != mHiddenThrough.cend() && next.key() == line)
        {
            coveredThrough = qMax(coveredThrough, next.value());
            ++next;
        }
    }
    if (firstChanged >= 0)
    {
        {
            // Visibility needs a new layout. Suppress QTextDocument's synthetic
            // contentsChange so the highlighter does not reprocess hidden text.
            const QSignalBlocker blockDocumentSignals(document);
            document->markContentsDirty(firstChanged, lastChanged - firstChanged);
        }
        const QTextCursor cursor = mEditor->textCursor();
        if (cursor.block().isVisible() == false)
        {
            QTextBlock visible = cursor.block().previous();
            while (visible.isValid() == true && visible.isVisible() == false)
            {
                visible = visible.previous();
            }
            if (visible.isValid() == true)
            {
                mEditor->QPlainTextEdit::setTextCursor(QTextCursor(visible));
            }
        }
        mEditor->viewport()->update();
    }
    mIsApplyingVisibility = false;
    mEditor->updateFoldPresentation();
}

void CodeStructureView::toggleFoldAtCaret()
{
    ToggleFold(mEditor->textCursor().blockNumber());
}

void CodeStructureView::ToggleFold(int32 line)
{
    if (mCollapsed.removeIf([line](const CollapsedFold& fold) { return fold.First.blockNumber() == line; }) > 0)
    {
        announce(tr("Expanded line %1.").arg(line + 1));
    }
    else
    {
        const QMap<int32, CodeSyntaxFold>::const_iterator fold = mAvailable.constFind(line);
        if (mSyntax.GetCurrentResultOrNull() == nullptr || fold == mAvailable.cend())
        {
            return;
        }
        addCollapsed(*fold);
        announce(tr("Collapsed %1 lines at line %2.").arg(fold->LastLine - line).arg(line + 1));
    }
    refreshVisibility();
    mEditor->viewport()->update();
}

void CodeStructureView::FoldAll()
{
    if (mSyntax.GetCurrentResultOrNull() == nullptr)
    {
        return;
    }
    mCollapsed.removeIf([](const CollapsedFold& fold) { return fold.Kind != CodeFoldKind::Manual; });
    mCollapsed.reserve(mCollapsed.size() + mAvailable.size());
    for (const CodeSyntaxFold& fold : mAvailable)
    {
        addCollapsed(fold);
    }
    refreshVisibility();
    announce(tr("Collapsed %1 ranges.").arg(mCollapsed.size()));
}

void CodeStructureView::UnfoldAll()
{
    mCollapsed.clear();
    mRestore = {};
    refreshVisibility();
    announce(tr("Expanded all lines."));
}

void CodeStructureView::Reveal(const QTextCursor& cursor)
{
    if (mIsApplyingVisibility == true || mCollapsed.isEmpty() == true)
    {
        return;
    }
    const int32 anchorLine = mEditor->document()->findBlock(cursor.anchor()).blockNumber();
    const int32 positionLine = cursor.blockNumber();
    const qsizetype removed = mCollapsed.removeIf([anchorLine, positionLine](const CollapsedFold& fold) {
        const int32 first = fold.First.blockNumber();
        const int32 last = fold.Last.blockNumber();
        return (anchorLine > first && anchorLine <= last) || (positionLine > first && positionLine <= last);
    });
    if (removed > 0)
    {
        refreshVisibility();
    }
}

bool CodeStructureView::HasFold(int32 line) const
{
    return (mSyntax.GetCurrentResultOrNull() != nullptr && mAvailable.contains(line) == true) ||
           mHiddenThrough.contains(line) == true;
}

int32 CodeStructureView::GetLastCollapsedLine(int32 line) const
{
    int32 last = mHiddenThrough.value(line, line);
    QMap<int32, int32>::const_iterator fold = mHiddenThrough.upperBound(line);
    while (fold != mHiddenThrough.cend() && fold.key() <= last)
    {
        last = qMax(last, fold.value());
        ++fold;
    }
    return last;
}

int32 CodeStructureView::GetHiddenLineCount(int32 line) const
{
    return GetLastCollapsedLine(line) - line;
}

QString CodeStructureView::DescribeFold(int32 line) const
{
    const int32 count = GetHiddenLineCount(line);
    if (count > 0)
    {
        return tr("Expand %1 hidden lines").arg(count);
    }
    return HasFold(line) == true ? tr("Collapse range") : QString();
}

QString CodeStructureView::GetPlaceholder(int32 line) const
{
    const QString label = mPlaceholderLabels.value(line);
    return label.isEmpty() == true ? QStringLiteral(" … ") : QStringLiteral(" … %1 ").arg(label);
}

void CodeStructureView::DuplicateFold(int32 sourceLine, int32 destinationLine)
{
    CodeSyntaxFold copy;
    for (const CollapsedFold& fold : mCollapsed)
    {
        if (fold.First.blockNumber() == sourceLine)
        {
            const int32 lineCount = fold.Last.blockNumber() - sourceLine;
            copy = {destinationLine, destinationLine + lineCount, fold.Kind, fold.Label};
            break;
        }
    }
    if (copy.LastLine > copy.FirstLine)
    {
        addCollapsed(copy);
        refreshVisibility();
    }
}

void CodeStructureView::foldSelection()
{
    const QTextCursor cursor = mEditor->textCursor();
    const int32 first = mEditor->document()->findBlock(cursor.selectionStart()).blockNumber();
    const QTextBlock end = mEditor->document()->findBlock(cursor.selectionEnd());
    const int32 last = end.blockNumber() - (cursor.selectionEnd() == end.position() ? 1 : 0);
    addCollapsed({first, last, CodeFoldKind::Manual, {}});
    refreshVisibility();
}

bool CodeStructureView::MoveAcrossFold(QKeyEvent* event)
{
    if (mHiddenThrough.isEmpty() == true || event->modifiers() != Qt::NoModifier ||
        mEditor->textCursor().hasSelection() == true)
    {
        return false;
    }
    QTextCursor cursor = mEditor->textCursor();
    const QTextBlock block = cursor.block();
    const int32 key = event->key();
    if ((key == Qt::Key_Right && cursor.atBlockEnd() == true) || key == Qt::Key_Down)
    {
        const int32 last = GetLastCollapsedLine(block.blockNumber());
        if (last > block.blockNumber())
        {
            const QTextBlock destination = mEditor->document()->findBlockByNumber(last + 1);
            if (destination.isValid() == true)
            {
                mEditor->setTextCursor(QTextCursor(destination));
            }
            return true;
        }
    }
    if ((key == Qt::Key_Left && cursor.atBlockStart() == true) || key == Qt::Key_Up)
    {
        QTextBlock previous = block.previous();
        if (previous.isValid() == true && previous.isVisible() == false)
        {
            while (previous.isValid() == true && previous.isVisible() == false)
            {
                previous = previous.previous();
            }
            if (previous.isValid() == true)
            {
                cursor = QTextCursor(previous);
                if (key == Qt::Key_Left)
                {
                    cursor.movePosition(QTextCursor::EndOfBlock);
                }
                mEditor->setTextCursor(cursor);
            }
            return true;
        }
    }
    return false;
}

QList<int32> CodeStructureView::findScopes(int32 position) const
{
    QList<int32> scopes;
    const CodeSyntaxResult* resultOrNull = mSyntax.GetCurrentResultOrNull();
    if (resultOrNull != nullptr)
    {
        int32 index = resultOrNull->FindContainingNode({position, position});
        while (index >= 0)
        {
            const CodeSyntaxNode& node = resultOrNull->Nodes.at(index);
            if (node.Label.isEmpty() == false)
            {
                scopes.prepend(index);
            }
            index = node.Parent;
        }
    }
    return scopes;
}

void CodeStructureView::UpdateHeader()
{
    const int32 previousHeight = GetHeaderHeight();
    const CodeSyntaxResult* resultOrNull = mSyntax.GetCurrentResultOrNull();
    mHeader->setEnabled(resultOrNull != nullptr);
    if (resultOrNull == nullptr)
    {
        return;
    }
    const bool hasOutline = resultOrNull->Outline.isEmpty() == false;
    if (hasOutline == true)
    {
        QStringList labels;
        for (int32 index : findScopes(mEditor->textCursor().position()))
        {
            labels.append(resultOrNull->Nodes.at(index).Label);
        }
        const QString breadcrumb = labels.join(QStringLiteral("  ›  "));
        if (mBreadcrumbs->text() != breadcrumb)
        {
            mBreadcrumbs->setText(mEditor->fontMetrics().elidedText(breadcrumb, Qt::ElideLeft, mEditor->width() / 2));
            mBreadcrumbs->setToolTip(breadcrumb);
        }
        mBreadcrumbs->setVisible(labels.isEmpty() == false);
        mStickyPosition = -1;
        const QTextBlock first = mEditor->firstVisibleBlock();
        const QList<int32> scopes = findScopes(first.position());
        for (int32 index : scopes)
        {
            const CodeSyntaxNode& node = resultOrNull->Nodes.at(index);
            if (node.Range.Start < first.position())
            {
                mStickyPosition = node.Range.Start;
                mSticky->setText(node.Label);
                mSticky->setToolTip(tr("Go to %1").arg(node.Label));
            }
        }
        mSticky->setVisible(mStickyPosition >= 0);
    }
    mHeader->setVisible(hasOutline);
    if (previousHeight != GetHeaderHeight())
    {
        mEditor->updateGutterWidth();
    }
}

int32 CodeStructureView::GetHeaderHeight() const
{
    return mHeader->isHidden() == true ? 0 : mHeader->sizeHint().height();
}

void CodeStructureView::LayoutHeader(const QRect& rectangle, int32 gutterWidth)
{
    mHeader->setGeometry(rectangle.left() + gutterWidth,
                         rectangle.top(),
                         qMax(0, rectangle.width() - gutterWidth),
                         GetHeaderHeight());
}

void CodeStructureView::goToPosition(int32 position)
{
    if (position < 0 || position >= mEditor->document()->characterCount())
    {
        return;
    }
    QTextCursor cursor(mEditor->document());
    cursor.setPosition(position);
    mEditor->GetNavigation().RememberCurrent();
    mEditor->setTextCursor(cursor);
    mEditor->centerCursor();
    mEditor->setFocus();
}

void CodeStructureView::showBreadcrumbs()
{
    const CodeSyntaxResult* resultOrNull = mSyntax.GetCurrentResultOrNull();
    if (resultOrNull == nullptr)
    {
        return;
    }
    const uint64 revision = resultOrNull->Revision;
    QMenu* menu = new QMenu(mBreadcrumbs);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    for (int32 index : findScopes(mEditor->textCursor().position()))
    {
        const CodeSyntaxNode& node = resultOrNull->Nodes.at(index);
        const int32 position = node.Range.Start;
        menu->addAction(node.Label, this, [this, position, revision]() {
            const CodeSyntaxResult* currentOrNull = mSyntax.GetCurrentResultOrNull();
            if (currentOrNull != nullptr && currentOrNull->Revision == revision)
            {
                goToPosition(position);
            }
        });
    }
    menu->popup(mBreadcrumbs->mapToGlobal(mBreadcrumbs->rect().bottomLeft()));
}

void CodeStructureView::showOutline()
{
    const CodeSyntaxResult* resultOrNull = mSyntax.GetCurrentResultOrNull();
    if (resultOrNull == nullptr || resultOrNull->Outline.isEmpty() == true)
    {
        return;
    }
    const uint64 revision = resultOrNull->Revision;
    QDialog* dialog = new QDialog(mEditor);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Document Outline"));
    QVBoxLayout* layout = new QVBoxLayout(dialog);
    QLineEdit* filter = new QLineEdit(dialog);
    filter->setPlaceholderText(tr("Filter symbols"));
    filter->setAccessibleName(tr("Filter symbols"));
    QListWidget* symbols = new QListWidget(dialog);
    symbols->setAccessibleName(tr("Document symbols"));
    layout->addWidget(filter);
    layout->addWidget(symbols);
    const qsizetype count = qMin(resultOrNull->Outline.size(), static_cast<qsizetype>(MAXIMUM_OUTLINE_ROWS));
    for (qsizetype row = 0; row < count; ++row)
    {
        const CodeSyntaxNode& node = resultOrNull->Nodes.at(resultOrNull->Outline.at(row));
        QListWidgetItem* item = new QListWidgetItem(node.Label, symbols);
        item->setData(Qt::UserRole, node.Range.Start);
    }
    connect(filter, &QLineEdit::textChanged, dialog, [symbols](const QString& text) {
        QListWidgetItem* firstOrNull = nullptr;
        for (int32 row = 0; row < symbols->count(); ++row)
        {
            QListWidgetItem* item = symbols->item(row);
            const bool isMatch = item->text().contains(text, Qt::CaseInsensitive);
            item->setHidden(isMatch == false);
            if (isMatch == true && firstOrNull == nullptr)
            {
                firstOrNull = item;
            }
        }
        symbols->setCurrentItem(firstOrNull);
    });
    connect(filter, &QLineEdit::returnPressed, dialog, &QDialog::accept);
    connect(symbols, &QListWidget::itemActivated, dialog, &QDialog::accept);
    symbols->setCurrentRow(0);
    filter->setFocus();
    connect(dialog, &QDialog::accepted, this, [this, symbols, revision]() {
        const CodeSyntaxResult* currentOrNull = mSyntax.GetCurrentResultOrNull();
        const QListWidgetItem* choiceOrNull = symbols->currentItem();
        if (currentOrNull != nullptr && currentOrNull->Revision == revision && choiceOrNull != nullptr)
        {
            goToPosition(choiceOrNull->data(Qt::UserRole).toInt());
        }
    });
    dialog->open();
}

void CodeStructureView::expandSelection()
{
    const CodeSyntaxResult* resultOrNull = mSyntax.GetCurrentResultOrNull();
    if (resultOrNull == nullptr)
    {
        return;
    }
    const QTextCursor current = mEditor->textCursor();
    if (current != mLastExpandedSelection)
    {
        mSelectionHistory.clear();
    }
    const CodeSyntaxRange selection{current.selectionStart(), current.selectionEnd()};
    int32 index = resultOrNull->FindContainingNode(selection);
    while (index >= 0 && resultOrNull->Nodes.at(index).Range == selection)
    {
        index = resultOrNull->Nodes.at(index).Parent;
    }
    if (index >= 0)
    {
        const CodeSyntaxRange range = resultOrNull->Nodes.at(index).Range;
        QTextCursor expanded = current;
        expanded.setPosition(range.Start);
        expanded.setPosition(range.End, QTextCursor::KeepAnchor);
        mSelectionHistory.append(current);
        mLastExpandedSelection = expanded;
        mEditor->setTextCursor(expanded);
    }
}

void CodeStructureView::shrinkSelection()
{
    if (mSelectionHistory.isEmpty() == false && mEditor->textCursor() == mLastExpandedSelection)
    {
        mLastExpandedSelection = mSelectionHistory.takeLast();
        mEditor->setTextCursor(mLastExpandedSelection);
    }
}

void CodeStructureView::announce(const QString& message)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QAccessibleAnnouncementEvent announcement(mEditor, message);
    QAccessible::updateAccessibility(&announcement);
#else
    mEditor->setAccessibleDescription(message);
#endif
}

QJsonArray CodeStructureView::SaveFolds() const
{
    QJsonArray records;
    for (const CollapsedFold& fold : mCollapsed)
    {
        const QTextBlock first = fold.First.block();
        records.append(QJsonObject{
            {QStringLiteral("line"), first.blockNumber()},
            {QStringLiteral("last"), fold.Last.blockNumber()},
            {QStringLiteral("kind"), static_cast<int32>(fold.Kind)},
            {QStringLiteral("header"), HashFoldHeader(first)},
        });
    }
    return records;
}

void CodeStructureView::RestoreFolds(const QJsonArray& records)
{
    mRestore = {};
    const qsizetype count = qMin(records.size(), static_cast<qsizetype>(MAXIMUM_STORED_FOLDS));
    for (qsizetype index = 0; index < count; ++index)
    {
        mRestore.append(records.at(index));
    }
    if (mSyntax.GetCurrentResultOrNull() != nullptr)
    {
        acceptResult();
    }
}
} // namespace ludus::editor
