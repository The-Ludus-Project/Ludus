#include "internal/main_window.h"
#include "internal/audio_workspace.h"
#include "internal/configuration_workspace.h"
#include "internal/content_browser.h"
#include "internal/document_well.h"
#include "internal/editor_commands.h"
#include "internal/editor_files.h"
#include "internal/error_list.h"
#include "internal/project_creation_dialog.h"
#include "internal/project_explorer.h"
#include "internal/project_setup_dialog.h"
#include "internal/renderer_workspace.h"
#include "internal/script_workspace.h"
#include "internal/timing_panel.h"
#include "internal/workspace_style.h"
#include "internal/world_workspace.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QWidget>

#include <ludus/foundation/base/core.h>

#include <cstring>

namespace ludus::editor
{
namespace
{
// Build output streams in many small chunks; parse it for the Error List once it pauses.
constexpr foundation::int32 BUILD_ERRORS_DELAY_MILLISECONDS = 250;
} // namespace

MainWindow::MainWindow(EditorController* controller, QWidget* parent, const QString& workspaceSettingsFile)
    : QMainWindow(parent), Controller_(controller), WorkspaceSettingsFile_(workspaceSettingsFile),
      TimingDock_(new QDockWidget(QStringLiteral("Timing"), this)), Timing_(new TimingPanel(controller, TimingDock_)),
      mDocuments(new DocumentWell(this)), mAudioDocument(new QScrollArea(this)),
      mProjectExplorerDock(new QDockWidget(QStringLiteral("Project Explorer"), this)),
      mProjectExplorer(new ProjectExplorer(mProjectExplorerDock)),
      mPropertiesDock(new QDockWidget(QStringLiteral("Properties"), this)),
      mProperties(new QStackedWidget(mPropertiesDock)), mNoProperties(new QLabel(mProperties)),
      mErrorListDock(new QDockWidget(QStringLiteral("Error List"), this)), mErrorList(new ErrorList(mErrorListDock)),
      mContentDock(new QDockWidget(QStringLiteral("Content"), this)),
      mRenderPreviewDock(new QDockWidget(QStringLiteral("Render Preview"), this)),
      mStandardToolbar(new QToolBar(QStringLiteral("Standard"), this)),
      mRenderGraphToolbar(new QToolBar(QStringLiteral("Render Graph"), this)),
      mNewGraphAction(CreateEditorCommand(RENDER_GRAPH_NEW_COMMAND, this)),
      mOpenGraphAction(CreateEditorCommand(RENDER_GRAPH_OPEN_COMMAND, this)),
      mValidateGraphAction(CreateEditorCommand(RENDER_GRAPH_VALIDATE_COMMAND, this)),
      mBuildGraphAction(CreateEditorCommand(RENDER_GRAPH_BUILD_COMMAND, this)),
      mBuildPreviewGraphAction(CreateEditorCommand(RENDER_GRAPH_BUILD_PREVIEW_COMMAND, this)),
      mLivePreviewGraphAction(CreateEditorCommand(RENDER_GRAPH_LIVE_PREVIEW_COMMAND, this)),
      mCookGraphAction(CreateEditorCommand(RENDER_GRAPH_COOK_COMMAND, this)),
      mCancelGraphAction(CreateEditorCommand(RENDER_GRAPH_CANCEL_COMMAND, this)),
      mOptionsAction(CreateEditorCommand(TOOLS_OPTIONS_COMMAND, this)),
      mProjectSettingsAction(CreateEditorCommand(PROJECT_SETTINGS_COMMAND, this)),
      mResetLayoutAction(CreateEditorCommand(WINDOW_RESET_LAYOUT_COMMAND, this))
{
    setWindowTitle(QStringLiteral("Ludus Editor"));
    setWindowIcon(QIcon(QStringLiteral(":/ludus/ludus-icon.png")));
    ApplyWorkspaceStyle(this);
    BuildUi();
    BuildMenus();
    InitializeWorkspace();
    connect(mDocuments, &DocumentWell::ActiveDocumentChanged, this, &MainWindow::renderActiveDocument);
    connect(mProjectExplorer, &ProjectExplorer::OpenRequested, this, &MainWindow::openProjectItem);
    connect(mErrorList, &ErrorList::EntryActivated, this, &MainWindow::activateErrorEntry);
    mBuildErrorsTimer.setSingleShot(true);
    mBuildErrorsTimer.setInterval(BUILD_ERRORS_DELAY_MILLISECONDS);
    connect(&mBuildErrorsTimer, &QTimer::timeout, this, &MainWindow::renderBuildErrors);
    connect(Audio_, &AudioWorkspace::DocumentChanged, this, &MainWindow::RenderDocumentActions);
    connect(Content_, &ContentWorkspace::BusyChanged, this, &MainWindow::RenderCapabilities);
    connect(Content_, &ContentWorkspace::CatalogChanged, Audio_, &AudioWorkspace::RefreshCatalog);
    connect(Audio_, &AudioWorkspace::ImportRequested, this, [this]() {
        showToolWindow(mContentDock);
        Content_->ChooseImport();
    });
    connect(Content_, &ContentWorkspace::OpenAudio, this, [this](const QString& id) {
        if (Audio_->OpenResource(id))
        {
            static_cast<void>(mDocuments->OpenDocument(mAudioDocument));
        }
    });
    connect(World_, &WorldWorkspace::DocumentChanged, this, &MainWindow::RenderDocumentActions);
    connect(World_, &WorldWorkspace::PlayRequested, this, [this]() {
        const auto& state = Controller_->State();
        const auto base = QFileInfo(state.DescriptorPath).absolutePath();
        const auto source = QDir(base).absoluteFilePath(state.Saved.SourceDir);
        const auto cwd = QDir(state.Saved.Version == 2 ? base : source).absoluteFilePath(state.Saved.RunCwd);
        bool matches = false;
        for (int i = 0; i + 1 < state.Saved.RunArgs.size(); ++i)
        {
            if (state.Saved.RunArgs[i] == QStringLiteral("--level") &&
                QFileInfo(QDir(cwd).absoluteFilePath(state.Saved.RunArgs[i + 1])).canonicalFilePath() == World_->Path())
            {
                matches = true;
            }
        }
        if (state.Saved.Target != QStringLiteral("ludus_world_demo") || matches == false)
        {
            StatusLabel_->setText(
                QStringLiteral("For this World adapter, set the executable target to ludus_world_demo and run "
                               "arguments to --level and this World path, then save Project settings."));
            return;
        }
        LaunchAfterPreview(LaunchAction::Run);
    });
    connect(Scripts_, &ScriptWorkspace::DocumentChanged, this, &MainWindow::RenderDocumentActions);
    connect(Renderer_, &RendererWorkspace::DocumentChanged, this, &MainWindow::RenderDocumentActions);
    connect(Renderer_, &RendererWorkspace::DocumentChanged, this, &MainWindow::renderGraphCommands);
    connect(Renderer_, &RendererWorkspace::JobFinished, this, &MainWindow::renderGraphErrors);
    connect(Renderer_, &RendererWorkspace::CommandsChanged, this, &MainWindow::renderGraphCommands);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget* before, QWidget* after) {
        LastEditedField_ = nullptr;
        disconnect(FocusUndoConnection_);
        disconnect(FocusRedoConnection_);
        if (auto* line = qobject_cast<QLineEdit*>(after))
        {
            FocusUndoConnection_ = connect(line, &QLineEdit::textChanged, this, &MainWindow::RenderDocumentActions);
        }
        else if (auto* plain = qobject_cast<QPlainTextEdit*>(after))
        {
            FocusUndoConnection_ =
                connect(plain, &QPlainTextEdit::undoAvailable, this, &MainWindow::RenderDocumentActions);
            FocusRedoConnection_ =
                connect(plain, &QPlainTextEdit::redoAvailable, this, &MainWindow::RenderDocumentActions);
        }
        if (Rendering_ == false && before == TargetBox_->lineEdit())
        {
            RenderFields();
        }
        RenderDocumentActions();
    });
    connect(Controller_, &EditorController::StateChanged, this, &MainWindow::OnStateChanged);
    OnStateChanged();
}

MainWindow::~MainWindow()
{
    // QWidget teardown changes focus and destroys children before QObject
    // disconnects receivers. Stop member callbacks while MainWindow is alive.
    disconnect(qApp, nullptr, this, nullptr);
    disconnect(Controller_, nullptr, this, nullptr);
    disconnect(FocusUndoConnection_);
    disconnect(FocusRedoConnection_);
    for (auto* child : findChildren<QObject*>())
    {
        disconnect(child, nullptr, this, nullptr);
    }
}

