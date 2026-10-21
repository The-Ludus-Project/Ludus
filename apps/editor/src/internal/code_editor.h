#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <QPlainTextEdit>
#include <QTimer>

QT_BEGIN_NAMESPACE
class QKeyEvent;
class QContextMenuEvent;
class QPaintEvent;
class QResizeEvent;
class QInputMethodEvent;
class QMimeData;
class QFocusEvent;
class QAction;
QT_END_NAMESPACE

namespace ludus::editor
{
class CodeEditorGutter;
class CodeSyntaxHighlighter;

/// Lexical profile only; semantic intelligence belongs to a language service.
enum class CodeLanguage : foundation::uint8
{
    PlainText,
    Slang,
};

enum class CodeEditKind : foundation::uint8
{
    Command,
    Typing,
    Backspace,
    Delete,
};

/// Reusable Qt text-input surface. Bound undo routes to the document owner;
/// unbound views use Qt undo. No file, compilation or language-server lifetime. GUI thread only.
class CodeEditor final : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit CodeEditor(QWidget* parentOrNull = nullptr);
    /// Select coloring/comment syntax without changing text or undo history.
    void SetLanguage(CodeLanguage language);
    /// Current lexical/comment profile; independent of formatter availability.
    [[nodiscard]] LUDUS_INLINE CodeLanguage GetLanguage() const noexcept
    {
        return mLanguage;
    }
    /// Apply a single native undo group; commands preserve read-only buffers.
    void IndentSelection(bool shouldOutdent = false);
    void ToggleLineComment();
    void DuplicateLines();
    void MoveLines(bool shouldMoveDown);
    void DeleteLines();
    /// Clamp a one-based line to the document and reveal its start.
    void GoToLine(foundation::int32 line);
    /// Bound views route every undo path to their document owner; unbound views
    /// retain Qt undo. Changing mode clears the unused Qt undo stack.
    void SetDocumentHistory(bool shouldUseDocumentHistory);
    void SetHistoryAvailability(bool canUndo, bool canRedo);
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    void Undo();
    void Redo();
    void SetCanFormat(bool canFormat);
    /// Nestable input/command boundaries, including edits spanning many ranges.
    void BeginEditOperation(CodeEditKind kind = CodeEditKind::Command);
    void EndEditOperation();

Q_SIGNALS:
    void FindRequested();
    void ReplaceRequested();
    void GoToLineRequested();
    void UndoRequested();
    void RedoRequested();
    void HistoryAvailabilityChanged();
    void EditStarted(CodeEditKind kind);
    void EditFinished();
    void EditBoundary();
    void FormatRequested();
    void ChooseFormatterRequested();

private:
    friend class CodeEditorGutter;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void insertFromMimeData(const QMimeData* source) override;
    void focusOutEvent(QFocusEvent* event) override;
    [[nodiscard]] foundation::int32 getGutterWidth() const;
    void updateGutterWidth();
    void paintGutter(QPaintEvent* event);
    void updateSelections();
    [[nodiscard]] QTextCursor getSelectedLines() const;
    CodeEditorGutter* mGutter;
    CodeSyntaxHighlighter* mHighlighter;
    CodeLanguage mLanguage = CodeLanguage::PlainText;
    QTimer mSelectionUpdate;
    bool mShouldUseDocumentHistory = false;
    bool mCanUndo = false;
    bool mCanRedo = false;
    QAction* mFormatAction;
    QAction* mChooseFormatterAction;
};
} // namespace ludus::editor
