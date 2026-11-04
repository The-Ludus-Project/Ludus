#pragma once

#include <ludus/foundation/base/types.h>

#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QToolButton;
class QLabel;
QT_END_NAMESPACE

namespace ludus::editor
{
class CodeDocumentTab;
class CodeEditor;
class CodeFileDocument;

/// The code editor's status bar (architecture, "Status bar"). When a code view
/// is active it shows the cursor position, the selection or cursor count, the
/// indentation, the encoding, the line endings, the language mode and the tool
/// (language-server) state. Items whose change M1 supports are clickable and
/// emit a request the shell acts on; the rest are shown for information only.
/// The language-server state is a placeholder until M4.
///
/// The widget mirrors one active tab at a time: it connects to that tab's editor
/// cursor/selection signals and reads its bound file host, and shows nothing when
/// no code tab is active. GUI thread only. It owns no document and never mutates
/// one directly; every change is requested through a signal.
class CodeStatusBar final : public QWidget
{
    Q_OBJECT
public:
    explicit CodeStatusBar(QWidget* parentOrNull = nullptr);

    /// Mirror `tabOrNull`: reconnect to its editor's cursor/selection signals and
    /// refresh every item. A null tab hides the bar. Call on ActiveTabChanged.
    void SetActiveTab(CodeDocumentTab* tabOrNull);

    /// Refresh every item from the current tab, after an edit or a profile change
    /// (encoding, line endings, indentation, language) that the shell applied.
    void Refresh();

    // Accessors for tests: the text each item currently shows.
    [[nodiscard]] QString GetPositionText() const;
    [[nodiscard]] QString GetSelectionText() const;
    [[nodiscard]] QString GetIndentText() const;
    [[nodiscard]] QString GetEncodingText() const;
    [[nodiscard]] QString GetLineEndingText() const;
    [[nodiscard]] QString GetLanguageText() const;
    [[nodiscard]] QString GetToolStateText() const;

Q_SIGNALS:
    /// The language item was clicked with a chosen language identifier (the
    /// registry's Identifier, for example "slang" or "plaintext").
    void LanguageChangeRequested(const QString& languageIdentifier);
    /// The line-ending item was clicked with the chosen ending ("lf" or "crlf").
    void LineEndingChangeRequested(const QString& ending);

private:
    void updatePosition();
    void updateSelection();
    void updateFromHost();
    void disconnectEditor();
    [[nodiscard]] CodeEditor* editorOrNull() const;
    [[nodiscard]] CodeFileDocument* hostOrNull() const;
    void showLanguageMenu();
    void showLineEndingMenu();
    void showGoToLine();

    CodeDocumentTab* mTabOrNull = nullptr;
    QToolButton* mPosition;
    QToolButton* mSelection;
    QToolButton* mIndent;
    QToolButton* mEncoding;
    QToolButton* mLineEnding;
    QToolButton* mLanguage;
    QToolButton* mToolState;
};
} // namespace ludus::editor
