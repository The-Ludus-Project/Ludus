#include "internal/options_dialog.h"

#include <ludus/foundation/base/types.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QStackedWidget>
#include <QStringList>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
// The page list stays narrow; pages get the rest, enough for full tool paths.
constexpr foundation::int32 PAGE_LIST_WIDTH_PIXELS = 160;
constexpr foundation::int32 MINIMUM_DIALOG_WIDTH_PIXELS = 640;
} // namespace

// Thanks to Microsoft, "Options dialog" (Visual Studio documentation),
// https://learn.microsoft.com/en-us/visualstudio/ide/reference/options-dialog-box-visual-studio:
// editor-wide preferences live in one Tools > Options dialog, a page list beside the
// selected page, rather than in the documents they affect.
OptionsDialog::OptionsDialog(QWidget* parentOrNull)
    : QDialog(parentOrNull), mPages(new QListWidget(this)), mStack(new QStackedWidget(this)),
      mOverrideTools(new QCheckBox(QStringLiteral("Override detected shader tools (advanced)"), this)),
      mHelper(new QLineEdit(this)), mCompiler(new QLineEdit(this)), mValidator(new QLineEdit(this)),
      mSpirvCross(new QLineEdit(this)), mWorkerSocket(new QLineEdit(this)), mResolved(new QLabel(this)),
      mProblem(new QLabel(this))
{
    setObjectName(QStringLiteral("optionsDialog"));
    setWindowTitle(QStringLiteral("Options"));
    setMinimumWidth(MINIMUM_DIALOG_WIDTH_PIXELS);
    mPages->setObjectName(QStringLiteral("optionsPages"));
    mPages->setFixedWidth(PAGE_LIST_WIDTH_PIXELS);

    QWidget* renderGraph = new QWidget(mStack);
    renderGraph->setObjectName(QStringLiteral("optionsRenderGraphPage"));
    QFormLayout* form = new QFormLayout(renderGraph);
    QLabel* summary = new QLabel(QStringLiteral("Render-graph builds use this project's selected SDK and the "
                                                "editor's prepared shader tools. Builds and runtime generations go "
                                                "to out/render-graph in the project; saved sources are never "
                                                "modified by a build."),
                                 renderGraph);
    summary->setWordWrap(true);
    form->addRow(summary);
    mResolved->setObjectName(QStringLiteral("renderGraphResolvedTools"));
    mResolved->setWordWrap(true);
    mResolved->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(mResolved);
    mOverrideTools->setObjectName(QStringLiteral("renderGraphOverrideTools"));
    form->addRow(mOverrideTools);
    mHelper->setObjectName(QStringLiteral("renderGraphShaderHelper"));
    mCompiler->setObjectName(QStringLiteral("renderGraphCompiler"));
    mValidator->setObjectName(QStringLiteral("renderGraphValidator"));
    mSpirvCross->setObjectName(QStringLiteral("renderGraphSpirvCross"));
    mWorkerSocket->setObjectName(QStringLiteral("renderGraphWorkerSocket"));
    mHelper->setPlaceholderText(QStringLiteral("Automatic from this project's selected SDK"));
    mCompiler->setPlaceholderText(QStringLiteral("Automatic from project setup / prepared editor tools"));
    mValidator->setPlaceholderText(QStringLiteral("Automatic when SPIR-V/GLSL is selected"));
    mSpirvCross->setPlaceholderText(QStringLiteral("Automatic when GLSL is selected"));
    mWorkerSocket->setPlaceholderText(QStringLiteral("existing ludus-shader-worker socket"));
    form->addRow(QStringLiteral("SDK shader helper"), mHelper);
    form->addRow(QStringLiteral("Slang compiler"), mCompiler);
    form->addRow(QStringLiteral("SPIR-V validator"), mValidator);
    form->addRow(QStringLiteral("SPIRV-Cross"), mSpirvCross);
    form->addRow(QStringLiteral("Native shader worker"), mWorkerSocket);
    mProblem->setObjectName(QStringLiteral("optionsProblem"));
    mProblem->setWordWrap(true);
    form->addRow(mProblem);
    mStack->addWidget(renderGraph);
    mPages->addItem(QStringLiteral("Render Graph"));
    mPages->setCurrentRow(0);

    QHBoxLayout* body = new QHBoxLayout;
    body->addWidget(mPages);
    body->addWidget(mStack, 1);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addLayout(body, 1);
    layout->addWidget(buttons);

    connect(mPages, &QListWidget::currentRowChanged, mStack, &QStackedWidget::setCurrentIndex);
    connect(mOverrideTools, &QCheckBox::toggled, this, &OptionsDialog::updateToolFields);
    connect(buttons, &QDialogButtonBox::accepted, this, &OptionsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &OptionsDialog::reject);
    SetResolvedBuildTools({}, {});
    updateToolFields();
}

void OptionsDialog::SetBuildSettings(const RenderGraphBuildSettings& settings)
{
    mOverrideTools->setChecked(settings.ShouldOverrideTools);
    mHelper->setText(settings.Tools.Helper);
    mCompiler->setText(settings.Tools.Compiler);
    mValidator->setText(settings.Tools.Validator);
    mSpirvCross->setText(settings.Tools.SpirvCross);
    mWorkerSocket->setText(settings.WorkerSocket);
    mProblem->clear();
    updateToolFields();
}

RenderGraphBuildSettings OptionsDialog::GetBuildSettings() const
{
    return RenderGraphBuildSettings
    {
        .ShouldOverrideTools = mOverrideTools->isChecked(),
        .Tools =
            RenderGraphBuildTools
            {
                .Helper = mHelper->text().trimmed(),
                .Compiler = mCompiler->text().trimmed(),
                .Validator = mValidator->text().trimmed(),
                .SpirvCross = mSpirvCross->text().trimmed(),
            },
        .WorkerSocket = mWorkerSocket->text().trimmed(),
    };
}

void OptionsDialog::SetResolvedBuildTools(const QString& sdk, const RenderGraphBuildTools& tools)
{
    if (sdk.isEmpty() == true)
    {
        mResolved->setText(QStringLiteral("Resolved paths are shown after a successful automatic build."));
        return;
    }
    QStringList lines{QStringLiteral("Last automatic build used SDK %1").arg(sdk)};
    for (const QString& tool : {tools.Helper, tools.Compiler, tools.Validator, tools.SpirvCross})
    {
        if (tool.isEmpty() == false)
        {
            lines.append(tool);
        }
    }
    lines.append(QStringLiteral("Selection and shader tools are detected again for each build."));
    mResolved->setText(lines.join(QLatin1Char('\n')));
}

void OptionsDialog::accept()
{
    const RenderGraphBuildSettings settings = GetBuildSettings();
    if (settings.ShouldOverrideTools == true &&
        (settings.Tools.Helper.isEmpty() == true || settings.Tools.Compiler.isEmpty() == true))
    {
        mPages->setCurrentRow(0);
        mProblem->setText(QStringLiteral("An override needs both the SDK shader helper and the Slang compiler. "
                                         "Fill both in, or turn the override off to use the project's SDK."));
        return;
    }
    QDialog::accept();
}

void OptionsDialog::updateToolFields()
{
    const bool isOverriding = mOverrideTools->isChecked();
    for (QLineEdit* tool : {mHelper, mCompiler, mValidator, mSpirvCross})
    {
        tool->setEnabled(isOverriding);
    }
}
} // namespace ludus::editor
