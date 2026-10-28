#include "internal/audio_workspace.h"
#include "internal/configuration_workspace.h"
#include "internal/content_browser.h"
#include "internal/document_well.h"
#include "internal/editor_commands.h"
#include "internal/error_list.h"
#include "internal/item_selection.h"
#include "internal/main_window.h"
#include "internal/project_explorer.h"
#include "internal/project_rendering_settings.h"
#include "internal/renderer_workspace.h"
#include "internal/script_workspace.h"
#include "internal/timing_panel.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QByteArray>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRect>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

namespace ludus::editor
{
namespace
{
// Version 6: the calm default (Project Explorer left, Properties right, diagnostics closed until needed);
// older layouts reset. Version 5 added the render-graph Render Preview tool window.
constexpr foundation::int32 LAYOUT_VERSION = 6;
constexpr foundation::int32 PERCENT = 100;
// First-run window: this share of the screen, at least the minimum below.
constexpr foundation::int32 DEFAULT_WINDOW_PERCENT = 85;
constexpr foundation::int32 MIN_WINDOW_WIDTH_PIXELS = 1100;
constexpr foundation::int32 MIN_WINDOW_HEIGHT_PIXELS = 760;
// Default tool-window extents leave documents about three fifths of the window: the outliner is the
// narrower side, Properties the wider one.
constexpr foundation::int32 EXPLORER_DOCK_PERCENT = 15;
constexpr foundation::int32 MIN_EXPLORER_DOCK_PIXELS = 220;
constexpr foundation::int32 MAX_EXPLORER_DOCK_PIXELS = 320;
constexpr foundation::int32 PROPERTIES_DOCK_PERCENT = 20;
constexpr foundation::int32 MIN_PROPERTIES_DOCK_PIXELS = 260;
constexpr foundation::int32 MAX_PROPERTIES_DOCK_PIXELS = 380;
// The bottom group needs a tab bar, the Error List filters and header, and a few rows.
constexpr foundation::int32 BOTTOM_DOCK_PERCENT = 27;
constexpr foundation::int32 MIN_BOTTOM_DOCK_PIXELS = 160;
constexpr foundation::int32 MAX_BOTTOM_DOCK_PIXELS = 300;
constexpr foundation::int32 PROPERTIES_HINT_MARGIN_PIXELS = 8;
constexpr foundation::int64 MAX_SETTINGS_BYTES = foundation::int64{64} * 1024;
constexpr foundation::int32 MAX_STATE_BYTES = 32 * 1024;
LUDUS_DEFINE_LOG_CATEGORY(LOG_EDITOR_LAYOUT, "Editor.Layout");
} // namespace

void MainWindow::BuildUi()
{
    mSetupNoticeOrNull = new QToolBar(QStringLiteral("Project setup needs attention"), this);
    mSetupNoticeOrNull->setObjectName(QStringLiteral("projectSetupNotice"));
    mSetupNoticeOrNull->setMovable(false);
    mSetupNoticeOrNull->setFloatable(false);
    QWidget* notice = new QWidget(mSetupNoticeOrNull);
    notice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // One compact line: the problem, its two commands and a dismiss button. The full
    // explanation is the text's tooltip; the status bar keeps reporting setup state.
    QHBoxLayout* noticeLayout = new QHBoxLayout(notice);
    noticeLayout->setContentsMargins(0, 0, 0, 0);
    mSetupNoticeTextOrNull = new QLabel(notice);
    mSetupNoticeTextOrNull->setObjectName(QStringLiteral("projectSetupNoticeText"));
    mSetupNoticeTextOrNull->setTextFormat(Qt::PlainText);
    mSetupNoticeTextOrNull->setWordWrap(false);
    mSetupNoticeTextOrNull->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    noticeLayout->addWidget(mSetupNoticeTextOrNull, 1);
    mSetupNoticeRepairOrNull = new QPushButton(QStringLiteral("Repair setup…"), notice);
    mSetupNoticeRepairOrNull->setObjectName(QStringLiteral("projectSetupNoticeRepair"));
    noticeLayout->addWidget(mSetupNoticeRepairOrNull);
    mSetupNoticeCheckOrNull = new QPushButton(QStringLiteral("Check again"), notice);
    mSetupNoticeCheckOrNull->setObjectName(QStringLiteral("projectSetupNoticeCheck"));
    noticeLayout->addWidget(mSetupNoticeCheckOrNull);
    QToolButton* dismiss = new QToolButton(notice);
    dismiss->setObjectName(QStringLiteral("projectSetupNoticeDismiss"));
    dismiss->setText(QStringLiteral("✕"));
    dismiss->setAutoRaise(true);
    dismiss->setAccessibleName(QStringLiteral("Dismiss setup notice"));
    dismiss->setToolTip(QStringLiteral("Hide this notice until the setup problem changes. Project > Repair Project "
                                       "Setup… stays available."));
    noticeLayout->addWidget(dismiss);
    mSetupNoticeOrNull->addWidget(notice);
    addToolBar(mSetupNoticeOrNull);
    addToolBarBreak();
    mSetupNoticeOrNull->hide();
    connect(mSetupNoticeRepairOrNull, &QPushButton::clicked, this, [this]() { SetupProjectAction_->trigger(); });
    connect(mSetupNoticeCheckOrNull, &QPushButton::clicked, this, [this]() { CheckSetupAction_->trigger(); });
    connect(dismiss, &QToolButton::clicked, this, [this]() {
        mDismissedSetupProblem = Controller_->State().SetupProblem;
        RenderStatus();
    });

    // Visual Studio layout: documents in the central well; tool windows dock around it.
    // The right dock area owns both right corners, so Project Explorer and Properties run
    // the full height and the bottom tool windows sit under the documents only.
    setCentralWidget(mDocuments);
    setDockNestingEnabled(true);
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    QScrollArea* projectScroll = new QScrollArea(mDocuments);
    ProjectSettings_ = projectScroll;
    projectScroll->setObjectName(QStringLiteral("projectSettingsScroll"));
    projectScroll->setWidgetResizable(true);
    projectScroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    projectScroll->setFrameShape(QFrame::NoFrame);

    // --- Project settings form ---
    auto* form = new QWidget(projectScroll);
    auto* projectLayout = new QVBoxLayout(form);
    auto* heading = new QLabel(QStringLiteral("Your project"), form);
    auto headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    projectLayout->addWidget(heading);
    const auto label = [form, projectLayout](const QString& name) {
        auto* value = new QLabel(form);
        value->setObjectName(name);
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        projectLayout->addWidget(value);
        return value;
    };
    ProjectIdentity_ = label(QStringLiteral("projectIdentity"));
    ProjectSetupStatus_ = label(QStringLiteral("projectSetupStatus"));
    ProjectNextStep_ = label(QStringLiteral("projectNextStep"));
    auto* lifecycle = new QWidget(form);
    auto* lifecycleLayout = new QGridLayout(lifecycle);
    lifecycleLayout->setContentsMargins(0, 0, 0, 0);
    const auto button = [lifecycle, lifecycleLayout](const QString& name, const QString& text) {
        auto* value = new QPushButton(text, lifecycle);
        value->setObjectName(name);
        const auto index = lifecycleLayout->count();
        lifecycleLayout->addWidget(value, index / 3, index % 3);
        return value;
    };
    ProjectCheckButton_ = button(QStringLiteral("projectCheckSetup"), QStringLiteral("Check setup"));
    ProjectRepairButton_ = button(QStringLiteral("projectRepairSetup"), QStringLiteral("Repair setup…"));
    ProjectSaveButton_ = button(QStringLiteral("projectSaveSettings"), QStringLiteral("Save settings"));
    ProjectRunButton_ = button(QStringLiteral("projectBuildRun"), QStringLiteral("Build and run"));
    ProjectStopButton_ = button(QStringLiteral("projectStop"), QStringLiteral("Stop"));
    mUseEditorEngineButtonOrNull =
        button(QStringLiteral("projectUseEditorEngine"), QStringLiteral("Use this editor's engine"));
    mUseEditorEngineButtonOrNull->setToolTip(
        QStringLiteral("Build and install the engine from this editor's checkout, select it for this project and "
                       "verify the project build. This can take several minutes."));
    mUseEditorEngineButtonOrNull->hide();
    projectLayout->addWidget(lifecycle);
    connect(ProjectCheckButton_, &QPushButton::clicked, this, [this]() { CheckSetupAction_->trigger(); });
    connect(ProjectRepairButton_, &QPushButton::clicked, this, [this]() { SetupProjectAction_->trigger(); });
    connect(ProjectSaveButton_, &QPushButton::clicked, this, [this]() { (void)SaveProjectSettings(); });
    connect(ProjectRunButton_, &QPushButton::clicked, this, [this]() { BuildRunAction_->trigger(); });
    connect(ProjectStopButton_, &QPushButton::clicked, this, [this]() { StopAction_->trigger(); });
    connect(mUseEditorEngineButtonOrNull, &QPushButton::clicked, Controller_, &EditorController::UseEditorEngine);
    auto* basic = new QFormLayout;
    projectLayout->addLayout(basic);
    auto* advanced = new QPushButton(QStringLiteral("Show build and launch details"), form);
    advanced->setObjectName(QStringLiteral("projectAdvanced"));
    advanced->setCheckable(true);
    auto* details = new QWidget(form);
    details->setObjectName(QStringLiteral("projectBuildDetails"));
    auto* formLayout = new QFormLayout(details);
    details->hide();
    projectLayout->addWidget(advanced);
    projectLayout->addWidget(details);
    connect(advanced, &QPushButton::toggled, details, &QWidget::setVisible);
    connect(advanced, &QPushButton::toggled, advanced, [advanced](bool expanded) {
        advanced->setText(expanded ? QStringLiteral("Hide build and launch details")
                                   : QStringLiteral("Show build and launch details"));
    });
    NameEdit_ = new QLineEdit(form);
    NameEdit_->setObjectName(QStringLiteral("projectName"));
    NameEdit_->setAccessibleName(QStringLiteral("Project name"));
    ProviderBox_ = new QComboBox(form);
    ProviderBox_->addItem(QStringLiteral("ludus"));
    ProviderBox_->addItem(QStringLiteral("cmake"));
    SourceDirEdit_ = new QLineEdit(form);
    SourceDirEdit_->setObjectName(QStringLiteral("projectSource"));
    PresetBox_ = new QComboBox(form);
    PresetBox_->addItem(QStringLiteral("Linux: Debug — investigate code"), QStringLiteral("linux-clang-debug"));
    PresetBox_->addItem(QStringLiteral("Linux: Development — everyday work"),
                        QStringLiteral("linux-clang-development"));
    PresetBox_->addItem(QStringLiteral("Linux: Release — distribute the game"), QStringLiteral("linux-clang-release"));
    PresetBox_->addItem(QStringLiteral("macOS: Debug — investigate code"), QStringLiteral("macos-clang-debug"));
    PresetBox_->addItem(QStringLiteral("macOS: Development — everyday work"),
                        QStringLiteral("macos-clang-development"));
    PresetBox_->addItem(QStringLiteral("macOS: Release — distribute the game"), QStringLiteral("macos-clang-release"));
    TargetBox_ = new QComboBox(form);
    TargetBox_->setObjectName(QStringLiteral("projectTarget"));
    TargetBox_->setEditable(true); // target can be entered before configuration
    CwdEdit_ = new QLineEdit(form);

    basic->addRow(QStringLiteral("&Name"), NameEdit_);
    formLayout->addRow(QStringLiteral("&Provider"), ProviderBox_);
    formLayout->addRow(QStringLiteral("&Source directory"), SourceDirEdit_);
    PresetBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    PresetBox_->setMinimumContentsLength(20);
    PresetBox_->setObjectName(QStringLiteral("projectProfile"));
    PresetBox_->setAccessibleName(QStringLiteral("Build profile"));
    PresetBox_->setToolTip(
        QStringLiteral("Development: everyday work. Debug: investigate code. Release: optimized distribution. "
                       "Save a changed profile before checking or repairing its setup."));
    basic->addRow(QStringLiteral("Build &profile"), PresetBox_);
    formLayout->addRow(QStringLiteral("Executable &target"), TargetBox_);
    formLayout->addRow(QStringLiteral("Run &working directory"), CwdEdit_);

    // Argument list editor: one string per item, Add/Remove; never a shell line.
    ArgsList_ = new QListWidget(form);
    ConfigureItemSelection(*ArgsList_);
    ArgsList_->setObjectName(QStringLiteral("projectArguments"));
    connect(ArgsList_->model(), &QAbstractItemModel::dataChanged, this, &MainWindow::OnFieldEdited);
    auto* argsButtons = new QWidget(form);
    auto* argsButtonsLayout = new QHBoxLayout(argsButtons);
    AddArgButton_ = new QPushButton(QStringLiteral("Add argument"), argsButtons);
    RemoveArgButton_ = new QPushButton(QStringLiteral("Remove argument"), argsButtons);
    argsButtonsLayout->addWidget(AddArgButton_);
    argsButtonsLayout->addWidget(RemoveArgButton_);
    argsButtonsLayout->addStretch();
    formLayout->addRow(QStringLiteral("Run &arguments"), ArgsList_);
    formLayout->addRow(QString(), argsButtons);

    projectScroll->setWidget(form);
    // Tool windows scroll their own overflow, so a tabbed neighbour's minimum width never
    // widens the dock column at the documents' expense.
    QScrollArea* contentScroll = new QScrollArea(mContentDock);
    contentScroll->setObjectName(QStringLiteral("contentScroll"));
    contentScroll->setWidgetResizable(true);
    contentScroll->setFrameShape(QFrame::NoFrame);
    contentScroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    Content_ = new ContentWorkspace(contentScroll);
    contentScroll->setWidget(Content_);
    mContentDock->setObjectName(QStringLiteral("contentDock"));
    mContentDock->setWidget(contentScroll);
    QScrollArea* audioScroll = mAudioDocument;
    audioScroll->setObjectName(QStringLiteral("audioWorkspaceScroll"));
    audioScroll->setWidgetResizable(true);
    audioScroll->setFrameShape(QFrame::NoFrame);
    Audio_ = new AudioWorkspace(audioScroll);
    audioScroll->setWidget(Audio_);
    World_ = new WorldWorkspace(Controller_->Tooling(), mDocuments);
    World_->setFocusPolicy(Qt::StrongFocus);
    Scripts_ = new ScriptWorkspace(Controller_, mDocuments);
    Renderer_ = new RendererWorkspace(Controller_->Tooling(), mDocuments);
    mRenderingSettingsOrNull = new ProjectRenderingSettings(*Renderer_, form);
    projectLayout->insertWidget(projectLayout->indexOf(advanced), mRenderingSettingsOrNull);
    // The render-graph preview borrows the device and extraction of whichever viewport the
    // World shows (its imported scene or its schema-bound panel) and follows it when it changes.
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    World_->UseProjectGraph(*Renderer_);
    Renderer_->EnableWorldRendering();
#endif
    Renderer_->SetPreviewSource(&World_->GetRenderPreview());
    connect(World_, &WorldWorkspace::RenderPreviewChanged, Renderer_, [this]() {
        Renderer_->SetPreviewSource(&World_->GetRenderPreview());
    });
    Configuration_ = new ConfigurationWorkspace(mDocuments);

    // Properties follows the active document: the World's entity inspector, the render
    // graph's node inspector, or a hint. Page order matches PropertiesPage.
    mPropertiesDock->setObjectName(QStringLiteral("propertiesDock"));
    mProperties->setObjectName(QStringLiteral("propertiesStack"));
    mNoProperties->setObjectName(QStringLiteral("propertiesEmpty"));
    mNoProperties->setWordWrap(true);
    mNoProperties->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    mNoProperties->setMargin(PROPERTIES_HINT_MARGIN_PIXELS);
    mNoProperties->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mProperties->addWidget(mNoProperties);
    mProperties->addWidget(World_->TakePropertiesPanel(mProperties));
    mProperties->addWidget(Renderer_->TakeInspectorPanel(mProperties));
    mProperties->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mPropertiesDock->setWidget(mProperties);
    // The render-graph preview shows the World viewport through the open graph.
    mRenderPreviewDock->setObjectName(QStringLiteral("renderPreviewDock"));
    QWidget* renderPreview = Renderer_->TakePreviewPanel(mRenderPreviewDock);
    renderPreview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mRenderPreviewDock->setWidget(renderPreview);

    mProjectExplorerDock->setObjectName(QStringLiteral("projectExplorerDock"));
    mProjectExplorerDock->setWidget(mProjectExplorer);
    mErrorListDock->setObjectName(QStringLiteral("errorListDock"));
    mErrorListDock->setWidget(mErrorList);

    InspectorDock_ = new QDockWidget(QStringLiteral("Live Inspector"), this);
    InspectorDock_->setObjectName(QStringLiteral("liveInspectorDock"));
    auto* inspectorScroll = new QScrollArea(InspectorDock_);
    inspectorScroll->setObjectName(QStringLiteral("liveInspectorScroll"));
    inspectorScroll->setWidgetResizable(true);
    inspectorScroll->setFrameShape(QFrame::NoFrame);
    inspectorScroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto* inspector = new QWidget(inspectorScroll);
    auto* inspectorLayout = new QVBoxLayout(inspector);
    // --- Runtime status area (describes the running app; not a framebuffer) ---
    RuntimeLabel_ = new QLabel(inspector);
    RuntimeLabel_->setText(QStringLiteral("No runtime."));
    RuntimeLabel_->setWordWrap(true);
    Properties_ = new QTableWidget(inspector);
    Properties_->setColumnCount(3);
    Properties_->setHorizontalHeaderLabels(
        {QStringLiteral("Property"), QStringLiteral("Value"), QStringLiteral("Scope")});
    Properties_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    Properties_->setSelectionBehavior(QAbstractItemView::SelectRows);
    ConfigureItemSelection(*Properties_);
    ApplySessionButton_ = new QPushButton(QStringLiteral("Apply to session"), inspector);
    ApplySessionButton_->setToolTip(QStringLiteral("Apply the selected edited value to the running session."));
    connect(ApplySessionButton_, &QPushButton::clicked, this, [this]() {
        const auto row = Properties_->currentRow();
        if (row >= 0 && Properties_->item(row, 1) != nullptr)
        {
            Controller_->EditProperty(row, Properties_->item(row, 1)->text());
        }
    });
    ApplyDocumentButton_ = new QPushButton(QStringLiteral("Copy live value to tuning draft"), inspector);
    ApplyDocumentButton_->setToolTip(
        QStringLiteral("Copy the selected live value into the tuning draft; save separately."));
    connect(ApplyDocumentButton_, &QPushButton::clicked, this, [this]() {
        Controller_->ApplyLivePropertyToTuningDocument(Properties_->currentRow());
    });
    connect(Properties_, &QTableWidget::currentCellChanged, this, [this]() {
        ApplyDocumentButton_->setEnabled(Controller_->CanApplyToTuningDocument(Properties_->currentRow()));
    });

    Properties_->setObjectName(QStringLiteral("liveProperties"));
    Properties_->setAccessibleName(QStringLiteral("Live runtime properties"));
    inspectorLayout->addWidget(RuntimeLabel_);
    inspectorLayout->addWidget(Properties_, 1);
    inspectorLayout->addWidget(ApplySessionButton_);
    inspectorLayout->addWidget(ApplyDocumentButton_);
    inspectorScroll->setWidget(inspector);
    InspectorDock_->setWidget(inspectorScroll);

    TimingDock_->setObjectName(QStringLiteral("timingDock"));
    Timing_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    TimingDock_->setWidget(Timing_);

    OutputDock_ = new QDockWidget(QStringLiteral("Output"), this);
    OutputDock_->setObjectName(QStringLiteral("outputDock"));
    QWidget* output = new QWidget(OutputDock_);
    QVBoxLayout* outputLayout = new QVBoxLayout(output);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    outputLayout->setSpacing(0);
    QToolBar* outputTools = new QToolBar(QStringLiteral("Output"), output);
    outputTools->setObjectName(QStringLiteral("outputToolbar"));
    outputTools->setToolButtonStyle(Qt::ToolButtonTextOnly);
    outputLayout->addWidget(outputTools);
    Output_ = new QPlainTextEdit(output);
    Output_->setObjectName(QStringLiteral("workspaceOutput"));
    Output_->setAccessibleName(QStringLiteral("Build and runtime output"));
    Output_->setReadOnly(true);
    Output_->setMaximumBlockCount(5000);
    outputLayout->addWidget(Output_, 1);
    OutputDock_->setWidget(output);
    arrangeDefaultDocks();

    StatusLabel_ = new QLabel(this);
    StatusLabel_->setObjectName(QStringLiteral("workspaceStatus"));
    StatusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addWidget(StatusLabel_, 1);

    // Thanks to David Lightbown, Designing the User Experience of Game
    // Development Tools (2015), ch. 5, pp. 80-86, 112-115: show task-relevant choices
    // progressively. Recents belong to Welcome and File, freeing authoring space.
    // https://www.uxofgametools.com/; docs/architecture/editor-design-review.md.
    Welcome_ = new QWidget(mDocuments);
    Welcome_->setObjectName(QStringLiteral("projectWelcome"));
    auto* recentPanel = Welcome_;
    auto* recentLayout = new QVBoxLayout(recentPanel);
    RecentEmptyLabel_ =
        new QLabel(QStringLiteral("No recent projects yet. Open a project to add it here."), recentPanel);
    RecentEmptyLabel_->setWordWrap(true);
    RecentList_ = new QListWidget(recentPanel);
    ConfigureItemSelection(*RecentList_);
    RecentList_->setObjectName(QStringLiteral("recentProjectsList"));
    RecentList_->setAccessibleName(QStringLiteral("Recent projects"));
    RecentList_->setWordWrap(true);
    RecentOpenButton_ = new QPushButton(QStringLiteral("Open Selected"), recentPanel);
    RecentOpenButton_->setObjectName(QStringLiteral("openRecentProjectButton"));
    BrowseProjectButton_ = new QPushButton(QStringLiteral("Open Project..."), recentPanel);
    recentLayout->setContentsMargins(24, 24, 24, 24);
    recentLayout->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("Welcome to Ludus"), recentPanel);
    auto* brand = new QLabel(recentPanel);
    brand->setObjectName(QStringLiteral("welcomeLogo"));
    brand->setAccessibleName(QStringLiteral("Ludus logo"));
    brand->setPixmap(QIcon(QStringLiteral(":/ludus/ludus-mark.png")).pixmap(48, 48));
    auto font = title->font();
    font.setBold(true);
    font.setPointSize(font.pointSize() + 6);
    title->setFont(font);
    auto* intro = new QLabel(QStringLiteral("Create a project or continue where you left off."), recentPanel);
    intro->setWordWrap(true);
    intro->setProperty("visualRole", QStringLiteral("secondary"));
    WelcomeNewButton_ = new QPushButton(QStringLiteral("New Project..."), recentPanel);
    WelcomeNewButton_->setObjectName(QStringLiteral("welcomeNewProject"));
    WelcomeNewButton_->setProperty("visualRole", QStringLiteral("primary"));
    auto* brandHeading = new QHBoxLayout;
    brandHeading->addWidget(brand);
    brandHeading->addWidget(title);
    brandHeading->addStretch();
    recentLayout->addLayout(brandHeading);
    recentLayout->addWidget(intro);
    auto* welcomeActions = new QHBoxLayout;
    welcomeActions->addWidget(WelcomeNewButton_);
    welcomeActions->addWidget(BrowseProjectButton_);
    welcomeActions->addStretch();
    recentLayout->addLayout(welcomeActions);
    recentLayout->addSpacing(12);
    recentLayout->addWidget(new QLabel(QStringLiteral("Recent projects"), recentPanel));
    recentLayout->addWidget(RecentEmptyLabel_);
    RecentEmptyLabel_->setAlignment(Qt::AlignTop);
    recentLayout->addWidget(RecentList_, 1);
    recentLayout->addStretch();
    auto* recentActions = new QHBoxLayout;
    recentActions->addWidget(RecentOpenButton_);
    recentActions->addStretch();
    recentLayout->addLayout(recentActions);
    // Document registration order is the Window menu order.
    mDocuments->AddDocument(Welcome_, QStringLiteral("Start Page"));
    mDocuments->AddDocument(World_, QStringLiteral("World"));
    mDocuments->AddDocument(Renderer_, QStringLiteral("Render Graph"));
    mDocuments->AddDocument(Scripts_, QStringLiteral("Scripts"));
    mDocuments->AddDocument(ProjectSettings_, QStringLiteral("Project Settings"));
    mDocuments->AddDocument(audioScroll, QStringLiteral("Audio"));
    mDocuments->AddDocument(Configuration_, QStringLiteral("Configuration"));
    static_cast<void>(mDocuments->OpenDocument(Welcome_));
    connect(RecentList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const QString path = item->data(Qt::UserRole).toString();
        QTimer::singleShot(0, this, [this, path]() { OpenProjectPath(path); });
    });
    connect(RecentList_, &QListWidget::currentItemChanged, this, [this]() {
        RecentOpenButton_->setEnabled(Controller_->Caps().CanOpen && RecentList_->currentItem() != nullptr);
    });
    connect(RecentOpenButton_, &QPushButton::clicked, this, [this]() {
        if (const auto* item = RecentList_->currentItem())
        {
            OpenProjectPath(item->data(Qt::UserRole).toString());
        }
    });
    connect(BrowseProjectButton_, &QPushButton::clicked, this, &MainWindow::OnOpenRequested);

    // Typed signal connections (pointer-to-member); no string-based SIGNAL/SLOT.
    connect(NameEdit_, &QLineEdit::textEdited, this, &MainWindow::OnFieldEdited);
    connect(SourceDirEdit_, &QLineEdit::textEdited, this, &MainWindow::OnFieldEdited);
    connect(CwdEdit_, &QLineEdit::textEdited, this, &MainWindow::OnFieldEdited);
    connect(ProviderBox_, &QComboBox::currentTextChanged, this, &MainWindow::OnFieldEdited);
    connect(PresetBox_, &QComboBox::currentTextChanged, this, &MainWindow::OnFieldEdited);
    connect(TargetBox_, &QComboBox::currentTextChanged, this, &MainWindow::OnFieldEdited);
    connect(AddArgButton_, &QPushButton::clicked, this, &MainWindow::OnAddArgument);
    connect(RemoveArgButton_, &QPushButton::clicked, this, &MainWindow::OnRemoveArgument);
}

