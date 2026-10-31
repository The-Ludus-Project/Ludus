#include "internal/code_editor.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_edit_scope.h"
#include "internal/code_navigation.h"
#include "internal/code_syntax_highlighter.h"

#include <QAction>
#include <QBrush>
#include <QColor>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QStringList>
#include <QStringView>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolTip>
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
#if defined(Q_OS_MACOS)
constexpr Qt::KeyboardModifier ASSISTANCE_MODIFIER = Qt::MetaModifier; // physical Control on macOS
#else
constexpr Qt::KeyboardModifier ASSISTANCE_MODIFIER = Qt::ControlModifier;
#endif
constexpr int32 COMPLETION_VISIBLE_ROWS = 9;
constexpr int32 COMPLETION_WIDTH = 420;
constexpr int32 SIGNATURE_WIDTH = 680;
constexpr int32 INDENT_GUIDE_ALPHA = 48;
constexpr int32 GUTTER_MARGIN = 6;
constexpr int32 DECIMAL_RADIX = 10;
constexpr int32 MAX_BRACKET_SCAN_CHARACTERS = 32768;
constexpr int32 CURRENT_LINE_ALPHA = 24;
constexpr int32 MATCHED_BRACKET_ALPHA = 96;
constexpr uint32 LIGHT_ERROR = 0xC4314B;
constexpr uint32 DARK_ERROR = 0xFF7B8A;
constexpr uint32 LIGHT_WARNING = 0x9A6700;
constexpr uint32 DARK_WARNING = 0xE3B341;
/// Marker diameter as a divisor of the line height.
constexpr int32 DIAGNOSTIC_MARKER_DIVISOR = 2;
/// An out-of-date report keeps its squiggle, drawn this much more faintly (alpha, 0-255).
constexpr int32 STALE_UNDERLINE_ALPHA = 140;
/// Message callout under the caret's line: spacing, shape and how much of the message it shows.
constexpr int32 CALLOUT_MARGIN = 6;
constexpr int32 CALLOUT_PADDING = 6;
constexpr int32 CALLOUT_ARROW_SIZE = 5;
constexpr int32 CALLOUT_CORNER_RADIUS = 4;
constexpr int32 CALLOUT_MAX_LINES = 3;
constexpr int32 CALLOUT_FILL_PERCENT = 16;
constexpr int32 CALLOUT_BORDER_ALPHA = 170;
constexpr int32 PERCENT = 100;

int32 GetIndentLength(const QString& text)
{
    int32 length = 0;
    while (length < text.size() && (text.at(length) == QLatin1Char(' ') || text.at(length) == QLatin1Char('\t')))
    {
        ++length;
    }
    return length;
}

/// UTF-16 length of the first scalars of text; an unpaired surrogate counts as one scalar.
int32 GetScalarOffset(const QString& text, int32 scalars)
{
    int32 offset = 0;
    for (int32 count = 0; count < scalars && offset < text.size(); ++count)
    {
        const bool isPair = text.at(offset).isHighSurrogate() == true && offset + 1 < text.size() &&
                            text.at(offset + 1).isLowSurrogate() == true;
        offset += isPair == true ? 2 : 1;
    }
    return offset;
}

QString MakeToolTipHtml(const QString& text)
{
    // Messages quote shader code such as vector<float, 3>; never let Qt guess rich text.
    return QStringLiteral("<p style=\"white-space:pre-wrap\">%1</p>").arg(text.toHtmlEscaped());
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
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::ToolTip)
        {
            const QHelpEvent* help = static_cast<QHelpEvent*>(event);
            if (mEditor->showGutterToolTip(*help) == false)
            {
                QToolTip::hideText();
                event->ignore();
            }
            return true;
        }
        return QWidget::event(event);
    }
    CodeEditor* mEditor;
};

