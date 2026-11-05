#include "internal/code_find_overlay.h"
#include "design_tokens.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_document_controller.h"
#include "internal/code_edit_scope.h"
#include "internal/code_editor.h"
#include "internal/code_navigation.h"
#include "internal/code_text.h"

#include <QByteArray>
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QSize>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

// The find/replace overlay (CE07, design section 4). It replaces the bottom find
// panel with a floating widget over the editor viewport. Searching and counting
// run once over the decoded text (QString::indexOf for a literal query, one
// QRegularExpression global pass for regex, whole word or counting), never
// QTextDocument::find in a loop; Replace All publishes one CodeEditSet built in a
// single forward mapping pass, so a large replace is linear. These are the rules
// the #983 host-verification note set for every multi-edit path.
namespace ludus::editor
{
namespace
{
using namespace foundation;

constexpr int32 OVERLAY_MARGIN = design::SPACING_SMALL;

// Apply the case pattern of `source` to `target` for preserve-case replace: an
// all-upper source upper-cases the target, an all-lower source lower-cases it,
// and a capitalized source capitalizes it. A mixed-case source is left as-is.
[[nodiscard]] QString ApplyCasePattern(const QString& source, const QString& target)
{
    if (source.isEmpty() == true || target.isEmpty() == true)
    {
        return target;
    }
    bool hasLetter = false;
    bool isAllUpper = true;
    bool isAllLower = true;
    for (const QChar character : source)
    {
        if (character.isLetter() == false)
        {
            continue;
        }
        hasLetter = true;
        isAllUpper = isAllUpper == true && character.isUpper() == true;
        isAllLower = isAllLower == true && character.isLower() == true;
    }
    if (hasLetter == false)
    {
        return target;
    }
    if (isAllUpper == true)
    {
        return target.toUpper();
    }
    if (isAllLower == true)
    {
        return target.toLower();
    }
    if (source.at(0).isUpper() == true)
    {
        return target.left(1).toUpper() + target.mid(1).toLower();
    }
    return target;
}
} // namespace

CodeFindOverlay::CodeFindOverlay(CodeEditor* editor, CodeDocumentController* controllerOrNull, QWidget* parentOrNull)
    : QWidget(parentOrNull != nullptr ? parentOrNull : editor), mEditor(editor), mControllerOrNull(controllerOrNull),
      mReplaceRow(new QWidget(this)), mFindText(new QLineEdit(this)), mReplacement(new QLineEdit(mReplaceRow)),
      mIsCaseSensitive(new QCheckBox(QStringLiteral("Match case"), this)),
      mIsRegex(new QCheckBox(QStringLiteral("Regex"), this)),
      mIsWholeWord(new QCheckBox(QStringLiteral("Whole word"), this)),
      mIsInSelection(new QCheckBox(QStringLiteral("In selection"), this)),
      mShouldPreserveCase(new QCheckBox(QStringLiteral("Preserve case"), mReplaceRow)), mStatus(new QLabel(this))
{
    LUDUS_ASSERT(editor != nullptr);
    setObjectName(QStringLiteral("codeFindOverlay"));
    setAutoFillBackground(true);
    // A raised frame reads as a floating overlay over the text, not part of it.
    QFrame* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("codeFindOverlayFrame"));
    frame->setFrameShape(QFrame::StyledPanel);
    QVBoxLayout* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(frame);
    QVBoxLayout* rows = new QVBoxLayout(frame);

    mFindText->setObjectName(QStringLiteral("codeEditorFind"));
    mFindText->setPlaceholderText(QStringLiteral("Find"));
    mFindText->setAccessibleName(QStringLiteral("Find text"));
    mFindText->setClearButtonEnabled(true);
    mStatus->setObjectName(QStringLiteral("codeEditorFindStatus"));
    mStatus->setAccessibleName(QStringLiteral("Search result"));
    QPushButton* previous = new QPushButton(QStringLiteral("Previous"), this);
    previous->setObjectName(QStringLiteral("codeEditorFindPrevious"));
    QPushButton* next = new QPushButton(QStringLiteral("Next"), this);
    next->setObjectName(QStringLiteral("codeEditorFindNext"));
    QPushButton* close = new QPushButton(QStringLiteral("Close"), this);
    close->setObjectName(QStringLiteral("codeEditorFindClose"));