void MainWindow::InitializeWorkspace()
{
    // Most of the screen goes to the editor, and most of the editor to documents.
    QSize size(MIN_WINDOW_WIDTH_PIXELS, MIN_WINDOW_HEIGHT_PIXELS);
    if (const QScreen* screenOrNull = QGuiApplication::primaryScreen(); screenOrNull != nullptr)
    {
        const QRect available = screenOrNull->availableGeometry();
        size = size.expandedTo(QSize(available.width() * DEFAULT_WINDOW_PERCENT / PERCENT,
                                     available.height() * DEFAULT_WINDOW_PERCENT / PERCENT));
    }
    resize(size);
    applyDefaultDockSizes();
    DefaultLayout_ = saveState(LAYOUT_VERSION);
    if (WorkspaceSettingsFile_.isEmpty())
    {
        const auto config = (QDir::isAbsolutePath(qEnvironmentVariable("XDG_CONFIG_HOME"))
                                 ? qEnvironmentVariable("XDG_CONFIG_HOME")
                                 : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
        if (config.isEmpty() == false)
        {
            WorkspaceSettingsFile_ = QDir(config).filePath(QStringLiteral("Ludus/Editor/workspace.json"));
        }
    }
    QFile file(WorkspaceSettingsFile_);
    if (WorkspaceSettingsFile_.isEmpty() || file.exists() == false)
    {
        return;
    }
    // This local file is only for state generated by this editor. Never load a
    // layout blob from a project, downloaded file, or host protocol response.
    QByteArray bytes;
    if (file.open(QIODevice::ReadOnly) && file.size() <= MAX_SETTINGS_BYTES)
    {
        bytes = file.read(MAX_SETTINGS_BYTES + 1);
        if (file.error() != QFileDevice::NoError || bytes.size() > MAX_SETTINGS_BYTES)
        {
            bytes.clear();
        }
    }
    const auto document = QJsonDocument::fromJson(bytes);
    const auto object = document.object();
    const auto geometryEncoded = object.value(QStringLiteral("geometry")).toString().toLatin1();
    const auto stateEncoded = object.value(QStringLiteral("state")).toString().toLatin1();
    const auto geometry = QByteArray::fromBase64(geometryEncoded);
    const auto state = QByteArray::fromBase64(stateEncoded);
    const auto defaultGeometry = saveGeometry();
    const bool compatible = document.isObject() && object.value(QStringLiteral("version")).toInt() == LAYOUT_VERSION &&
                            object.value(QStringLiteral("qtVersion")).toString() == QString::fromLatin1(qVersion()) &&
                            geometry.isEmpty() == false && geometry.size() <= 4096 && state.isEmpty() == false &&
                            state.size() <= MAX_STATE_BYTES && geometry.toBase64() == geometryEncoded &&
                            state.toBase64() == stateEncoded;
    if (compatible == false || restoreGeometry(geometry) == false || restoreState(state, LAYOUT_VERSION) == false)
    {
        (void)restoreGeometry(defaultGeometry);
        ResetWorkspaceLayout();
        statusBar()->showMessage(QStringLiteral("Workspace layout was incompatible or invalid. Using defaults."),
                                 10000);
        return;
    }
    mShouldSizeDefaultDocks = false;
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (mShouldSizeDefaultDocks == true)
    {
        // Dock sizes requested before the first layout pass are not honoured reliably;
        // size the default arrangement against the real window once it is shown.
        mShouldSizeDefaultDocks = false;
        QTimer::singleShot(0, this, &MainWindow::applyDefaultDockSizes);
    }
}

void MainWindow::arrangeDefaultDocks()
{
    // Calm default: the documents get the window. Left: Project Explorer (with Content and Render
    // Preview). Right: Properties (with Live Inspector), so a node's properties stay visible beside
    // its preview. Bottom, under the documents: Error List with Output, Timing and Profiling; that
    // group starts closed and opens when there is something to show (see applyDefaultDockSizes).
    const QList<QDockWidget*> docks = getToolWindows();
    for (QDockWidget* dock : docks)
    {
        dock->setFloating(false);
        removeDockWidget(dock);
    }
    addDockWidget(Qt::LeftDockWidgetArea, mProjectExplorerDock);
    tabifyDockWidget(mProjectExplorerDock, mContentDock);
    tabifyDockWidget(mProjectExplorerDock, mRenderPreviewDock);
    addDockWidget(Qt::RightDockWidgetArea, mPropertiesDock);
    tabifyDockWidget(mPropertiesDock, InspectorDock_);
    addDockWidget(Qt::BottomDockWidgetArea, mErrorListDock);
    tabifyDockWidget(mErrorListDock, OutputDock_);
    tabifyDockWidget(mErrorListDock, TimingDock_);
    tabifyDockWidget(mErrorListDock, mProfilingDock);
    for (QDockWidget* dock : docks)
    {
        dock->show();
    }
    mProjectExplorerDock->raise();
    mPropertiesDock->raise();
    mErrorListDock->raise();
}

QList<QDockWidget*> MainWindow::getToolWindows() const
{
    const QList<QDockWidget*> docks{
        mProjectExplorerDock,
        mContentDock,
        mRenderPreviewDock,
        mPropertiesDock,
        InspectorDock_,
        mErrorListDock,
        OutputDock_,
        TimingDock_,
        mProfilingDock,
    };
    return docks;
}

void MainWindow::setViewportFocused(bool isFocused)
{
    {
        const QSignalBlocker blocker(mFocusViewportAction);
        mFocusViewportAction->setChecked(isFocused);
    }
    if (isFocused == mIsViewportFocused)
    {
        return;
    }
    if (isFocused == true)
    {
        // Captured before anything is hidden, so leaving restores docks, tabs, sizes and toolbars.
        mFocusRestoreState = saveState(LAYOUT_VERSION);
        mFocusRestoreDocumentOrNull = mDocuments->GetActiveDocumentOrNull();
        mIsViewportFocused = true;
        for (QDockWidget* dock : getToolWindows())
        {
            dock->hide();
        }
        for (QToolBar* toolbar : {mStandardToolbar, GameToolbar_, mRenderGraphToolbar, mSetupNoticeOrNull})
        {
            toolbar->hide();
        }
        // The viewport widgets stay where they are; only their neighbours hide, so the
        // native surface resizes instead of being recreated.
        if (mFocusRestoreDocumentOrNull == Renderer_)
        {
            Renderer_->SetChromeVisible(false);
            Renderer_->GetCanvas().setFocus(Qt::ShortcutFocusReason);
        }
        else
        {
            World_->SetChromeVisible(false);
            static_cast<void>(mDocuments->OpenDocument(World_));
        }
        return;
    }
    mIsViewportFocused = false;
    World_->SetChromeVisible(true);
    Renderer_->SetChromeVisible(true);
    if (restoreState(mFocusRestoreState, LAYOUT_VERSION) == false)
    {
        LUDUS_LOG_TEXT(LOG_EDITOR_LAYOUT, Warning, "Could not restore the layout from before Focus Viewport.");
    }
    mFocusRestoreState.clear();
    QWidget* previousOrNull = mFocusRestoreDocumentOrNull;
    mFocusRestoreDocumentOrNull = nullptr;
    if (previousOrNull != nullptr && mDocuments->IsDocumentAvailable(previousOrNull) == true)
    {
        static_cast<void>(mDocuments->OpenDocument(previousOrNull));
    }
    // The setup notice follows project state, not the restored toolbar visibility.
    RenderStatus();
}

QByteArray MainWindow::getPersistentLayoutState() const
{
    return mIsViewportFocused == true ? mFocusRestoreState : saveState(LAYOUT_VERSION);
}

foundation::int32 MainWindow::getDefaultBottomDockHeight() const
{
    return qBound(MIN_BOTTOM_DOCK_PIXELS, height() * BOTTOM_DOCK_PERCENT / PERCENT, MAX_BOTTOM_DOCK_PIXELS);
}

void MainWindow::applyDefaultDockSizes()
{
    const foundation::int32 explorer =
        qBound(MIN_EXPLORER_DOCK_PIXELS, width() * EXPLORER_DOCK_PERCENT / PERCENT, MAX_EXPLORER_DOCK_PIXELS);
    const foundation::int32 properties =
        qBound(MIN_PROPERTIES_DOCK_PIXELS, width() * PROPERTIES_DOCK_PERCENT / PERCENT, MAX_PROPERTIES_DOCK_PIXELS);
    // The diagnostics group is closed until it has something to show: new errors or warnings reveal the
    // Error List, a build reveals Output, a play session reveals Timing and recording reveals Profiling.
    for (QDockWidget* dock : {mErrorListDock, OutputDock_, TimingDock_, mProfilingDock})
    {
        dock->hide();
    }
    if (mErrorList->GetCount(ErrorListSeverity::Error) != 0 || mErrorList->GetCount(ErrorListSeverity::Warning) != 0)
    {
        openToolWindow(mErrorListDock);
    }
    resizeDocks({mProjectExplorerDock, mPropertiesDock}, {explorer, properties}, Qt::Horizontal);
}

void MainWindow::ResetWorkspaceLayout()
{
    setViewportFocused(false);
    mHasTimingOpenedForPlay = false;
    (void)restoreState(DefaultLayout_, LAYOUT_VERSION);
    // Also recover panes that were floating, closed or moved when the user requested reset.
    arrangeDefaultDocks();
    for (QToolBar* toolbar : {mStandardToolbar, GameToolbar_, mRenderGraphToolbar})
    {
        toolbar->show();
    }
    applyDefaultDockSizes();
    RenderStatus();
}

void MainWindow::buildWindowMenu(QMenu* menu)
{
    const auto addCommand = [this, menu](const EditorCommand& command) {
        QAction* action = CreateEditorCommand(command, this);
        menu->addAction(action);
        return action;
    };
    QAction* nextDocument = addCommand(WINDOW_NEXT_DOCUMENT_COMMAND);
    QAction* previousDocument = addCommand(WINDOW_PREVIOUS_DOCUMENT_COMMAND);
    connect(nextDocument, &QAction::triggered, this, [this]() {
        static_cast<void>(mDocuments->ActivateAdjacentDocument(true));
    });
    connect(previousDocument, &QAction::triggered, this, [this]() {
        static_cast<void>(mDocuments->ActivateAdjacentDocument(false));
    });
    menu->addSeparator();
    QAction* vertical = addCommand(WINDOW_NEW_VERTICAL_GROUP_COMMAND);
    QAction* horizontal = addCommand(WINDOW_NEW_HORIZONTAL_GROUP_COMMAND);
    QAction* next = addCommand(WINDOW_MOVE_NEXT_GROUP_COMMAND);
    QAction* previous = addCommand(WINDOW_MOVE_PREVIOUS_GROUP_COMMAND);
    QAction* merge = addCommand(WINDOW_MERGE_GROUPS_COMMAND);
    menu->addSeparator();
    QAction* close = addCommand(WINDOW_CLOSE_DOCUMENT_COMMAND);
    QAction* closeAll = addCommand(WINDOW_CLOSE_ALL_DOCUMENTS_COMMAND);
    connect(vertical, &QAction::triggered, this, [this]() {
        static_cast<void>(mDocuments->SplitActiveDocument(Qt::Horizontal));
    });
    connect(horizontal, &QAction::triggered, this, [this]() {
        static_cast<void>(mDocuments->SplitActiveDocument(Qt::Vertical));
    });
    connect(next, &QAction::triggered, this, [this]() { static_cast<void>(mDocuments->MoveActiveDocument(1)); });
    connect(previous, &QAction::triggered, this, [this]() { static_cast<void>(mDocuments->MoveActiveDocument(-1)); });
    connect(merge, &QAction::triggered, mDocuments, &DocumentWell::MergeGroups);
    // Both close commands go through the well, so an owner-closed document asks its owner
    // through DocumentWell::CloseRequested instead of being hidden.
    connect(close, &QAction::triggered, this, [this]() {
        QWidget* activeOrNull = mDocuments->GetActiveDocumentOrNull();
        if (activeOrNull != nullptr)
        {
            mDocuments->CloseDocument(activeOrNull);
        }
    });
    connect(closeAll, &QAction::triggered, mDocuments, &DocumentWell::CloseAllDocuments);
    // Group commands are enabled from the well's current arrangement when the menu opens.
    connect(menu, &QMenu::aboutToShow, this, [this, menu, vertical, horizontal, next, previous, merge, close]() {
        vertical->setEnabled(mDocuments->CanSplit(Qt::Horizontal));
        horizontal->setEnabled(mDocuments->CanSplit(Qt::Vertical));
        const foundation::int32 group = mDocuments->GetActiveGroupIndex();
        next->setEnabled(mDocuments->GetActiveDocumentOrNull() != nullptr && group + 1 < mDocuments->GetGroupCount());
        previous->setEnabled(mDocuments->GetActiveDocumentOrNull() != nullptr && group > 0);
        merge->setEnabled(mDocuments->GetGroupCount() > 1);
        close->setEnabled(mDocuments->GetActiveDocumentOrNull() != nullptr);
        renderWindowDocuments(menu);
    });
    menu->addSeparator();
    // Every tool window can be shown or hidden here (checked while visible).
    for (QDockWidget* dock : {mProjectExplorerDock,
                              mPropertiesDock,
                              mErrorListDock,
                              OutputDock_,
                              mContentDock,
                              mRenderPreviewDock,
                              InspectorDock_,
                              TimingDock_,
                              mProfilingDock})
    {
        menu->addAction(dock->toggleViewAction());
    }
    menu->addSeparator();
    menu->addAction(mResetLayoutAction);
    connect(mResetLayoutAction, &QAction::triggered, this, &MainWindow::ResetWorkspaceLayout);
    menu->addSeparator()->setObjectName(QStringLiteral("windowDocumentsSeparator"));
}

void MainWindow::renderWindowDocuments(QMenu* menu)
{
    // Visual Studio lists open windows last; Ludus lists every available document, checked
    // when its tab is open, so closed documents can be reopened from here too.
    for (QAction* action : menu->actions())
    {
        if (action->property("windowDocument").isValid() == true)
        {
            menu->removeAction(action);
            action->deleteLater();
        }
    }
    foundation::int32 number = 1;
    for (QWidget* document : mDocuments->GetDocuments())
    {
        if (mDocuments->IsDocumentAvailable(document) == false)
        {
            continue;
        }
        QString title = mDocuments->GetDocumentTitle(document);
        title.replace(QStringLiteral("&"), QStringLiteral("&&"));
        QAction* action = menu->addAction(QStringLiteral("&%1 %2").arg(number).arg(title));
        action->setProperty("windowDocument", document->objectName());
        action->setCheckable(true);
        action->setChecked(mDocuments->IsDocumentOpen(document));
        connect(action, &QAction::triggered, this, [this, document]() {
            static_cast<void>(mDocuments->OpenDocument(document));
        });
        ++number;
    }
}

void MainWindow::openToolWindow(QDockWidget* dock)
{
    const bool wasHidden = dock->isHidden();
    dock->show();
    dock->raise();
    if (wasHidden == true && dockWidgetArea(dock) == Qt::BottomDockWidgetArea)
    {
        // A short window starts with the bottom group closed; open it at the default height.
        resizeDocks({dock}, {getDefaultBottomDockHeight()}, Qt::Vertical);
    }
}

void MainWindow::showToolWindow(QDockWidget* dock)
{
    openToolWindow(dock);
    dock->widget()->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::SaveWorkspaceLayout() const
{
    SaveProjectWorkspace();
    if (WorkspaceSettingsFile_.isEmpty())
    {
        return;
    }
    const QByteArray state = getPersistentLayoutState();
    const auto geometry = saveGeometry();
    if (state.size() > MAX_STATE_BYTES || geometry.size() > 4096 ||
        QDir().mkpath(QFileInfo(WorkspaceSettingsFile_).absolutePath()) == false)
    {
        LUDUS_LOG_TEXT(LOG_EDITOR_LAYOUT, Warning, "Could not save workspace layout; document saves are unaffected.");
        return;
    }
    QJsonObject object;
    object.insert(QStringLiteral("version"), LAYOUT_VERSION);
    object.insert(QStringLiteral("qtVersion"), QString::fromLatin1(qVersion()));
    object.insert(QStringLiteral("geometry"), QString::fromLatin1(geometry.toBase64()));
    object.insert(QStringLiteral("state"), QString::fromLatin1(state.toBase64()));
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QSaveFile file(WorkspaceSettingsFile_);
    file.setDirectWriteFallback(false);
    if (bytes.size() > MAX_SETTINGS_BYTES || file.open(QIODevice::WriteOnly) == false ||
        file.write(bytes) != bytes.size() || file.commit() == false)
    {
        LUDUS_LOG_TEXT(LOG_EDITOR_LAYOUT, Warning, "Could not save workspace layout; document saves are unaffected.");
    }
}

void MainWindow::SaveProjectWorkspace() const
{
    if (ProjectLayoutRoot_.isEmpty() || WorkspaceSettingsFile_.isEmpty())
    {
        return;
    }
    const auto key = QCryptographicHash::hash(ProjectLayoutRoot_.toUtf8(), QCryptographicHash::Sha256).toHex();
    const auto path = QFileInfo(WorkspaceSettingsFile_).absolutePath() + QStringLiteral("/projects/") +
                      QString::fromLatin1(key) + QStringLiteral(".json");
    QJsonObject object{{QStringLiteral("version"), LAYOUT_VERSION},
                       {QStringLiteral("qtVersion"), QString::fromLatin1(qVersion())},
                       {QStringLiteral("root"), ProjectLayoutRoot_},
                       {QStringLiteral("state"), QString::fromLatin1(getPersistentLayoutState().toBase64())},
                       {QStringLiteral("documents"), mDocuments->SaveState()},
                       {QStringLiteral("world"), World_->Session()}};
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (bytes.size() > MAX_SETTINGS_BYTES || QDir().mkpath(QFileInfo(path).absolutePath()) == false ||
        file.open(QIODevice::WriteOnly) == false || file.write(bytes) != bytes.size() || file.commit() == false)
    {
        LUDUS_LOG_TEXT(LOG_EDITOR_LAYOUT,
                       Warning,
                       "Could not save project viewport preferences; document saves are unaffected.");
    }
}
void MainWindow::SetProjectWorkspace(const QString& root, bool restore)
{
    const auto canonical = root.isEmpty() ? QString() : QFileInfo(root).canonicalFilePath();
    if (restore == false)
    {
        if (canonical != ProjectLayoutRoot_)
        {
            SaveProjectWorkspace();
            ProjectLayoutRoot_ = canonical;
        }
        return;
    }
    if (canonical.isEmpty() || WorkspaceSettingsFile_.isEmpty())
    {
        return;
    }
    // A project's saved arrangement replaces whatever Focus Viewport would have returned to.
    setViewportFocused(false);
    const auto key = QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex();
    QFile file(QFileInfo(WorkspaceSettingsFile_).absolutePath() + QStringLiteral("/projects/") +
               QString::fromLatin1(key) + QStringLiteral(".json"));
    if (file.open(QIODevice::ReadOnly) == false || file.size() > MAX_SETTINGS_BYTES)
    {
        return;
    }
    const auto bytes = file.read(MAX_SETTINGS_BYTES + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > MAX_SETTINGS_BYTES)
    {
        return;
    }
    const auto document = QJsonDocument::fromJson(bytes);
    const auto object = document.object();
    if (document.isObject() == false || object.value(QStringLiteral("version")).toInt() != LAYOUT_VERSION ||
        object.value(QStringLiteral("qtVersion")).toString() != QString::fromLatin1(qVersion()) ||
        object.value(QStringLiteral("root")).toString() != canonical)
    {
        return;
    }
    const auto encoded = object.value(QStringLiteral("state")).toString().toLatin1();
    const auto state = QByteArray::fromBase64(encoded);
    const auto previous = saveState(LAYOUT_VERSION);
    if (state.isEmpty() == false && state.size() <= MAX_STATE_BYTES && state.toBase64() == encoded &&
        restoreState(state, LAYOUT_VERSION) == false)
    {
        (void)restoreState(previous, LAYOUT_VERSION);
    }
    World_->RestoreSession(object.value(QStringLiteral("world")).toObject());
    // Malformed or stale arrangements leave the project's default World document open.
    static_cast<void>(mDocuments->RestoreState(object.value(QStringLiteral("documents")).toObject()));
    if (mDocuments->GetActiveDocumentOrNull() == nullptr)
    {
        static_cast<void>(mDocuments->OpenDocument(World_));
    }
}

} // namespace ludus::editor