CodeEditor::CodeEditor(QWidget* parentOrNull)
    : QPlainTextEdit(parentOrNull), mGutter(new CodeEditorGutter(this)),
      mHighlighter(new CodeSyntaxHighlighter(document())),
      mFormatAction(new QAction(QStringLiteral("Format Document"), this)),
      mChooseFormatterAction(new QAction(QStringLiteral("Choose Formatter..."), this))
{
    mCompletionList = new QListWidget(this);
    mCompletionList->setWindowFlags(Qt::ToolTip);
    mCompletionList->setAttribute(Qt::WA_ShowWithoutActivating);
    mCompletionList->setFocusPolicy(Qt::NoFocus);
    mCompletionList->setObjectName(QStringLiteral("codeCompletionList"));
    mCompletionList->setAccessibleName(QStringLiteral("Shader completion suggestions"));
    connect(mCompletionList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        Q_EMIT CompletionChosen(mCompletionList->row(item));
    });
    mSignature = new QLabel(this);
    mSignature->setWindowFlags(Qt::ToolTip);
    mSignature->setAttribute(Qt::WA_ShowWithoutActivating);
    mSignature->setObjectName(QStringLiteral("codeSignatureHelp"));
    mSignature->setAccessibleName(QStringLiteral("Shader signature help"));
    mSignature->setMargin(INDENT_WIDTH);
    mSignature->setTextFormat(Qt::RichText);
    mSignature->setWordWrap(true);
    mSignature->setMaximumWidth(SIGNATURE_WIDTH);
    mCompletionList->hide();
    mSignature->hide();

    QAction* complete = new QAction(QStringLiteral("Complete Shader Symbol (Ctrl+Space)"), this);
    connect(complete, &QAction::triggered, this, [this]() { Q_EMIT AssistanceRequested(false); });
    complete->setObjectName(QStringLiteral("codeEditorComplete"));
    complete->setEnabled(false);
    addAction(complete);
    QAction* signature = new QAction(QStringLiteral("Shader Signature Help (Ctrl+Shift+Space)"), this);
    connect(signature, &QAction::triggered, this, [this]() { Q_EMIT AssistanceRequested(true); });
    signature->setObjectName(QStringLiteral("codeEditorSignature"));
    signature->setEnabled(false);
    addAction(signature);
    QFont textFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    textFont.setStyleHint(QFont::Monospace);
    textFont.setFixedPitch(true);
    // Some hosts provide a proportional fallback for the system's fixed-font alias.
    if (QFontInfo(textFont).fixedPitch() == false)
    {
        for (const QString& family : QFontDatabase::families())
        {
            if (QFontDatabase::isFixedPitch(family) == true)
            {
                textFont.setFamily(family);
                break;
            }
        }
    }
    setFont(textFont);
    QSettings preferences;
    SetFontPointSize(preferences.value(QStringLiteral("codeEditor/fontPoints"), DEFAULT_FONT_POINTS).toInt());
    SetWordWrap(preferences.value(QStringLiteral("codeEditor/wordWrap"), false).toBool());
    SetIndentGuidesVisible(preferences.value(QStringLiteral("codeEditor/indentGuides"), true).toBool());
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * INDENT_WIDTH);
    mHighlighter->SetPalette(palette());
    mSelectionUpdate.setSingleShot(true);
    connect(&mSelectionUpdate, &QTimer::timeout, this, &CodeEditor::updateSelections);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]() { mSelectionUpdate.start(0); });
    connect(document(), &QTextDocument::contentsChanged, this, [this]() { mSelectionUpdate.start(0); });
    connect(document(), &QTextDocument::contentsChange, this, &CodeEditor::markDiagnosticsOutOfDate);
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
    addShortcut(QStringLiteral("Increase Text Size"), QKeySequence::ZoomIn, [this]() {
        SetFontPointSize(font().pointSize() + 1);
    });
    addShortcut(QStringLiteral("Decrease Text Size"), QKeySequence::ZoomOut, [this]() {
        SetFontPointSize(font().pointSize() - 1);
    });
    addShortcut(QStringLiteral("Reset Text Size"), QKeySequence(Qt::CTRL | Qt::Key_0), [this]() {
        SetFontPointSize(DEFAULT_FONT_POINTS);
    });
    addShortcut(QStringLiteral("Next Diagnostic"), QKeySequence(Qt::Key_F8), [this]() { GoToDiagnostic(); });
    addShortcut(QStringLiteral("Previous Diagnostic"), QKeySequence(Qt::SHIFT | Qt::Key_F8), [this]() {
        GoToDiagnostic(true);
    });
    QAction* pairs = new QAction(QStringLiteral("Auto-close Brackets"), this);
    pairs->setCheckable(true);
    mShouldPairBrackets = preferences.value(QStringLiteral("codeEditor/autoCloseBrackets"), true).toBool();
    pairs->setChecked(mShouldPairBrackets);
    connect(pairs, &QAction::toggled, this, [this](bool shouldPair) {
        mShouldPairBrackets = shouldPair;
        QSettings().setValue(QStringLiteral("codeEditor/autoCloseBrackets"), shouldPair);
    });
    addAction(pairs);
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
    mNavigation = new CodeNavigation(*this);
}

