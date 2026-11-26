#include "design_tokens.h"
#include "internal/activity_chip.h"
#include "internal/audio_workspace.h"
#include "internal/code_document_tab.h"
#include "internal/code_editor.h"
#include "internal/code_editor_area.h"
#include "internal/code_file_document.h"
#include "internal/code_language_registry.h"
#include "internal/code_references_window.h"
#include "internal/code_status_bar.h"
#include "internal/configuration_workspace.h"
#include "internal/content_browser.h"
#include "internal/document_well.h"
#include "internal/edit_history.h"
#include "internal/editor_commands.h"
#include "internal/editor_icons.h"
#include "internal/editor_style.h"
#include "internal/engine_pin.h"
#include "internal/error_list.h"
#include "internal/item_selection.h"
#include "internal/main_window.h"
#include "internal/project_explorer.h"
#include "internal/project_rendering_settings.h"
#include "internal/project_store.h"
#include "internal/recent_project_delegate.h"
#include "internal/renderer_workspace.h"
#include "internal/script_workspace.h"
#include "internal/search_tool_window.h"
#include "internal/settings_page.h"
#include "internal/state_view.h"
#include "internal/theme_fonts.h"
#include "internal/timing_panel.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QByteArray>
#include <QClipboard>
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
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
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
#include <QTabBar>
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
constexpr foundation::int32 LAYOUT_VERSION = 7;
constexpr foundation::int32 PERCENT = 100;
/// How long a World edit message stays in the status bar before the workspace status returns.
constexpr foundation::int32 WORLD_STATUS_MESSAGE_MILLISECONDS = 8000;
// First-run window: this share of the screen, at least the minimum below.
constexpr foundation::int32 DEFAULT_WINDOW_PERCENT = 85;
constexpr foundation::int32 MIN_WINDOW_WIDTH_PIXELS = 1100;
constexpr foundation::int32 MIN_WINDOW_HEIGHT_PIXELS = 760;
// Default tool-window extents leave documents about three fifths of the window: the outliner is the
// narrower side, Properties the wider one.
// The explorer grows with the window for longer document names; its minimum keeps a World document beside both
// side tool windows usable when a window is made small after its layout was set.
constexpr foundation::int32 EXPLORER_DOCK_PERCENT = 18;
constexpr foundation::int32 MIN_EXPLORER_DOCK_PIXELS = 220;
constexpr foundation::int32 MAX_EXPLORER_DOCK_PIXELS = 360;
constexpr foundation::int32 PROPERTIES_DOCK_PERCENT = 20;
constexpr foundation::int32 MIN_PROPERTIES_DOCK_PIXELS = 260;
constexpr foundation::int32 MAX_PROPERTIES_DOCK_PIXELS = 380;
// The bottom group needs a tab bar, the Error List filters and header, and a few rows.
constexpr foundation::int32 BOTTOM_DOCK_PERCENT = 27;
constexpr foundation::int32 MIN_BOTTOM_DOCK_PIXELS = 160;
constexpr foundation::int32 MAX_BOTTOM_DOCK_PIXELS = 300;
constexpr foundation::int32 PROPERTIES_HINT_MARGIN_PIXELS = design::SPACING_SMALL;
constexpr foundation::int64 MAX_SETTINGS_BYTES = foundation::int64{64} * 1024;
constexpr foundation::int32 MAX_STATE_BYTES = 32 * 1024;
/// Width of the setup notice's progress bar, and how long its "ready" confirmation stays.
constexpr foundation::int32 NOTICE_PROGRESS_WIDTH = 160;
constexpr foundation::int32 NOTICE_PROGRESS_HEIGHT = design::SPACING_SMALL;
constexpr foundation::int32 NOTICE_READY_MILLISECONDS = 6000;
/// Fewest characters of a build profile name the profile field keeps when the page is narrow.
constexpr foundation::int32 MIN_PROFILE_CHARACTERS = 20;
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
    // One compact line that walks a repair through its states: what is wrong and one primary action
    // (Repair project), then progress while it runs, then a short confirmation. The full explanation is the
    // text's tooltip; the status bar keeps reporting setup state. Nothing here names an internal result code.
    QHBoxLayout* noticeLayout = new QHBoxLayout(notice);
    noticeLayout->setContentsMargins(0, 0, 0, 0);
    noticeLayout->setSpacing(design::SPACING_SMALL);
    mSetupNoticeIconOrNull = new QLabel(notice);
    mSetupNoticeIconOrNull->setObjectName(QStringLiteral("projectSetupNoticeIcon"));
    mSetupNoticeIconOrNull->setFixedSize(EDITOR_ICON_SIZE, EDITOR_ICON_SIZE);
    noticeLayout->addWidget(mSetupNoticeIconOrNull);
    mSetupNoticeTextOrNull = new QLabel(notice);
    mSetupNoticeTextOrNull->setObjectName(QStringLiteral("projectSetupNoticeText"));
    mSetupNoticeTextOrNull->setTextFormat(Qt::PlainText);
    mSetupNoticeTextOrNull->setWordWrap(false);
    QFont headlineFont = mSetupNoticeTextOrNull->font();
    headlineFont.setBold(true);
    mSetupNoticeTextOrNull->setFont(headlineFont);
    mSetupNoticeTextOrNull->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    noticeLayout->addWidget(mSetupNoticeTextOrNull);
    mSetupNoticeDetailOrNull = new QLabel(notice);
    mSetupNoticeDetailOrNull->setObjectName(QStringLiteral("projectSetupNoticeDetail"));
    mSetupNoticeDetailOrNull->setTextFormat(Qt::PlainText);
    mSetupNoticeDetailOrNull->setWordWrap(false);
    mSetupNoticeDetailOrNull->setProperty("visualRole", QStringLiteral("secondary"));
    mSetupNoticeDetailOrNull->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    noticeLayout->addWidget(mSetupNoticeDetailOrNull, 1);
    mSetupNoticeProgressOrNull = new QProgressBar(notice);
    mSetupNoticeProgressOrNull->setObjectName(QStringLiteral("projectSetupNoticeProgress"));
    mSetupNoticeProgressOrNull->setTextVisible(false);
    mSetupNoticeProgressOrNull->setFixedSize(NOTICE_PROGRESS_WIDTH, NOTICE_PROGRESS_HEIGHT);
    mSetupNoticeProgressOrNull->hide();
    noticeLayout->addWidget(mSetupNoticeProgressOrNull);
    mSetupNoticeRepairOrNull = new QPushButton(QStringLiteral("Repair project"), notice);
    mSetupNoticeRepairOrNull->setObjectName(QStringLiteral("projectSetupNoticeRepair"));
    mSetupNoticeRepairOrNull->setProperty("visualRole", QStringLiteral("primary"));
    noticeLayout->addWidget(mSetupNoticeRepairOrNull);
    mSetupNoticeCheckOrNull = new QPushButton(QStringLiteral("Check again"), notice);
    mSetupNoticeCheckOrNull->setObjectName(QStringLiteral("projectSetupNoticeCheck"));
    noticeLayout->addWidget(mSetupNoticeCheckOrNull);
    mSetupNoticeDismissOrNull = new QToolButton(notice);
    QToolButton* dismiss = mSetupNoticeDismissOrNull;
    dismiss->setObjectName(QStringLiteral("projectSetupNoticeDismiss"));
    dismiss->setText(QStringLiteral("✕"));
    dismiss->setAutoRaise(true);
    dismiss->setAccessibleName(QStringLiteral("Dismiss setup notice"));
    dismiss->setToolTip(QStringLiteral("Hide this notice until the setup problem changes. Project > Repair Project "
                                       "Setup… stays available."));
    noticeLayout->addWidget(dismiss);
    mSetupReadyTimerOrNull = new QTimer(this);
    mSetupReadyTimerOrNull->setSingleShot(true);
    mSetupReadyTimerOrNull->setInterval(NOTICE_READY_MILLISECONDS);
    connect(mSetupReadyTimerOrNull, &QTimer::timeout, this, [this]() {
        mSetupNoticeStage = SetupNoticeStage::Hidden;
        RenderStatus();
    });
    mSetupNoticeOrNull->addWidget(notice);
    addToolBar(mSetupNoticeOrNull);
    addToolBarBreak();
    mSetupNoticeOrNull->hide();
    connect(mSetupNoticeRepairOrNull, &QPushButton::clicked, this, [this]() {
        if (mSetupNoticeRepairOrNull->property("opensSettings").toBool() == true)
        {
            static_cast<void>(mDocuments->OpenDocument(ProjectSettings_));
            return;
        }
        SetupProjectAction_->trigger();
    });
    connect(mSetupNoticeCheckOrNull, &QPushButton::clicked, this, [this]() { CheckSetupAction_->trigger(); });
    connect(dismiss, &QToolButton::clicked, this, [this]() {
        mDismissedSetupProblem = Controller_->State().SetupProblem;
        if (mSetupNoticeStage == SetupNoticeStage::Ready)
        {
            mSetupNoticeStage = SetupNoticeStage::Hidden;
        }
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

    // --- Project settings page ---
    // One readable column, left-aligned in the page: a header with the page's primary action (Save settings), then
    // Project, Build and Rendering resolution sections whose forms share one label column. Each action sits with the
    // state it acts on, sized to its words.
    auto* form = new QWidget(projectScroll);
    form->setObjectName(QStringLiteral("projectSettingsPage"));
    QHBoxLayout* pageLayout = new QHBoxLayout(form);
    pageLayout->setContentsMargins(design::SPACING_EXTRA_LARGE,
                                   design::SPACING_LARGE,
                                   design::SPACING_EXTRA_LARGE,
                                   design::SPACING_EXTRA_LARGE);
    mProjectColumnOrNull = new SettingsColumn(form);
    mProjectColumnOrNull->setObjectName(QStringLiteral("projectSettingsColumn"));
    pageLayout->addWidget(mProjectColumnOrNull, 1, Qt::AlignTop);
    pageLayout->addStretch(0);
    SettingsColumn* column = mProjectColumnOrNull;
    QVBoxLayout* projectLayout = new QVBoxLayout(column);
    projectLayout->setContentsMargins(0, 0, 0, 0);
    projectLayout->setSpacing(design::SPACING_LARGE);

    QHBoxLayout* header = new QHBoxLayout;
    header->setSpacing(design::SPACING_MEDIUM);
    QLabel* heading = new QLabel(QStringLiteral("Your project"), column);
    heading->setObjectName(QStringLiteral("projectSettingsHeading"));
    heading->setFont(MakeTypeFont(heading->font(), TypeRole::Heading));
    header->addWidget(heading, 1);
    ProjectSaveButton_ = new QPushButton(QStringLiteral("Save settings"), column);
    ProjectSaveButton_->setObjectName(QStringLiteral("projectSaveSettings"));
    ProjectSaveButton_->setProperty("visualRole", QStringLiteral("primary"));
    ProjectSaveButton_->setToolTip(QStringLiteral("Save this project's settings and its rendering resolution. "
                                                  "Available once something here has changed."));
    header->addWidget(ProjectSaveButton_, 0, Qt::AlignVCenter);
    projectLayout->addLayout(header);
    ProjectNextStep_ = new QLabel(column);
    ProjectNextStep_->setObjectName(QStringLiteral("projectNextStep"));
    MarkSelectableValue(*ProjectNextStep_);
    ProjectNextStep_->setProperty("visualRole", QStringLiteral("secondary"));
    projectLayout->addWidget(ProjectNextStep_);
    const auto value = [](const QString& name, QWidget* parent) {
        QLabel* label = new QLabel(parent);
        label->setObjectName(name);
        MarkSelectableValue(*label);
        return label;
    };

    // Project: what this project is and where it lives.
    SettingsSection* projectSection = new SettingsSection(QStringLiteral("Project"), column);
    projectSection->setObjectName(QStringLiteral("projectSection"));
    projectLayout->addWidget(projectSection);
    QFormLayout& identity = projectSection->GetForm();
    NameEdit_ = new QLineEdit(projectSection);
    NameEdit_->setObjectName(QStringLiteral("projectName"));
    NameEdit_->setAccessibleName(QStringLiteral("Project name"));
    identity.addRow(QStringLiteral("&Name"), NameEdit_);
    QWidget* folderRow = new QWidget(projectSection);
    QHBoxLayout* folderLayout = new QHBoxLayout(folderRow);
    folderLayout->setContentsMargins(0, 0, 0, 0);
    folderLayout->setSpacing(design::SPACING_SMALL);
    mProjectFolderOrNull = value(QStringLiteral("projectFolder"), folderRow);
    mProjectFolderOrNull->setAccessibleName(QStringLiteral("Project folder"));
    mProjectFolderOrNull->setWordWrap(false);
    folderLayout->addWidget(mProjectFolderOrNull, 1);
    QToolButton* copyFolder = new QToolButton(folderRow);
    copyFolder->setObjectName(QStringLiteral("projectCopyFolder"));
    copyFolder->setIcon(GetEditorIcon(EditorIcon::Copy));
    copyFolder->setIconSize(QSize(design::SPACING_LARGE, design::SPACING_LARGE));
    copyFolder->setToolButtonStyle(Qt::ToolButtonIconOnly);
    copyFolder->setText(QStringLiteral("Copy"));
    copyFolder->setToolTip(QStringLiteral("Copy the project folder's path"));
    copyFolder->setAccessibleName(QStringLiteral("Copy project folder"));
    copyFolder->setAutoRaise(true);
    folderLayout->addWidget(copyFolder, 0, Qt::AlignTop);
    connect(copyFolder, &QToolButton::clicked, this, [this]() {
        QGuiApplication::clipboard()->setText(mProjectFolderOrNull->text());
    });
    identity.addRow(QStringLiteral("Folder"), folderRow);
    mProjectEngineOrNull = value(QStringLiteral("projectEngine"), projectSection);
    identity.addRow(QStringLiteral("Engine"), mProjectEngineOrNull);
    mProjectComponentsOrNull = value(QStringLiteral("projectComponents"), projectSection);
    identity.addRow(QStringLiteral("Components"), mProjectComponentsOrNull);

    // Build: the profile, whether this computer can build it, and the actions that check, repair and run it.
    SettingsSection* buildSection = new SettingsSection(QStringLiteral("Build"), column);
    buildSection->setObjectName(QStringLiteral("projectBuildSection"));
    projectLayout->addWidget(buildSection);
    QFormLayout& build = buildSection->GetForm();
    PresetBox_ = new QComboBox(buildSection);
    PresetBox_->addItem(QStringLiteral("Linux: Debug — investigate code"), QStringLiteral("linux-clang-debug"));
    PresetBox_->addItem(QStringLiteral("Linux: Development — everyday work"),
                        QStringLiteral("linux-clang-development"));
    PresetBox_->addItem(QStringLiteral("Linux: Release — distribute the game"), QStringLiteral("linux-clang-release"));
    PresetBox_->addItem(QStringLiteral("macOS: Debug — investigate code"), QStringLiteral("macos-clang-debug"));
    PresetBox_->addItem(QStringLiteral("macOS: Development — everyday work"),
                        QStringLiteral("macos-clang-development"));
    PresetBox_->addItem(QStringLiteral("macOS: Release — distribute the game"), QStringLiteral("macos-clang-release"));
    for (const QString& preset : GetNativeProjectPresets())
    {
        if (preset.startsWith(QStringLiteral("windows-msvc-")) == true)
        {
            const QString generator = preset.startsWith(QStringLiteral("windows-msvc-visual-studio-")) == true
                                          ? QStringLiteral("Visual Studio")
                                          : QStringLiteral("Ninja");
            PresetBox_->addItem(QStringLiteral("Windows (%1): %2").arg(generator, GetFlavorForProfile(preset)), preset);
        }
    }
    // The field grows to the column, so the whole profile name shows ("macOS: Development — everyday work"); only a
    // column too narrow for it shortens the name, rather than making the page scroll sideways.
    PresetBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    PresetBox_->setMinimumContentsLength(MIN_PROFILE_CHARACTERS);
    PresetBox_->setObjectName(QStringLiteral("projectProfile"));
    PresetBox_->setAccessibleName(QStringLiteral("Build profile"));
    PresetBox_->setToolTip(
        QStringLiteral("Development: everyday work. Debug: investigate code. Release: optimized distribution. "
                       "Save a changed profile before checking or repairing its setup."));
    build.addRow(QStringLiteral("Build &profile"), PresetBox_);
    mSavedProfileOrNull = value(QStringLiteral("projectSavedProfile"), buildSection);
    mSavedProfileOrNull->setProperty("visualRole", QStringLiteral("secondary"));
    build.addRow(QStringLiteral("Saved profile"), mSavedProfileOrNull);

    QWidget* setup = new QWidget(buildSection);
    setup->setObjectName(QStringLiteral("projectSetup"));
    QVBoxLayout* setupLayout = new QVBoxLayout(setup);
    setupLayout->setContentsMargins(0, 0, 0, 0);
    setupLayout->setSpacing(design::SPACING_SMALL);
    QHBoxLayout* setupHeadline = new QHBoxLayout;
    setupHeadline->setSpacing(design::SPACING_SMALL);
    mSetupChipOrNull = new StatusChip(setup);
    mSetupChipOrNull->setObjectName(QStringLiteral("projectSetupChip"));
    setupHeadline->addWidget(mSetupChipOrNull, 0, Qt::AlignTop);
    ProjectSetupStatus_ = value(QStringLiteral("projectSetupStatus"), setup);
    setupHeadline->addWidget(ProjectSetupStatus_, 1);
    setupLayout->addLayout(setupHeadline);
    // The check's own lines (selected engine, revision, installation...) as quiet name and value rows.
    mSetupDetailsOrNull = new QWidget(setup);
    mSetupDetailsOrNull->setObjectName(QStringLiteral("projectSetupDetails"));
    QFormLayout* setupDetails = new QFormLayout(mSetupDetailsOrNull);
    ConfigureSettingsForm(*setupDetails);
    setupDetails->setHorizontalSpacing(design::SPACING_MEDIUM);
    setupDetails->setVerticalSpacing(design::SPACING_EXTRA_SMALL);
    setupLayout->addWidget(mSetupDetailsOrNull);
    QHBoxLayout* setupActions = new QHBoxLayout;
    setupActions->setSpacing(design::SPACING_SMALL);
    // The object name precedes the visible label, matching every call below.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const auto button = [setupActions](QWidget* parent, const QString& name, const QString& text) {
        QPushButton* action = new QPushButton(text, parent);
        action->setObjectName(name);
        setupActions->addWidget(action);
        return action;
    };
    ProjectCheckButton_ = button(setup, QStringLiteral("projectCheckSetup"), QStringLiteral("Check setup"));
    ProjectRepairButton_ = button(setup, QStringLiteral("projectRepairSetup"), QStringLiteral("Repair setup…"));
    mUseEditorEngineButtonOrNull =
        button(setup, QStringLiteral("projectUseEditorEngine"), QStringLiteral("Use this editor's engine"));
    mUseEditorEngineButtonOrNull->setToolTip(
        QStringLiteral("Build and install the engine from this editor's checkout, select it for this project and "
                       "verify the project build. This can take several minutes."));
    mUseEditorEngineButtonOrNull->hide();
    setupActions->addStretch();
    setupLayout->addLayout(setupActions);
    build.addRow(QStringLiteral("Setup"), setup);

    QWidget* run = new QWidget(buildSection);
    QHBoxLayout* runLayout = new QHBoxLayout(run);
    runLayout->setContentsMargins(0, 0, 0, 0);
    runLayout->setSpacing(design::SPACING_SMALL);
    ProjectRunButton_ = new QPushButton(QStringLiteral("Build and launch"), run);
    ProjectRunButton_->setObjectName(QStringLiteral("projectBuildRun"));
    runLayout->addWidget(ProjectRunButton_);
    ProjectStopButton_ = new QPushButton(QStringLiteral("Stop"), run);
    ProjectStopButton_->setObjectName(QStringLiteral("projectStop"));
    runLayout->addWidget(ProjectStopButton_);
    runLayout->addStretch();
    build.addRow(QStringLiteral("Launch"), run);
    connect(ProjectCheckButton_, &QPushButton::clicked, this, [this]() { CheckSetupAction_->trigger(); });
    connect(ProjectRepairButton_, &QPushButton::clicked, this, [this]() { SetupProjectAction_->trigger(); });
    connect(ProjectSaveButton_, &QPushButton::clicked, this, [this]() { (void)SaveProjectSettings(); });
    connect(ProjectRunButton_, &QPushButton::clicked, this, [this]() { BuildRunAction_->trigger(); });
    connect(ProjectStopButton_, &QPushButton::clicked, this, [this]() { StopAction_->trigger(); });
    connect(mUseEditorEngineButtonOrNull, &QPushButton::clicked, Controller_, &EditorController::UseEditorEngine);

    // Build and launch details: what the build runs and how it launches, folded away until asked for.
    QToolButton* advanced = new QToolButton(buildSection);
    advanced->setObjectName(QStringLiteral("projectAdvanced"));
    advanced->setText(QStringLiteral("Show build and launch details"));
    advanced->setCheckable(true);
    advanced->setAutoRaise(true);
    // A disclosure chevron beside the words, like a Properties section header.
    advanced->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advanced->setArrowType(Qt::RightArrow);
    QWidget* details = new QWidget(buildSection);
    details->setObjectName(QStringLiteral("projectBuildDetails"));
    QFormLayout* formLayout = new QFormLayout(details);
    details->hide();
    buildSection->GetFooter().addWidget(advanced, 0, Qt::AlignLeft);
    buildSection->GetFooter().addWidget(details);
    connect(advanced, &QToolButton::toggled, details, &QWidget::setVisible);
    connect(advanced, &QToolButton::toggled, advanced, [advanced](bool isExpanded) {
        advanced->setText(isExpanded == true ? QStringLiteral("Hide build and launch details")
                                             : QStringLiteral("Show build and launch details"));
        advanced->setArrowType(isExpanded == true ? Qt::DownArrow : Qt::RightArrow);
    });
    ProviderBox_ = new QComboBox(details);
    ProviderBox_->addItem(QStringLiteral("ludus"));
    ProviderBox_->addItem(QStringLiteral("cmake"));
    SourceDirEdit_ = new QLineEdit(details);
    SourceDirEdit_->setObjectName(QStringLiteral("projectSource"));
    TargetBox_ = new QComboBox(details);
    TargetBox_->setObjectName(QStringLiteral("projectTarget"));
    TargetBox_->setEditable(true); // target can be entered before configuration
    CwdEdit_ = new QLineEdit(details);
    formLayout->addRow(QStringLiteral("&Provider"), ProviderBox_);
    formLayout->addRow(QStringLiteral("&Source directory"), SourceDirEdit_);
    formLayout->addRow(QStringLiteral("Executable &target"), TargetBox_);
    formLayout->addRow(QStringLiteral("Run &working directory"), CwdEdit_);

    // Argument list editor: one string per item, Add/Remove; never a shell line.
    ArgsList_ = new QListWidget(details);
    ConfigureItemSelection(*ArgsList_);
    ArgsList_->setObjectName(QStringLiteral("projectArguments"));
    connect(ArgsList_->model(), &QAbstractItemModel::dataChanged, this, &MainWindow::OnFieldEdited);
    QWidget* argumentButtons = new QWidget(details);
    QHBoxLayout* argumentButtonsLayout = new QHBoxLayout(argumentButtons);
    argumentButtonsLayout->setContentsMargins(0, 0, 0, 0);
    AddArgButton_ = new QPushButton(QStringLiteral("Add argument"), argumentButtons);
    RemoveArgButton_ = new QPushButton(QStringLiteral("Remove argument"), argumentButtons);
    argumentButtonsLayout->addWidget(AddArgButton_);
    argumentButtonsLayout->addWidget(RemoveArgButton_);
    argumentButtonsLayout->addStretch();
    formLayout->addRow(QStringLiteral("Run &arguments"), ArgsList_);
    formLayout->addRow(QString(), argumentButtons);
    column->ShareLabelColumn(*formLayout);
    projectLayout->addStretch();

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
    mContentDock->setWindowIcon(GetEditorIcon(EditorIcon::Content));
    mContentDock->setWidget(contentScroll);
    QScrollArea* audioScroll = mAudioDocument;
    audioScroll->setObjectName(QStringLiteral("audioWorkspaceScroll"));
    audioScroll->setWidgetResizable(true);
    audioScroll->setFrameShape(QFrame::NoFrame);
    Audio_ = new AudioWorkspace(audioScroll);
    audioScroll->setWidget(Audio_);
    World_ = new WorldWorkspace(Controller_->Tooling(), mDocuments);
    // A click takes focus so the World's shortcuts apply, but Tab passes over the container to its controls: it has
    // nothing of its own to show focus on.
    World_->setFocusPolicy(Qt::ClickFocus);
    // The World reports edits in the window's status bar while it is the shown document.
    // A failure stays until another message replaces it, so its repair remains readable.
    connect(World_, &WorldWorkspace::StatusMessage, this, [this](const QString& message, bool isFailure) {
        if (World_->isVisible() == true)
        {
            statusBar()->showMessage(message, isFailure == true ? 0 : WORLD_STATUS_MESSAGE_MILLISECONDS);
        }
    });
    Scripts_ = new ScriptWorkspace(Controller_, mDocuments);
    Renderer_ = new RendererWorkspace(Controller_->Tooling(), mDocuments);
    mCodeArea = new CodeEditorArea(mDocuments);
    mCodeArea->setObjectName(QStringLiteral("codeEditorArea"));
    Renderer_->SetCodeEditorArea(*mCodeArea);
    connect(Renderer_, &RendererWorkspace::ShaderCodeWorkspaceRequested, this, [this]() {
        static_cast<void>(mDocuments->OpenDocument(mCodeArea));
    });
    connect(Renderer_, &RendererWorkspace::GraphWorkspaceRequested, this, [this]() {
        static_cast<void>(mDocuments->OpenDocument(Renderer_));
    });
    // A code tab's load failure is surfaced where Open Externally stays available.
    connect(mCodeArea, &CodeEditorArea::OpenFailed, this, [this](const QString& file, const QString& explanation) {
        const QString message =
            explanation.isEmpty() == true ? QStringLiteral("Could not open %1.").arg(file) : explanation;
        statusBar()->showMessage(message);
    });
    // Closing a dirty code tab asks Save / Don't Save / Cancel before it closes.
    connect(mCodeArea, &CodeEditorArea::DirtyCloseRequested, this, [this](CodeDocumentTab* tab) {
        static_cast<void>(mCodeArea->PromptCloseDirtyTab(tab));
    });
    // Save follows the active code tab, so its label and state track the tabs.
    connect(mCodeArea, &CodeEditorArea::TabsChanged, this, &MainWindow::RenderDocumentActions);
    connect(mCodeArea, &CodeEditorArea::ActiveTabChanged, this, &MainWindow::RenderDocumentActions);
    connect(mCodeArea, &CodeEditorArea::ActiveTabChanged, this, &MainWindow::updateContextStrip);
    // Rendering resolution: the active renderer graph's output size and scale, saved with the project's settings.
    SettingsSection* renderingSection = new SettingsSection(QStringLiteral("Rendering resolution"), column);
    renderingSection->setObjectName(QStringLiteral("projectRenderingSection"));
    projectLayout->insertWidget(projectLayout->count() - 1, renderingSection);
    mRenderingSettingsOrNull = new ProjectRenderingSettings(*Renderer_, renderingSection);
    renderingSection->GetFooter().addWidget(mRenderingSettingsOrNull);
    column->ShareLabelColumn(mRenderingSettingsOrNull->GetForm());
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
    mPropertiesDock->setWindowIcon(GetEditorIcon(EditorIcon::Configuration));
    mProperties->setObjectName(QStringLiteral("propertiesStack"));
    mNoProperties->setObjectName(QStringLiteral("propertiesEmpty"));
    QWidget* noPropertiesPage = new QWidget(mProperties);
    QVBoxLayout* noPropertiesLayout = new QVBoxLayout(noPropertiesPage);
    noPropertiesLayout->setContentsMargins(PROPERTIES_HINT_MARGIN_PIXELS,
                                           design::SPACING_EXTRA_LARGE,
                                           PROPERTIES_HINT_MARGIN_PIXELS,
                                           PROPERTIES_HINT_MARGIN_PIXELS);
    mNoProperties->setParent(noPropertiesPage);
    QHBoxLayout* noPropertiesRow = new QHBoxLayout;
    noPropertiesRow->addStretch();
    noPropertiesRow->addWidget(mNoProperties, 1);
    noPropertiesRow->addStretch();
    noPropertiesLayout->addLayout(noPropertiesRow);
    noPropertiesLayout->addStretch();
    noPropertiesPage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mProperties->addWidget(noPropertiesPage);
    mProperties->addWidget(World_->TakePropertiesPanel(mProperties));
    mProperties->addWidget(Renderer_->TakeInspectorPanel(mProperties));
    mProperties->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    // The inspector wraps and stacks its rows down to this width; narrower, it would have to clip them.
    mProperties->setMinimumWidth(WORLD_PROPERTIES_MINIMUM_WIDTH);
    mPropertiesDock->setWidget(mProperties);
    // The render-graph preview shows the World viewport through the open graph.
    mRenderPreviewDock->setObjectName(QStringLiteral("renderPreviewDock"));
    mRenderPreviewDock->setWindowIcon(GetEditorIcon(EditorIcon::RenderGraph));
    QWidget* renderPreview = Renderer_->TakePreviewPanel(mRenderPreviewDock);
    renderPreview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mRenderPreviewDock->setWidget(renderPreview);

    mProjectExplorerDock->setObjectName(QStringLiteral("projectExplorerDock"));
    mProjectExplorerDock->setWindowIcon(GetEditorIcon(EditorIcon::Folder));
    // With no project the explorer has nothing to list, so the dock says so and offers the way in instead.
    mProjectExplorerPagesOrNull = new QStackedWidget(mProjectExplorerDock);
    mProjectExplorerPagesOrNull->setObjectName(QStringLiteral("projectExplorerPages"));
    mProjectExplorerPagesOrNull->addWidget(mProjectExplorer);
    QWidget* noProjectPage = new QWidget(mProjectExplorerPagesOrNull);
    QVBoxLayout* noProjectLayout = new QVBoxLayout(noProjectPage);
    noProjectLayout->setContentsMargins(PROPERTIES_HINT_MARGIN_PIXELS,
                                        design::SPACING_EXTRA_LARGE,
                                        PROPERTIES_HINT_MARGIN_PIXELS,
                                        PROPERTIES_HINT_MARGIN_PIXELS);
    mNoProjectExplorerOrNull = new StateView(noProjectPage);
    mNoProjectExplorerOrNull->setObjectName(QStringLiteral("projectExplorerNoProject"));
    StateViewContent noProject;
    noProject.Icon = EditorIcon::OpenProject;
    noProject.Message = QStringLiteral("No project is open");
    noProject.Cause = QStringLiteral("A project's worlds, render graphs, shaders and scripts are listed here.");
    noProject.ActionLabel = QStringLiteral("Open Project...");
    mNoProjectExplorerOrNull->SetContent(noProject);
    // In a row, not aligned, so the wrapped text gets the height it needs at the dock's width.
    QHBoxLayout* noProjectRow = new QHBoxLayout;
    noProjectRow->addStretch();
    noProjectRow->addWidget(mNoProjectExplorerOrNull, 1);
    noProjectRow->addStretch();
    noProjectLayout->addLayout(noProjectRow);
    noProjectLayout->addStretch();
    noProjectPage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mProjectExplorerPagesOrNull->addWidget(noProjectPage);
    mProjectExplorerPagesOrNull->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    connect(mNoProjectExplorerOrNull, &StateView::ActionTriggered, this, &MainWindow::OnOpenRequested);
    mProjectExplorerDock->setWidget(mProjectExplorerPagesOrNull);
    mErrorListDock->setObjectName(QStringLiteral("errorListDock"));
    mErrorListDock->setWindowIcon(GetEditorIcon(EditorIcon::Error));
    mErrorListDock->setWidget(mErrorList);

    // The Search tool window (CE07): project-wide find and Replace in Files. Its
    // scope provider is backed by the code area's registry (for dirty buffers)
    // and the active project root, updated in the project chain. Activating a
    // result opens the file and reveals the match in the same action.
    mSearchScopeOrNull = new CodeAreaSearchScope(mCodeArea->GetRegistry());
    mSearchDockOrNull = new QDockWidget(QStringLiteral("Search"), this);
    mSearchDockOrNull->setObjectName(QStringLiteral("searchDock"));
    mSearchDockOrNull->setWindowIcon(GetEditorIcon(EditorIcon::Error));
    mSearchOrNull = new SearchToolWindow(mCodeArea->GetRegistry(), *mSearchScopeOrNull, mSearchDockOrNull);
    mSearchDockOrNull->setWidget(mSearchOrNull);
    connect(mSearchOrNull,
            &SearchToolWindow::OpenRequested,
            this,
            [this](const QString& path, foundation::int32 line, foundation::int32 column) {
                openCodeFile(path, false, line, column);
            });

    // The References tool window (code editor M4): Find References in a package
    // shader lists its result here, kept while the author follows it.
    mReferencesDockOrNull = new QDockWidget(QStringLiteral("References"), this);
    mReferencesDockOrNull->setObjectName(QStringLiteral("referencesDock"));
    mReferencesDockOrNull->setWindowIcon(GetEditorIcon(EditorIcon::Error));
    mReferencesOrNull = new CodeReferencesWindow(mReferencesDockOrNull);
    mReferencesDockOrNull->setWidget(mReferencesOrNull);
    Renderer_->SetReferencesWindow(mReferencesOrNull);
    connect(mReferencesOrNull, &CodeReferencesWindow::ResultsShown, this, [this]() {
        openToolWindow(mReferencesDockOrNull);
    });

    InspectorDock_ = new QDockWidget(QStringLiteral("Live Inspector"), this);
    InspectorDock_->setObjectName(QStringLiteral("liveInspectorDock"));
    InspectorDock_->setWindowIcon(GetEditorIcon(EditorIcon::LiveInspector));
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
    TimingDock_->setWindowIcon(GetEditorIcon(EditorIcon::Timing));
    Timing_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    TimingDock_->setWidget(Timing_);

    // History lists the active document's edits; it does no work while closed (renderHistory).
    mHistoryDockOrNull = new QDockWidget(QStringLiteral("History"), this);
    mHistoryDockOrNull->setObjectName(QStringLiteral("historyDock"));
    mHistoryDockOrNull->setWindowIcon(GetEditorIcon(EditorIcon::Undo));
    mHistoryPanelOrNull = new EditHistoryPanel(mHistoryDockOrNull);
    mHistoryPanelOrNull->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mHistoryDockOrNull->setWidget(mHistoryPanelOrNull);
    connect(mHistoryPanelOrNull, &EditHistoryPanel::JumpRequested, this, &MainWindow::onHistoryJumpRequested);
    connect(mHistoryDockOrNull, &QDockWidget::visibilityChanged, this, [this](bool isVisible) {
        if (isVisible == true)
        {
            renderHistory();
        }
    });

    OutputDock_ = new QDockWidget(QStringLiteral("Output"), this);
    OutputDock_->setObjectName(QStringLiteral("outputDock"));
    OutputDock_->setWindowIcon(GetEditorIcon(EditorIcon::Output));
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
    // The code editor's status items sit left of the activity bar, shown only
    // while a code view has a tab active (architecture, "Status bar").
    mCodeStatusBarOrNull = new CodeStatusBar(this);
    statusBar()->addPermanentWidget(mCodeStatusBarOrNull);
    connect(mCodeArea, &CodeEditorArea::ActiveTabChanged, this, [this](CodeDocumentTab* tabOrNull) {
        mCodeStatusBarOrNull->SetActiveTab(mDocuments->GetActiveDocumentOrNull() == mCodeArea ? tabOrNull : nullptr);
    });
    // A save, edit or profile change can change the shown items (dirty, endings).
    connect(mCodeArea, &CodeEditorArea::TabsChanged, mCodeStatusBarOrNull, [this]() {
        mCodeStatusBarOrNull->Refresh();
    });
    // Language change applies a manual language override to the active view.
    connect(mCodeStatusBarOrNull, &CodeStatusBar::LanguageChangeRequested, this, [this](const QString& identifier) {
        CodeDocumentTab* tab = mCodeArea->GetActiveTabOrNull();
        if (tab == nullptr)
        {
            return;
        }
        const foundation::BorrowedView<const CodeLanguageDescriptor> descriptors =
            CodeLanguageRegistry::Get().GetDescriptors();
        for (foundation::usize index = 0; index < descriptors.GetSize(); ++index)
        {
            const CodeLanguageDescriptor& descriptor = descriptors.GetDataOrNull()[index];
            const QString candidate = QString::fromUtf8(descriptor.Identifier.GetDataOrNull(),
                                                        static_cast<qsizetype>(descriptor.Identifier.GetSize()));
            if (candidate == identifier)
            {
                tab->GetEditor().SetLanguage(descriptor.Language);
                mCodeStatusBarOrNull->Refresh();
                return;
            }
        }
    });
    // Line-ending change rewrites the host profile; the dirty save path emits it.
    connect(mCodeStatusBarOrNull, &CodeStatusBar::LineEndingChangeRequested, this, [this](const QString& ending) {
        CodeDocumentTab* tab = mCodeArea->GetActiveTabOrNull();
        CodeFileDocument* host = tab == nullptr ? nullptr : tab->GetHostOrNull();
        if (host == nullptr || host->IsReadOnly() == true)
        {
            return;
        }
        host->SetProfileLineEnding(ending == QStringLiteral("crlf") ? CodeLineEnding::CrLf : CodeLineEnding::Lf);
        mCodeStatusBarOrNull->Refresh();
    });
    // Right side: the running job and the problem counts, which open the Error List.
    mActivityBarOrNull = new StatusActivityBar(this);
    statusBar()->addPermanentWidget(mActivityBarOrNull);
    connect(mActivityBarOrNull, &StatusActivityBar::ProblemsRequested, this, [this]() {
        showToolWindow(mErrorListDock);
    });
    connect(mErrorList, &ErrorList::CountsChanged, this, &MainWindow::renderActivity);

    // Thanks to David Lightbown, Designing the User Experience of Game
    // Development Tools (2015), ch. 5, pp. 80-86, 112-115: show task-relevant choices
    // progressively. Recents belong to Welcome and File, freeing authoring space.
    // https://www.uxofgametools.com/; docs/architecture/editor-design-review.md.
    Welcome_ = new QWidget(mDocuments);
    Welcome_->setObjectName(QStringLiteral("projectWelcome"));
    Welcome_->setAutoFillBackground(true);
    auto* recentPanel = Welcome_;
    auto* recentLayout = new QVBoxLayout(recentPanel);
    mRecentEmptyStateOrNull = new StateView(recentPanel);
    mRecentEmptyStateOrNull->setObjectName(QStringLiteral("recentProjectsEmpty"));
    RecentList_ = new QListWidget(recentPanel);
    ConfigureItemSelection(*RecentList_);
    RecentList_->setObjectName(QStringLiteral("recentProjectsList"));
    RecentList_->setAccessibleName(QStringLiteral("Recent projects"));
    RecentList_->setWordWrap(true);
    RecentList_->setProperty(ITEM_PADDING_PROPERTY, RECENT_PROJECT_ITEM_PADDING);
    RecentList_->setItemDelegate(new RecentProjectDelegate(RecentList_));
    RecentOpenButton_ = new QPushButton(QStringLiteral("Open Selected"), recentPanel);
    RecentOpenButton_->setObjectName(QStringLiteral("openRecentProjectButton"));
    mRecentRemoveButtonOrNull = new QPushButton(QStringLiteral("Remove from List"), recentPanel);
    mRecentRemoveButtonOrNull->setObjectName(QStringLiteral("removeRecentProjectButton"));
    mRecentRemoveButtonOrNull->setToolTip(QStringLiteral("Forget the selected project. Its files are not deleted."));
    mRecentRemoveMissingButtonOrNull = new QPushButton(recentPanel);
    mRecentRemoveMissingButtonOrNull->setObjectName(QStringLiteral("removeMissingRecentProjectsButton"));
    mRecentRemoveMissingButtonOrNull->setToolTip(
        QStringLiteral("Forget every project whose file was moved or deleted. Nothing on disk changes."));
    mRecentRemoveMissingButtonOrNull->hide();
    BrowseProjectButton_ = new QPushButton(QStringLiteral("Open Project..."), recentPanel);
    recentLayout->setContentsMargins(design::SPACING_EXTRA_LARGE,
                                     design::SPACING_EXTRA_LARGE,
                                     design::SPACING_EXTRA_LARGE,
                                     design::SPACING_EXTRA_LARGE);
    recentLayout->setSpacing(design::SPACING_MEDIUM);
    auto* title = new QLabel(QStringLiteral("Welcome to Ludus"), recentPanel);
    auto* brand = new QLabel(recentPanel);
    brand->setObjectName(QStringLiteral("welcomeLogo"));
    brand->setAccessibleName(QStringLiteral("Ludus logo"));
    brand->setPixmap(QIcon(QStringLiteral(":/ludus/ludus-mark.png")).pixmap(48, 48));
    title->setFont(MakeTypeFont(title->font(), TypeRole::Heading));
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
    mWelcomeNewReasonOrNull = new QLabel(recentPanel);
    mWelcomeNewReasonOrNull->setObjectName(QStringLiteral("welcomeNewProjectReason"));
    mWelcomeNewReasonOrNull->setWordWrap(true);
    mWelcomeNewReasonOrNull->setProperty("visualRole", QStringLiteral("secondary"));
    mWelcomeNewReasonOrNull->hide();
    recentLayout->addWidget(mWelcomeNewReasonOrNull);
    // A project that could not be opened is explained here, in the Start Page's own words, with one action.
    mWelcomeProblemOrNull = new StateView(recentPanel);
    mWelcomeProblemOrNull->setObjectName(QStringLiteral("welcomeProblem"));
    mWelcomeProblemOrNull->hide();
    recentLayout->addWidget(mWelcomeProblemOrNull);
    connect(mWelcomeProblemOrNull, &StateView::ActionTriggered, BrowseProjectButton_, &QPushButton::click);
    recentLayout->addSpacing(design::SPACING_MEDIUM);
    recentLayout->addWidget(new QLabel(QStringLiteral("Recent projects"), recentPanel));
    recentLayout->addWidget(mRecentEmptyStateOrNull, 0, Qt::AlignHCenter);
    recentLayout->addWidget(RecentList_, 1);
    recentLayout->addStretch();
    auto* recentActions = new QHBoxLayout;
    recentActions->addWidget(RecentOpenButton_);
    recentActions->addWidget(mRecentRemoveButtonOrNull);
    recentActions->addStretch();
    recentActions->addWidget(mRecentRemoveMissingButtonOrNull);
    recentLayout->addLayout(recentActions);
    // Document registration order is the Window menu order.
    mDocuments->AddDocument(Welcome_, QStringLiteral("Start Page"));
    mDocuments->AddDocument(World_, QStringLiteral("World"));
    mDocuments->AddDocument(Renderer_, QStringLiteral("Render Graph"));
    mDocuments->AddDocument(Scripts_, QStringLiteral("Scripts"));
    mDocuments->AddDocument(mCodeArea, QStringLiteral("Code"));
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
        mRecentRemoveButtonOrNull->setEnabled(Controller_->Caps().CanOpen && RecentList_->currentItem() != nullptr);
    });
    connect(mRecentRemoveButtonOrNull, &QPushButton::clicked, this, [this]() {
        if (const QListWidgetItem* item = RecentList_->currentItem())
        {
            Controller_->RemoveRecentProject(item->data(Qt::UserRole).toString());
        }
    });
    connect(mRecentRemoveMissingButtonOrNull, &QPushButton::clicked, this, [this]() {
        QStringList missingPaths;
        for (const RecentProject& project : Controller_->RecentProjects())
        {
            if (QFileInfo(project.DescriptorPath).isFile() == false)
            {
                missingPaths.append(project.DescriptorPath);
            }
        }
        for (const QString& path : missingPaths)
        {
            Controller_->RemoveRecentProject(path);
        }
    });
    RecentList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(RecentList_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        const QListWidgetItem* itemOrNull = RecentList_->itemAt(position);
        if (itemOrNull == nullptr)
        {
            return;
        }
        const QString path = itemOrNull->data(Qt::UserRole).toString();
        QMenu menu(RecentList_);
        QAction* open = menu.addAction(QStringLiteral("Open"));
        open->setEnabled(Controller_->Caps().CanOpen == true);
        QAction* remove = menu.addAction(QStringLiteral("Remove from Recent Projects"));
        remove->setObjectName(QStringLiteral("removeRecentProjectAction"));
        remove->setEnabled(Controller_->Caps().CanOpen == true);
        const QAction* chosen = menu.exec(RecentList_->viewport()->mapToGlobal(position));
        if (chosen == open)
        {
            QTimer::singleShot(0, this, [this, path]() { OpenProjectPath(path); });
        }
        else if (chosen == remove)
        {
            Controller_->RemoveRecentProject(path);
        }
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
    hideIdleRayPaths();
    mShouldSizeDefaultDocks = false;
}

