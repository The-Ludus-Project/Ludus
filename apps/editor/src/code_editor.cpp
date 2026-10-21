#include "internal/code_editor.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_edit_scope.h"
#include "internal/code_syntax_highlighter.h"

#include <QAction>
#include <QColor>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMimeData>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QStringView>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QVector>

// Thanks to The Qt Company, "Code Editor Example", Qt Widgets documentation,
// CodeEditor::lineNumberAreaPaintEvent: paint only visible text-block numbers.
// Adapted to the existing Ludus palette and a reusable language-neutral view.
// https://doc.qt.io/qt-6/qtwidgets-widgets-codeeditor-example.html
// See docs/architecture/editor-gui-systems.md#source-editing-direction.
namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 INDENT_WIDTH = 4;
constexpr int32 GUTTER_MARGIN = 6;
constexpr int32 DECIMAL_RADIX = 10;
constexpr int32 MAX_BRACKET_SCAN_CHARACTERS = 32768;
constexpr int32 CURRENT_LINE_ALPHA = 24;
constexpr int32 MATCHED_BRACKET_ALPHA = 96;

int32 GetIndentLength(const QString& text)
{
    int32 length = 0;
    while (length < text.size() && (text.at(length) == QLatin1Char(' ') || text.at(length) == QLatin1Char('\t')))
    {
        ++length;
    }
    return length;
}
} // namespace

class CodeEditorGutter final : public QWidget
{
public:
    explicit CodeEditorGutter(CodeEditor* editor) : QWidget(editor), mEditor(editor)
    {
        LUDUS_ASSERT(editor != nullptr);
        setObjectName(QStringLiteral("codeEditorGutter"));
        setAccessibleName(QStringLiteral("Line numbers"));
    }

private:
    void paintEvent(QPaintEvent* event) override
    {
        mEditor->paintGutter(event);
    }
    CodeEditor* mEditor;
};

CodeEditor::CodeEditor(QWidget* parentOrNull)
    : QPlainTextEdit(parentOrNull), mGutter(new CodeEditorGutter(this)),
      mHighlighter(new CodeSyntaxHighlighter(document())),
      mFormatAction(new QAction(QStringLiteral("Format Document"), this)),
      mChooseFormatterAction(new QAction(QStringLiteral("Choose Formatter..."), this))
{
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * INDENT_WIDTH);
    mHighlighter->SetPalette(palette());
    mSelectionUpdate.setSingleShot(true);
    connect(&mSelectionUpdate, &QTimer::timeout, this, &CodeEditor::updateSelections);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]() { mSelectionUpdate.start(0); });
    connect(document(), &QTextDocument::contentsChanged, this, [this]() { mSelectionUpdate.start(0); });
    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateGutterWidth);
    connect(this, &QPlainTextEdit::undoAvailable, this, &CodeEditor::HistoryAvailabilityChanged);
    connect(this, &QPlainTextEdit::redoAvailable, this, &CodeEditor::HistoryAvailabilityChanged);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rectangle, int32 verticalDelta) {
        if (verticalDelta != 0)
        {
            mGutter->scroll(0, verticalDelta);
        }
        else
        {
            mGutter->update(0, rectangle.y(), mGutter->width(), rectangle.height());
        }
    });
    const auto addShortcut = [this](const QString& label, const QKeySequence& shortcut, const auto& operation) {
        QAction* action = new QAction(label, this);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetShortcut);
        connect(action, &QAction::triggered, this, operation);
        addAction(action);
    };
    addShortcut(QStringLiteral("Find"), QKeySequence::Find, [this]() { Q_EMIT FindRequested(); });
    addShortcut(QStringLiteral("Replace"), QKeySequence::Replace, [this]() { Q_EMIT ReplaceRequested(); });
    addShortcut(QStringLiteral("Go to Line"), QKeySequence(Qt::CTRL | Qt::Key_G), [this]() {
        Q_EMIT GoToLineRequested();
    });
    addShortcut(QStringLiteral("Toggle Line Comment"), QKeySequence(Qt::CTRL | Qt::Key_Slash), [this]() {
        ToggleLineComment();
    });
    addShortcut(QStringLiteral("Duplicate Lines"), QKeySequence(Qt::CTRL | Qt::Key_D), [this]() { DuplicateLines(); });
    addShortcut(QStringLiteral("Move Lines Up"), QKeySequence(Qt::ALT | Qt::Key_Up), [this]() { MoveLines(false); });
    addShortcut(QStringLiteral("Move Lines Down"), QKeySequence(Qt::ALT | Qt::Key_Down), [this]() { MoveLines(true); });
    addShortcut(QStringLiteral("Delete Lines"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K), [this]() {
        DeleteLines();
    });
    mFormatAction->setObjectName(QStringLiteral("codeEditorFormat"));
    mFormatAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    mFormatAction->setShortcutContext(Qt::WidgetShortcut);
    mFormatAction->setEnabled(false);
    mChooseFormatterAction->setEnabled(false);
    addAction(mFormatAction);
    addAction(mChooseFormatterAction);
    connect(mFormatAction, &QAction::triggered, this, &CodeEditor::FormatRequested);
    connect(mChooseFormatterAction, &QAction::triggered, this, &CodeEditor::ChooseFormatterRequested);
    updateGutterWidth();
}