void MainWindow::BuildMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    OpenAction_ = fileMenu->addAction(QStringLiteral("&Open Project..."));
    OpenAction_->setShortcut(QKeySequence::Open);
    auto* samples = fileMenu->addMenu(QStringLiteral("Open &Sample"));
    samples->setObjectName(QStringLiteral("openSampleMenu"));
    for (const auto& id : {QStringLiteral("cornell-box"),
                           QStringLiteral("live-edit-game"),
                           QStringLiteral("scripted-game"),
                           QStringLiteral("editor-sdk-project")})
    {
        auto* action = samples->addAction(id);
        connect(action, &QAction::triggered, this, [this, id]() {
            if (Controller_->Caps().CanOpen == false)
            {
                return;
            }
            QString descriptor;
            if (CopyEditorSample(id, descriptor) == false)
            {
                QMessageBox::warning(this,
                                     QStringLiteral("Open Sample"),
                                     QStringLiteral("Could not create a writable copy of the sample."));
                return;
            }
            OpenProjectPath(descriptor);
        });
    }
    // Render graphs open as documents, like a file in Visual Studio's File > New and Open.
    fileMenu->addSeparator();
    fileMenu->addAction(mNewGraphAction);
    fileMenu->addAction(mOpenGraphAction);
    connect(mNewGraphAction, &QAction::triggered, this, [this]() {
        if (Renderer_->PromptNewGraph() == true)
        {
            static_cast<void>(mDocuments->OpenDocument(Renderer_));
        }
    });
    connect(mOpenGraphAction, &QAction::triggered, this, [this]() {
        if (Renderer_->PromptOpenGraph() == true)
        {
            static_cast<void>(mDocuments->OpenDocument(Renderer_));
        }
    });
    fileMenu->addSeparator();
    RecentMenu_ = fileMenu->addMenu(QStringLiteral("Recent &Projects"));
    RecentMenu_->setObjectName(QStringLiteral("recentProjectsMenu"));
    connect(RecentMenu_, &QMenu::aboutToShow, this, &MainWindow::RenderRecentProjects);
    ClearRecentAction_ = fileMenu->addAction(QStringLiteral("Clear Recent Projects"));
    ClearRecentAction_->setObjectName(QStringLiteral("clearRecentProjectsAction"));
    connect(ClearRecentAction_, &QAction::triggered, Controller_, &EditorController::ClearRecentProjects);
    CloseProjectAction_ = fileMenu->addAction(QStringLiteral("Close Project"));
    CloseProjectAction_->setObjectName(QStringLiteral("closeProjectAction"));
    CloseProjectAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")));
    connect(CloseProjectAction_, &QAction::triggered, this, &MainWindow::OnCloseProject);
    SaveAction_ = fileMenu->addAction(QStringLiteral("&Save Project Settings"));
    SaveAction_->setObjectName(QStringLiteral("document.save"));
    SaveAction_->setShortcut(QKeySequence::Save);
    ReloadAction_ = fileMenu->addAction(QStringLiteral("&Reload"));
    auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    UndoAction_ = editMenu->addAction(QStringLiteral("&Undo"));
    RedoAction_ = editMenu->addAction(QStringLiteral("&Redo"));
    UndoAction_->setObjectName(QStringLiteral("document.undo"));
    RedoAction_->setObjectName(QStringLiteral("document.redo"));
    UndoAction_->setShortcut(QKeySequence::Undo);
    RedoAction_->setShortcuts(QKeySequence::keyBindings(QKeySequence::Redo));
    connect(UndoAction_, &QAction::triggered, this, &MainWindow::OnUndoRequested);
    connect(RedoAction_, &QAction::triggered, this, &MainWindow::OnRedoRequested);
    // Visual Studio order: File, Edit, View, Project, Build, ..., Window, Help.
    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));

    QMenu* projectMenu = menuBar()->addMenu(QStringLiteral("&Project"));
    projectMenu->addAction(mProjectSettingsAction);
    connect(mProjectSettingsAction, &QAction::triggered, this, [this]() {
        static_cast<void>(mDocuments->OpenDocument(ProjectSettings_));
    });
    NewProjectAction_ = new QAction(QStringLiteral("&New Project..."), this);
    fileMenu->insertAction(OpenAction_, NewProjectAction_);
    NewProjectAction_->setShortcut(QKeySequence::New);
    // The Project menu shares the same action and capability gate.
    projectMenu->addAction(NewProjectAction_);
    NewProjectAction_->setObjectName(QStringLiteral("newProjectAction"));
    CheckSetupAction_ = projectMenu->addAction(QStringLiteral("&Check Setup"));
    CheckSetupAction_->setObjectName(QStringLiteral("checkSetupAction"));
    SetupProjectAction_ = projectMenu->addAction(QStringLiteral("&Repair Project Setup..."));
    SetupProjectAction_->setObjectName(QStringLiteral("setupProjectAction"));
    connect(NewProjectAction_, &QAction::triggered, this, &MainWindow::OnNewProject);
    connect(CheckSetupAction_, &QAction::triggered, Controller_, &EditorController::CheckProjectSetup);
    connect(SetupProjectAction_, &QAction::triggered, this, &MainWindow::OnSetupProject);

#if defined(Q_OS_WASM)
    SaveAction_->setText(QStringLiteral("Save and &Download Project Folder"));
    ExportProjectAction_ = fileMenu->addAction(QStringLiteral("Download Project Folder..."));
    connect(ExportProjectAction_, &QAction::triggered, this, [this]() {
        if ((Scripts_->Dirty() && Scripts_->Save() == false))
        {
            return;
        }
        if (Controller_->Caps().CanSave)
        {
            Controller_->Save();
        }
        if (Controller_->State().Dirty() == false)
        {
            (void)DownloadEditorProject(this, Controller_->State().DescriptorPath);
        }
    });
#endif
    QMenu* buildMenu = menuBar()->addMenu(QStringLiteral("&Build"));
    ConfigureAction_ = buildMenu->addAction(QStringLiteral("&Configure / Refresh Targets"));
    BuildAction_ = buildMenu->addAction(QStringLiteral("&Build"));
    BuildRunAction_ = buildMenu->addAction(QStringLiteral("Build and &Run"));
    BuildDebugAction_ = buildMenu->addAction(QStringLiteral("Build && &Debug in RAD..."));
    BuildDebugAction_->setObjectName(QStringLiteral("buildDebugAction"));
    BuildDebugAction_->setToolTip(QStringLiteral("Debug native games in RAD. Optional setup is offered when missing."));
    BuildDebugAction_->setShortcut(QKeySequence(Qt::Key_F5));
    connect(BuildDebugAction_, &QAction::triggered, this, [this]() { LaunchAfterPreview(LaunchAction::Debug); });
    connect(Controller_, &EditorController::DebuggerSetupRequested, this, &MainWindow::OnDebuggerSetupRequested);
    StopAction_ = buildMenu->addAction(QStringLiteral("&Stop"));
    // Render-graph commands: one QAction each, shared with the Render Graph toolbar.
    buildMenu->addSeparator();
    for (QAction* action : {mValidateGraphAction,
                            mBuildGraphAction,
                            mBuildPreviewGraphAction,
                            mLivePreviewGraphAction,
                            mCookGraphAction,
                            mCancelGraphAction})
    {
        buildMenu->addAction(action);
    }
    connect(mValidateGraphAction, &QAction::triggered, this, [this]() {
        static_cast<void>(Renderer_->Validate());
        renderGraphCommands();
    });
    connect(mBuildGraphAction, &QAction::triggered, this, [this]() {
        static_cast<void>(Renderer_->Build());
        renderGraphCommands();
    });
    connect(mBuildPreviewGraphAction, &QAction::triggered, this, [this]() {
        // The explicit gesture shows its result; Live Preview rebuilds never raise the window.
        mRenderPreviewDock->show();
        mRenderPreviewDock->raise();
        static_cast<void>(Renderer_->BuildAndPreview());
        renderGraphCommands();
    });
    connect(mCookGraphAction, &QAction::triggered, this, [this]() {
        static_cast<void>(Renderer_->CookRuntime());
        renderGraphCommands();
    });
    connect(mCancelGraphAction, &QAction::triggered, this, [this]() {
        Renderer_->CancelJob();
        renderGraphCommands();
    });
    // The Renderer document owns Live Preview; the toggle shows its state, including when
    // Cancel or a project change turns it off or an enable request is refused.
    connect(mLivePreviewGraphAction, &QAction::toggled, this, [this](bool isEnabled) {
        Renderer_->SetLivePreview(isEnabled);
        const QSignalBlocker blocker(mLivePreviewGraphAction);
        mLivePreviewGraphAction->setChecked(Renderer_->IsLivePreviewEnabled());
    });
    connect(Renderer_, &RendererWorkspace::LivePreviewChanged, this, [this](bool isEnabled) {
        const QSignalBlocker blocker(mLivePreviewGraphAction);
        mLivePreviewGraphAction->setChecked(isEnabled);
    });

    // Toolbars use short text labels (QAction::iconText) so every command stays readable
    // in a 1280-pixel window; menus and tooltips keep the full names.
    mStandardToolbar->setObjectName(QStringLiteral("standardToolbar"));
    mStandardToolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addToolBar(mStandardToolbar);
    OpenAction_->setIconText(QStringLiteral("Open"));
    SaveAction_->setIconText(QStringLiteral("Save"));
    for (QAction* action : {OpenAction_, SaveAction_, UndoAction_, RedoAction_})
    {
        mStandardToolbar->addAction(action);
    }
    GameToolbar_ = addToolBar(QStringLiteral("Game"));
    GameToolbar_->setObjectName(QStringLiteral("gameToolbar"));
    GameToolbar_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    BuildRunAction_->setIconText(QStringLiteral("Run"));
    BuildDebugAction_->setIconText(QStringLiteral("Debug"));
    GameToolbar_->addAction(BuildAction_);
    GameToolbar_->addAction(BuildRunAction_);
    GameToolbar_->addAction(BuildDebugAction_);

    QMenu* releaseMenu = menuBar()->addMenu(QStringLiteral("&Release"));
    SetupReleaseAction_ = releaseMenu->addAction(QStringLiteral("Set Up &Releases..."));
    PackageReleaseAction_ = releaseMenu->addAction(QStringLiteral("&Package Release..."));