void CodeEditor::SetFontPointSize(int32 points)
{
    QFont textFont = font();
    textFont.setPointSize(qBound(MINIMUM_FONT_POINTS, points, MAXIMUM_FONT_POINTS));
    setFont(textFont);
    QSettings().setValue(QStringLiteral("codeEditor/fontPoints"), textFont.pointSize());
    Q_EMIT PresentationChanged();
}

void CodeEditor::SetWordWrap(bool shouldWrap)
{
    setLineWrapMode(shouldWrap == true ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    QSettings().setValue(QStringLiteral("codeEditor/wordWrap"), shouldWrap);
    Q_EMIT PresentationChanged();
}

void CodeEditor::SetIndentGuidesVisible(bool isVisible)
{
    mShouldShowIndentGuides = isVisible;
    setProperty("indentGuidesVisible", isVisible);
    QSettings().setValue(QStringLiteral("codeEditor/indentGuides"), isVisible);
    viewport()->update();
    Q_EMIT PresentationChanged();
}

void CodeEditor::GoToDiagnostic(bool shouldGoBackward)
{
    if (mDiagnostics.isEmpty() == true)
    {
        return;
    }
    const int32 position = textCursor().selectionStart();
    int32 selected = -1;
    int32 boundary = 0;
    for (int32 index = 0; index < mDiagnostics.size(); ++index)
    {
        const int32 candidate = mDiagnostics.at(index).Range.selectionStart();
        const int32 extreme = mDiagnostics.at(boundary).Range.selectionStart();
        if ((shouldGoBackward == true && candidate > extreme) || (shouldGoBackward == false && candidate < extreme))
        {
            boundary = index;
        }
        if ((shouldGoBackward == true && candidate < position) || (shouldGoBackward == false && candidate > position))
        {
            if (selected < 0 ||
                (shouldGoBackward == true && candidate > mDiagnostics.at(selected).Range.selectionStart()) ||
                (shouldGoBackward == false && candidate < mDiagnostics.at(selected).Range.selectionStart()))
            {
                selected = index;
            }
        }
    }
    mNavigation->RememberCurrent();
    setTextCursor(mDiagnostics.at(selected < 0 ? boundary : selected).Range);
    ensureCursorVisible();
    setFocus();
}

void CodeEditor::paintEvent(QPaintEvent* event)
{
    QPlainTextEdit::paintEvent(event);
    if (mShouldShowIndentGuides == true)
    {
        paintIndentGuides(event);
    }
    paintDiagnosticCallout();
}

void CodeEditor::paintIndentGuides(const QPaintEvent* event)
{
    QPainter painter(viewport());
    QColor color = palette().color(QPalette::Text);
    color.setAlpha(INDENT_GUIDE_ALPHA);
    painter.setPen(color);
    const int32 spaceWidth = fontMetrics().horizontalAdvance(QLatin1Char(' '));
    QTextBlock block = firstVisibleBlock();
    while (block.isValid() == true)
    {
        const QRectF rectangle = blockBoundingGeometry(block).translated(contentOffset());
        if (rectangle.top() > event->rect().bottom())
        {
            break;
        }
        if (block.isVisible() == true && rectangle.bottom() >= event->rect().top())
        {
            const QString text = block.text();
            int32 columns = 0;
            for (const QChar character : text)
            {
                if (character == QLatin1Char(' '))
                {
                    ++columns;
                }
                else if (character == QLatin1Char('\t'))
                {
                    columns += INDENT_WIDTH - columns % INDENT_WIDTH;
                }
                else
                {
                    break;
                }
            }
            const int32 left = cursorRect(QTextCursor(block)).left();
            for (int32 column = INDENT_WIDTH; column <= columns; column += INDENT_WIDTH)
            {
                const int32 x = left + (column - INDENT_WIDTH) * spaceWidth;
                painter.drawLine(QPointF(x, rectangle.top()), QPointF(x, rectangle.top() + fontMetrics().height()));
            }
        }
        block = block.next();
    }
}

/// The message of the diagnostics on the caret's line, under that line and pointing at the error.
/// Painted over the lines below instead of shifting them, so the text never jumps; it goes away
/// when the caret moves off the line or the line is edited.
void CodeEditor::paintDiagnosticCallout()
{
    if (mDiagnostics.isEmpty() == true || mHasEditedSinceReport == true)
    {
        return;
    }
    const QTextBlock block = textCursor().block();
    QStringList messages;
    QTextCursor anchor;
    CodeDiagnosticSeverity severity = CodeDiagnosticSeverity::Warning;
    for (const DiagnosticMarker& marker : mDiagnostics)
    {
        if (document()->findBlock(marker.Range.selectionStart()) != block || marker.Message.isEmpty() == true)
        {
            continue;
        }
        if (messages.contains(marker.Message) == false)
        {
            messages.append(marker.Message);
        }
        if (anchor.isNull() == true)
        {
            anchor = marker.Range;
            anchor.setPosition(marker.Range.selectionStart());
        }
        severity = marker.Severity == CodeDiagnosticSeverity::Error ? CodeDiagnosticSeverity::Error : severity;
    }
    if (messages.isEmpty() == true)
    {
        return;
    }
    const QFontMetrics metrics(font());
    const int32 viewportWidth = viewport()->width();
    const int32 textWidthLimit = viewportWidth - 2 * (CALLOUT_MARGIN + CALLOUT_PADDING);
    if (textWidthLimit <= 0)
    {
        return;
    }
    const QString text = messages.join(QLatin1Char('\n'));
    QRect textRect = metrics.boundingRect(QRect(0, 0, textWidthLimit, 0), Qt::TextWordWrap | Qt::AlignLeft, text);
    textRect.setHeight(qMin(textRect.height(), metrics.lineSpacing() * CALLOUT_MAX_LINES));
    const int32 boxWidth = textRect.width() + 2 * CALLOUT_PADDING;
    const int32 boxHeight = textRect.height() + 2 * CALLOUT_PADDING;
    const QRectF blockRect = blockBoundingGeometry(block).translated(contentOffset());
    const int32 errorX = cursorRect(anchor).left();
    const int32 boxLeft = qBound(CALLOUT_MARGIN,
                                 errorX - CALLOUT_PADDING,
                                 qMax(viewportWidth - boxWidth - CALLOUT_MARGIN, CALLOUT_MARGIN));
    const int32 boxTop = static_cast<int32>(blockRect.bottom()) + CALLOUT_ARROW_SIZE;
    const QRect box(boxLeft, boxTop, boxWidth, boxHeight);
    const QColor accent = getDiagnosticColor(severity);
    QColor fill = palette().color(QPalette::Base);
    fill = QColor::fromRgb(
        (fill.red() * (PERCENT - CALLOUT_FILL_PERCENT) + accent.red() * CALLOUT_FILL_PERCENT) / PERCENT,
        (fill.green() * (PERCENT - CALLOUT_FILL_PERCENT) + accent.green() * CALLOUT_FILL_PERCENT) / PERCENT,
        (fill.blue() * (PERCENT - CALLOUT_FILL_PERCENT) + accent.blue() * CALLOUT_FILL_PERCENT) / PERCENT);
    QColor border = accent;
    border.setAlpha(CALLOUT_BORDER_ALPHA);
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(border, 1, mAreDiagnosticsCurrent == true ? Qt::SolidLine : Qt::DashLine));
    painter.setBrush(fill);
    QPainterPath shape;
    shape.addRoundedRect(QRectF(box), CALLOUT_CORNER_RADIUS, CALLOUT_CORNER_RADIUS);
    const int32 arrowX = qBound(box.left() + CALLOUT_CORNER_RADIUS + CALLOUT_ARROW_SIZE,
                                errorX,
                                box.right() - CALLOUT_CORNER_RADIUS - CALLOUT_ARROW_SIZE);
    QPainterPath arrow;
    arrow.moveTo(arrowX - CALLOUT_ARROW_SIZE, box.top());
    arrow.lineTo(arrowX, box.top() - CALLOUT_ARROW_SIZE);
    arrow.lineTo(arrowX + CALLOUT_ARROW_SIZE, box.top());
    arrow.closeSubpath();
    painter.drawPath(shape.united(arrow));
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(box.adjusted(CALLOUT_PADDING, CALLOUT_PADDING, -CALLOUT_PADDING, -CALLOUT_PADDING),
                     Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
                     text);
}

