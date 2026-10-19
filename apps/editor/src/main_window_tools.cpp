// MainWindow routing between tool windows and documents: Project Explorer opens
// documents, the Error List navigates to diagnostics, Properties follows the
// active document, and render-graph commands follow the Renderer document.
// The tool windows and the document well are separate components; this file
// only connects their requests to the document owners' existing operations.

#include "internal/audio_workspace.h"
#include "internal/configuration_workspace.h"
#include "internal/document_well.h"
#include "internal/error_list.h"
#include "internal/main_window.h"
#include "internal/project_explorer.h"
#include "internal/renderer_workspace.h"
#include "internal/script_workspace.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>

#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
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

namespace ludus::editor
{
namespace
{
enum PropertiesPage : foundation::uint8
{
    EMPTY_PROPERTIES_PAGE,
    WORLD_PROPERTIES_PAGE,
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
                statusBar()->showMessage(
                    QStringLiteral("No render graph in this project owns %1.").arg(QDir::toNativeSeparators(path)),
                    STATUS_MESSAGE_MILLISECONDS);
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
            static_cast<void>(mDocuments->OpenDocument(Scripts_));
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
    else
    {
        mNoProperties->setText(activeOrNull == Renderer_
                                   ? QStringLiteral("Select a node on the render-graph canvas; its fields are in the "
                                                    "Render Graph document's Inspector tab.")
                                   : QStringLiteral("The active document has no selection with properties. Select a "
                                                    "World entity to edit it here."));
        mProperties->setCurrentIndex(EMPTY_PROPERTIES_PAGE);
    }
    RenderDocumentActions();
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
                                 title(QStringLiteral("Project Settings"), {}, Controller_->State().Dirty()));
    mDocuments->SetDocumentTitle(Configuration_, title(QStringLiteral("Configuration"), {}, Configuration_->Dirty()));
}

void MainWindow::renderGraphCommands()
{
    const bool isLoaded = Renderer_->GetDocument().IsLoaded();
    const bool isIdle = Renderer_->IsJobActive() == false;
    for (QAction* action : {mValidateGraphAction, mBuildGraphAction, mBuildPreviewGraphAction, mCookGraphAction})
    {
        action->setEnabled(isLoaded == true && isIdle == true);
    }
    mLivePreviewGraphAction->setEnabled(isLoaded);
    renderDocumentTitles();
}

void MainWindow::renderGraphErrors()
{
    const QList<ErrorListEntry> entries = MakeRenderGraphErrorEntries(Renderer_->GetLastResult());
    mErrorList->ReplaceEntries(ErrorListProducer::RenderGraph, entries);
    for (const ErrorListEntry& entry : entries)
    {
        if (entry.Severity == ErrorListSeverity::Error)
        {
            // Like VS after a failed build: bring the Error List forward without taking focus.
            mErrorListDock->show();
            mErrorListDock->raise();
            break;
        }
    }
    renderGraphCommands();
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
