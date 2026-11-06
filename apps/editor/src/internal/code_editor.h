#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include "internal/code_latency_probe.h"
#include "internal/code_multi_cursor.h"

#include <QColor>
#include <QList>
#include <QPlainTextEdit>
#include <QString>
#include <QStringList>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>

QT_BEGIN_NAMESPACE
class QKeyEvent;
class QMouseEvent;
class QPoint;
class QContextMenuEvent;
class QPaintEvent;
class QResizeEvent;
class QInputMethodEvent;
class QMimeData;
class QFocusEvent;
class QHelpEvent;
class QAction;
class QListWidget;
class QLabel;
QT_END_NAMESPACE

namespace ludus::editor
{
class CodeEditorGutter;
class CodeSyntaxHighlighter;
class CodeNavigation;

/// Lexical profile only; semantic intelligence belongs to a language service.
enum class CodeLanguage : foundation::uint8
{
    PlainText,
    Slang,
    /// The Ludus Shading Language (`.lusl`, `.luslh`); docs/modules/tooling/shader-language.md.
    Lusl,
};

enum class CodeEditKind : foundation::uint8
{
    Command,
    Typing,
    Backspace,
    Delete,
};

enum class CodeDiagnosticSeverity : foundation::uint8
{
    Error,
    Warning,
};

/// A copied compiler message to mark in the text; the producer keeps its report.
struct CodeDiagnostic final
{
    foundation::int32 Line = 0;   ///< One-based line; messages outside the text are ignored.
    foundation::int32 Column = 0; ///< One-based Unicode scalar column, or 0 when only the line is known.
    foundation::int32 Length = 0; ///< Unicode scalars from Column; 0 marks the single position.
    CodeDiagnosticSeverity Severity = CodeDiagnosticSeverity::Error;
    QString Message;
};

/// Reusable Qt text-input surface. Bound undo routes to the document owner;
/// unbound views use Qt undo. No file, compilation or language-server lifetime. GUI thread only.
class CodeEditor final : public QPlainTextEdit
{
    Q_OBJECT
public:
    /// Initial and reset text size in typographic points.
    static constexpr foundation::int32 DEFAULT_FONT_POINTS = 13;
    /// Lower bound for persisted and interactive text sizes, in points.
    static constexpr foundation::int32 MINIMUM_FONT_POINTS = 8;
    /// Upper bound for persisted and interactive text sizes, in points.
    static constexpr foundation::int32 MAXIMUM_FONT_POINTS = 32;
    explicit CodeEditor(QWidget* parentOrNull = nullptr);
    /// Select coloring/comment syntax without changing text or undo history.
    void SetLanguage(CodeLanguage language);
    /// Clamp and persist text size in points without changing text or history. GUI thread only.
    void SetFontPointSize(foundation::int32 points);
    /// Persist visual wrapping without inserting newlines or changing history. GUI thread only.
    void SetWordWrap(bool shouldWrap);
    /// Persist visible indentation guides; text and history are unchanged. GUI thread only.
    void SetIndentGuidesVisible(bool isVisible);
    /// Visit the next/previous anchored diagnostic in source order, wrapping at either end.
    void GoToDiagnostic(bool shouldGoBackward = false);
    /// Current lexical/comment profile; independent of formatter availability.
    [[nodiscard]] LUDUS_INLINE CodeLanguage GetLanguage() const noexcept
    {
        return mLanguage;
    }
    /// Apply a single native undo group; commands preserve read-only buffers.
    void IndentSelection(bool shouldOutdent = false);
    /// Multiple-cursor commands (CE06). AddCursorAbove/Below place a collapsed
    /// cursor at the same column on the adjacent line; AddNextOccurrence selects
    /// the word under the caret, then the next match on repeat; AddAllOccurrences
    /// selects every match at once; ClearSecondaryCursors collapses to one. They
    /// delegate to the view's multi-cursor overlay and are no-ops in a read-only
    /// buffer. The keymap profiles bind them; menus and the palette call them.
    void AddCursorAbove();
    void AddCursorBelow();
    void AddNextOccurrence();
    void AddAllOccurrences();
    void ClearSecondaryCursors();
    void ToggleLineComment();
    void DuplicateLines();
    void MoveLines(bool shouldMoveDown);
    void DeleteLines();
    /// Clamp a one-based line, and an optional one-based Unicode scalar column, to
    /// the document and reveal that position. Column 0 selects the line start.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): line then column is the universal order.
    void GoToLine(foundation::int32 line, foundation::int32 column = 0);
    /// View-only source history and semantic navigation presentation; owned by this editor.
    [[nodiscard]] LUDUS_INLINE CodeNavigation& GetNavigation() const noexcept
    {
        return *mNavigation;
    }
    /// Replace the marked diagnostics: gutter markers, underlines, hover text, and the message
    /// callout under the caret's line until that text is edited.
    /// Markers are anchored in the current text and move with later edits; the
    /// first edit marks them out of date until the producer replaces them.
    /// isCurrent false says the report already describes an earlier text version.
    void SetDiagnostics(const QList<CodeDiagnostic>& diagnostics, bool isCurrent);
    [[nodiscard]] LUDUS_INLINE foundation::int32 GetDiagnosticCount() const noexcept
    {
        return static_cast<foundation::int32>(mDiagnostics.size());
    }
    /// Current anchored range of a marked diagnostic, or a null cursor when out of range.
    [[nodiscard]] QTextCursor GetDiagnosticRange(foundation::int32 index) const;
    [[nodiscard]] LUDUS_INLINE bool AreDiagnosticsCurrent() const noexcept
    {
        return mAreDiagnosticsCurrent;
    }
    /// Hover text for messages covering a document position, or empty when none do.
    [[nodiscard]] QString DescribeDiagnosticsAt(foundation::int32 position) const;
    /// Hover text for every message starting on a zero-based line, or empty.
    [[nodiscard]] QString DescribeLineDiagnostics(foundation::int32 blockNumber) const;
    /// Bound views route every undo path to their document owner; unbound views
    /// retain Qt undo. Changing mode clears the unused Qt undo stack.
    void SetDocumentHistory(bool shouldUseDocumentHistory);
    void SetHistoryAvailability(bool canUndo, bool canRedo);
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    void Undo();
    void Redo();
    void SetCanFormat(bool canFormat);
    /// Present copied assistance results at the caret. No text mutation; choosing
    /// an item emits its row to the version-checking editing controller.
    void ShowCompletions(const QStringList& labels, const QStringList& details);
    void ShowSignature(const QString& html);
    void HideAssistance();
    /// Nestable input/command boundaries, including edits spanning many ranges.
    void BeginEditOperation(CodeEditKind kind = CodeEditKind::Command);
    void EndEditOperation();
    /// The keypress-to-paint latency probe for this view. Idle until Start();
    /// used by the native latency run and interactive qualification.
    [[nodiscard]] CodeLatencyProbe& GetLatencyProbe() const noexcept;
    /// The secondary-cursor overlay for this view (CE06). Owned by the editor;
    /// the controller drives it when restoring a multi-cursor selection set.
    [[nodiscard]] CodeMultiCursor& GetMultiCursor() const noexcept;
    /// Repaint the secondary carets and selections and announce the new cursor
    /// count (CE06). The multi-cursor overlay calls this after it changes the set.
    void NotifyMultiCursorChanged();

Q_SIGNALS:
    /// A font, wrapping or guide preference changed on the GUI thread.
    void PresentationChanged();
    /// Diagnostic markers were replaced or their freshness changed.
    void DiagnosticsChanged();
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
    void AssistanceRequested(bool isSignature);
    void AssistanceDismissed();
    void CompletionChosen(foundation::int32 row);
    void SignatureOverloadRequested(foundation::int32 direction);
    /// The number of cursors changed (1 plus the secondary cursors, CE06). The
    /// status bar shows the count; emitted only when the count actually changes.
    void CursorCountChanged(foundation::int32 count);
    /// A code-editor keymap command fired while this view had focus (CE16/CE26):
    /// toggle a breakpoint on the caret line, or fold/unfold. The gutter and fold
    /// model that act on these land in later milestones (M8, M3); the commands
    /// and their bindings are registered now so the keymap profiles cover them.
    void ToggleBreakpointRequested();
    void FoldAllRequested();
    void UnfoldAllRequested();
    void ToggleFoldRequested();

private:
    QListWidget* mCompletionList;
    QLabel* mSignature;
    friend class CodeEditorGutter;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void paintIndentGuides(const QPaintEvent* event);
    void paintDiagnosticCallout();
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void handleKeyPress(QKeyEvent* event);
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    /// Slang name under a viewport point outside comments and strings, or a null cursor.
    [[nodiscard]] QTextCursor getDefinitionLinkAt(const QPoint& position) const;
    /// Underline the name under the pointer while the definition modifier is held.
    void updateDefinitionLink(const QPoint& position, Qt::KeyboardModifiers modifiers);
    void changeEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void insertFromMimeData(const QMimeData* source) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    /// The structural-editing pairs for the current language, from the registry
    /// (CE17). HasStructuralEditing is false for plain text, so these are empty
    /// and no auto-closing, surrounding or smart indentation happens.
    [[nodiscard]] bool languageHasStructuralEditing() const;
    /// The close character auto-closing pairs with `open`, or a null QChar when
    /// `open` opens no auto-closing pair for the current language.
    [[nodiscard]] QChar autoClosingCloseFor(QChar open) const;
    /// The close character the surrounding pair wraps a selection in for `open`,
    /// or a null QChar when `open` surrounds nothing.
    [[nodiscard]] QChar surroundingCloseFor(QChar open) const;
    /// Whether `character` closes any auto-closing pair of the current language,
    /// so typing it next steps over the existing close (and Backspace between a
    /// pair removes both).
    [[nodiscard]] bool isCloseOfAutoClosingPair(QChar character) const;
    /// Whether `character` is the open of any bracket pair of the current
    /// language, for smart indentation after a trailing open bracket.
    [[nodiscard]] bool isOpenBracket(QChar character) const;
    [[nodiscard]] QChar bracketCloseFor(QChar open) const;
    [[nodiscard]] foundation::int32 getGutterWidth() const;
    void updateGutterWidth();
    void paintGutter(QPaintEvent* event);
    [[nodiscard]] bool showGutterToolTip(const QHelpEvent& help);
    void updateSelections();
    void appendDiagnosticSelections(QList<QTextEdit::ExtraSelection>& inoutSelections) const;
    [[nodiscard]] QColor getDiagnosticColor(CodeDiagnosticSeverity severity) const;
    [[nodiscard]] QString describeDiagnostics(foundation::int32 position, foundation::int32 blockNumber) const;
    void markDiagnosticsOutOfDate(foundation::int32 position, foundation::int32 removed, foundation::int32 added);
    [[nodiscard]] QTextCursor getSelectedLines() const;