void CodeEditor::SetLanguage(CodeLanguage language)
{
    mLanguage = language;
    for (QAction* action : actions())
    {
        if (action->objectName() == QStringLiteral("codeEditorComplete") ||
            action->objectName() == QStringLiteral("codeEditorSignature"))
        {
            action->setEnabled(language == CodeLanguage::Slang);
        }
    }
    mHighlighter->SetLanguage(language);
    mNavigation->RefreshActions();
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
        menu->setToolTipsVisible(true);
        menu->addSeparator();
        menu->addActions(actions());
        menu->exec(event->globalPos());
        delete menu;
        return;
    }
    QMenu menu(this);
    menu.setToolTipsVisible(true);
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
    if (mNavigation->IsChoosing() == false)
    {
        Q_EMIT AssistanceDismissed();
    }
    HideAssistance();
    QPlainTextEdit::focusOutEvent(event);
    Q_EMIT EditBoundary();
}

void CodeEditor::SetDiagnostics(const QList<CodeDiagnostic>& diagnostics, bool isCurrent)
{
    mDiagnostics.clear();
    for (const CodeDiagnostic& diagnostic : diagnostics)
    {
        const QTextBlock block = document()->findBlockByNumber(diagnostic.Line - 1);
        if (diagnostic.Line < 1 || block.isValid() == false)
        {
            continue;
        }
        const QString text = block.text();
        DiagnosticMarker marker =
        {
            .Range = QTextCursor(document()),
            .Severity = diagnostic.Severity,
            .Message = diagnostic.Message,
            .IsLineOnly = diagnostic.Column < 1,
        };
        int32 start = GetIndentLength(text);
        int32 end = static_cast<int32>(text.size());
        if (marker.IsLineOnly == false)
        {
            start = GetScalarOffset(text, diagnostic.Column - 1);
            end = start + GetScalarOffset(text.sliced(start), qMax(diagnostic.Length, 0));
        }
        marker.Range.setPosition(block.position() + start);
        marker.Range.setPosition(block.position() + end, QTextCursor::KeepAnchor);
        mDiagnostics.append(marker);
    }
    mAreDiagnosticsCurrent = isCurrent;
    mHasEditedSinceReport = false;
    Q_EMIT DiagnosticsChanged();
    // Snapshot for telling highlighter reformatting apart from same-length edits.
    mDiagnosticText = mDiagnostics.isEmpty() == false ? document()->toRawText() : QString();
    mGutter->update();
    mSelectionUpdate.start(0);
}

