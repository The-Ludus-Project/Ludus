// MainWindow routing between tool windows and documents: Project Explorer opens
// documents, the Error List navigates to diagnostics, Properties follows the
// active document, render-graph commands follow the Renderer document, and
// Tools > Options edits the Renderer document's build settings.
// The tool windows and the document well are separate components; this file
// only connects their requests to the document owners' existing operations.

#include "internal/activity_chip.h"
#include "internal/audio_workspace.h"
#include "internal/configuration_workspace.h"
#include "internal/content_browser.h"
#include "internal/controller.h"
#include "internal/document_well.h"
#include "internal/editor_commands.h"
#include "internal/error_list.h"
#include "internal/external_code_editor.h"
#include "internal/main_window.h"
#include "internal/options_dialog.h"
#include "internal/project_explorer.h"
#include "internal/render_graph_job.h"
#include "internal/renderer_workspace.h"
#include "internal/script_workspace.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>

#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QLatin1StringView>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolBar>

namespace ludus::editor
{
namespace
{
enum PropertiesPage : foundation::uint8
{
    EMPTY_PROPERTIES_PAGE,
    WORLD_PROPERTIES_PAGE,
    RENDER_GRAPH_PROPERTIES_PAGE,
};
constexpr foundation::int32 STATUS_MESSAGE_MILLISECONDS = 8000;
constexpr QLatin1StringView RENDER_GRAPH_SUFFIX{".lurender.json"};
constexpr QLatin1StringView MAIN_RENDER_GRAPH{"main.lurender.json"};

/// Root render graph of the package that contains a shader file: the nearest folder,
/// from the shader's own up to root, holding a .lurender.json (main.lurender.json first).
// The shader file and the project root are distinct named paths.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] QString FindPackageGraph(const QString& shader, const QString& root)
{
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    QDir folder = QFileInfo(shader).absoluteDir();
    while (canonicalRoot.isEmpty() == false &&
           (folder.canonicalPath() == canonicalRoot ||
            folder.canonicalPath().startsWith(canonicalRoot + QLatin1Char('/')) == true))
    {
        if (folder.exists(MAIN_RENDER_GRAPH) == true)
        {
            return folder.filePath(MAIN_RENDER_GRAPH);
        }
        const QStringList graphs =
            folder.entryList({QStringLiteral("*") + RENDER_GRAPH_SUFFIX}, QDir::Files | QDir::NoSymLinks, QDir::Name);
        if (graphs.isEmpty() == false)
        {
            return folder.filePath(graphs.first());
        }
        if (folder.cdUp() == false)
        {
            break;
        }
    }
    return {};
}
} // namespace

void MainWindow::openProjectItem(ProjectItemKind kind, const QString& path)
{
    switch (kind)
    {
        case ProjectItemKind::Folder:
            return;
        case ProjectItemKind::ProjectSettings:
            static_cast<void>(mDocuments->OpenDocument(ProjectSettings_));
            return;
        case ProjectItemKind::World:
            // Opening another World asks about unsaved changes first, like World > Open.
            if (QFileInfo(path).canonicalFilePath() != QFileInfo(World_->Path()).canonicalFilePath() &&
                (World_->ConfirmDiscard() == false || World_->Open(path) == false))
            {
                return;
            }
            static_cast<void>(mDocuments->OpenDocument(World_));
            return;
        case ProjectItemKind::RenderGraph:
        case ProjectItemKind::Shader: {
            // A shader opens the render-graph package that owns it; its source editor
            // is the Render Graph document's Source tab.
            const QString graph =
                kind == ProjectItemKind::RenderGraph ? path : FindPackageGraph(path, ProjectLayoutRoot_);
            const RenderGraphDocument& document = Renderer_->GetDocument();
            const QString current = document.IsLoaded() == true
                                        ? QDir(document.GetPackageRoot()).filePath(document.GetRootGraph())
                                        : QString();
            const bool isCurrent = graph.isEmpty() == false &&
                                   QFileInfo(graph).canonicalFilePath() == QFileInfo(current).canonicalFilePath();
            if (graph.isEmpty() == true)
            {
                ExternalCodeEditor editor;
                QString error;
                if (editor.Open(this, path, error) == false && error.isEmpty() == false)
                {
                    statusBar()->showMessage(error, STATUS_MESSAGE_MILLISECONDS);
                }
                return;
            }
            if (isCurrent == false && (Renderer_->ConfirmDiscard() == false || Renderer_->OpenGraph(graph) == false))
            {
                return;
            }
            static_cast<void>(mDocuments->OpenDocument(Renderer_));
            return;
        }
        case ProjectItemKind::Scripts:
            if (path.isEmpty() == false)
            {
                ExternalCodeEditor editor;
                QString error;
                if (editor.Open(this, path, error) == false && error.isEmpty() == false)
                {
                    statusBar()->showMessage(error, STATUS_MESSAGE_MILLISECONDS);
                }
            }
            else
            {
                static_cast<void>(mDocuments->OpenDocument(Scripts_));
            }
            return;
        case ProjectItemKind::Audio:
            static_cast<void>(mDocuments->OpenDocument(mAudioDocument));
            return;
        case ProjectItemKind::Content:
            showToolWindow(mContentDock);
            return;
        case ProjectItemKind::Configuration:
            static_cast<void>(mDocuments->OpenDocument(Configuration_));
            return;
    }
}

void MainWindow::activateErrorEntry(const ErrorListEntry& entry)
{
    if (entry.Producer == ErrorListProducer::RenderPreview)
    {
        static_cast<void>(mDocuments->OpenDocument(Renderer_));
        showToolWindow(mRenderPreviewDock);
        return;
    }
    if (entry.Producer == ErrorListProducer::RenderGraph)
    {
        // RendererWorkspace owns the authored-location mapping (graph node, source line or
        // the manifest record of a generated wrapper); its rows match the Error List's.
        static_cast<void>(mDocuments->OpenDocument(Renderer_));
        if (Renderer_->ActivateDiagnostic(entry.SourceIndex) == false)
        {
            statusBar()->showMessage(QStringLiteral("This diagnostic has no authored location in the open graph."),
                                     STATUS_MESSAGE_MILLISECONDS);
        }
        return;
    }
    // Build diagnostics refer to arbitrary project files; select the line in Output.
    showToolWindow(OutputDock_);
    const QTextBlock block = Output_->document()->findBlockByNumber(entry.SourceIndex);
    if (block.isValid() == true)
    {
        QTextCursor cursor(block);
        cursor.select(QTextCursor::LineUnderCursor);
        Output_->setTextCursor(cursor);
        Output_->centerCursor();
    }
}

void MainWindow::renderActiveDocument()
{
    QWidget* activeOrNull = mDocuments->GetActiveDocumentOrNull();
    if (activeOrNull == World_)
    {
        mProperties->setCurrentIndex(WORLD_PROPERTIES_PAGE);
    }
    else if (activeOrNull == Renderer_)
    {
        // The Renderer document keeps its inspector current with its own selection.
        mProperties->setCurrentIndex(RENDER_GRAPH_PROPERTIES_PAGE);
        collapseResolvedGraphErrors();
    }
    else
    {
        mNoProperties->setText(QStringLiteral("The active document has no selection with properties. Select a "
                                              "World entity or a render-graph node to edit it here."));
        mProperties->setCurrentIndex(EMPTY_PROPERTIES_PAGE);
    }
    RenderDocumentActions();
    updateContextStrip();
}

void MainWindow::renderActivity()
{
    if (mActivityBarOrNull == nullptr)
    {
        return;
    }
    mActivityBarOrNull->SetActivity(
        GetActivityLabel(Controller_->State().OperationPhase, Content_->Importing(), Renderer_->IsJobActive()),
        -1.0);
    mActivityBarOrNull->SetCounts(mErrorList->GetCount(ErrorListSeverity::Error),
                                  mErrorList->GetCount(ErrorListSeverity::Warning));
}

void MainWindow::updateContextStrip()
{
    const bool isPaired = mDocuments->IsDocumentCurrent(Renderer_) == true &&
                          mDocuments->IsDocumentCurrent(World_) == true &&
                          mDocuments->GetGroupIndex(Renderer_) != mDocuments->GetGroupIndex(World_) &&
                          mDocuments->GetSplitOrientation() == Qt::Horizontal;
    Renderer_->SetSourceCompanionLayout(isPaired);
    World_->SetChromeVisible(mIsViewportFocused == false && isPaired == false);
    const bool shouldShow = mIsViewportFocused == false && mDocuments->IsDocumentCurrent(Renderer_) == true;
    if (mRenderGraphToolbar->isHidden() == shouldShow)
    {
        mRenderGraphToolbar->setVisible(shouldShow);
    }
}

void MainWindow::renderDocumentTitles()
{
    // Tabs show the document's file and a `*` while it has unsaved changes, like VS.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): fallback name and file path are distinct.
    const auto title = [](const QString& name, const QString& path, bool isDirty) {
        const QString file = QFileInfo(path).fileName();
        return (file.isEmpty() == true ? name : file) + (isDirty == true ? QStringLiteral("*") : QString());
    };
    const RenderGraphDocument& graph = Renderer_->GetDocument();
    mDocuments->SetDocumentTitle(World_, title(QStringLiteral("World"), World_->Path(), World_->Dirty()));
    mDocuments->SetDocumentTitle(Renderer_,
                                 title(QStringLiteral("Render Graph"),
                                       graph.IsLoaded() == true ? graph.GetRootGraph() : QString(),
                                       Renderer_->IsDirty()));
    mDocuments->SetDocumentTitle(Scripts_, title(QStringLiteral("Scripts"), {}, Scripts_->Dirty()));
    mDocuments->SetDocumentTitle(ProjectSettings_,
                                 title(QStringLiteral("Project Settings"),
                                       {},
                                       Controller_->State().Dirty() == true || Renderer_->IsDirty() == true));
    mDocuments->SetDocumentTitle(Configuration_, title(QStringLiteral("Configuration"), {}, Configuration_->Dirty()));
}

void MainWindow::renderGraphCommands()
{
    mCodeViewportAction->setEnabled(Renderer_->GetDocument().IsLoaded() == true &&
                                    mDocuments->IsDocumentAvailable(Renderer_) == true &&
                                    mDocuments->IsDocumentAvailable(World_) == true);
    mRemoveUnusedShadersAction->setEnabled(Renderer_->CanRemoveUnusedShaders());
    const bool canBuild = Renderer_->CanBuild();
    for (QAction* action : {mValidateGraphAction, mBuildGraphAction, mBuildPreviewGraphAction, mCookGraphAction})
    {
        action->setEnabled(canBuild);
    }
    mNewGraphAction->setEnabled(Renderer_->CanCreateGraph());
    mOpenGraphAction->setEnabled(Renderer_->CanCreateGraph());
    mCancelGraphAction->setEnabled(Renderer_->IsJobActive());
    mLivePreviewGraphAction->setEnabled(Renderer_->GetDocument().IsLoaded());
    renderDocumentTitles();
}

void MainWindow::showOptions()
{
    OptionsDialog* dialog = new OptionsDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->SetBuildSettings(Renderer_->GetBuildSettings());
    dialog->SetResolvedBuildTools(Renderer_->GetResolvedSdk(), Renderer_->GetResolvedBuildTools());
    // Tool overrides belong to the project they were chosen for; a project change while the
    // dialog is open discards them.
    const foundation::uint64 epoch = Controller_->State().ProjectEpoch;
    connect(dialog, &QDialog::accepted, this, [this, dialog, epoch]() {
        if (Controller_->State().ProjectEpoch == epoch)
        {
            Renderer_->SetBuildSettings(dialog->GetBuildSettings());
        }
    });
    dialog->open();
}

void MainWindow::renderGraphErrors()
{
    const RenderGraphJobResult& result = Renderer_->GetLastResult();
    if (result.IsSuccessful == false && result.Code == QStringLiteral("MissingTools"))
    {
        Controller_->ReportProjectSetupProblem(result.Message);
    }
    // ErrorList::ProblemsReported brings the list forward for new errors and warnings.
    const QString problem = Renderer_->GetRenderProblem();
    const RenderGraphDocument& document = Renderer_->GetDocument();
    const QString file =
        document.IsLoaded() == true ? QDir(document.GetPackageRoot()).filePath(document.GetRootGraph()) : QString{};
    mErrorList->ReplaceEntries(ErrorListProducer::RenderPreview,
                               MakeRenderPreviewErrorEntries(Renderer_->GetActivationProblem(), file));
    // A successful compile does not establish that the viewport accepted its generation.
    mErrorList->ReplaceEntries(ErrorListProducer::RenderGraph,
                               Renderer_->GetDiagnosticCount() == 0 ||
                                       (result.IsSuccessful == true && problem.isEmpty() == false)
                                   ? QList<ErrorListEntry>{}
                                   : MakeRenderGraphErrorEntries(result));
    renderGraphCommands();
    collapseResolvedGraphErrors();
}

void MainWindow::collapseResolvedGraphErrors()
{
    if (mDocuments->GetActiveDocumentOrNull() != Renderer_ || mErrorList->GetCount(ErrorListSeverity::Error) != 0 ||
        mErrorList->GetCount(ErrorListSeverity::Warning) != 0 || mErrorListDock->visibleRegion().isEmpty() == true)
    {
        return;
    }
    // Only collapse the bottom tab group when Error List is the visible tab. An explicitly
    // selected Output, Timing or Profiling view remains open; new problems reveal Error List.
    for (QDockWidget* dock : {mErrorListDock, OutputDock_, TimingDock_, mProfilingDock})
    {
        dock->hide();
    }
}

void MainWindow::revealErrorList()
{
    if (mIsViewportFocused == true)
    {
        // Focus Viewport stays in place; the status bar says where to look.
        statusBar()->showMessage(QStringLiteral("New errors or warnings are in the Error List (%1).")
                                     .arg(QKeySequence(QString::fromUtf8(VIEW_ERROR_LIST_COMMAND.Shortcut))
                                              .toString(QKeySequence::NativeText)),
                                 STATUS_MESSAGE_MILLISECONDS);
        return;
    }
    // Like Visual Studio after a failed build: bring the Error List forward without taking focus.
    openToolWindow(mErrorListDock);
}

void MainWindow::renderBuildErrors()
{
    const QString output = Output_->toPlainText();
    if (output == mParsedBuildOutput)
    {
        return;
    }
    mParsedBuildOutput = output;
    mErrorList->ReplaceEntries(ErrorListProducer::Build, ParseBuildErrorEntries(output));
}
} // namespace ludus::editor