void CodeEditor::SetLanguage(CodeLanguage language)
{
    mLanguage = language;
    mHighlighter->SetLanguage(language);
    mSelectionUpdate.start(0);
}

void CodeEditor::SetDocumentHistory(bool shouldUseDocumentHistory)
{
    mShouldUseDocumentHistory = shouldUseDocumentHistory;
    document()->setUndoRedoEnabled(shouldUseDocumentHistory == false);
    Q_EMIT HistoryAvailabilityChanged();
}

void CodeEditor::SetHistoryAvailability(bool canUndo, bool canRedo)
{
    if (mCanUndo != canUndo || mCanRedo != canRedo)
    {
        mCanUndo = canUndo;
        mCanRedo = canRedo;
        Q_EMIT HistoryAvailabilityChanged();
    }
}

bool CodeEditor::CanUndo() const
{
    return mShouldUseDocumentHistory == true ? mCanUndo : document()->isUndoAvailable();
}

bool CodeEditor::CanRedo() const
{
    return mShouldUseDocumentHistory == true ? mCanRedo : document()->isRedoAvailable();
}

void CodeEditor::Undo()
{
    if (isReadOnly() == true)
    {
        return;
    }
    if (mShouldUseDocumentHistory == true)
    {
        Q_EMIT UndoRequested();
    }
    else
    {
        undo();
    }
}

void CodeEditor::Redo()
{
    if (isReadOnly() == true)
    {
        return;
    }
    if (mShouldUseDocumentHistory == true)
    {
        Q_EMIT RedoRequested();
    }
    else
    {
        redo();
    }
}

void CodeEditor::SetCanFormat(bool canFormat)
{
    mFormatAction->setEnabled(canFormat);
#if !defined(Q_OS_WASM)
    mChooseFormatterAction->setEnabled(true);
#endif
}

void CodeEditor::BeginEditOperation(CodeEditKind kind)
{
    Q_EMIT EditStarted(kind);
}

void CodeEditor::EndEditOperation()
{
    Q_EMIT EditFinished();
}