#if defined(Q_OS_MACOS)
    SetupReleaseAction_->setToolTip(QStringLiteral(
        "Build an ad-hoc signed macOS app. Developer ID signing and notarization require a separate workflow."));
    PackageReleaseAction_->setToolTip(SetupReleaseAction_->toolTip());
#endif
    connect(SetupReleaseAction_, &QAction::triggered, this, &MainWindow::OnSetupRelease);
    connect(PackageReleaseAction_, &QAction::triggered, this, &MainWindow::OnPackageRelease);
    QMenu* playMenu = menuBar()->addMenu(QStringLiteral("&Play"));
    PlayAction_ = playMenu->addAction(QStringLiteral("Build and Play"));
    PlayAction_->setObjectName(QStringLiteral("play.start"));
    PlayAction_->setIconText(QStringLiteral("Play"));
#if defined(Q_OS_WASM)
    PlayAction_->setText(QStringLiteral("Play Web Sample"));
    PlayAction_->setToolTip(
        QStringLiteral("Play the shipped web build. Workspace source edits require a desktop rebuild."));
#endif
#if defined(Q_OS_MACOS)
    PlayAction_->setToolTip(QStringLiteral("Build and Play uses the saved macOS Debug or Development profile."));
    BuildDebugAction_->setToolTip(QStringLiteral("RAD debugging is available on Linux x64 only."));
#endif
    BuildReloadAction_ = playMenu->addAction(QStringLiteral("Build and Reload Code"));
    BuildReloadAction_->setObjectName(QStringLiteral("play.reload"));
    BuildReloadAction_->setIconText(QStringLiteral("Reload Code"));
    AutoReloadAction_ = playMenu->addAction(QStringLiteral("Automatically Reload Source Changes"));
    AutoReloadAction_->setCheckable(true);
    connect(AutoReloadAction_, &QAction::toggled, Controller_, &EditorController::SetAutoReload);
    ReloadAssetAction_ = playMenu->addAction(QStringLiteral("Reload Frame-clear Configuration..."));
    connect(ReloadAssetAction_, &QAction::triggered, this, [this]() {
        const auto directory = QFileInfo(Controller_->State().DescriptorPath).absolutePath();
        const auto path = QFileDialog::getOpenFileName(this,
                                                       QStringLiteral("Frame-clear configuration"),
                                                       directory,
                                                       QStringLiteral("JSON configuration (*.json)"));
        if (path.isEmpty() == false)
        {
            Controller_->ReloadClearConfiguration(path);
        }
    });
    PauseAction_ = playMenu->addAction(QStringLiteral("Pause"));
    StepAction_ = playMenu->addAction(QStringLiteral("Step"));
    ResumeAction_ = playMenu->addAction(QStringLiteral("Resume"));
    RefreshPropertiesAction_ = playMenu->addAction(QStringLiteral("Refresh Inspector"));
    auto* details = playMenu->addAction(QStringLiteral("Refresh Session Details"));
    connect(details, &QAction::triggered, Controller_, &EditorController::RefreshSessionDetails);
    UndoSessionAction_ = playMenu->addAction(QStringLiteral("Undo Session Edit"));
    RedoSessionAction_ = playMenu->addAction(QStringLiteral("Redo Session Edit"));
    playMenu->addSeparator();
    UndoTuningAction_ = playMenu->addAction(QStringLiteral("Undo Tuning Document Edit"));
    RedoTuningAction_ = playMenu->addAction(QStringLiteral("Redo Tuning Document Edit"));
    SaveTuningAction_ = playMenu->addAction(QStringLiteral("Save Tuning Document"));
    DiscardTuningAction_ = playMenu->addAction(QStringLiteral("Discard Tuning Draft"));
    connect(UndoSessionAction_, &QAction::triggered, Controller_, &EditorController::UndoSessionEdit);
    connect(RedoSessionAction_, &QAction::triggered, Controller_, &EditorController::RedoSessionEdit);
    connect(UndoTuningAction_, &QAction::triggered, Controller_, &EditorController::UndoTuningDocument);
    connect(RedoTuningAction_, &QAction::triggered, Controller_, &EditorController::RedoTuningDocument);
    connect(SaveTuningAction_, &QAction::triggered, Controller_, &EditorController::SaveTuningDocument);
    connect(DiscardTuningAction_, &QAction::triggered, Controller_, &EditorController::DiscardTuningDocumentDraft);
    connect(PlayAction_, &QAction::triggered, this, [this]() {
#if defined(Q_OS_WASM)
        Controller_->Play();
#else
        LaunchAfterPreview(LaunchAction::Play);
#endif
    });
    connect(BuildReloadAction_, &QAction::triggered, this, [this]() {
        if (Scripts_->ConfirmDiscard())
        {
            Controller_->BuildReload();
        }
    });
    connect(PauseAction_, &QAction::triggered, this, [this]() { Controller_->PlayCommand(QStringLiteral("Pause")); });
    connect(StepAction_, &QAction::triggered, this, [this]() { Controller_->PlayCommand(QStringLiteral("Step")); });
    connect(ResumeAction_, &QAction::triggered, this, [this]() { Controller_->PlayCommand(QStringLiteral("Resume")); });
    connect(RefreshPropertiesAction_, &QAction::triggered, Controller_, &EditorController::RefreshProperties);

    GameToolbar_->addSeparator();
    GameToolbar_->addAction(PlayAction_);
    GameToolbar_->addAction(PauseAction_);
    GameToolbar_->addAction(StepAction_);
    GameToolbar_->addAction(ResumeAction_);
    GameToolbar_->addAction(BuildReloadAction_);
    GameToolbar_->addAction(StopAction_);

    mRenderGraphToolbar->setObjectName(QStringLiteral("renderGraphToolbar"));
    mRenderGraphToolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addToolBar(mRenderGraphToolbar);
    for (QAction* action : {mValidateGraphAction,
                            mBuildGraphAction,
                            mBuildPreviewGraphAction,
                            mLivePreviewGraphAction,
                            mCancelGraphAction})
    {
        mRenderGraphToolbar->addAction(action);
    }

    // Output commands live on the Output tool window's own toolbar.
    ClearAction_ = new QAction(QStringLiteral("&Clear Output"), this);
    ClearAction_->setObjectName(QStringLiteral("output.clear"));
    ClearAction_->setIconText(QStringLiteral("Clear"));
    CopyAction_ = new QAction(QStringLiteral("Copy &Job Details"), this);
    CopyAction_->setObjectName(QStringLiteral("output.copyJobDetails"));
    QToolBar* outputTools = OutputDock_->findChild<QToolBar*>(QStringLiteral("outputToolbar"));
    LUDUS_REQUIRE(outputTools != nullptr);
    outputTools->addAction(ClearAction_);
    outputTools->addAction(CopyAction_);

    // View shows and focuses one tool window (Visual Studio's View menu).
    const auto addToolWindowCommand = [this, viewMenu](const EditorCommand& command, QDockWidget* dock) {
        QAction* action = CreateEditorCommand(command, this);
        connect(action, &QAction::triggered, this, [this, dock]() { showToolWindow(dock); });
        viewMenu->addAction(action);
    };
    addToolWindowCommand(VIEW_PROJECT_EXPLORER_COMMAND, mProjectExplorerDock);
    addToolWindowCommand(VIEW_PROPERTIES_COMMAND, mPropertiesDock);
    addToolWindowCommand(VIEW_ERROR_LIST_COMMAND, mErrorListDock);
    addToolWindowCommand(VIEW_OUTPUT_COMMAND, OutputDock_);
    addToolWindowCommand(VIEW_CONTENT_COMMAND, mContentDock);
    addToolWindowCommand(VIEW_RENDER_PREVIEW_COMMAND, mRenderPreviewDock);
    addToolWindowCommand(VIEW_LIVE_INSPECTOR_COMMAND, InspectorDock_);
    addToolWindowCommand(VIEW_TIMING_COMMAND, TimingDock_);
    viewMenu->addSeparator();
    QMenu* toolbars = viewMenu->addMenu(QStringLiteral("&Toolbars"));
    for (QToolBar* toolbar : {mStandardToolbar, GameToolbar_, mRenderGraphToolbar})
    {
        toolbars->addAction(toolbar->toggleViewAction());
    }

    // Visual Studio keeps editor-wide preferences in Tools > Options.
    QMenu* toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
    toolsMenu->setObjectName(QStringLiteral("toolsMenu"));
    toolsMenu->addAction(mOptionsAction);
    connect(mOptionsAction, &QAction::triggered, this, &MainWindow::showOptions);

    QMenu* windowMenu = menuBar()->addMenu(QStringLiteral("&Window"));
    windowMenu->setObjectName(QStringLiteral("windowMenu"));
    buildWindowMenu(windowMenu);

    auto* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    auto* shortcuts = helpMenu->addAction(QStringLiteral("Keyboard Shortcuts..."));
    shortcuts->setObjectName(QStringLiteral("keyboardShortcutsAction"));
    shortcuts->setShortcut(QKeySequence(Qt::Key_F1));
    connect(shortcuts, &QAction::triggered, this, &MainWindow::ShowShortcuts);
    // Thanks to the Qt Group, QKeySequence documentation, StandardKey:
    // https://doc.qt.io/qt-6/qkeysequence.html. Platform bindings and QAction
    // capability gates apply equally to menus, buttons and shortcuts.
    BuildAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")));
    BuildRunAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+F5")));
    PlayAction_->setShortcut(QKeySequence(Qt::Key_F6));
    StopAction_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F6));
    connect(WelcomeNewButton_, &QPushButton::clicked, NewProjectAction_, &QAction::trigger);

    connect(OpenAction_, &QAction::triggered, this, &MainWindow::OnOpenRequested);
    connect(SaveAction_, &QAction::triggered, this, &MainWindow::OnSaveRequested);
    connect(ReloadAction_, &QAction::triggered, this, &MainWindow::OnReloadRequested);
    connect(ConfigureAction_, &QAction::triggered, Controller_, &EditorController::Configure);
    connect(BuildAction_, &QAction::triggered, this, [this]() {
        if (Scripts_->ConfirmDiscard())
        {
            Controller_->Build();
        }
    });
    connect(BuildRunAction_, &QAction::triggered, this, [this]() { LaunchAfterPreview(LaunchAction::Run); });
    connect(StopAction_, &QAction::triggered, Controller_, &EditorController::Stop);
    connect(ClearAction_, &QAction::triggered, Controller_, &EditorController::ClearOutput);
    connect(CopyAction_, &QAction::triggered, this, &MainWindow::OnCopyJobDetails);
}

