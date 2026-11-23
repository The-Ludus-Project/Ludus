#include "internal/renderer_workspace.h"

#include "internal/code_conflict_banner.h"
#include "internal/code_document_tab.h"
#include "internal/code_editor_area.h"
#include "internal/code_formatter.h"
#include "internal/lusl_formatting.h"
#include "internal/render_graph_shader_assistance.h"
#include "internal/render_graph_shader_language.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringList>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

namespace ludus::editor
{
namespace
{
const QString SHADER_TAB_PREFIX = QStringLiteral("shader:");
const QString GENERATED_TAB_PREFIX = QStringLiteral("generated:");
constexpr const char* SHADER_API_FILE_PROPERTY = "shaderApiFile";

class ShaderTabBinding final : public QObject
{
public:
    ShaderTabBinding(RenderGraphDocument& inoutDocument, CodeDocumentTab& inoutTab)
        : QObject(&inoutTab), Document(inoutDocument)
    {
    }

    RenderGraphCodeDocument Document;
};
} // namespace

void RendererWorkspace::SetCodeEditorArea(CodeEditorArea& inoutArea)
{
    mShaderCodeArea = &inoutArea;
    mSourceController->Bind(nullptr);
    const foundation::int32 sourceView = mViews->indexOf(mSourceSplit->parentWidget());
    if (sourceView >= 0)
    {
        mViews->setTabVisible(sourceView, false);
    }

    QWidget* header = new QWidget(&inoutArea);
    QVBoxLayout* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->addWidget(mGraphSourceHeader);
    headerLayout->addWidget(mSourceStatus);
    mGraphSourceHeader = header;
    mSourceSplit = &inoutArea.InstallSessionWidgets(*header, *mSourceInterface->parentWidget());
    connect(mSourceSplit, &QSplitter::splitterMoved, this, &RendererWorkspace::rememberSourceSplitter);
    mReturnToGraph->show();
    mSourceFile->setPlaceholderText(QStringLiteral("Select package source"));
    QPushButton* sourceActionOrNull = findChild<QPushButton*>(QStringLiteral("renderGraphModeSource"));
    if (sourceActionOrNull != nullptr)
    {
        sourceActionOrNull->setText(QStringLiteral("Open Source"));
        sourceActionOrNull->setToolTip(QStringLiteral("Open a package source in Code"));
        sourceActionOrNull->setCheckable(false);
    }
    connect(mSourceFile, &QComboBox::activated, this, [this](int index) {
        CodeDocumentTab* originOrNull = activeGraphCodeTab();
        const QString file = mSourceFile->itemText(index);
        if (originOrNull != nullptr && originOrNull->GetPath() != file)
        {
            static_cast<void>(originOrNull->GetEditor().GetNavigation().Navigate({file, file, {}, 0, 0, false}));
        }
    });
    connect(&inoutArea, &CodeEditorArea::ActiveTabChanged, this, &RendererWorkspace::updateGraphCodeContext);
    connect(&inoutArea, &CodeEditorArea::SessionActionRequested, this, &RendererWorkspace::returnToBufferSizing);
    updateGraphCodeContext();
}

CodeDocumentTab* RendererWorkspace::activeGraphCodeTab() const
{
    CodeDocumentTab* tabOrNull = mShaderCodeArea.isNull() == true ? nullptr : mShaderCodeArea->GetActiveTabOrNull();
    return tabOrNull != nullptr && tabOrNull->GetSessionKey().isEmpty() == false ? tabOrNull : nullptr;
}

const CodeEditor& RendererWorkspace::activeGraphSourceEditor() const
{
    const CodeDocumentTab* tabOrNull = activeGraphCodeTab();
    return tabOrNull == nullptr ? *mSource : tabOrNull->GetEditor();
}

bool RendererWorkspace::IsGraphCodeActive() const
{
    return activeGraphCodeTab() != nullptr;
}

bool RendererWorkspace::isGraphCodeVisible() const
{
    return mShaderCodeArea.isNull() == false && mShaderCodeArea->isVisible() == true;
}

void RendererWorkspace::SetCodeFocused(bool isFocused)
{
    mIsCodeFocused = isFocused;
    mSourceFocusToggle->setText(isFocused == true ? QStringLiteral("Restore Layout") : QStringLiteral("Focus Source"));
    updateGraphCodeContext();
}

RenderGraphShaderAssistance* RendererWorkspace::activeShaderAssistance() const
{
    if (mShaderCodeArea.isNull() == true)
    {
        return mSourceAssistance;
    }
    CodeDocumentTab* tabOrNull = activeGraphCodeTab();
    return tabOrNull == nullptr ? nullptr : tabOrNull->findChild<RenderGraphShaderAssistance*>();
}

QString RendererWorkspace::shaderInterfaceFile() const
{
    if (mShaderCodeArea.isNull() == true)
    {
        return mSourceFile->currentText();
    }
    const CodeDocumentTab* tabOrNull = activeGraphCodeTab();
    if (tabOrNull == nullptr)
    {
        return {};
    }
    return tabOrNull->GetSessionKey().startsWith(GENERATED_TAB_PREFIX) == true
               ? tabOrNull->property(SHADER_API_FILE_PROPERTY).toString()
               : tabOrNull->GetPath();
}

void RendererWorkspace::updateGraphCodeContext()
{
    if (mShaderCodeArea.isNull() == true)
    {
        return;
    }
    const CodeDocumentTab* tabOrNull = activeGraphCodeTab();
    mGraphSourceHeader->setVisible(tabOrNull != nullptr);
    if (tabOrNull != nullptr && tabOrNull->GetSessionKey().startsWith(SHADER_TAB_PREFIX) == true)
    {
        const QSignalBlocker blocker(mSourceFile);
        mSourceFile->setCurrentText(tabOrNull->GetPath());
    }
    if (tabOrNull != nullptr && tabOrNull->GetSessionKey().startsWith(GENERATED_TAB_PREFIX) == true)
    {
        const QSignalBlocker blocker(mSourceFile);
        mSourceFile->setCurrentIndex(-1);
    }
    mSourceStatus->setVisible(tabOrNull != nullptr);
    updateSourceInterfaceVisibility();
    updateSourceInterfaceContents();
}

void RendererWorkspace::rememberGraphCodeOrigin()
{
    mGraphCodeOrigin = {mSelectedFile, mSelectedConfiguration, mSelectedNode};
}

bool RendererWorkspace::OpenGraphSource(const QString& file, bool isPreview)
{
    if (mDocument.GetFiles().contains(file) == false)
    {
        return false;
    }
    rememberGraphCodeOrigin();
    return openShaderCodeTab(file, isPreview) != nullptr;
}

void RendererWorkspace::returnToGraph()
{
    const RenderGraphNodeSelection origin = mGraphCodeOrigin;
    mGraphCodeOrigin = {};
    mViews->setCurrentWidget(mCanvas);
    QJsonObject graph;
    if (origin.File.isEmpty() == false && mDocument.GetGraph(origin.File, graph) == true)
    {
        static_cast<void>(SelectNode(origin.File, origin.Configuration, origin.Node));
    }
    Q_EMIT GraphWorkspaceRequested();
    FocusActiveView();
}

void RendererWorkspace::openGraphCodeLine(const QString& file, foundation::int32 line, foundation::int32 column)
{
    CodeDocumentTab* originOrNull = isGraphCodeVisible() == true ? activeGraphCodeTab() : nullptr;
    const bool isInCurrentFile = originOrNull != nullptr && originOrNull->GetPath() == file;
    if (originOrNull == nullptr)
    {
        rememberGraphCodeOrigin();
    }
    else if (isInCurrentFile == false)
    {
        originOrNull->GetEditor().GetNavigation().RememberCurrent();
    }
    CodeDocumentTab* tabOrNull = openShaderCodeTab(file, false);
    if (tabOrNull != nullptr)
    {
        tabOrNull->GetEditor().GoToLine(line, column, isInCurrentFile);
    }
}

void RendererWorkspace::refreshGraphCodeDiagnostics()
{
    for (CodeDocumentTab* tab : mShaderCodeArea->GetTabs())
    {
        if (tab->GetSessionKey().startsWith(SHADER_TAB_PREFIX) == true)
        {
            applySourceDiagnostics(tab->GetEditor(), tab->GetPath());
        }
    }
}

void RendererWorkspace::setUpGraphCodeFormatting(CodeDocumentTab& inoutTab)
{
    CodeEditor& editor = inoutTab.GetEditor();
    if (editor.GetLanguage() == CodeLanguage::Lusl)
    {
        editor.SetCanFormat(true);
        connect(&editor, &CodeEditor::FormatRequested, &inoutTab, [this, &inoutTab]() {
            mSourceStatus->setText(FormatLuslDocument(inoutTab.GetController()));
        });
        return;
    }
    if (editor.GetLanguage() != CodeLanguage::Slang)
    {
        return;
    }
    CodeFormatter* formatter = new CodeFormatter(inoutTab.GetController(), &inoutTab);
    editor.SetCanFormat(CodeFormatter::IsAvailable());
    connect(&editor, &CodeEditor::FormatRequested, &inoutTab, [this, &inoutTab, formatter]() {
        const QString path = QDir(mDocument.GetPackageRoot()).filePath(inoutTab.GetPath());
        const bool hasStarted = formatter->Start(MakeShaderFormatter(FindShaderFormatter(), path));
        inoutTab.GetEditor().SetCanFormat(hasStarted == false);
        mSourceStatus->setText(formatter->GetMessage());
    });
    connect(formatter, &CodeFormatter::Finished, &inoutTab, [this, &editor](bool, const QString& message) {
        mSourceStatus->setText(message);
        editor.SetCanFormat(CodeFormatter::IsAvailable());
    });
    connect(&inoutTab, &CodeDocumentTab::SessionClosed, formatter, &CodeFormatter::Cancel);
    connect(&editor, &CodeEditor::ChooseFormatterRequested, &inoutTab, [&inoutTab]() {
        const QString program = QFileDialog::getOpenFileName(&inoutTab,
                                                             QStringLiteral("Choose clang-format executable"),
                                                             FindShaderFormatter());
        if (program.isEmpty() == false)
        {
            QSettings().setValue(QStringLiteral("codeEditor/clangFormat"), program);
        }
    });
}

void RendererWorkspace::revealShaderCodeTab(CodeDocumentTab& inoutTab)
{
    Q_EMIT ShaderCodeWorkspaceRequested();
    static_cast<void>(mShaderCodeArea->ActivateTab(inoutTab));
    updateGraphCodeContext();
}

CodeDocumentTab* RendererWorkspace::openShaderCodeTab(const QString& file, bool isPreview)
{
    if (mShaderCodeArea.isNull() == true || mDocument.GetFiles().contains(file) == false)
    {
        return nullptr;
    }

    if (mGraphCodeOrigin.File.isEmpty() == true)
    {
        rememberGraphCodeOrigin();
    }
    const bool shouldPreview = isPreview == true && mDocument.IsFileDirty(file) == false;
    CodeDocumentTab& tab = mShaderCodeArea->OpenSessionTab(SHADER_TAB_PREFIX + file, file, shouldPreview);
    if (tab.GetController().IsBound() == false)
    {
        ShaderTabBinding* binding = new ShaderTabBinding(mDocument, tab);
        binding->Document.Bind(file, mEpoch);
        connect(&tab, &CodeDocumentTab::Failed, &tab, [&tab](const QString& message) {
            tab.GetBanner().ShowExplanation(message);
        });
        tab.BindSessionHost(&binding->Document);

        tab.ShowEditorControls();
        setUpGraphCodeFormatting(tab);
        CodeEditor& editor = tab.GetEditor();
        CodeNavigation& navigation = editor.GetNavigation();
        navigation.BeginSource(mDocument.GetPackageRoot(), file, mDocument.GetFiles());
        navigation.EndSource();
        navigation.SetViewHost(this, &mSource->GetNavigation());
        applySourceDiagnostics(editor, file);
        RenderGraphShaderAssistance* assistance =
            new RenderGraphShaderAssistance(mDocument, editor, tab.GetController(), mTooling, &tab);
        QWidget* controls = new QWidget(&tab);
        QHBoxLayout* row = new QHBoxLayout(controls);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(assistance, 1);
        QPushButton* include = new QPushButton(QStringLiteral("Insert Shared Include…"), controls);
        include->setObjectName(QStringLiteral("renderGraphInsertSharedIncludeInTab"));
        row->addWidget(include);
        connect(include, &QPushButton::clicked, &tab, [this, file, &tab]() {
            const QString message = insertSharedShaderInclude(tab, file);
            if (message.isEmpty() == false)
            {
                tab.GetBanner().ShowExplanation(message);
            }
        });
        QLabel* status = assistance->GetStatusLabel();
        tab.AddSessionControls(*controls, *status);
        controls->setVisible(IsRenderGraphShaderSource(file));
        status->hide();
        connect(assistance, &RenderGraphShaderAssistance::StatusTextChanged, status, [status]() {
            status->setVisible(status->text().isEmpty() == false);
        });
        assistance->Update(mProjectRoot, mEpoch, manifestFile(), file, mBuildSettings);
        connect(assistance, &RenderGraphShaderAssistance::ResourceApiChanged, &tab, [this, &tab]() {
            if (activeGraphCodeTab() == &tab)
            {
                updateSourceInterfaceContents();
            }
        });
        connect(&tab, &CodeDocumentTab::SessionClosed, assistance, [assistance]() {
            assistance->Update({}, 0, {}, {}, {});
        });
        connect(&tab, &CodeDocumentTab::Edited, this, [this, file, &tab]() {
            tab.GetBanner().Dismiss();
            sourceTextEdited(file);
        });
        connect(&editor, &CodeEditor::UndoRequested, this, [this]() { restoreShaderTabHistory(false); });
        connect(&editor, &CodeEditor::RedoRequested, this, [this]() { restoreShaderTabHistory(true); });
        mShaderCodeArea->RefreshSessionTabs();
    }
    revealShaderCodeTab(tab);
    return &tab;
}

CodeNavigation* RendererWorkspace::OpenNavigationTarget(const CodeNavigationTarget& target)
{
    if (mShaderCodeArea.isNull() == true)
    {
        return nullptr;
    }
    if (target.IsReadOnly == false)
    {
        if (target.Text.isEmpty() == false && QString::fromUtf8(mDocument.GetDraft(target.File)) != target.Text)
        {
            return nullptr;
        }
        CodeDocumentTab* tabOrNull = openShaderCodeTab(target.File, true);
        return tabOrNull == nullptr ? nullptr : &tabOrNull->GetEditor().GetNavigation();
    }

    const QString source = shaderInterfaceFile();
    const QByteArray digest = QCryptographicHash::hash(target.Text.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QString key = GENERATED_TAB_PREFIX + QString::fromLatin1(digest) + QLatin1Char(':') + source;
    CodeDocumentTab& tab = mShaderCodeArea->OpenSessionTab(key, QStringLiteral("Generated shader source.slang"), true);
    tab.setProperty(SHADER_API_FILE_PROPERTY, source);
    const QString explanation = QStringLiteral("Read-only · Generated shader source\n%1").arg(target.Label);
    if (tab.IsReadOnly() == false)
    {
        tab.SetReadOnlySource(target.Text, explanation);
    }
    else
    {
        tab.GetBanner().ShowExplanation(explanation);
    }
    CodeNavigation& navigation = tab.GetEditor().GetNavigation();
    navigation.SetViewHost(this, &mSource->GetNavigation());
    mShaderCodeArea->RefreshSessionTabs();
    revealShaderCodeTab(tab);
    return &navigation;
}

CodeNavigation* RendererWorkspace::ActivateNavigationView(CodeNavigation* viewOrNull, const QString& file)
{
    if (mShaderCodeArea.isNull() == true)
    {
        return nullptr;
    }
    for (CodeDocumentTab* tab : mShaderCodeArea->GetTabs())
    {
        if (&tab->GetEditor().GetNavigation() == viewOrNull)
        {
            revealShaderCodeTab(*tab);
            return viewOrNull;
        }
    }
    CodeDocumentTab* tabOrNull = file.isEmpty() == true ? nullptr : openShaderCodeTab(file, true);
    return tabOrNull == nullptr ? nullptr : &tabOrNull->GetEditor().GetNavigation();
}

void RendererWorkspace::refreshShaderCodeTabs()
{
    if (mShaderCodeArea.isNull() == true)
    {
        return;
    }
    const QStringList files = mDocument.GetFiles();
    for (CodeDocumentTab* tab : mShaderCodeArea->GetTabs())
    {
        if (tab->GetSessionKey().startsWith(SHADER_TAB_PREFIX) == false)
        {
            continue;
        }
        const QString& file = tab->GetPath();
        if (files.contains(file) == false)
        {
            mShaderCodeArea->CloseSessionTab(*tab);
            continue;
        }
        tab->MirrorFromHost();
        CodeNavigation& navigation = tab->GetEditor().GetNavigation();
        navigation.BeginSource(mDocument.GetPackageRoot(), file, files);
        navigation.EndSource();
        RenderGraphShaderAssistance* assistanceOrNull = tab->findChild<RenderGraphShaderAssistance*>();
        if (assistanceOrNull != nullptr)
        {
            assistanceOrNull->Update(mProjectRoot, mEpoch, manifestFile(), file, mBuildSettings);
        }
    }
    mShaderCodeArea->RefreshSessionTabs();
}

void RendererWorkspace::resetShaderCodeTabs()
{
    clearBufferSizingReturn();
    mGraphCodeOrigin = {};
    if (mShaderCodeArea.isNull() == false)
    {
        mShaderCodeArea->CloseSessionTabs();
    }
}

void RendererWorkspace::restoreShaderTabHistory(bool shouldRedo)
{
    if ((shouldRedo == true ? mDocument.Redo() : mDocument.Undo()) == false)
    {
        return;
    }
    restoreSourceHistory(false);
    restoreGraphCodeSelection(mDocument.GetHistoryTextState());
}

void RendererWorkspace::restoreGraphCodeSelection(const RenderGraphTextState& state)
{
    if (state.File.isEmpty() == false)
    {
        CodeDocumentTab* tabOrNull = openShaderCodeTab(state.File, false);
        if (tabOrNull != nullptr)
        {
            tabOrNull->GetController().RestoreSelection(state.Selection);
        }
    }
}
} // namespace ludus::editor
