#include "internal/code_editor_panel.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_editor.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;

CodeEditorPanel::CodeEditorPanel(CodeEditor* editor, QWidget* parentOrNull)
    : QWidget(parentOrNull), mEditor(editor), mSearch(new QWidget(this)), mReplaceRow(new QWidget(mSearch)),
      mFindText(new QLineEdit(mSearch)), mReplacement(new QLineEdit(mReplaceRow)),
      mIsCaseSensitive(new QCheckBox(QStringLiteral("Match case"), mSearch)), mStatus(new QLabel(mSearch))
{
    LUDUS_ASSERT(editor != nullptr);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(mSearch);
    layout->addWidget(editor, 1);
    QVBoxLayout* searchLayout = new QVBoxLayout(mSearch);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout* findRow = new QHBoxLayout;
    mFindText->setObjectName(QStringLiteral("codeEditorFind"));
    mFindText->setPlaceholderText(QStringLiteral("Find"));
    mFindText->setAccessibleName(QStringLiteral("Find text"));
    mFindText->setClearButtonEnabled(true);
    mStatus->setObjectName(QStringLiteral("codeEditorFindStatus"));
    mStatus->setAccessibleName(QStringLiteral("Search result"));
    QPushButton* previous = new QPushButton(QStringLiteral("Previous"), mSearch);
    QPushButton* next = new QPushButton(QStringLiteral("Next"), mSearch);
    QPushButton* close = new QPushButton(QStringLiteral("Close"), mSearch);
    findRow->addWidget(mFindText, 1);
    findRow->addWidget(previous);
    findRow->addWidget(next);
    findRow->addWidget(close);
    searchLayout->addLayout(findRow);
    QHBoxLayout* options = new QHBoxLayout;
    options->addWidget(mIsCaseSensitive);
    options->addWidget(mStatus, 1);
    searchLayout->addLayout(options);
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
    replacementRow->addWidget(replace);
    replacementRow->addWidget(replaceAll);
    searchLayout->addWidget(mReplaceRow);
    connect(editor, &CodeEditor::FindRequested, this, [this]() { showSearch(false); });
    connect(editor, &CodeEditor::ReplaceRequested, this, [this]() { showSearch(true); });
    connect(editor, &CodeEditor::GoToLineRequested, this, [this]() {
        bool isAccepted = false;
        const int32 line = QInputDialog::getInt(this,
                                                QStringLiteral("Go to Line"),
                                                QStringLiteral("Line"),
                                                mEditor->textCursor().blockNumber() + 1,
                                                1,
                                                mEditor->blockCount(),
                                                1,
                                                &isAccepted);
        if (isAccepted == true)
        {
            mEditor->GoToLine(line);
        }
    });
    connect(previous, &QPushButton::clicked, this, [this]() { findText(true); });
    connect(next, &QPushButton::clicked, this, [this]() { findText(false); });
    connect(mFindText, &QLineEdit::returnPressed, this, [this]() { findText(false); });
    connect(replace, &QPushButton::clicked, this, &CodeEditorPanel::replaceText);
    connect(replaceAll, &QPushButton::clicked, this, &CodeEditorPanel::replaceAll);
    connect(mFindText, &QLineEdit::textChanged, mStatus, &QLabel::clear);
    connect(close, &QPushButton::clicked, this, [this]() {
        mSearch->hide();
        mEditor->setFocus();
    });
    QShortcut* escape = new QShortcut(QKeySequence(Qt::Key_Escape), mSearch);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, close, &QPushButton::click);
    mSearch->hide();
}

void CodeEditorPanel::showSearch(bool shouldReplace)
{
    mSearch->show();
    mReplaceRow->setVisible(shouldReplace == true && mEditor->isReadOnly() == false);
    const QString selection = mEditor->textCursor().selectedText();
    if (selection.isEmpty() == false && selection.contains(QChar::ParagraphSeparator) == false)
    {
        mFindText->setText(selection);
    }
    mStatus->clear();
    mFindText->setFocus();
    mFindText->selectAll();
}

void CodeEditorPanel::findText(bool shouldSearchBackward)
{
    if (mFindText->text().isEmpty() == true)
    {
        return;
    }
    QTextDocument::FindFlags flags;
    if (mIsCaseSensitive->isChecked() == true)
    {
        flags |= QTextDocument::FindCaseSensitively;
    }
    if (shouldSearchBackward == true)
    {
        flags |= QTextDocument::FindBackward;
    }
    QTextCursor found = mEditor->document()->find(mFindText->text(), mEditor->textCursor(), flags);
    if (found.isNull() == true)
    {
        QTextCursor boundary(mEditor->document());
        boundary.movePosition(shouldSearchBackward == true ? QTextCursor::End : QTextCursor::Start);
        found = mEditor->document()->find(mFindText->text(), boundary, flags);
    }
    if (found.isNull() == true)
    {
        mStatus->setText(QStringLiteral("No matches"));
    }
    else
    {
        mEditor->setTextCursor(found);
        mEditor->ensureCursorVisible();
        mStatus->clear();
    }
}

void CodeEditorPanel::replaceText()
{
    if (mEditor->isReadOnly() == true || mFindText->text().isEmpty() == true)
    {
        return;
    }
    QTextCursor cursor = mEditor->textCursor();
    const Qt::CaseSensitivity sensitivity =
        mIsCaseSensitive->isChecked() == true ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (cursor.hasSelection() == true && cursor.selectedText().compare(mFindText->text(), sensitivity) == 0)
    {
        cursor.beginEditBlock();
        cursor.insertText(mReplacement->text());
        cursor.endEditBlock();
        mEditor->setTextCursor(cursor);
    }
    findText(false);
}

void CodeEditorPanel::replaceAll()
{
    if (mEditor->isReadOnly() == true || mFindText->text().isEmpty() == true)
    {
        return;
    }
    QTextDocument::FindFlags flags;
    if (mIsCaseSensitive->isChecked() == true)
    {
        flags |= QTextDocument::FindCaseSensitively;
    }
    QTextCursor group(mEditor->document());
    group.beginEditBlock();
    QTextCursor cursor(mEditor->document());
    int32 count = 0;
    while (true)
    {
        cursor = mEditor->document()->find(mFindText->text(), cursor, flags);
        if (cursor.isNull() == true)
        {
            break;
        }
        cursor.insertText(mReplacement->text());
        ++count;
    }
    group.endEditBlock();
    mStatus->setText(QStringLiteral("%1 replacements").arg(count));
}
} // namespace ludus::editor