    mIsRegex->setObjectName(QStringLiteral("codeEditorRegex"));
    mIsRegex->setToolTip(QStringLiteral("Interpret the query as a regular expression."));
    mIsWholeWord->setObjectName(QStringLiteral("codeEditorWholeWord"));
    mIsWholeWord->setToolTip(QStringLiteral("Match only whole words."));
    mIsInSelection->setObjectName(QStringLiteral("codeEditorInSelection"));
    mIsInSelection->setToolTip(QStringLiteral("Limit find and replace to the current selection."));
    mShouldPreserveCase->setObjectName(QStringLiteral("codeEditorPreserveCase"));
    mShouldPreserveCase->setToolTip(QStringLiteral("Match the case of each replaced occurrence."));

    // Row 1: the find field and its Previous/Next/Close buttons.
    QHBoxLayout* findRow = new QHBoxLayout;
    findRow->addWidget(mFindText, 1);
    findRow->addWidget(previous);
    findRow->addWidget(next);
    findRow->addWidget(close);
    rows->addLayout(findRow);
    // Row 2: the matching options and the match-count status.
    QHBoxLayout* optionRow = new QHBoxLayout;
    optionRow->addWidget(mIsCaseSensitive);
    optionRow->addWidget(mIsWholeWord);
    optionRow->addWidget(mIsRegex);
    optionRow->addWidget(mIsInSelection);
    optionRow->addWidget(mStatus, 1);
    rows->addLayout(optionRow);
    // Row 3: the replacement field, preserve case and the replace buttons.
    QHBoxLayout* replacementRow = new QHBoxLayout(mReplaceRow);
    replacementRow->setContentsMargins(0, 0, 0, 0);
    mReplacement->setObjectName(QStringLiteral("codeEditorReplacement"));
    mReplacement->setPlaceholderText(QStringLiteral("Replace with"));
    mReplacement->setAccessibleName(QStringLiteral("Replacement text"));
    QPushButton* replace = new QPushButton(QStringLiteral("Replace"), mReplaceRow);
    replace->setObjectName(QStringLiteral("codeEditorReplace"));
    QPushButton* replaceAll = new QPushButton(QStringLiteral("Replace All"), mReplaceRow);
    replaceAll->setObjectName(QStringLiteral("codeEditorReplaceAll"));
    replacementRow->addWidget(mReplacement, 1);
    replacementRow->addWidget(mShouldPreserveCase);
    replacementRow->addWidget(replace);
    replacementRow->addWidget(replaceAll);
    rows->addWidget(mReplaceRow);

    connect(mEditor, &CodeEditor::FindRequested, this, [this]() { Reveal(false); });
    connect(mEditor, &CodeEditor::ReplaceRequested, this, [this]() { Reveal(true); });
    connect(previous, &QPushButton::clicked, this, [this]() { findNext(true); });
    connect(next, &QPushButton::clicked, this, [this]() { findNext(false); });
    connect(mFindText, &QLineEdit::returnPressed, this, [this]() { findNext(false); });
    connect(replace, &QPushButton::clicked, this, &CodeFindOverlay::replaceOne);
    connect(replaceAll, &QPushButton::clicked, this, &CodeFindOverlay::replaceAll);
    connect(close, &QPushButton::clicked, this, &CodeFindOverlay::Dismiss);
    // Re-count on any change to the query or the options that affect matching.
    connect(mFindText, &QLineEdit::textChanged, this, &CodeFindOverlay::refreshMatchCount);
    for (QCheckBox* option : {mIsCaseSensitive, mIsRegex, mIsWholeWord})
    {
        connect(option, &QCheckBox::toggled, this, &CodeFindOverlay::refreshMatchCount);
    }
    connect(mIsInSelection, &QCheckBox::toggled, this, [this](bool isChecked) {
        if (isChecked == true)
        {
            captureSelectionRange();
        }
        refreshMatchCount();
    });

    // Escape dismisses the overlay from any of its fields or buttons.
    QShortcut* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, &CodeFindOverlay::Dismiss);
    hide();
}