void MainWindow::LaunchAfterPreview(LaunchAction action, const QString& debugger, bool setup)
{
    if (AudioLaunchPending_ || AudioClosing_ || Scripts_->ConfirmDiscard() == false ||
        World_->ConfirmDiscard() == false || Renderer_->ConfirmDiscard() == false)
    {
        return;
    }
    ResumeViewport_ = World_->PreviewRunning();
    ResumeViewportEpoch_ = Controller_->State().ProjectEpoch;
    World_->StopPreview();
    AudioLaunchPending_ = true;
    Audio_->setEnabled(false);
    Audio_->ShutdownPreview();
    RenderCapabilities();
    const auto epoch = Controller_->State().ProjectEpoch;
    const auto digest = Controller_->State().SavedDigest;
    auto* timer = new QTimer(this);
    timer->setInterval(20);
    connect(timer, &QTimer::timeout, this, [this, timer, epoch, digest, action, debugger, setup]() {
        if (Audio_->PreviewFinished() == false)
        {
            return;
        }
        timer->stop();
        timer->deleteLater();
        AudioLaunchPending_ = false;
        if (AudioClosing_)
        {
            return;
        }
        Audio_->ResetPreview();
        Audio_->setEnabled(Controller_->State().Document == DocumentState::ProjectLoaded);
        RenderCapabilities();
        if (epoch != Controller_->State().ProjectEpoch || digest != Controller_->State().SavedDigest)
        {
            return;
        }
        switch (action)
        {
            case LaunchAction::Run:
                Controller_->BuildRun();
                break;
            case LaunchAction::Play:
                Controller_->Play();
                break;
            case LaunchAction::Debug:
                Controller_->BuildDebug(debugger, setup);
                break;
        }
    });
    timer->start();
}

ProjectDescriptor MainWindow::DraftFromFields() const
{
    ProjectDescriptor draft = Controller_->State().Draft;
    draft.Name = NameEdit_->text();
    draft.ProviderKind = ProviderBox_->currentText() == QStringLiteral("cmake") ? Provider::Cmake : Provider::Ludus;
    draft.SourceDir = SourceDirEdit_->text();
    draft.Preset = PresetBox_->currentData().toString();
    draft.Target = TargetBox_->currentText();
    draft.RunCwd = CwdEdit_->text();
    draft.RunArgs.clear();
    for (int i = 0; i < ArgsList_->count(); ++i)
    {
        draft.RunArgs.append(ArgsList_->item(i)->text());
    }
    return draft;
}

void MainWindow::OnFieldEdited()
{
    if (Rendering_)
    {
        return; // field change came from RenderFields, not a user edit
    }
    // Rendering blocks edit signals. Accept this input synchronously so Save or
    // a project switch cannot overtake a queued field edit.
    const ProjectDescriptor draft = DraftFromFields();
    auto* field = sender();
    Controller_->EditDraft(draft, field != nullptr && field == LastEditedField_);
    LastEditedField_ = field;
}

void MainWindow::OnAddArgument()
{
    if (Rendering_)
    {
        return;
    }
    auto* item = new QListWidgetItem(QString(), ArgsList_);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    ArgsList_->editItem(item);
    OnFieldEdited();
}

void MainWindow::OnRemoveArgument()
{
    if (Rendering_)
    {
        return;
    }
    delete ArgsList_->takeItem(ArgsList_->currentRow());
    OnFieldEdited();
}

void MainWindow::OnOpenRequested()
{
    if (Controller_->Caps().CanOpen == false)
    {
        return;
    }
    const QString directory = Controller_->State().DescriptorPath.isEmpty()
                                  ? QString()
                                  : QFileInfo(Controller_->State().DescriptorPath).absolutePath();
    const auto epoch = Controller_->State().ProjectEpoch;
    OpenEditorDocument(this,
                       {
                           .Title = QStringLiteral("Open Project Folder"),
                           .Directory = directory,
                           .Filter = QStringLiteral("Ludus Project (*.json)"),
                           .ProjectFolder = true,
                       },
                       [this, epoch](const QString& path) {
                           if (Controller_->State().ProjectEpoch == epoch)
                           {
                               OpenProjectPath(path);
                           }
                       });
}

void MainWindow::OpenProjectPath(const QString& path)
{
    if (path.isEmpty() || Controller_->Caps().CanOpen == false || Content_->Importing() || AudioClosing_ ||
        (Audio_->ConfirmDiscard() == false || Scripts_->ConfirmDiscard() == false ||
         World_->ConfirmDiscard() == false || Renderer_->ConfirmDiscard() == false))
    {
        return;
    }
    Audio_->StopPreview();
    if (ConfirmProjectChange(QStringLiteral("opening another project")) == false)
    {
        return;
    }
    Controller_->OpenProject(path);
}

void MainWindow::OnSaveRequested()
{
    QWidget* areaOrNull = getActiveWorkspaceOrNull();
    if (areaOrNull == World_)
    {
        (void)World_->Save();
        // A saved World may be new; keep Project Explorer's snapshot current.
        mProjectExplorer->Refresh();
        return;
    }
    if (areaOrNull == Scripts_)
    {
        (void)Scripts_->Save();
        return;
    }
    if (areaOrNull == Renderer_)
    {
        static_cast<void>(Renderer_->Save());
        mProjectExplorer->Refresh();
        return;
    }
    if (areaOrNull == Configuration_)
    {
        Configuration_->Save();
        return;
    }
    if (areaOrNull != nullptr && areaOrNull->isAncestorOf(Audio_))
    {
        if (Audio_->CanSave())
        {
            Audio_->Save();
        }
        return;
    }
    if (areaOrNull == ProjectSettings_)
    {
        (void)SaveProjectSettings();
    }
}

bool MainWindow::SaveProjectSettings()
{
    CommitProjectFields();
    const auto& state = Controller_->State();
    if (Content_->Importing() && state.Draft.SourceDir != state.Saved.SourceDir)
    {
        StatusLabel_->setText(QStringLiteral("Finish or cancel content work before saving a changed source root."));
        return false;
    }
    if (state.Draft.SourceDir != state.Saved.SourceDir &&
        (Audio_->ConfirmDiscard() == false || Scripts_->ConfirmDiscard() == false ||
         World_->ConfirmDiscard() == false || Renderer_->ConfirmDiscard() == false))
    {
        return false;
    }
    Controller_->Save();
#if defined(Q_OS_WASM)
    if (Controller_->State().Dirty() == false)
    {
        (void)DownloadEditorProject(this, Controller_->State().DescriptorPath);
    }
#endif
    return Controller_->State().Dirty() == false;
}

void MainWindow::OnReloadRequested()
{
    if (Content_->Importing())
    {
        return;
    }
    CommitProjectFields();
    if (Audio_->ConfirmDiscard() == false || Scripts_->ConfirmDiscard() == false || World_->ConfirmDiscard() == false ||
        Renderer_->ConfirmDiscard() == false)
    {
        return;
    }
    Audio_->StopPreview();
    if (Controller_->State().Dirty())
    {
        const auto choice = QMessageBox::question(this,
                                                  QStringLiteral("Unsaved changes"),
                                                  QStringLiteral("Discard changes and reload from disk?"),
                                                  QMessageBox::Discard | QMessageBox::Cancel);
        if (choice != QMessageBox::Discard)
        {
            return;
        }
    }
    Controller_->Reload();
}