QTextCursor CodeEditor::GetDiagnosticRange(int32 index) const
{
    return index >= 0 && index < mDiagnostics.size() ? mDiagnostics.at(index).Range : QTextCursor();
}

QString CodeEditor::DescribeDiagnosticsAt(int32 position) const
{
    return describeDiagnostics(position, -1);
}

QString CodeEditor::DescribeLineDiagnostics(int32 blockNumber) const
{
    return describeDiagnostics(-1, blockNumber);
}

QString CodeEditor::describeDiagnostics(int32 position, int32 blockNumber) const
{
    QStringList lines;
    const int32 positionBlock = position >= 0 ? document()->findBlock(position).blockNumber() : -1;
    for (const DiagnosticMarker& marker : mDiagnostics)
    {
        const int32 start = marker.Range.selectionStart();
        const int32 end = marker.Range.selectionEnd();
        const int32 markerBlock = document()->findBlock(start).blockNumber();
        // A point marker is underlined on its neighbouring character, so hover covers both.
        const bool isCovered = marker.IsLineOnly == true ? markerBlock == positionBlock
                                                         : position >= qMax(start - (start == end ? 1 : 0), 0) &&
                                                               position <= end + (start == end ? 1 : 0);
        if ((position >= 0 && isCovered == true) || (blockNumber >= 0 && markerBlock == blockNumber))
        {
            lines.append((marker.Severity == CodeDiagnosticSeverity::Error ? QStringLiteral("Error: ")
                                                                           : QStringLiteral("Warning: ")) +
                         marker.Message);
        }
    }
    if (lines.isEmpty() == false && mAreDiagnosticsCurrent == false)
    {
        lines.prepend(QStringLiteral("Out of date: reported for an earlier version of this text. Build again."));
    }
    return lines.join(QLatin1Char('\n'));
}

void CodeEditor::markDiagnosticsOutOfDate(int32 position, int32 removed, int32 added)
{
    if (mDiagnostics.isEmpty() == true)
    {
        return;
    }
    // Replacing the whole text (a different file) leaves no position to anchor to.
    // Same-length reports include the highlighter's reformatting of a one-line text.
    const int32 previousCount = document()->characterCount() - added + removed;
    if (position == 0 && removed != added && removed + 1 >= previousCount)
    {
        SetDiagnostics({}, true);
        return;
    }
    if (mHasEditedSinceReport == true)
    {
        return;
    }
    // The highlighter reports same-length format changes; only changed characters are edits.
    bool isChanged = removed != added;
    for (int32 i = position; isChanged == false && i < position + added && i < mDiagnosticText.size(); ++i)
    {
        isChanged = document()->characterAt(i) != mDiagnosticText.at(i);
    }
    if (isChanged == true)
    {
        // A report that was already out of date stays so; either way the callout makes way for typing.
        const bool wasCurrent = mAreDiagnosticsCurrent;
        mHasEditedSinceReport = true;
        mAreDiagnosticsCurrent = false;
        if (wasCurrent == true)
        {
            Q_EMIT DiagnosticsChanged();
        }
        mDiagnosticText.clear();
        mGutter->update();
        mSelectionUpdate.start(0);
    }
}

