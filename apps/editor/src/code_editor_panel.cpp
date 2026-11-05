#include "internal/code_editor_panel.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_find_overlay.h"
#include "internal/code_editor.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QString>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;

CodeEditorPanel::CodeEditorPanel(CodeEditor* editor, CodeDocumentController* controllerOrNull, QWidget* parentOrNull)
    : QWidget(parentOrNull), mEditor(editor)
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
    layout->addWidget(editor, 1);

    // The find/replace overlay (CE07) floats over the editor's viewport rather
    // than taking a row of its own; it is parented to the editor so it tracks the
    // viewport, is owned by it, and never steals layout space. Find (Ctrl+F) and
    // Replace reveal it through the editor's FindRequested/ReplaceRequested.
    static_cast<void>(new CodeFindOverlay(editor, controllerOrNull, editor));

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
}
} // namespace ludus::editor