bool MainWindow::event(QEvent* event)
{
    const bool wasHandled = QMainWindow::event(event);
    // Dock tab bars are created and refilled by the layout, so follow its changes.
    if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::ChildAdded ||
        event->type() == QEvent::Polish)
    {
        syncDockTabIcons();
    }
    return wasHandled;
}

void MainWindow::syncDockTabIcons()
{
    const QList<QDockWidget*> docks = findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly);
    for (QTabBar* tabBar : findChildren<QTabBar*>(Qt::FindDirectChildrenOnly))
    {
        // macOS clips a dock tab's label mid-word ("Live Inspec") where the other platforms elide it. Labels that
        // do not fit turn into icons (EditorStyle); a label is only ever elided when even that is too wide.
        if (tabBar->elideMode() != Qt::ElideRight)
        {
            tabBar->setElideMode(Qt::ElideRight);
        }
        for (int index = 0; index < tabBar->count(); ++index)
        {
            for (const QDockWidget* dock : docks)
            {
                if (dock->windowTitle() == tabBar->tabText(index) &&
                    tabBar->tabIcon(index).cacheKey() != dock->windowIcon().cacheKey())
                {
                    tabBar->setTabIcon(index, dock->windowIcon());
                }
            }
        }
    }
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
    // The first run with no stored keymap profile offers the Ludus / VS Code
    // choice once the window is visible, then re-applies the chosen profile.
    chooseKeymapOnFirstUse();
}