QColor CodeEditor::getDiagnosticColor(CodeDiagnosticSeverity severity) const
{
    const bool isLight = IsLightCodePalette(palette());
    if (severity == CodeDiagnosticSeverity::Error)
    {
        return QColor::fromRgb(isLight == true ? LIGHT_ERROR : DARK_ERROR);
    }
    return QColor::fromRgb(isLight == true ? LIGHT_WARNING : DARK_WARNING);
}

void CodeEditor::appendDiagnosticSelections(QList<QTextEdit::ExtraSelection>& inoutSelections) const
{
    // Errors are appended last so they draw over warnings on shared characters.
    for (const CodeDiagnosticSeverity severity : {CodeDiagnosticSeverity::Warning, CodeDiagnosticSeverity::Error})
    {
        for (const DiagnosticMarker& marker : mDiagnostics)
        {
            if (marker.Severity != severity)
            {
                continue;
            }
            QTextEdit::ExtraSelection selection;
            selection.cursor = marker.Range;
            if (selection.cursor.hasSelection() == false)
            {
                // Underline the character after a point, or before it at the end of a line.
                if (selection.cursor.atBlockEnd() == true && selection.cursor.atBlockStart() == false)
                {
                    selection.cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
                }
                else if (selection.cursor.atBlockEnd() == false)
                {
                    selection.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                }
            }
            if (selection.cursor.hasSelection() == false)
            {
                continue;
            }
            // An out-of-date report keeps its squiggle, fainter, so the error stays findable.
            QColor underline = getDiagnosticColor(severity);
            if (mAreDiagnosticsCurrent == false)
            {
                underline.setAlpha(STALE_UNDERLINE_ALPHA);
            }
            selection.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            selection.format.setUnderlineColor(underline);
            inoutSelections.append(selection);
        }
    }
}

bool CodeEditor::showGutterToolTip(const QHelpEvent& help)
{
    const QPoint position = help.pos();
    QTextBlock block = firstVisibleBlock();
    int32 top = static_cast<int32>(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() == true && top <= position.y())
    {
        const int32 height = static_cast<int32>(blockBoundingRect(block).height());
        if (block.isVisible() == true && position.y() < top + height)
        {
            const QString text = DescribeLineDiagnostics(block.blockNumber());
            if (text.isEmpty() == true)
            {
                return false;
            }
            QToolTip::showText(help.globalPos(), MakeToolTipHtml(text), mGutter);
            return true;
        }
        block = block.next();
        top += height;
    }
    return false;
}

