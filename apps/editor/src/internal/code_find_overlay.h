#pragma once

#include <ludus/foundation/base/types.h>

#include <QList>
#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QResizeEvent;
class QShowEvent;
QT_END_NAMESPACE

namespace ludus::editor
{
class CodeEditor;
class CodeDocumentController;

/// The find/replace overlay that replaces the bottom find panel (CE07, design
/// section 4). It floats over the top-right of the editor's viewport, like an
/// ordinary code editor's find widget, and offers regex, whole word, in
/// selection, a live match count and preserve-case replace. Find and the match
/// count search the decoded text once (QString::indexOf or one QRegularExpression
/// global match), never QTextDocument::find in a loop; Replace All publishes one
/// CodeEditSet built in a single pass through the bound controller, so a large
/// replace stays linear. With no bound controller (an unbound view) Replace All
/// falls back to a single Qt edit block that still lands as one undo step.
///
/// The widget object names (codeEditorFind, codeEditorReplacement,
/// codeEditorReplace, codeEditorReplaceAll, codeEditorFindStatus and the
/// "Match case" check box) are kept from the bottom panel so existing tests and
/// the status strings are unchanged.
class CodeFindOverlay final : public QWidget
{
    Q_OBJECT
public:
    /// @param controllerOrNull The controller bound to `editor`, so Replace All
    /// can publish its matches as one edit set (CE05); when null, Replace All
    /// falls back to the Qt edit-block path that still lands as one undo step.
    explicit CodeFindOverlay(CodeEditor* editor,
                             CodeDocumentController* controllerOrNull = nullptr,
                             QWidget* parentOrNull = nullptr);

    /// Reveal the overlay, show the replace row when `shouldReplace` and the view
    /// is writable, seed the find field from a single-line selection, and focus
    /// the find field. Called on Find (Ctrl+F) and Replace (Ctrl+H/Alt+Cmd+F).
    void Reveal(bool shouldReplace);
    /// Hide the overlay and return focus to the editor.
    void Dismiss();

protected:
    /// Accepts every key its fields leave unhandled (Return after a find, for
    /// example), so typing in the overlay never edits the editor beneath it.
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    /// One match in decoded (UTF-16) editor coordinates: [Start, End).
    struct Match final
    {
        foundation::int32 Start = 0;
        foundation::int32 End = 0;
    };
    /// Find the match at or after (or, backward, before) the caret and select it,
    /// wrapping once at the end or start. Updates the match count.
    void findNext(bool shouldSearchBackward);
    /// Replace the current selection when it is the active match, then find next.
    void replaceOne();
    /// Replace every match as one edit set (CE05) through the controller, or the
    /// Qt edit-block fallback. Honors in-selection, regex, whole word and
    /// preserve case. Returns the number replaced through the status label.
    void replaceAll();
    /// Recompute and show the match count ("N of M", or "M matches", or an empty
    /// query clears it). Counts in one pass over the decoded text.
    void refreshMatchCount();
    /// The decoded editor text (CRLF normalized to LF), the same coordinates the
    /// editor's cursor uses.
    [[nodiscard]] QString decodedText() const;
    /// Compile the query into a QRegularExpression honoring regex, whole word and
    /// case. An invalid regex sets `outError` and returns false; an empty query
    /// returns false with `outError` cleared.
    [[nodiscard]] bool buildPattern(class QRegularExpression& outPattern, QString& outError) const;
    /// Every match of the compiled pattern within [rangeStart, rangeEnd) of
    /// `text`, in one global pass. A zero-width match advances by one to make
    /// progress, as an editor's find does.
    [[nodiscard]] QList<Match> collectMatches(const QString& text,
                                              const class QRegularExpression& pattern,
                                              foundation::int32 rangeStart,
                                              foundation::int32 rangeEnd) const;
    /// The replacement for one match: the literal replacement, with regex
    /// backreferences expanded when regex is on, and the match's case pattern
    /// applied when preserve case is on.
    [[nodiscard]] QString replacementFor(const class QRegularExpressionMatch& match, const QString& matchedText) const;
    /// Publish the ordered edits as one CodeEditSet through the controller; map
    /// every match's editor positions to bytes in one pass. Returns false when no
    /// controller is bound so the caller can use the Qt fallback.
    [[nodiscard]] bool applyReplacements(const QList<Match>& matches, const QList<QString>& replacements);
    /// The in-selection range in editor positions, or the whole document. The
    /// range is captured when In selection is turned on, so edits do not shrink
    /// it mid-operation.
    void captureSelectionRange();
    [[nodiscard]] bool isInSelectionActive() const;

    CodeEditor* mEditor;
    CodeDocumentController* mControllerOrNull = nullptr;
    QWidget* mReplaceRow;
    QLineEdit* mFindText;
    QLineEdit* mReplacement;
    QCheckBox* mIsCaseSensitive;
    QCheckBox* mIsRegex;
    QCheckBox* mIsWholeWord;
    QCheckBox* mIsInSelection;
    QCheckBox* mShouldPreserveCase;
    QLabel* mStatus;
    /// The frozen in-selection range [start, end) in editor positions; valid only
    /// while In selection is checked.
    foundation::int32 mSelectionRangeStart = 0;
    foundation::int32 mSelectionRangeEnd = 0;
};
} // namespace ludus::editor