void CodeFindOverlay::Reveal(bool shouldReplace)
{
    mReplaceRow->setVisible(shouldReplace == true && mEditor->isReadOnly() == false);
    const QString selection = mEditor->textCursor().selectedText();
    const bool isMultiLine = selection.contains(QChar::ParagraphSeparator) == true;
    if (selection.isEmpty() == false && isMultiLine == false)
    {
        mFindText->setText(selection);
    }
    else if (isMultiLine == true)
    {
        // A multi-line selection is the natural target for In selection.
        mIsInSelection->setChecked(true);
    }
    show();
    raise();
    resize(sizeHint());
    resizeEvent(nullptr);
    refreshMatchCount();
    mFindText->setFocus();
    mFindText->selectAll();
}

void CodeFindOverlay::Dismiss()
{
    hide();
    mEditor->setFocus();
}

void CodeFindOverlay::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    resizeEvent(nullptr);
}

// (Escape is handled by a QShortcut with WidgetWithChildren context; see the
// constructor. No event filter is installed.)

void CodeFindOverlay::keyPressEvent(QKeyEvent* event)
{
    // A key the overlay's fields leave unhandled must stop here. QLineEdit emits
    // returnPressed and then ignores Return, and the overlay is a child of the
    // editor, so without this Enter in the find field would reach the editor and
    // replace the match it just selected with a line break.
    event->accept();
}

void CodeFindOverlay::resizeEvent(QResizeEvent* event)
{
    if (event != nullptr)
    {
        QWidget::resizeEvent(event);
    }
    // Anchor to the top-right of the editor's viewport, like an ordinary editor's
    // find widget, leaving the vertical scroll bar clear.
    QWidget* viewport = mEditor->viewport();
    const QSize wanted = sizeHint();
    const int32 width = qMin(wanted.width(), viewport->width() - 2 * OVERLAY_MARGIN);
    const QPoint topRight = viewport->mapTo(parentWidget(), QPoint(viewport->width() - OVERLAY_MARGIN, OVERLAY_MARGIN));
    setGeometry(topRight.x() - width, topRight.y(), width, wanted.height());
}

QString CodeFindOverlay::decodedText() const
{
    // The controller decodes the host's bytes with DecodeCodeText, so the editor's
    // own plain text is the decoded text in editor coordinates. Using it keeps
    // find positions identical to what the user sees without re-decoding bytes.
    QString text = mEditor->document()->toRawText();
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return text;
}

