#pragma once

#include <ludus/foundation/base/types.h>

#include <QPlainTextEdit>
#include <QTimer>

QT_BEGIN_NAMESPACE
class QKeyEvent;
class QContextMenuEvent;
class QPaintEvent;
class QResizeEvent;
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

/// Reusable Qt text-input surface. Owns focused-buffer undo only; no files,
/// package history, compilation or language-server lifetime. GUI thread only.
class CodeEditor final : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit CodeEditor(QWidget* parentOrNull = nullptr);
    /// Select coloring/comment syntax without changing text or undo history.
    void SetLanguage(CodeLanguage language);
    /// Apply a single native undo group; commands preserve read-only buffers.
    void IndentSelection(bool shouldOutdent = false);
    void ToggleLineComment();
    void DuplicateLines();
    void MoveLines(bool shouldMoveDown);
    void DeleteLines();
    /// Clamp a one-based line to the document and reveal its start.
    void GoToLine(foundation::int32 line);

Q_SIGNALS:
    void FindRequested();
    void ReplaceRequested();
    void GoToLineRequested();

private:
    friend class CodeEditorGutter;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    [[nodiscard]] foundation::int32 getGutterWidth() const;
    void updateGutterWidth();
    void paintGutter(QPaintEvent* event);
    void updateSelections();
    [[nodiscard]] QTextCursor getSelectedLines() const;
    CodeEditorGutter* mGutter;
    CodeSyntaxHighlighter* mHighlighter;
    CodeLanguage mLanguage = CodeLanguage::PlainText;
    QTimer mSelectionUpdate;
};
} // namespace ludus::editor