void MainWindow::OnCopyJobDetails()
{
    QApplication::clipboard()->setText(Controller_->JobDetails());
}

void MainWindow::OnStateChanged()
{
    RenderFields();
    RenderRecentProjects();
    RenderCapabilities();
    RenderStatus();
    const auto& state = Controller_->State();
    const bool loaded = state.Document == DocumentState::ProjectLoaded;
    const bool isEnteringWelcome = loaded == false && mDocuments->IsDocumentAvailable(Welcome_) == false;
    const bool isEnteringProject = loaded && mDocuments->IsDocumentAvailable(Welcome_);
    if (isEnteringWelcome == true)
    {
        // Capture the project's dock sizes and documents before Welcome replaces them.
        SetProjectWorkspace({}, false);
    }
    // Project documents need a loaded project; Configuration is an independent offline document.
    for (QWidget* document : mDocuments->GetDocuments())
    {
        mDocuments->SetDocumentAvailable(document,
                                         document == Welcome_ ? loaded == false
                                                              : loaded == true || document == Configuration_);
    }
    if (isEnteringWelcome)
    {
        static_cast<void>(mDocuments->OpenDocument(Welcome_));
    }
    else if (isEnteringProject)
    {
        // A project opens on its World, like a solution opening on its startup document, with
        // Project Settings (setup status and build commands) as a background tab. A saved
        // per-project arrangement may replace this below.
        static_cast<void>(mDocuments->OpenDocument(ProjectSettings_));
        static_cast<void>(mDocuments->OpenDocument(World_));
        World_->setFocus();
    }
#if defined(Q_OS_WASM)
    Audio_->setEnabled(false);
    Audio_->setToolTip(QStringLiteral("Audio authoring and preview require the desktop editor."));
#else
    Audio_->setEnabled(state.Document == DocumentState::ProjectLoaded && AudioClosing_ == false &&
                       AudioLaunchPending_ == false);
#endif
    if (state.Document == DocumentState::ProjectLoaded)
    {
        const auto root = QDir(QFileInfo(state.DescriptorPath).absolutePath()).absoluteFilePath(state.Saved.SourceDir);
        const bool restore =
            QFileInfo(root).canonicalFilePath() != ProjectLayoutRoot_ || state.ProjectEpoch != ProjectLayoutEpoch_;
        if (restore && QFileInfo(root).canonicalFilePath() == ProjectLayoutRoot_)
        {
            SaveProjectWorkspace();
        }
        ProjectLayoutEpoch_ = state.ProjectEpoch;
        SetProjectWorkspace(root, false);
        World_->SetProject(root, state.ProjectEpoch);
        if (restore)
        {
            SetProjectWorkspace(root, true);
        }
        Scripts_->SetProject(QFileInfo(state.DescriptorPath).absolutePath(), root, state.Saved.Preset);
        Renderer_->SetProject(QFileInfo(state.DescriptorPath).absolutePath(), state.ProjectEpoch);
        if (restore == true)
        {
            // A new project identity retires the previous project's render-graph rows.
            mErrorList->ReplaceEntries(ErrorListProducer::RenderGraph, {});
            mProjectExplorer->SetProject(root, state.Saved.Name);
        }
        Audio_->SetRoot(QDir(root).absoluteFilePath(QStringLiteral("content")));
        Content_->SetProject(QDir(root).absoluteFilePath(QStringLiteral("content")), state.ProjectEpoch);
    }

    else
    {
        SetProjectWorkspace({}, false);
        World_->SetProject({}, state.ProjectEpoch);
        Scripts_->SetProject({}, {}, {});
        Renderer_->SetProject({}, state.ProjectEpoch);
        mErrorList->ReplaceEntries(ErrorListProducer::RenderGraph, {});
        if (isEnteringWelcome == true)
        {
            mProjectExplorer->SetProject({}, {});
        }
        Audio_->SetRoot(QString());
        Content_->SetProject(QString(), state.ProjectEpoch);
    }

    if (ResumeViewport_ && state.OperationPhase == Phase::Idle && AudioLaunchPending_ == false)
    {
        ResumeViewport_ = false;
        if (CloseConfirmed_ == false && loaded && state.ProjectEpoch == ResumeViewportEpoch_)
        {
            World_->ResumePreview();
        }
    }
    // If a close was requested and the workspace is now closeable, finish.
    RenderProperties();
    Timing_->Render();
    renderGraphCommands();
    renderDocumentTitles();
    if (CloseConfirmed_ && Controller_->Caps().CanCloseImmediately)
    {
        close();
    }
}

void MainWindow::RenderRecentProjects()
{
    const auto& projects = Controller_->RecentProjects();
    QStringList stamp;
    for (const RecentProject& project : projects)
    {
        stamp.append(project.Name);
        stamp.append(project.DescriptorPath);
        stamp.append(QFileInfo(project.DescriptorPath).isFile() ? QString() : QStringLiteral("Missing"));
    }
    RecentEmptyLabel_->setVisible(projects.isEmpty());
    RecentList_->setVisible(projects.isEmpty() == false);
    if (stamp == RecentProjectsStamp_)
    {
        return;
    }
    RecentProjectsStamp_ = stamp;
    const auto* selected = RecentList_->currentItem();
    const QString selectedPath = selected != nullptr ? selected->data(Qt::UserRole).toString() : QString();
    const QSignalBlocker blocker(RecentList_);
    RecentList_->clear();
    RecentMenu_->clear();
    for (const RecentProject& project : projects)
    {
        const bool missing = QFileInfo(project.DescriptorPath).isFile() == false;
        const QString name = missing ? QStringLiteral("%1 (missing)").arg(project.Name) : project.Name;
        auto* item = new QListWidgetItem(name + QStringLiteral("\n") + project.DescriptorPath, RecentList_);
        item->setIcon(windowIcon());
        item->setData(Qt::UserRole, project.DescriptorPath);
        item->setToolTip(project.DescriptorPath);
        if (project.DescriptorPath == selectedPath)
        {
            RecentList_->setCurrentItem(item);
        }
        // Escape ampersands so project names/paths are literal menu labels.
        QString label = name + QStringLiteral(" — ") + project.DescriptorPath;
        label.replace(QStringLiteral("&"), QStringLiteral("&&"));
        auto* action = RecentMenu_->addAction(windowIcon(), label);
        action->setData(project.DescriptorPath);
        action->setToolTip(project.DescriptorPath);
        connect(action, &QAction::triggered, this, [this, path = project.DescriptorPath]() {
            QTimer::singleShot(0, this, [this, path]() { OpenProjectPath(path); });
        });
    }
    if (RecentList_->currentItem() == nullptr && RecentList_->count() != 0)
    {
        RecentList_->setCurrentRow(0);
    }
    RecentOpenButton_->setEnabled(Controller_->Caps().CanOpen && RecentList_->currentItem() != nullptr);
}

void MainWindow::RenderFields()
{
    Rendering_ = true;
    const QSignalBlocker b1(NameEdit_);
    const QSignalBlocker b2(ProviderBox_);
    const QSignalBlocker b3(SourceDirEdit_);
    const QSignalBlocker b4(PresetBox_);
    const QSignalBlocker b5(TargetBox_);
    const QSignalBlocker b6(CwdEdit_);
    const QSignalBlocker b7(ArgsList_);

    const ProjectDescriptor& draft = Controller_->State().Draft;
    const bool identityChanged = RenderedProjectEpoch_ != Controller_->State().ProjectEpoch;
    RenderedProjectEpoch_ = Controller_->State().ProjectEpoch;
    // Qt's setText clears selection and text Undo. Only replace content when
    // the document changed; ordinary state notifications preserve edit buffers.
    // Thanks to Qt Group, QLineEdit, text property/Undo documentation:
    // https://doc.qt.io/qt-6/qlineedit.html#text-prop
    const auto text = [identityChanged](QLineEdit* edit, const QString& value) {
        if (identityChanged || edit->text() != value)
        {
            edit->setText(value);
        }
    };
    text(NameEdit_, draft.Name);
    text(SourceDirEdit_, draft.SourceDir);
    text(CwdEdit_, draft.RunCwd);
    const auto provider = draft.ProviderKind == Provider::Cmake ? QStringLiteral("cmake") : QStringLiteral("ludus");
    if (ProviderBox_->currentText() != provider)
    {
        ProviderBox_->setCurrentText(provider);
    }
    auto* profiles = static_cast<QStandardItemModel*>(PresetBox_->model());
    for (foundation::int32 index = 0; index < PresetBox_->count(); ++index)
    {
        const auto preset = PresetBox_->itemData(index).toString();
        const bool supported = draft.Version == 2 || preset == QStringLiteral("linux-clang-debug") ||
                               preset == QStringLiteral("linux-clang-development");
        profiles->item(index)->setEnabled(supported);
    }
    if (PresetBox_->currentData().toString() != draft.Preset)
    {
        PresetBox_->setCurrentIndex(PresetBox_->findData(draft.Preset));
    }

    // Executable-only dropdown after Configure; the current (possibly invalid)
    // target stays visible until corrected.
    const QStringList& discovered = Controller_->State().DiscoveredTargets;
    const QString current = draft.Target;
    if (identityChanged || (RenderedTargets_ != discovered && TargetBox_->lineEdit()->hasFocus() == false))
    {
        TargetBox_->clear();
        TargetBox_->addItems(discovered);
        RenderedTargets_ = discovered;
    }
    text(TargetBox_->lineEdit(), current);

    QStringList displayedArgs;
    for (foundation::int32 index = 0; index < ArgsList_->count(); ++index)
    {
        displayedArgs.append(ArgsList_->item(index)->text());
    }
    if (identityChanged || displayedArgs != draft.RunArgs)
    {
        const auto selected = ArgsList_->currentRow();
        ArgsList_->clear();
        for (const QString& arg : draft.RunArgs)
        {
            auto* item = new QListWidgetItem(arg, ArgsList_);
            item->setFlags(item->flags() | Qt::ItemIsEditable);
        }
        if (identityChanged == false && selected >= 0 && selected < ArgsList_->count())
        {
            ArgsList_->setCurrentRow(selected);
        }
    }

    // Render output and runtime status.
    const auto output = Controller_->Log().Text();
    if (Output_->toPlainText() != output)
    {
        Output_->setPlainText(output);
        mBuildErrorsTimer.start();
    }
    Rendering_ = false;
}