void CodeEditor::contextMenuEvent(QContextMenuEvent* event)
{
    if (mShouldUseDocumentHistory == false)
    {
        QMenu* menu = createStandardContextMenu();
        menu->addSeparator();
        menu->addActions(actions());
        menu->exec(event->globalPos());
        delete menu;
        return;
    }
    QMenu menu(this);
    const auto addCommand =
        [&menu, this](const QString& label, QKeySequence::StandardKey key, bool isEnabled, const auto& operation) {
            QAction* action = menu.addAction(label);
            action->setShortcut(key);
            action->setEnabled(isEnabled);
            connect(action, &QAction::triggered, this, operation);
        };
    addCommand(QStringLiteral("Undo"), QKeySequence::Undo, CanUndo(), [this]() { Undo(); });
    addCommand(QStringLiteral("Redo"), QKeySequence::Redo, CanRedo(), [this]() { Redo(); });
    menu.addSeparator();
    const bool hasSelection = textCursor().hasSelection();
    addCommand(QStringLiteral("Cut"), QKeySequence::Cut, hasSelection == true && isReadOnly() == false, [this]() {
        const CodeEditScope operation(*this);
        cut();
    });
    addCommand(QStringLiteral("Copy"), QKeySequence::Copy, hasSelection, [this]() { copy(); });
    addCommand(QStringLiteral("Paste"), QKeySequence::Paste, canPaste() == true && isReadOnly() == false, [this]() {
        const CodeEditScope operation(*this);
        paste();
    });
    addCommand(QStringLiteral("Select All"), QKeySequence::SelectAll, true, [this]() { selectAll(); });
    menu.addSeparator();
    menu.addActions(actions());
    menu.exec(event->globalPos());
}

void CodeEditor::inputMethodEvent(QInputMethodEvent* event)
{
    const CodeEditScope operation(*this);
    QPlainTextEdit::inputMethodEvent(event);
}

void CodeEditor::insertFromMimeData(const QMimeData* source)
{
    const CodeEditScope operation(*this);
    QPlainTextEdit::insertFromMimeData(source);
}

void CodeEditor::focusOutEvent(QFocusEvent* event)
{
    QPlainTextEdit::focusOutEvent(event);
    Q_EMIT EditBoundary();
}

int32 CodeEditor::getGutterWidth() const
{
    int32 digits = 1;
    int32 lines = blockCount();
    while (lines >= DECIMAL_RADIX)
    {
        lines /= DECIMAL_RADIX;
        ++digits;
    }
    return GUTTER_MARGIN * 2 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateGutterWidth()
{
    const int32 width = getGutterWidth();
    setViewportMargins(width, 0, 0, 0);
    const QRect rectangle = contentsRect();
    mGutter->setGeometry(rectangle.left(), rectangle.top(), width, rectangle.height());
}

void CodeEditor::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    updateGutterWidth();
}

void CodeEditor::changeEvent(QEvent* event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
    {
        mHighlighter->SetPalette(palette());
        mSelectionUpdate.start(0);
    }
    else if (event->type() == QEvent::FontChange)
    {
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * INDENT_WIDTH);
        updateGutterWidth();
    }
}

void CodeEditor::paintGutter(QPaintEvent* event)
{
    QPainter painter(mGutter);
    painter.fillRect(event->rect(), palette().color(QPalette::AlternateBase));
    painter.setPen(palette().color(QPalette::Text));
    QTextBlock block = firstVisibleBlock();
    int32 top = static_cast<int32>(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() == true && top <= event->rect().bottom())
    {
        const int32 height = static_cast<int32>(blockBoundingRect(block).height());
        if (block.isVisible() == true && top + height >= event->rect().top())
        {
            painter.drawText(0,
                             top,
                             mGutter->width() - GUTTER_MARGIN,
                             fontMetrics().height(),
                             Qt::AlignRight,
                             QString::number(block.blockNumber() + 1));
        }
        block = block.next();
        top += height;
    }
}