void MainWindow::applyKeymap()
{
    mKeymap.ApplyToActions(this);
}

void MainWindow::chooseKeymapOnFirstUse()
{
    if (mKeymap.HasStoredProfile() == true)
    {
        return;
    }
    // Offscreen (headless tests and CI) never shows the modal chooser; the keymap
    // defaults to Ludus there, which is also the first-run default on a desktop
    // until the user picks a profile. The choice is only prompted interactively.
    if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
    {
        return;
    }
    QMessageBox chooser(this);
    chooser.setObjectName(QStringLiteral("keymapFirstUseChooser"));
    chooser.setWindowTitle(QStringLiteral("Choose a keymap"));
    chooser.setText(QStringLiteral("Choose the editor keymap. You can change it later in Options."));
    chooser.setInformativeText(QStringLiteral("Ludus keeps Ctrl+K for the command palette. VS Code uses Ctrl+Shift+P "
                                              "and frees Ctrl+K for chords."));
    QPushButton* ludusButton = chooser.addButton(QStringLiteral("Ludus"), QMessageBox::AcceptRole);
    QPushButton* vsCodeButton = chooser.addButton(QStringLiteral("VS Code"), QMessageBox::AcceptRole);
    chooser.setDefaultButton(ludusButton);
    chooser.exec();
    mKeymap.SetActiveProfile(chooser.clickedButton() == vsCodeButton ? CodeKeymapProfile::VsCode
                                                                     : CodeKeymapProfile::Ludus);
    applyKeymap();
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
    tabifyDockWidget(mPropertiesDock, mHistoryDockOrNull);
    addDockWidget(Qt::BottomDockWidgetArea, mErrorListDock);
    tabifyDockWidget(mErrorListDock, mSearchDockOrNull);
    tabifyDockWidget(mErrorListDock, mReferencesDockOrNull);
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
        mHistoryDockOrNull,
        mErrorListDock,
        mSearchDockOrNull,
        mReferencesDockOrNull,
        OutputDock_,
        TimingDock_,
        mProfilingDock,
    };
    return docks;
}