void MainWindow::RenderCapabilities()
{
    const Capabilities caps = Controller_->Caps();
    PlayAction_->setEnabled(Controller_->CanPlay() && AudioLaunchPending_ == false);
    BuildReloadAction_->setEnabled(Controller_->CanBuildReload());
    const auto play = Controller_->PlayState().Phase;
    const bool debuggerStopped = Controller_->PlayState().DebuggerStopped;
    AutoReloadAction_->setEnabled(play == PlayPhase::Running || play == PlayPhase::Paused);
    ReloadAssetAction_->setEnabled(debuggerStopped == false &&
                                   (play == PlayPhase::Running || play == PlayPhase::Paused));
    {
        const QSignalBlocker blocker(AutoReloadAction_);
        AutoReloadAction_->setChecked(Controller_->AutoReloadEnabled());
    }
    PauseAction_->setEnabled(debuggerStopped == false && play == PlayPhase::Running);
    ResumeAction_->setEnabled(debuggerStopped == false && play == PlayPhase::Paused);
    StepAction_->setEnabled(debuggerStopped == false && play == PlayPhase::Paused);
    RefreshPropertiesAction_->setEnabled(play == PlayPhase::Running || play == PlayPhase::Paused);
    ApplySessionButton_->setEnabled(Controller_->CanEditProperties());
    ApplyDocumentButton_->setEnabled(Controller_->CanApplyToTuningDocument(Properties_->currentRow()));
    UndoSessionAction_->setEnabled(Controller_->CanUndoSession());
    RedoSessionAction_->setEnabled(Controller_->CanRedoSession());
    const auto& playState = Controller_->PlayState();
    UndoTuningAction_->setEnabled(playState.TuningDocumentAvailable && playState.TuningCanUndo);
    RedoTuningAction_->setEnabled(playState.TuningDocumentAvailable && playState.TuningCanRedo);
    SaveTuningAction_->setEnabled(Controller_->CanSaveTuningDocument());
    DiscardTuningAction_->setEnabled(playState.TuningDocumentAvailable && playState.TuningDocumentDirty);
    NewProjectAction_->setEnabled(caps.CanProjectCreate && Content_->Importing() == false);
    WelcomeNewButton_->setEnabled(caps.CanProjectCreate && Content_->Importing() == false);
    CloseProjectAction_->setEnabled(caps.CanCloseProject && Content_->Importing() == false &&
                                    AudioLaunchPending_ == false && AudioClosing_ == false);
    CloseProjectAction_->setToolTip(QStringLiteral("Close the project and return to Welcome. Stop active work first."));
    CheckSetupAction_->setEnabled(caps.CanProjectCheck);
    SetupProjectAction_->setEnabled(caps.CanProjectSetup);
    OpenAction_->setEnabled(caps.CanOpen && Content_->Importing() == false);
    RecentMenu_->setEnabled(caps.CanOpen && Content_->Importing() == false &&
                            Controller_->RecentProjects().isEmpty() == false);
    RecentList_->setEnabled(caps.CanOpen && Content_->Importing() == false);
    RecentOpenButton_->setEnabled(caps.CanOpen && Content_->Importing() == false &&
                                  RecentList_->currentItem() != nullptr);
    BrowseProjectButton_->setEnabled(caps.CanOpen && Content_->Importing() == false);
    ClearRecentAction_->setEnabled(caps.CanOpen && Controller_->RecentProjects().isEmpty() == false);
    RenderDocumentActions();
    ReloadAction_->setEnabled(caps.CanReload && Content_->Importing() == false);
    ConfigureAction_->setEnabled(caps.CanConfigure);
    BuildAction_->setEnabled(caps.CanBuild);
    BuildRunAction_->setEnabled(caps.CanBuildRun && AudioLaunchPending_ == false);
    BuildDebugAction_->setEnabled(caps.CanBuildDebug && AudioLaunchPending_ == false);
    StopAction_->setEnabled(caps.CanStop);
    SetupReleaseAction_->setEnabled(caps.CanReleaseInit);
    PackageReleaseAction_->setEnabled(caps.CanPackage);
#if defined(Q_OS_WASM)
    ExportProjectAction_->setEnabled(Controller_->State().Document == DocumentState::ProjectLoaded);
    const auto desktop =
        QStringLiteral("Requires the desktop editor: this browser workspace cannot run local tools or native games.");
    for (auto* action : {NewProjectAction_,
                         CheckSetupAction_,
                         SetupProjectAction_,
                         ConfigureAction_,
                         BuildAction_,
                         BuildRunAction_,
                         BuildDebugAction_,
                         SetupReleaseAction_,
                         PackageReleaseAction_,
                         PlayAction_,
                         BuildReloadAction_})
    {
        action->setToolTip(desktop);
    }
    WelcomeNewButton_->setToolTip(desktop);
    if (Controller_->State().Dirty())
    {
        MarkBrowserEdited();
    }
#endif
    ClearAction_->setEnabled(caps.CanClearOutput);
    CopyAction_->setEnabled(caps.CanCopyJobDetails);

    // Lifecycle buttons reuse the same command gates as menus/toolbars.
    ProjectCheckButton_->setEnabled(CheckSetupAction_->isEnabled());
    ProjectCheckButton_->setToolTip(CheckSetupAction_->toolTip());
    mSetupNoticeRepairOrNull->setEnabled(SetupProjectAction_->isEnabled());
    mSetupNoticeCheckOrNull->setEnabled(CheckSetupAction_->isEnabled());
    ProjectRepairButton_->setEnabled(SetupProjectAction_->isEnabled());
    ProjectRepairButton_->setToolTip(SetupProjectAction_->toolTip());
    ProjectRunButton_->setEnabled(BuildRunAction_->isEnabled());
    ProjectRunButton_->setToolTip(BuildRunAction_->toolTip());
    ProjectStopButton_->setEnabled(StopAction_->isEnabled());
    // One-click drift repair, gated like Repair Project Setup.
    const bool shouldOfferEditorEngine = ShouldOfferEditorEngine(Controller_->State().EnginePin);
    mUseEditorEngineButtonOrNull->setVisible(shouldOfferEditorEngine);
    mUseEditorEngineButtonOrNull->setEnabled(shouldOfferEditorEngine && SetupProjectAction_->isEnabled());
    ProjectSaveButton_->setEnabled(caps.CanSave && Content_->Importing() == false);
#if defined(Q_OS_WASM)
    ProjectSaveButton_->setText(QStringLiteral("Save and download"));
#endif

    const bool editable = caps.CanEdit;
    NameEdit_->setEnabled(editable);
    ProviderBox_->setEnabled(editable);
    SourceDirEdit_->setEnabled(editable);
    PresetBox_->setEnabled(editable);
    TargetBox_->setEnabled(editable);
    CwdEdit_->setEnabled(editable);
    ArgsList_->setEnabled(editable);
    AddArgButton_->setEnabled(editable);
    RemoveArgButton_->setEnabled(editable);
}

