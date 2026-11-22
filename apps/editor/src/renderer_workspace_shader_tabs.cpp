#include "internal/renderer_workspace.h"

#include "internal/code_conflict_banner.h"
#include "internal/code_document_tab.h"
#include "internal/code_editor_area.h"
#include "internal/render_graph_shader_assistance.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QWidget>

namespace ludus::editor
{
namespace
{
const QString SHADER_TAB_PREFIX = QStringLiteral("shader:");
const QString GENERATED_TAB_PREFIX = QStringLiteral("generated:");

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
    mSource->GetNavigation().SetViewHost(this);
    connect(&inoutArea, &CodeEditorArea::SessionActionRequested, this, &RendererWorkspace::returnToBufferSizing);
}

void RendererWorkspace::revealShaderCodeTab(CodeDocumentTab& inoutTab)
{
    Q_EMIT ShaderCodeWorkspaceRequested();
    static_cast<void>(mShaderCodeArea->ActivateTab(inoutTab));
}

CodeDocumentTab* RendererWorkspace::openShaderCodeTab(const QString& file, bool isPreview)
{
    if (mShaderCodeArea.isNull() == true || mDocument.GetFiles().contains(file) == false)
    {
        return nullptr;
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

        CodeEditor& editor = tab.GetEditor();
        CodeNavigation& navigation = editor.GetNavigation();
        navigation.BeginSource(mDocument.GetPackageRoot(), file, mDocument.GetFiles());
        navigation.EndSource();
        navigation.SetViewHost(this, &mSource->GetNavigation());
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
        status->hide();
        connect(assistance, &RenderGraphShaderAssistance::StatusTextChanged, status, [status]() {
            status->setVisible(status->text().isEmpty() == false);
        });
        assistance->Update(mProjectRoot, mEpoch, manifestFile(), file, mBuildSettings);
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

    const QByteArray digest = QCryptographicHash::hash(target.Text.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QString key = GENERATED_TAB_PREFIX + QString::fromLatin1(digest);
    CodeDocumentTab& tab = mShaderCodeArea->OpenSessionTab(key, QStringLiteral("Generated shader source.slang"), true);
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
    if (viewOrNull == &mSource->GetNavigation())
    {
        if (mSourceFile->findText(file) < 0)
        {
            return nullptr;
        }
        openSourceLine(file, 1);
        Q_EMIT GraphWorkspaceRequested();
        return viewOrNull;
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
    const RenderGraphTextState state = mDocument.GetHistoryTextState();
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