    /// Diagnostic anchored by a document cursor; Qt moves the range with edits.
    struct DiagnosticMarker final
    {
        QTextCursor Range;
        CodeDiagnosticSeverity Severity = CodeDiagnosticSeverity::Error;
        QString Message;
        bool IsLineOnly = false;
    };
    CodeEditorGutter* mGutter;
    CodeSyntaxHighlighter* mHighlighter;
    CodeLanguage mLanguage = CodeLanguage::PlainText;
    QTimer mSelectionUpdate;
    QList<DiagnosticMarker> mDiagnostics;
    QString mDiagnosticText;
    bool mAreDiagnosticsCurrent = true;
    /// The text changed after the markers were set; they still point at where the problem was.
    bool mHasEditedSinceReport = false;
    bool mShouldShowIndentGuides = true;
    bool mShouldPairBrackets = true;
    bool mShouldUseDocumentHistory = false;
    bool mCanUndo = false;
    bool mCanRedo = false;
    QAction* mFormatAction;
    QAction* mChooseFormatterAction;
    CodeNavigation* mNavigation;
    /// Name underlined for Ctrl+click (Command+click on macOS) Go to Definition.
    QTextCursor mDefinitionLink;
    /// A definition click owns the press until release, so dragging never selects.
    bool mIsDefinitionClick = false;
    /// Keypress-to-paint latency probe; idle unless explicitly started.
    mutable CodeLatencyProbe mLatencyProbe;
    /// Secondary-cursor overlay (CE06). A plain value member bound to this view;
    /// it holds no Qt-child lifetime and is destroyed with the editor.
    mutable CodeMultiCursor mMultiCursor;
};
} // namespace ludus::editor
