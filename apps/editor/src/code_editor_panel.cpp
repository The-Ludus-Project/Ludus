#include "internal/code_editor_panel.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_edit_scope.h"
#include "internal/code_editor.h"
#include "internal/code_navigation.h"

#include <QCheckBox>
#include <QFont>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QString>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
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
    QHBoxLayout* tools = new QHBoxLayout;
    QToolButton* commands = new QToolButton(this);
    commands->setText(QStringLiteral("Edit"));
    commands->setToolButtonStyle(Qt::ToolButtonTextOnly);
    commands->setObjectName(QStringLiteral("codeEditorCommands"));
    commands->setAccessibleName(QStringLiteral("Editing, source navigation and keyboard shortcuts"));
    commands->setPopupMode(QToolButton::InstantPopup);
    QMenu* commandMenu = new QMenu(commands);
    commandMenu->setToolTipsVisible(true);
    commandMenu->addActions(editor->actions());
    commands->setMenu(commandMenu);
    tools->addWidget(commands);
    QSpinBox* textSize = new QSpinBox(this);
    textSize->setObjectName(QStringLiteral("codeEditorTextSize"));
    textSize->setAccessibleName(QStringLiteral("Editor text size in points"));
    textSize->setRange(CodeEditor::MINIMUM_FONT_POINTS, CodeEditor::MAXIMUM_FONT_POINTS);
    textSize->setSuffix(QStringLiteral(" pt"));
    tools->addWidget(textSize);
    QCheckBox* wrap = new QCheckBox(QStringLiteral("Wrap"), this);
    wrap->setObjectName(QStringLiteral("codeEditorWordWrap"));
    wrap->setToolTip(QStringLiteral("Wrap long lines to the editor width without changing the source."));
    tools->addWidget(wrap);
    QCheckBox* guides = new QCheckBox(QStringLiteral("Guides"), this);
    guides->setObjectName(QStringLiteral("codeEditorIndentGuides"));
    guides->setToolTip(QStringLiteral("Show indentation guides."));
    tools->addWidget(guides);
    tools->addStretch();
    QLabel* position = new QLabel(this);
    position->setObjectName(QStringLiteral("codeEditorPosition"));
    const auto updatePosition = [editor, position]() {
        const QTextCursor cursor = editor->textCursor();
        // Count Unicode scalars, matching the compiler's column convention.
        const QString text = cursor.block().text();
        int32 column = 1;
        for (int32 index = 0; index < cursor.positionInBlock(); ++index)
        {
            if (text.at(index).isLowSurrogate() == false || index == 0 || text.at(index - 1).isHighSurrogate() == false)
            {
                ++column;
            }
        }
        position->setText(QStringLiteral("Ln %1, Col %2").arg(cursor.blockNumber() + 1).arg(column));
    };
    connect(editor, &QPlainTextEdit::cursorPositionChanged, this, updatePosition);
    updatePosition();
    tools->addWidget(position);
    QPushButton* diagnostics = new QPushButton(this);
    diagnostics->setObjectName(QStringLiteral("codeEditorDiagnostics"));
    diagnostics->setToolTip(QStringLiteral("Next diagnostic (F8); previous diagnostic (Shift+F8)."));
    const auto updateDiagnostics = [editor, diagnostics]() {
        const int32 count = editor->GetDiagnosticCount();
        QString label = count == 0 ? QStringLiteral("No issues")
                                   : (count == 1 ? QStringLiteral("1 issue") : QStringLiteral("%1 issues").arg(count));
        if (editor->AreDiagnosticsCurrent() == false && count > 0)
        {
            label += QStringLiteral(" · stale");
        }
        diagnostics->setText(label);
        diagnostics->setEnabled(count > 0);
    };
    connect(editor, &CodeEditor::DiagnosticsChanged, this, updateDiagnostics);
    connect(diagnostics, &QPushButton::clicked, editor, [editor]() { editor->GoToDiagnostic(); });
    updateDiagnostics();
    tools->addWidget(diagnostics);
    const auto updatePresentation = [editor, textSize, wrap, guides]() {
        const QSignalBlocker sizeBlocker(textSize);
        const QSignalBlocker wrapBlocker(wrap);
        const QSignalBlocker guidesBlocker(guides);
        textSize->setValue(editor->font().pointSize());
        wrap->setChecked(editor->lineWrapMode() == QPlainTextEdit::WidgetWidth);
        guides->setChecked(editor->property("indentGuidesVisible").toBool());
    };
    connect(editor, &CodeEditor::PresentationChanged, this, updatePresentation);
    connect(textSize, &QSpinBox::valueChanged, editor, &CodeEditor::SetFontPointSize);
    connect(wrap, &QCheckBox::toggled, editor, &CodeEditor::SetWordWrap);
    connect(guides, &QCheckBox::toggled, editor, &CodeEditor::SetIndentGuidesVisible);
    updatePresentation();
    layout->addLayout(tools);
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
        mEditor->GetNavigation().RememberCurrent();
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
    const CodeEditScope operation(*mEditor);
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
    const CodeEditScope operation(*mEditor);
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