bool CodeEditor::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::ToolTip)
    {
        const QHelpEvent* help = static_cast<QHelpEvent*>(event);
        const QString text = DescribeDiagnosticsAt(cursorForPosition(help->pos()).position());
        if (text.isEmpty() == true)
        {
            QToolTip::hideText();
            event->ignore();
        }
        else
        {
            QToolTip::showText(help->globalPos(), MakeToolTipHtml(text), viewport());
        }
        return true;
    }
    return QPlainTextEdit::viewportEvent(event);
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
    // Diagnostic marker space is reserved permanently, so reports never shift the text.
    const int32 marker = fontMetrics().height() / DIAGNOSTIC_MARKER_DIVISOR + GUTTER_MARGIN;
    return GUTTER_MARGIN * 2 + marker + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
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
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(event->rect(), palette().color(QPalette::AlternateBase));
    const int32 diameter = fontMetrics().height() / DIAGNOSTIC_MARKER_DIVISOR;
    QTextBlock block = firstVisibleBlock();
    int32 top = static_cast<int32>(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() == true && top <= event->rect().bottom())
    {
        const int32 height = static_cast<int32>(blockBoundingRect(block).height());
        if (block.isVisible() == true && top + height >= event->rect().top())
        {
            bool hasMarker = false;
            CodeDiagnosticSeverity severity = CodeDiagnosticSeverity::Warning;
            for (const DiagnosticMarker& marker : mDiagnostics)
            {
                if (document()->findBlock(marker.Range.selectionStart()) == block)
                {
                    hasMarker = true;
                    if (marker.Severity == CodeDiagnosticSeverity::Error)
                    {
                        severity = CodeDiagnosticSeverity::Error;
                    }
                }
            }
            if (hasMarker == true)
            {
                // Out-of-date reports draw hollow, matching their dotted underline.
                const QColor color = getDiagnosticColor(severity);
                painter.setPen(mAreDiagnosticsCurrent == true ? QPen(Qt::NoPen) : QPen(color));
                painter.setBrush(mAreDiagnosticsCurrent == true ? QBrush(color) : QBrush(Qt::NoBrush));
                painter.drawEllipse(
                    QRectF(GUTTER_MARGIN, top + (fontMetrics().height() - diameter) / 2.0, diameter, diameter));
            }
            painter.setPen(palette().color(QPalette::Text));
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
    appendDiagnosticSelections(selections);

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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): line then column is the universal order.
void CodeEditor::GoToLine(int32 line, int32 column)
{
    const int32 clamped = qBound(1, line, blockCount());
    const QTextBlock block = document()->findBlockByNumber(clamped - 1);
    QTextCursor cursor(block);
    if (column > 1)
    {
        cursor.setPosition(block.position() + GetScalarOffset(block.text(), column - 1));
    }
    if (cursor != textCursor())
    {
        mNavigation->RememberCurrent();
    }
    setTextCursor(cursor);
    ensureCursorVisible();
    setFocus();
}

void CodeEditor::keyPressEvent(QKeyEvent* event)
{
    const int32 revision = document()->revision();
    handleKeyPress(event);
    if (document()->revision() != revision && mLanguage == CodeLanguage::Slang && event->text().size() == 1 &&
        (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) == Qt::NoModifier)
    {
        const QChar typed = event->text().at(0);
        const bool isSignature = typed == QLatin1Char('(') || typed == QLatin1Char(',');
        if (isSignature == true || typed == QLatin1Char('.'))
        {
            Q_EMIT AssistanceRequested(isSignature);
        }
    }
}

void CodeEditor::handleKeyPress(QKeyEvent* event)
{
    const bool hasPopup = mCompletionList->isVisible() == true || mSignature->isVisible() == true;
    if (event->key() == Qt::Key_Escape)
    {
        Q_EMIT AssistanceDismissed();
    }
    if (event->key() == Qt::Key_Escape && hasPopup == true)
    {
        HideAssistance();
        event->accept();
        return;
    }
    if (mLanguage == CodeLanguage::Slang && isReadOnly() == false && event->key() == Qt::Key_Space &&
        (event->modifiers() == ASSISTANCE_MODIFIER || event->modifiers() == (ASSISTANCE_MODIFIER | Qt::ShiftModifier)))
    {
        Q_EMIT AssistanceRequested(event->modifiers().testFlag(Qt::ShiftModifier));
        event->accept();
        return;
    }
    if (mSignature->isVisible() == true && event->modifiers() == Qt::AltModifier &&
        (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown))
    {
        Q_EMIT SignatureOverloadRequested(event->key() == Qt::Key_PageUp ? -1 : 1);
        event->accept();
        return;
    }
    if (mCompletionList->isVisible() == true)
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Tab)
        {
            Q_EMIT CompletionChosen(mCompletionList->currentRow());
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)
        {
            const int32 direction = event->key() == Qt::Key_Up ? -1 : 1;
            mCompletionList->setCurrentRow((mCompletionList->currentRow() + direction + mCompletionList->count()) %
                                           mCompletionList->count());
            event->accept();
            return;
        }
        mCompletionList->hide();
    }

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
    if (isReadOnly() == false && mShouldPairBrackets == true && mLanguage == CodeLanguage::Slang &&
        (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::ShiftModifier) &&
        textCursor().hasSelection() == false &&
        ((event->text().size() == 1 && QStringView(u"()[]{}").contains(event->text().at(0)) == true) ||
         event->key() == Qt::Key_Backspace))
    {
        QTextCursor cursor = textCursor();
        const int32 position = cursor.positionInBlock();
        // Refresh only this block so a just-typed comment/string is respected before Qt's queued pass.
        mHighlighter->rehighlightBlock(cursor.block());
        const CodeBlockData* dataOrNull = static_cast<CodeBlockData*>(cursor.block().userData());
        bool canPair = dataOrNull != nullptr;
        if (dataOrNull != nullptr)
        {
            for (const CodeExcludedRange& range : dataOrNull->ExcludedRanges)
            {
                if (position > range.Start &&
                    (position < range.End || (position == range.End && range.IsOpenEnded == true)))
                {
                    canPair = false;
                }
            }
        }
        const QStringView pairs(u"()[]{}");
        const QString typed = event->text();
        const int32 pairIndex = typed.size() == 1 ? static_cast<int32>(pairs.indexOf(typed.at(0))) : -1;
        const QChar next = document()->characterAt(cursor.position());
        if (canPair == true && pairIndex >= 0)
        {
            if (pairIndex % 2 == 1 && next == typed.at(0))
            {
                cursor.movePosition(QTextCursor::NextCharacter);
                setTextCursor(cursor);
                Q_EMIT EditBoundary();
                return;
            }
            if (pairIndex % 2 == 0 &&
                (next.isSpace() == true || next.isNull() == true || QStringView(u")]}").contains(next) == true))
            {
                const CodeEditScope operation(*this);
                cursor.beginEditBlock();
                cursor.insertText(typed + pairs.at(pairIndex + 1));
                cursor.movePosition(QTextCursor::PreviousCharacter);
                cursor.endEditBlock();
                setTextCursor(cursor);
                return;
            }
        }
        const int32 previousPair = static_cast<int32>(pairs.indexOf(document()->characterAt(cursor.position() - 1)));
        if (canPair == true && event->key() == Qt::Key_Backspace && previousPair >= 0 && previousPair % 2 == 0 &&
            next == pairs.at(previousPair + 1))
        {
            const CodeEditScope operation(*this);
            cursor.beginEditBlock();
            cursor.deletePreviousChar();
            cursor.deleteChar();
            cursor.endEditBlock();
            setTextCursor(cursor);
            return;
        }
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
        const QString baseIndentation = prefix.left(GetIndentLength(prefix));
        QString indentation = baseIndentation;
        bool shouldExpandPair = false;
        if (mLanguage != CodeLanguage::PlainText && prefix.trimmed().endsWith(QLatin1Char('{')) == true)
        {
            indentation += QString(INDENT_WIDTH, QLatin1Char(' '));
            if (mShouldPairBrackets == true && document()->characterAt(cursor.position()) == QLatin1Char('}'))
            {
                mHighlighter->rehighlightBlock(cursor.block());
                const CodeBlockData* dataOrNull = static_cast<CodeBlockData*>(cursor.block().userData());
                if (dataOrNull != nullptr)
                {
                    const isize opening = prefix.lastIndexOf(QLatin1Char('{'));
                    for (const CodeBracket& bracket : dataOrNull->Brackets)
                    {
                        shouldExpandPair = shouldExpandPair == true || bracket.Position == opening;
                    }
                }
            }
        }
        cursor.insertText(QLatin1Char('\n') + indentation);
        if (shouldExpandPair == true)
        {
            const int32 contentPosition = cursor.position();
            cursor.insertText(QLatin1Char('\n') + baseIndentation);
            cursor.setPosition(contentPosition);
        }
        cursor.endEditBlock();
        setTextCursor(cursor);
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}
void CodeEditor::HideAssistance()
{
    mCompletionList->hide();
    mSignature->hide();
}