void MainWindow::RenderStatus()
{
    const WorkspaceState& state = Controller_->State();
    const auto& project = state.Saved;
    const QString engine = project.HasEngine
                               ? QStringLiteral("Required engine: Ludus %1\nRequired components: %2")
                                     .arg(project.Engine.Version, project.Engine.Components.join(QStringLiteral(", ")))
                               : QStringLiteral("Engine selection is owned by this project's build definitions.");
    ProjectIdentity_->setText(
        QStringLiteral("Project folder: %1\n%2\nSaved build profile: %3")
            .arg(QDir::toNativeSeparators(QFileInfo(state.DescriptorPath).absolutePath()), engine, project.Preset));
    QString setupStatus = state.SetupStatus.isEmpty() ? QStringLiteral("Setup has not been checked for this project.")
                                                      : state.SetupStatus;
    if (state.EnginePin.State != EnginePinState::None && state.EnginePin.State != EnginePinState::Current)
    {
        setupStatus += QStringLiteral("\nEngine: %1").arg(state.EnginePin.Message);
    }
    ProjectSetupStatus_->setText(setupStatus);
    const bool hasSetupProblem =
        state.Document == DocumentState::ProjectLoaded && state.SetupProblem.isEmpty() == false;
    mSetupNoticeOrNull->setVisible(hasSetupProblem);
    mSetupNoticeTextOrNull->setToolTip(state.SetupProblem);
    mSetupNoticeTextOrNull->setText(
        QStringLiteral("Project setup needs attention: %1\nRepair now, or keep editing and repair later. %2")
            .arg(state.SetupProblem.section(QLatin1Char('\n'), 0, 0),
                 state.Dirty() == true
                     ? QStringLiteral("Save project settings to enable repair.")
                     : QStringLiteral("Repair updates local build settings and verifies a build and tests.")));

    QString next;
#if defined(Q_OS_WASM)
    next = QStringLiteral("Download your changes to keep them. Use the desktop editor to repair setup and build "
                          "your project. Browser Play runs a shipped sample; it does not rebuild your changes.");
#else
    if (state.OperationPhase == Phase::CleanupUnknown)
    {
        next =
            QStringLiteral("Process cleanup could not be confirmed. Open Output and copy job details before recovery.");
    }
    else if (state.Busy())
    {
        next =
            QStringLiteral("An operation is in progress. Follow its progress in Output; Stop requests cancellation.");
    }
    else if (Controller_->PlayState().Phase != PlayPhase::Stopped)
    {
        next = QStringLiteral("A live session is open. Use its inspector and Play controls, or Stop before repairing "
                              "setup or starting a separate build and run.");
    }
    else if (state.Dirty())
    {
        next = QStringLiteral("Save your project settings before checking setup, repairing, or building. "
                              "Other work areas save their own documents separately.");
    }
    else if (project.Version != 2 || project.ProviderKind != Provider::Cmake)
    {
        next = QStringLiteral("This project owns its engine and build setup. Use Build → Configure / Refresh Targets "
                              "to discover executables, then Build and run. SDK repair is available for version-2 "
                              "CMake game projects. Build and launch details exposes this project's settings.");
    }
    else
    {
        next = QStringLiteral("Check setup to verify this computer's tools and engine. Repair setup fixes missing or "
                              "stale settings and verifies a build and tests. Build and run launches the saved target "
                              "in a separate window. Open Content, Scene, Audio or Scripts to work on game content.");
    }
#endif
    ProjectNextStep_->setText(next);
    QString phase;
    switch (state.OperationPhase)
    {
        case Phase::Idle:
            phase = QStringLiteral("Idle");
            break;
        case Phase::Starting:
            phase = QStringLiteral("Starting");
            break;
        case Phase::Configuring:
            phase = QStringLiteral("Configuring");
            break;
        case Phase::Building:
            phase = QStringLiteral("Building");
            break;
        case Phase::Publishing:
            phase = QStringLiteral("Publishing generation");
            break;
        case Phase::Launching:
            phase = QStringLiteral("Launching");
            break;
        case Phase::Debugging:
            phase = QStringLiteral("RAD session open");
            break;
        case Phase::Running:
            phase = QStringLiteral("Running");
            break;
        case Phase::Stopping:
            phase = QStringLiteral("Stopping");
            break;
        case Phase::CleanupUnknown:
            phase = QStringLiteral("Cleanup unknown");
            break;
    }

    QString status = phase;
    if (state.Result.Kind != Outcome::None)
    {
        status += QStringLiteral("  —  last result: %1").arg(QString::fromLatin1(ResultCodeName(state.Result.Code)));
        if (state.Result.Message.isEmpty() == false)
        {
            status += QStringLiteral(" (%1)").arg(state.Result.Message.section(QLatin1Char('\n'), 0, 0));
        }
    }
    if (state.OperationPhase == Phase::CleanupUnknown)
    {
        status += QStringLiteral("  —  operator recovery required; see Copy Job Details");
    }
    if (state.SetupStatus.isEmpty() == false)
    {
        status += QStringLiteral("\n") + state.SetupStatus.section(QLatin1Char('\n'), 0, 0);
    }
    StatusLabel_->setText(status);
    StatusLabel_->setToolTip(status + QStringLiteral("\n") + state.Result.Message + QStringLiteral("\n") +
                             state.SetupStatus);

    if (state.OperationPhase == Phase::Debugging)
    {
        RuntimeLabel_->setText(
            QStringLiteral("Set breakpoints, Run and Step in RAD. The game may be paused or running. "
                           "Stop ends this debugger session and its game."));
    }
    else if (Controller_->PlayState().Phase != PlayPhase::Stopped)
    {
        const auto& play = Controller_->PlayState();
        RuntimeLabel_->setText(QStringLiteral("Play session %1 · generation %2 · PID %4\n%3")
                                   .arg(play.Session, play.Generation, play.Message)
                                   .arg(play.HostPid));
    }
    else if (state.OperationPhase == Phase::Running)
    {
        RuntimeLabel_->setText(
            QStringLiteral("The application is running in its own window. This status area describes it; "
                           "it does not embed the game's framebuffer."));
    }
    else
    {
        RuntimeLabel_->setText(QStringLiteral("No runtime."));
    }
}

void MainWindow::OnDebuggerSetupRequested()
{
    if (Controller_->Caps().CanBuildDebug == false)
    {
        return;
    }
    const uint64 epoch = Controller_->State().ProjectEpoch;
    const QString digest = Controller_->State().SavedDigest;
    auto* dialog =
        new QMessageBox(QMessageBox::Information,
                        QStringLiteral("Set Up RAD Debugger"),
                        QStringLiteral("RAD is unavailable. Set Up downloads and builds the pinned debugger locally, "
                                       "then continues debugging. Missing system dependencies are listed in Output. "
                                       "You can also choose an existing RAD executable."),
                        QMessageBox::Cancel,
                        this);
    dialog->setObjectName(QStringLiteral("debuggerSetupDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* setup = dialog->addButton(QStringLiteral("Set Up RAD"), QMessageBox::AcceptRole);
    setup->setObjectName(QStringLiteral("setupRadButton"));
    auto* choose = dialog->addButton(QStringLiteral("Choose Existing Installation..."), QMessageBox::ActionRole);
    choose->setObjectName(QStringLiteral("chooseRadButton"));
    dialog->setDefaultButton(QMessageBox::Cancel);
    connect(dialog, &QMessageBox::finished, this, [this, dialog, setup, choose, epoch, digest](int) {
        const auto current = [this, epoch, digest]() {
            return Controller_->Caps().CanBuildDebug && Controller_->State().ProjectEpoch == epoch &&
                   Controller_->State().SavedDigest == digest;
        };
        if (current() == false)
        {
            return;
        }
        if (dialog->clickedButton() == setup)
        {
            LaunchAfterPreview(LaunchAction::Debug, {}, true);
        }
        else if (dialog->clickedButton() == choose)
        {
            auto* picker = new QFileDialog(this, QStringLiteral("Choose RAD Executable"));
            picker->setAttribute(Qt::WA_DeleteOnClose);
            picker->setFileMode(QFileDialog::ExistingFile);
            connect(picker, &QFileDialog::fileSelected, this, [this, current](const QString& path) {
                if (current())
                {
                    LaunchAfterPreview(LaunchAction::Debug, path);
                }
            });
            picker->open();
        }
    });
    dialog->open();
}

void MainWindow::OnSetupProject()
{
    const auto epoch = Controller_->State().ProjectEpoch;
    ProjectSetupDialog* dialog =
        new ProjectSetupDialog(QFileInfo(Controller_->State().DescriptorPath).absolutePath(),
                               this,
                               {
                                   .ExpectedFlavor = GetFlavorForProfile(Controller_->State().Saved.Preset),
                                   .ToolingRoot = Controller_->Tooling().ToolingRoot,
                               });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog, epoch]() {
        if (Controller_->State().ProjectEpoch == epoch)
        {
            const auto options = dialog->Options();
            Controller_->SetupProject(options.Sdk, options.WebSdk, options.PrepareEngine, options.DisableWeb);
        }
    });
    dialog->open();
}

bool MainWindow::ConfirmProjectChange(const QString& action)
{
    CommitProjectFields();
    if (Controller_->State().Document != DocumentState::ProjectLoaded || Controller_->State().Dirty() == false)
    {
        return true;
    }
    const auto choice = QMessageBox::question(this,
                                              QStringLiteral("Unsaved project settings"),
                                              QStringLiteral("Save project settings before %1?").arg(action),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (choice == QMessageBox::Save)
    {
        return SaveProjectSettings();
    }
    return choice == QMessageBox::Discard;
}

void MainWindow::OnNewProject()
{
    if (Controller_->Caps().CanProjectCreate == false || Content_->Importing() ||
        (Audio_->ConfirmDiscard() == false || Scripts_->ConfirmDiscard() == false ||
         World_->ConfirmDiscard() == false || Renderer_->ConfirmDiscard() == false) ||
        ConfirmProjectChange(QStringLiteral("creating another project")) == false)
    {
        return;
    }
    Audio_->StopPreview();
    auto* dialog = new ProjectCreationDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog]() { Controller_->CreateProject(dialog->Options()); });
    dialog->open();
}