void CodeEditor::updateSelections()
{
    QList<QTextEdit::ExtraSelection> selections;
    QTextEdit::ExtraSelection currentLine;
    currentLine.cursor = textCursor();
    currentLine.cursor.clearSelection();
    QColor lineColor = palette().color(QPalette::Highlight);
    lineColor.setAlpha(CURRENT_LINE_ALPHA);
    currentLine.format.setBackground(lineColor);
    currentLine.format.setProperty(QTextFormat::FullWidthSelection, true);
    selections.append(currentLine);

    const QTextCursor cursor = textCursor();
    const int32 caret = cursor.position();
    const QTextBlock currentBlock = cursor.block();
    const CodeBlockData* currentDataOrNull = static_cast<CodeBlockData*>(currentBlock.userData());
    if (cursor.hasSelection() == true || mLanguage == CodeLanguage::PlainText || currentDataOrNull == nullptr)
    {
        setExtraSelections(selections);
        return;
    }
    int32 bracketPosition = -1;
    QChar bracket;
    // Prefer the character before the caret, matching ordinary code-editor behavior.
    for (const int32 candidate : {caret - 1, caret})
    {
        for (const CodeBracket& entry : currentDataOrNull->Brackets)
        {
            if (currentBlock.position() + entry.Position == candidate)
            {
                bracketPosition = candidate;
                bracket = entry.Character;
                break;
            }
        }
        if (bracketPosition >= 0)
        {
            break;
        }
    }
    if (bracketPosition < 0)
    {
        setExtraSelections(selections);
        return;
    }
    const QStringView pairs(u"()[]{}");
    const int32 pairIndex = static_cast<int32>(pairs.indexOf(bracket));
    const bool isForward = pairIndex % 2 == 0;
    QVector<QChar> nesting;
    nesting.append(bracket);
    bool hasMismatch = false;
    int32 match = -1;
    QTextBlock block = currentBlock;
    while (block.isValid() == true)
    {
        const int32 distance = block.position() - currentBlock.position();
        if (distance > MAX_BRACKET_SCAN_CHARACTERS || distance < -MAX_BRACKET_SCAN_CHARACTERS)
        {
            break;
        }
        const CodeBlockData* dataOrNull = static_cast<CodeBlockData*>(block.userData());
        if (dataOrNull != nullptr)
        {
            const int32 count = static_cast<int32>(dataOrNull->Brackets.size());
            for (int32 index = 0; index < count; ++index)
            {
                const CodeBracket& entry = dataOrNull->Brackets.at(isForward == true ? index : count - 1 - index);
                const int32 position = block.position() + entry.Position;
                if ((isForward == true && position <= bracketPosition) ||
                    (isForward == false && position >= bracketPosition))
                {
                    continue;
                }
                if (position - bracketPosition > MAX_BRACKET_SCAN_CHARACTERS ||
                    bracketPosition - position > MAX_BRACKET_SCAN_CHARACTERS)
                {
                    break;
                }
                const int32 entryIndex = static_cast<int32>(pairs.indexOf(entry.Character));
                const bool isOpening = entryIndex % 2 == 0;
                if (isOpening == isForward)
                {
                    nesting.append(entry.Character);
                }
                else
                {
                    const int32 openingIndex = static_cast<int32>(pairs.indexOf(nesting.last()));
                    const QChar expected = pairs.at(isForward == true ? openingIndex + 1 : openingIndex - 1);
                    if (entry.Character != expected)
                    {
                        hasMismatch = true;
                        break;
                    }
                    nesting.removeLast();
                    if (nesting.isEmpty() == true)
                    {
                        match = position;
                        break;
                    }
                }
            }
        }
        if (match >= 0 || hasMismatch == true)
        {
            break;
        }
        block = isForward == true ? block.next() : block.previous();
    }
    if (match >= 0)
    {
        for (const int32 position : {bracketPosition, match})
        {
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(document());
            selection.cursor.setPosition(position);
            selection.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            QColor color = palette().color(QPalette::Highlight);
            color.setAlpha(MATCHED_BRACKET_ALPHA);
            selection.format.setBackground(color);
            selections.append(selection);
        }
    }
    setExtraSelections(selections);
}

QTextCursor CodeEditor::getSelectedLines() const
{
    const QTextCursor current = textCursor();
    const QTextBlock first = document()->findBlock(current.selectionStart());
    QTextBlock last = document()->findBlock(current.selectionEnd());
    if (current.hasSelection() == true && last.position() == current.selectionEnd())
    {
        last = last.previous();
    }
    QTextCursor result(first);
    result.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    return result;
}