void CodeEditor::ShowCompletions(const QStringList& labels, const QStringList& details)
{
    HideAssistance();
    mCompletionList->clear();
    for (int32 index = 0; index < labels.size(); ++index)
    {
        QListWidgetItem* item = new QListWidgetItem(labels.at(index), mCompletionList);
        item->setToolTip(QStringLiteral("<pre>%1</pre>").arg(details.value(index).toHtmlEscaped()));
    }
    if (labels.isEmpty() == true || hasFocus() == false)
    {
        return;
    }
    mCompletionList->setCurrentRow(0);
    mCompletionList->resize(COMPLETION_WIDTH,
                            qMin(COMPLETION_VISIBLE_ROWS, static_cast<int32>(labels.size())) *
                                    mCompletionList->sizeHintForRow(0) +
                                INDENT_WIDTH);
    const QPoint location = viewport()->mapToGlobal(cursorRect().bottomLeft());
    const QRect available = screen()->availableGeometry();
    mCompletionList->move(qBound(available.left(), location.x(), available.right() - mCompletionList->width()),
                          qMin(location.y(), available.bottom() - mCompletionList->height()));
    mCompletionList->show();
}

void CodeEditor::ShowSignature(const QString& html)
{
    mSignature->hide();
    if (html.isEmpty() == true || hasFocus() == false)
    {
        return;
    }
    mSignature->setText(html);
    mSignature->adjustSize();
    const QPoint location = viewport()->mapToGlobal(cursorRect().bottomLeft());
    const QRect available = screen()->availableGeometry();
    mSignature->move(qBound(available.left(), location.x(), available.right() - mSignature->width()),
                     qMin(location.y(), available.bottom() - mSignature->height()));
    mSignature->show();
}

} // namespace ludus::editor