void MainWindow::OnCloseProject()
{
    if (Controller_->Caps().CanCloseProject == false || Content_->Importing() || AudioLaunchPending_ || AudioClosing_ ||
        (Audio_->ConfirmDiscard() == false || Scripts_->ConfirmDiscard() == false ||
         World_->ConfirmDiscard() == false || Renderer_->ConfirmDiscard() == false) ||
        ConfirmProjectChange(QStringLiteral("closing the project")) == false)
    {
        return;
    }
    Audio_->StopPreview();
    // Confirmation authorizes this discard; reset through the model's edit gate.
    if (Controller_->State().Dirty())
    {
        Controller_->EditDraft(Controller_->State().Saved);
    }
    (void)Controller_->CloseProject();
}

void MainWindow::ShowShortcuts()
{
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Keyboard Shortcuts"));
    auto* layout = new QFormLayout(dialog);
    QList<QAction*> actions{NewProjectAction_,
                            OpenAction_,
                            SaveAction_,
                            UndoAction_,
                            RedoAction_,
                            CloseProjectAction_,
                            BuildAction_,
                            BuildRunAction_,
                            BuildDebugAction_,
                            PlayAction_,
                            StopAction_};
    // Catalogue commands (render graph, tool windows, window management) that have shortcuts.
    for (QAction* action : findChildren<QAction*>(Qt::FindDirectChildrenOnly))
    {
        const QString id = action->objectName();
        if (action->shortcut().isEmpty() == false && actions.contains(action) == false &&
            (id.startsWith(QStringLiteral("renderGraph.")) == true || id.startsWith(QStringLiteral("view.")) == true ||
             id.startsWith(QStringLiteral("window.")) == true))
        {
            actions.append(action);
        }
    }
    for (QAction* action : actions)
    {
        QString label = action->text();
        label.remove(QLatin1Char('&'));
        layout->addRow(label, new QLabel(action->shortcut().toString(QKeySequence::NativeText), dialog));
    }
    auto* note = new QLabel(QStringLiteral("Shortcuts follow the same availability rules as menu actions. "
                                           "Save and Undo follow the active document tab. Undo edits focused text "
                                           "first; Project Settings also has document history. Stop active work "
                                           "before closing a project."),
                            dialog);
    note->setWordWrap(true);
    layout->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addRow(buttons);
    dialog->open();
}

void MainWindow::OnSetupRelease()
{
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Set Up Releases"));
    auto* layout = new QFormLayout(dialog);
    auto* platform = new QComboBox(dialog);
#if defined(Q_OS_MACOS)
    platform->addItem(QStringLiteral("macOS app"), QStringLiteral("macos"));
#else
    platform->addItem(QStringLiteral("Linux native"), QStringLiteral("linux-x64"));
#endif
    platform->addItem(QStringLiteral("Browser (Emscripten)"), QStringLiteral("web"));
    auto* target = new QLineEdit(dialog);
    target->setPlaceholderText(QStringLiteral("username/game (optional)"));
    auto* note = new QLabel(dialog);
    const auto updateNote = [note, platform]() {
        if (platform->currentData().toString() == QStringLiteral("macos"))
        {
            note->setText(
                QStringLiteral("Save release files and a GitHub package workflow into this project's repository. "
                               "Configure a matching Release SDK on GitHub. macOS creates an ad-hoc signed app; "
                               "Developer ID signing and notarization require a separate workflow. "
                               "Setup never uploads or overwrites existing files."));
        }
        else
        {
            note->setText(QStringLiteral(
                "Save release files and an itch.io GitHub workflow into this project's repository. "
                "Set ITCH_IO_TARGET on GitHub if left blank, and BUTLER_API_KEY in the itch-release environment. "
                "Setup never uploads or overwrites existing files."));
        }
    };
    connect(platform, &QComboBox::currentIndexChanged, dialog, updateNote);
    updateNote();
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addRow(QStringLiteral("Platform"), platform);
    layout->addRow(QStringLiteral("itch.io project"), target);
    layout->addRow(note);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::accepted, this, [this, platform, target]() {
        Controller_->SetupRelease(platform->currentData().toString(), target->text().trimmed());
    });
    dialog->open();
}

void MainWindow::OnPackageRelease()
{
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Package Release"));
    auto* layout = new QFormLayout(dialog);
    auto* profile = new QComboBox(dialog);
    profile->setEditable(true);
#if defined(Q_OS_MACOS)
    profile->addItem(QStringLiteral("macos-release"));
#else
    profile->addItem(QStringLiteral("linux-release"));
#endif
    profile->addItem(QStringLiteral("web-release"));
    auto* version = new QLineEdit(QStringLiteral("0.1.0"), dialog);
    auto* sdk = new QLineEdit(dialog);
    sdk->setPlaceholderText(QStringLiteral("Optional Release SDK prefix"));
    auto* note = new QLabel(
        QStringLiteral("Build and validate a player package. The package directory appears in Output. "
                       "Browser projects need a configured web-emscripten-release preset. Packaging never uploads."),
        dialog);
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addRow(QStringLiteral("Package profile"), profile);
    layout->addRow(QStringLiteral("Version"), version);
    layout->addRow(QStringLiteral("SDK"), sdk);
    layout->addRow(note);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::accepted, this, [this, profile, version, sdk]() {
        Controller_->PackageRelease(profile->currentText(), version->text().trimmed(), sdk->text().trimmed());
    });
    dialog->open();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (AudioClosing_ == false)
    {
        if (ConfirmProjectChange(QStringLiteral("quitting")) == false)
        {
            event->ignore();
            return;
        }
        if ((Audio_->ConfirmDiscard() == false || Scripts_->ConfirmDiscard() == false ||
             World_->ConfirmDiscard() == false || Renderer_->ConfirmDiscard() == false) ||
            Configuration_->ConfirmDiscard() == false)
        {
            event->ignore();
            return;
        }
        const bool busy = Controller_->Caps().CanCloseImmediately == false || Content_->Importing();
        if (busy && CloseConfirmed_ == false)
        {
            const auto choice = QMessageBox::question(
                this,
                QStringLiteral("Operation running"),
                QStringLiteral("An operation is running. Stop it and close, or keep the editor open?"),
                QMessageBox::Close | QMessageBox::Cancel);
            if (choice != QMessageBox::Close)
            {
                event->ignore();
                return;
            }
            CloseConfirmed_ = true;
        }
        World_->StopPreview();
        AudioClosing_ = true;
        Audio_->ShutdownPreview();
        Content_->Shutdown();
        if (Controller_->Caps().CanCloseImmediately == false)
        {
            (void)Controller_->RequestClose();
        }
    }
    if (Audio_->PreviewFinished() == false || Content_->Finished() == false)
    {
        event->ignore();
        QTimer::singleShot(20, this, [this]() { close(); });
        return;
    }
    if (Controller_->Caps().CanCloseImmediately)
    {
        SaveWorkspaceLayout();
        event->accept();
        return;
    }
    event->ignore();
}

void MainWindow::RenderProperties()
{
    const auto& snapshot = Controller_->PlayState();
    const QString stamp = QString::fromUtf8(QJsonDocument(snapshot.Properties).toJson(QJsonDocument::Compact));
    if (stamp == PropertiesStamp_)
    {
        return;
    }
    PropertiesStamp_ = stamp;
    const QSignalBlocker blocker(Properties_);
    Properties_->setRowCount(static_cast<int>(snapshot.Properties.size()));
    for (int row = 0; row < snapshot.Properties.size(); ++row)
    {
        const auto property = snapshot.Properties.at(row).toObject();
        const int kind = property.value(QStringLiteral("kind")).toInt(-1);
        QString text;
        if (kind == 2)
        {
            const auto bits = static_cast<foundation::uint32>(property.value(QStringLiteral("bits")).toInteger());
            foundation::float32 value = 0.0F;
            std::memcpy(&value, &bits, sizeof(value));
            text = QString::number(static_cast<foundation::float64>(value), 'g', 9);
        }
        else if (kind == 0)
        {
            text = property.value(QStringLiteral("value")).toBool() ? QStringLiteral("true") : QStringLiteral("false");
        }
        else if (kind == 4)
        {
            text = property.value(QStringLiteral("value")).toString();
        }
        else
        {
            text = QString::number(property.value(QStringLiteral("value")).toInteger());
        }
        auto* name = new QTableWidgetItem(property.value(QStringLiteral("label")).toString());
        auto* value = new QTableWidgetItem(text);
        const bool writable = property.value(QStringLiteral("writable")).toBool();
        if (writable == false)
        {
            value->setFlags(value->flags() & ~Qt::ItemIsEditable);
        }
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        auto* scope = new QTableWidgetItem(property.value(QStringLiteral("scope")).toInt() == 1
                                               ? QStringLiteral("Persistable")
                                               : QStringLiteral("Session/output"));
        scope->setFlags(scope->flags() & ~Qt::ItemIsEditable);
        Properties_->setItem(row, 0, name);
        Properties_->setItem(row, 1, value);
        Properties_->setItem(row, 2, scope);
    }
}

} // namespace ludus::editor