void CodeEditor::IndentSelection(bool shouldOutdent)
{
    if (isReadOnly() == true)
    {
        return;
    }
    const CodeEditScope operation(*this);
    QTextCursor current = textCursor();
    current.beginEditBlock();
    if (current.hasSelection() == false && shouldOutdent == false)
    {
        current.insertText(QString(INDENT_WIDTH - current.positionInBlock() % INDENT_WIDTH, QLatin1Char(' ')));
    }
    else
    {
        const QTextCursor lines = getSelectedLines();
        const int32 start = lines.selectionStart();
        QTextBlock block = document()->findBlock(lines.selectionEnd());
        while (block.isValid() == true && block.position() >= start)
        {
            QTextCursor edit(block);
            if (shouldOutdent == true)
            {
                const QString text = block.text();
                int32 removed = 0;
                while (removed < INDENT_WIDTH && removed < text.size() && text.at(removed) == QLatin1Char(' '))
                {
                    ++removed;
                }
                if (removed == 0 && text.startsWith(QLatin1Char('\t')) == true)
                {
                    removed = 1;
                }
                edit.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, removed);
                edit.removeSelectedText();
            }
            else
            {
                edit.insertText(QString(INDENT_WIDTH, QLatin1Char(' ')));
            }
            block = block.previous();
        }
    }
    current.endEditBlock();
    setTextCursor(current);
}

void CodeEditor::ToggleLineComment()
{
    if (isReadOnly() == true || mLanguage == CodeLanguage::PlainText)
    {
        return;
    }
    const CodeEditScope operation(*this);
    const QTextCursor lines = getSelectedLines();
    const int32 start = lines.selectionStart();
    const int32 end = lines.selectionEnd();
    bool shouldRemove = true;
    for (QTextBlock block = document()->findBlock(start); block.isValid() == true && block.position() <= end;
         block = block.next())
    {
        const QString text = block.text();
        const int32 indentation = GetIndentLength(text);
        if (indentation < text.size() && QStringView(text).sliced(indentation).startsWith(u"//") == false)
        {
            shouldRemove = false;
            break;
        }
    }
    QTextCursor current = textCursor();
    current.beginEditBlock();
    QTextBlock block = document()->findBlock(end);
    while (block.isValid() == true && block.position() >= start)
    {
        const QString text = block.text();
        const int32 indentation = GetIndentLength(text);
        if (indentation < text.size())
        {
            QTextCursor edit(block);
            edit.setPosition(block.position() + indentation);
            if (shouldRemove == true)
            {
                int32 count = 2;
                if (text.size() > indentation + count && text.at(indentation + count) == QLatin1Char(' '))
                {
                    ++count;
                }
                edit.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, count);
                edit.removeSelectedText();
            }
            else
            {
                edit.insertText(QStringLiteral("// "));
            }
        }
        block = block.previous();
    }
    current.endEditBlock();
    setTextCursor(current);
}

void CodeEditor::DuplicateLines()
{
    if (isReadOnly() == true)
    {
        return;
    }
    const CodeEditScope operation(*this);
    QTextCursor lines = getSelectedLines();
    QString text = lines.selectedText();
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    lines.setPosition(lines.selectionEnd());
    lines.beginEditBlock();
    lines.insertText(QLatin1Char('\n') + text);
    lines.endEditBlock();
    setTextCursor(lines);
}