bool CodeFindOverlay::buildPattern(QRegularExpression& outPattern, QString& outError) const
{
    outError.clear();
    const QString query = mFindText->text();
    if (query.isEmpty() == true)
    {
        return false;
    }
    QString source = mIsRegex->isChecked() == true ? query : QRegularExpression::escape(query);
    if (mIsWholeWord->isChecked() == true)
    {
        source = QStringLiteral("\\b(?:%1)\\b").arg(source);
    }
    QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
    if (mIsCaseSensitive->isChecked() == false)
    {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    QRegularExpression pattern(source, options);
    if (pattern.isValid() == false)
    {
        outError = pattern.errorString();
        return false;
    }
    outPattern = pattern;
    return true;
}

QList<CodeFindOverlay::Match> CodeFindOverlay::collectMatches(const QString& text,
                                                              const QRegularExpression& pattern,
                                                              int32 rangeStart,
                                                              int32 rangeEnd) const
{
    QList<Match> matches;
    // One iterator over the text: QRegularExpression::match validates the whole
    // subject as UTF-16 on every call, so a match() per occurrence is quadratic.
    // The global iterator validates once and steps over zero-width matches.
    QRegularExpressionMatchIterator iterator = pattern.globalMatch(text, rangeStart);
    while (iterator.hasNext() == true)
    {
        const QRegularExpressionMatch match = iterator.next();
        const int32 start = static_cast<int32>(match.capturedStart());
        const int32 end = static_cast<int32>(match.capturedEnd());
        if (start >= rangeEnd)
        {
            break; // The match begins past the range; stop.
        }
        if (end > rangeEnd)
        {
            break; // A match that would cross the in-selection boundary is excluded.
        }
        matches.append(Match{ .Start = start, .End = end });
    }
    return matches;
}

void CodeFindOverlay::refreshMatchCount()
{
    QRegularExpression pattern;
    QString error;
    if (buildPattern(pattern, error) == false)
    {
        mStatus->setText(error.isEmpty() == true ? QString{} : QStringLiteral("Invalid pattern: %1").arg(error));
        return;
    }
    const QString text = decodedText();
    int32 rangeStart = 0;
    int32 rangeEnd = static_cast<int32>(text.size());
    if (isInSelectionActive() == true)
    {
        rangeStart = mSelectionRangeStart;
        rangeEnd = mSelectionRangeEnd;
    }
    const QList<Match> matches = collectMatches(text, pattern, rangeStart, rangeEnd);
    if (matches.isEmpty() == true)
    {
        mStatus->setText(QStringLiteral("No matches"));
        return;
    }
    // "N of M" when the caret sits on a match, else just the total.
    const int32 caret = mEditor->textCursor().selectionStart();
    int32 currentIndex = -1;
    for (int32 index = 0; index < matches.size(); ++index)
    {
        if (matches.at(index).Start == caret && mEditor->textCursor().selectionEnd() == matches.at(index).End)
        {
            currentIndex = index;
            break;
        }
    }
    if (currentIndex >= 0)
    {
        mStatus->setText(QStringLiteral("%1 of %2").arg(currentIndex + 1).arg(matches.size()));
    }
    else
    {
        mStatus->setText(matches.size() == 1 ? QStringLiteral("1 match")
                                             : QStringLiteral("%1 matches").arg(matches.size()));
    }
}

void CodeFindOverlay::findNext(bool shouldSearchBackward)
{
    QRegularExpression pattern;
    QString error;
    if (buildPattern(pattern, error) == false)
    {
        mStatus->setText(error.isEmpty() == true ? QString{} : QStringLiteral("Invalid pattern: %1").arg(error));
        return;
    }
    const QString text = decodedText();
    int32 rangeStart = 0;
    int32 rangeEnd = static_cast<int32>(text.size());
    if (isInSelectionActive() == true)
    {
        rangeStart = mSelectionRangeStart;
        rangeEnd = mSelectionRangeEnd;
    }
    const QList<Match> matches = collectMatches(text, pattern, rangeStart, rangeEnd);
    if (matches.isEmpty() == true)
    {
        mStatus->setText(QStringLiteral("No matches"));
        return;
    }
    // Pick the next match after the caret (or before it, backward), wrapping once.
    const int32 caret =
        shouldSearchBackward == true ? mEditor->textCursor().selectionStart() : mEditor->textCursor().selectionEnd();
    int32 chosen = shouldSearchBackward == true ? matches.size() - 1 : 0;
    if (shouldSearchBackward == true)
    {
        for (int32 index = matches.size() - 1; index >= 0; --index)
        {
            if (matches.at(index).End <= caret)
            {
                chosen = index;
                break;
            }
        }
    }
    else
    {
        for (int32 index = 0; index < matches.size(); ++index)
        {
            if (matches.at(index).Start >= caret)
            {
                chosen = index;
                break;
            }
        }
    }
    const Match match = matches.at(chosen);
    mEditor->GetNavigation().RememberCurrent();
    QTextCursor cursor(mEditor->document());
    cursor.setPosition(match.Start);
    cursor.setPosition(match.End, QTextCursor::KeepAnchor);
    mEditor->setTextCursor(cursor);
    mEditor->ensureCursorVisible();
    mStatus->setText(QStringLiteral("%1 of %2").arg(chosen + 1).arg(matches.size()));
}

QString CodeFindOverlay::replacementFor(const QRegularExpressionMatch& match, const QString& matchedText) const
{
    QString replacement = mReplacement->text();
    if (mIsRegex->isChecked() == true)
    {
        // Expand $1..$9 and ${name} backreferences for a regex replacement.
        QString expanded;
        for (int32 index = 0; index < replacement.size(); ++index)
        {
            const QChar character = replacement.at(index);
            if (character == QLatin1Char('$') && index + 1 < replacement.size())
            {
                const QChar following = replacement.at(index + 1);
                if (following.isDigit() == true)
                {
                    expanded += match.captured(following.digitValue());
                    ++index;
                    continue;
                }
                if (following == QLatin1Char('$'))
                {
                    expanded += QLatin1Char('$');
                    ++index;
                    continue;
                }
            }
            expanded += character;
        }
        replacement = expanded;
    }
    if (mShouldPreserveCase->isChecked() == true)
    {
        replacement = ApplyCasePattern(matchedText, replacement);
    }
    return replacement;
}

void CodeFindOverlay::replaceOne()
{
    if (mEditor->isReadOnly() == true || mFindText->text().isEmpty() == true)
    {
        return;
    }
    QRegularExpression pattern;
    QString error;
    if (buildPattern(pattern, error) == false)
    {
        mStatus->setText(error.isEmpty() == true ? QString{} : QStringLiteral("Invalid pattern: %1").arg(error));
        return;
    }
    // Replace the current selection only when it exactly matches an occurrence at
    // the caret; otherwise just move to the next match.
    const QTextCursor cursor = mEditor->textCursor();
    const QString text = decodedText();
    const int32 start = cursor.selectionStart();
    const int32 end = cursor.selectionEnd();
    if (cursor.hasSelection() == true)
    {
        const QRegularExpressionMatch match = pattern.match(text, start);
        if (match.hasMatch() == true && static_cast<int32>(match.capturedStart()) == start &&
            static_cast<int32>(match.capturedEnd()) == end)
        {
            const QString replacement = replacementFor(match, text.mid(start, end - start));
            const QList<Match> one = {Match{ .Start = start, .End = end }};
            const QList<QString> replacements = {replacement};
            if (applyReplacements(one, replacements) == false)
            {
                const CodeEditScope operation(*mEditor);
                QTextCursor edit = cursor;
                edit.beginEditBlock();
                edit.insertText(replacement);
                edit.endEditBlock();
                mEditor->setTextCursor(edit);
            }
        }
    }
    findNext(false);
}

bool CodeFindOverlay::applyReplacements(const QList<Match>& matches, const QList<QString>& replacements)
{
    if (mControllerOrNull == nullptr)
    {
        return false;
    }
    const CodeTextCapture capture = mControllerOrNull->Capture();
    if (capture.Identity.Document == 0)
    {
        return false;
    }
    const bool hasCrLf = capture.Text.contains("\r\n");
    // Map every match's editor positions to byte offsets by walking forward from
    // one CodePositionOffset (never GetCodeByteOffset per match), and own each
    // replacement's bytes so the borrowed edit set stays valid across ApplyEditSet.
    QList<CodeTextEdit> edits;
    edits.reserve(matches.size());
    QList<QByteArray> insertedBytes;
    insertedBytes.reserve(matches.size());
    CodePositionOffset mapped;
    for (int32 index = 0; index < matches.size(); ++index)
    {
        usize startBytes = 0;
        usize endBytes = 0;
        if (AdvanceCodeByteOffset(capture.Text, matches.at(index).Start, mapped, startBytes) == false ||
            AdvanceCodeByteOffset(capture.Text, matches.at(index).End, mapped, endBytes) == false)
        {
            continue; // A match inside a surrogate pair cannot be mapped; skip it.
        }
        QByteArray replacementBytes = replacements.at(index).toUtf8();
        if (hasCrLf == true)
        {
            replacementBytes.replace("\n", "\r\n");
        }
        insertedBytes.append(replacementBytes);
        const QByteArray& owned = insertedBytes.last();
        edits.append(CodeTextEdit
        {
            .StartBytes = startBytes,
            .RemovedBytes = endBytes - startBytes,
            .Inserted = {owned.constData(), static_cast<usize>(owned.size())},
        });
    }
    if (edits.isEmpty() == true)
    {
        mStatus->setText(QStringLiteral("0 replacements"));
        return true;
    }
    const CodeTextSelection beforeSelections[] = {mControllerOrNull->CaptureSelection()};
    const CodeEditSet set =
    {
        .Edits = {edits.constData(), static_cast<usize>(edits.size())},
        .Before = {{beforeSelections, 1}},
        .After = {},
        .Transaction = {},
    };
    const CodeEditStatus status = mControllerOrNull->ApplyEditSet(capture.Identity, set);
    if (status == CodeEditStatus::Capacity)
    {
        mStatus->setText(QStringLiteral("Nothing was replaced: %1 replacements would make the file larger than its "
                                        "size limit. Shorten the replacement or replace fewer matches.")
                             .arg(edits.size()));
        return true;
    }
    if (status != CodeEditStatus::Ok)
    {
        mStatus->setText(QStringLiteral("Nothing was replaced because the source changed. Run Replace All again."));
        return true;
    }
    mStatus->setText(QStringLiteral("%1 replacements").arg(edits.size()));
    return true;
}

void CodeFindOverlay::replaceAll()
{
    if (mEditor->isReadOnly() == true || mFindText->text().isEmpty() == true)
    {
        return;
    }
    QRegularExpression pattern;
    QString error;
    if (buildPattern(pattern, error) == false)
    {
        mStatus->setText(error.isEmpty() == true ? QString{} : QStringLiteral("Invalid pattern: %1").arg(error));
        return;
    }
    const QString text = decodedText();
    int32 rangeStart = 0;
    int32 rangeEnd = static_cast<int32>(text.size());
    if (isInSelectionActive() == true)
    {
        rangeStart = mSelectionRangeStart;
        rangeEnd = mSelectionRangeEnd;
    }
    const QList<Match> matches = collectMatches(text, pattern, rangeStart, rangeEnd);
    if (matches.isEmpty() == true)
    {
        mStatus->setText(QStringLiteral("0 replacements"));
        return;
    }
    // The common case (a literal find and replace, no regex, no preserve case)
    // reuses one replacement string and needs no per-match regex re-match: the
    // work stays one pass. Backreferences and preserve case need the match, so
    // only then is the pattern re-matched at each occurrence.
    const bool shouldReMatchEach = mIsRegex->isChecked() == true || mShouldPreserveCase->isChecked() == true;
    QList<QString> replacements;
    replacements.reserve(matches.size());
    if (shouldReMatchEach == false)
    {
        const QString replacement = mReplacement->text();
        for (int32 index = 0; index < matches.size(); ++index)
        {
            replacements.append(replacement);
        }
    }
    else
    {
        // Re-walk the same iterator rather than calling match() per occurrence,
        // which would revalidate the whole text each time (see collectMatches).
        QRegularExpressionMatchIterator iterator = pattern.globalMatch(text, matches.first().Start);
        int32 index = 0;
        while (iterator.hasNext() == true && index < matches.size())
        {
            const QRegularExpressionMatch regexMatch = iterator.next();
            if (static_cast<int32>(regexMatch.capturedStart()) != matches.at(index).Start)
            {
                continue;
            }
            const Match& found = matches.at(index);
            replacements.append(replacementFor(regexMatch, text.mid(found.Start, found.End - found.Start)));
            ++index;
        }
        if (replacements.size() != matches.size())
        {
            mStatus->setText(
                QStringLiteral("Nothing was replaced because the matches changed. Run Replace All again."));
            return;
        }
    }
    if (applyReplacements(matches, replacements) == true)
    {
        return;
    }
    // Fallback (no bound controller): one Qt edit block, applied from the end so
    // earlier offsets stay valid, still one undo step.
    const CodeEditScope operation(*mEditor);
    QTextCursor group(mEditor->document());
    group.beginEditBlock();
    for (int32 index = matches.size() - 1; index >= 0; --index)
    {
        QTextCursor edit(mEditor->document());
        edit.setPosition(matches.at(index).Start);
        edit.setPosition(matches.at(index).End, QTextCursor::KeepAnchor);
        edit.insertText(replacements.at(index));
    }
    group.endEditBlock();
    mStatus->setText(QStringLiteral("%1 replacements").arg(matches.size()));
}

void CodeFindOverlay::captureSelectionRange()
{
    const QTextCursor cursor = mEditor->textCursor();
    mSelectionRangeStart = cursor.selectionStart();
    mSelectionRangeEnd = cursor.selectionEnd();
}

bool CodeFindOverlay::isInSelectionActive() const
{
    return mIsInSelection->isChecked() == true && mSelectionRangeEnd > mSelectionRangeStart;
}
} // namespace ludus::editor