#if !defined(LUDUS_EDITOR_NATIVE_PREVIEW)
void MainWindow::hideIdleRayPaths()
{
    // Builds without native preview do not create a Ray Paths dock.
}
#endif

void MainWindow::showCodeAndViewport()
{
    setViewportFocused(false);
    const bool isGraphCodeCurrent =
        mDocuments->IsDocumentCurrent(mCodeArea) == true && Renderer_->IsGraphCodeActive() == true;
    if ((isGraphCodeCurrent == false && Renderer_->ShowSourceView() == false) ||
        mDocuments->ShowDocumentsSideBySide(mCodeArea, World_) == false)
    {
        statusBar()->showMessage(QStringLiteral("Open a project render graph with sources to use Code and Viewport."));
        return;
    }
    CodeDocumentTab* tabOrNull = mCodeArea->GetActiveTabOrNull();
    if (tabOrNull != nullptr)
    {
        tabOrNull->GetEditor().setFocus(Qt::ShortcutFocusReason);
    }
    updateContextStrip();
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
        for (QToolBar* toolbar :
             {mStandardToolbar, GameToolbar_, mRenderGraphToolbar, mSessionToolbar, mSetupNoticeOrNull})
        {
            toolbar->hide();
        }
        // The viewport widgets stay where they are; only their neighbours hide, so the
        // native surface resizes instead of being recreated.
        if (mFocusRestoreDocumentOrNull == Renderer_)
        {
            Renderer_->SetChromeVisible(false);
            Renderer_->FocusActiveView();
        }
        else if (mFocusRestoreDocumentOrNull == mCodeArea && Renderer_->IsGraphCodeActive() == true)
        {
            Renderer_->SetCodeFocused(true);
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
    Renderer_->SetCodeFocused(false);
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
    updateContextStrip();
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
    // Error List, a build reveals Output, a play session reveals Timing, recording reveals Profiling,
    // Find in Files reveals Search and Find References reveals References.
    for (QDockWidget* dock : {mErrorListDock,
                              mSearchDockOrNull,
                              mReferencesDockOrNull,
                              OutputDock_,
                              TimingDock_,
                              mProfilingDock,
                              mHistoryDockOrNull})
    {
        if (dock != nullptr)
        {
            dock->hide();
        }
    }
    if (mErrorList->GetCount(ErrorListSeverity::Error) != 0 || mErrorList->GetCount(ErrorListSeverity::Warning) != 0)
    {
        openToolWindow(mErrorListDock);
    }
    resizeDocks({mProjectExplorerDock, mPropertiesDock}, {explorer, properties}, Qt::Horizontal);
    // resizeDocks only records the sizes and posts a layout request; apply them now, so the default arrangement
    // is the one shown and saved rather than appearing an event-loop pass later.
    layout()->activate();
}

void MainWindow::ResetWorkspaceLayout()
{
    setViewportFocused(false);
    mHasTimingOpenedForPlay = false;
    (void)restoreState(DefaultLayout_, LAYOUT_VERSION);
    // Also recover panes that were floating, closed or moved when the user requested reset.
    arrangeDefaultDocks();
    for (QToolBar* toolbar : {mStandardToolbar, GameToolbar_})
    {
        toolbar->show();
    }
    updateContextStrip();
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
    QAction* reopen = addCommand(WINDOW_REOPEN_DOCUMENT_COMMAND);
    QAction* closeAll = addCommand(WINDOW_CLOSE_ALL_DOCUMENTS_COMMAND);
    reopen->setAutoRepeat(false);
    reopen->setEnabled(mDocuments->HasClosedDocuments());
    connect(reopen, &QAction::triggered, mDocuments, &DocumentWell::ReopenClosedDocument);
    connect(mDocuments, &DocumentWell::ClosedDocumentHistoryChanged, reopen, [this, reopen]() {
        reopen->setEnabled(mDocuments->HasClosedDocuments());
    });
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
                              mSearchDockOrNull,
                              mReferencesDockOrNull,
                              OutputDock_,
                              mContentDock,
                              mRenderPreviewDock,
                              InspectorDock_,
                              mHistoryDockOrNull,
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
        // A menu has no tab to carry the unsaved dot, so the entry keeps the `*` the tab no longer needs.
        if (mDocuments->IsDocumentModified(document) == true)
        {
            title.append(QStringLiteral("*"));
        }
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
                       {QStringLiteral("world"), World_->Session()},
                       {QStringLiteral("renderer"), Renderer_->GetSourceViewState()},
                       {QStringLiteral("codeTabs"), mCodeArea->SaveState()}};
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
    hideIdleRayPaths();
    World_->RestoreSession(object.value(QStringLiteral("world")).toObject());
    // Reopen the project's code tabs before the document arrangement, so the
    // saved layout can show the Code document with its tabs already present.
    mCodeArea->RestoreState(object.value(QStringLiteral("codeTabs")).toObject());
    // Malformed or stale arrangements leave the project's default World document open.
    static_cast<void>(mDocuments->RestoreState(object.value(QStringLiteral("documents")).toObject()));
    static_cast<void>(Renderer_->RestoreSourceViewState(object.value(QStringLiteral("renderer")).toObject()));
    if (mDocuments->GetActiveDocumentOrNull() == nullptr)
    {
        static_cast<void>(mDocuments->OpenDocument(World_));
    }
}

} // namespace ludus::editor