void CodeEditor::MoveLines(bool shouldMoveDown)
{
    if (isReadOnly() == true)
    {
        return;
    }
    const CodeEditScope operation(*this);
    QTextCursor lines = getSelectedLines();
    const int32 start = lines.selectionStart();
    const int32 end = lines.selectionEnd();
    const QTextBlock adjacent =
        shouldMoveDown == true ? document()->findBlock(end).next() : document()->findBlock(start).previous();
    if (adjacent.isValid() == false)
    {
        return;
    }
    QString selected = lines.selectedText();
    selected.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    const QString adjacentText = adjacent.text();
    const int32 newStart =
        shouldMoveDown == true ? start + static_cast<int32>(adjacentText.size()) + 1 : adjacent.position();
    const int32 rangeStart = shouldMoveDown == true ? start : adjacent.position();
    const int32 rangeEnd = shouldMoveDown == true ? adjacent.position() + adjacent.length() - 1 : end;
    lines.setPosition(rangeStart);
    lines.setPosition(rangeEnd, QTextCursor::KeepAnchor);
    lines.beginEditBlock();
    lines.insertText(shouldMoveDown == true ? adjacentText + QLatin1Char('\n') + selected
                                            : selected + QLatin1Char('\n') + adjacentText);
    lines.endEditBlock();
    lines.setPosition(newStart);
    lines.setPosition(newStart + static_cast<int32>(selected.size()), QTextCursor::KeepAnchor);
    setTextCursor(lines);
}

void CodeEditor::DeleteLines()
{
    if (isReadOnly() == true)
    {
        return;
    }
    const CodeEditScope operation(*this);
    QTextCursor lines = getSelectedLines();
    const QTextBlock last = document()->findBlock(lines.selectionEnd());
    if (last.next().isValid() == true)
    {
        lines.setPosition(last.next().position(), QTextCursor::KeepAnchor);
    }
    else if (lines.selectionStart() > 0)
    {
        const int32 end = lines.selectionEnd();
        lines.setPosition(lines.selectionStart() - 1);
        lines.setPosition(end, QTextCursor::KeepAnchor);
    }
    lines.beginEditBlock();
    lines.removeSelectedText();
    lines.endEditBlock();
    setTextCursor(lines);
}

void CodeEditor::GoToLine(int32 line)
{
    const int32 clamped = qBound(1, line, blockCount());
    setTextCursor(QTextCursor(document()->findBlockByNumber(clamped - 1)));
    ensureCursorVisible();
    setFocus();
}

void CodeEditor::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::Undo) == true)
    {
        Undo();
        return;
    }
    if (event->matches(QKeySequence::Redo) == true)
    {
        Redo();
        return;
    }
    CodeEditKind kind = CodeEditKind::Command;
    if (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::ShiftModifier)
    {
        if (event->key() == Qt::Key_Backspace)
        {
            kind = CodeEditKind::Backspace;
        }
        else if (event->key() == Qt::Key_Delete)
        {
            kind = CodeEditKind::Delete;
        }
        else if (event->text().isEmpty() == false && event->text().at(0).isPrint() == true)
        {
            kind = CodeEditKind::Typing;
        }
    }
    const CodeEditScope operation(*this, kind);
    if (isReadOnly() == false && event->modifiers() == Qt::NoModifier && event->key() == Qt::Key_Tab)
    {
        IndentSelection();
        return;
    }
    if (isReadOnly() == false && event->key() == Qt::Key_Backtab &&
        (event->modifiers() == Qt::ShiftModifier || event->modifiers() == Qt::NoModifier))
    {
        IndentSelection(true);
        return;
    }
    if (isReadOnly() == false && event->modifiers() == Qt::NoModifier &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter))
    {
        QTextCursor cursor = textCursor();
        cursor.beginEditBlock();
        cursor.removeSelectedText();
        const QString prefix = cursor.block().text().left(cursor.positionInBlock());
        QString indentation = prefix.left(GetIndentLength(prefix));
        if (mLanguage != CodeLanguage::PlainText && prefix.trimmed().endsWith(QLatin1Char('{')) == true)
        {
            indentation += QString(INDENT_WIDTH, QLatin1Char(' '));
        }
        cursor.insertText(QLatin1Char('\n') + indentation);
        cursor.endEditBlock();
        setTextCursor(cursor);
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}
} // namespace ludus::editor
