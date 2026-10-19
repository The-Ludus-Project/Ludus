#pragma once

// MainWindow: renders controller state and emits actions. It owns no second
// project settings model and starts no processes (design.md sections 3, 5).
//
// The shell follows Visual Studio's layout: a central DocumentWell holds document
// views (World, render graph, scripts, project settings, audio, configuration) in
// tab groups, and tool windows (Project Explorer, Properties, Error List, Output,
// Content, Render Preview, Live Inspector, Timing) are dockable, tabbable
// QDockWidgets. Commands are QActions shared by menus, toolbars and shortcuts; Save
// and Undo follow the active document, and Tools > Options edits editor preferences.
// See docs/development/editor-workspace.md#workspace-layout.
//
// Private editor header (not installed). Uses standard Qt layouts/controls only.
// While synchronizing fields from state it blocks edit signals (QSignalBlocker)
// so rendering never dispatches new edits. Field edits are accepted before Save
// or an identity change can overtake them.

#include "internal/controller.h"

#include <QByteArray>
#include <QMainWindow>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QTimer>

QT_BEGIN_NAMESPACE
class QComboBox;
class QDockWidget;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QAction;
class QScrollArea;
class QStackedWidget;
class QTableWidget;
class QToolBar;
QT_END_NAMESPACE

namespace ludus::editor
{

class AudioWorkspace;
class ConfigurationWorkspace;
class ContentWorkspace;
class DocumentWell;
class ErrorList;
class ProjectExplorer;
struct ErrorListEntry;
enum class ProjectItemKind : foundation::uint8;
class ScriptWorkspace;
class RendererWorkspace;
class TimingPanel;
class WorldWorkspace;
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(EditorController* controller,
                        QWidget* parent = nullptr,
                        const QString& workspaceSettingsFile = {});
    ~MainWindow() override;

    /// Central document well; documents open, close and regroup through it.
    [[nodiscard]] DocumentWell& GetDocumentWell() noexcept
    {
        return *mDocuments;
    }

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void OnStateChanged();
    void OnOpenRequested();
    void OnSaveRequested();
    void OnUndoRequested();
    void OnRedoRequested();
    void OnReloadRequested();
    void OnFieldEdited();
    void OnAddArgument();
    void OnRemoveArgument();
    void OnCopyJobDetails();
    void OnSetupProject();
    void OnDebuggerSetupRequested();
    void OnNewProject();
    void OnCloseProject();
    void ShowShortcuts();
    void OnSetupRelease();
    void OnPackageRelease();

private:
    enum class LaunchAction : foundation::uint8
    {
        Run,
        Play,
        Debug
    };
    void LaunchAfterPreview(LaunchAction action, const QString& debugger = {}, bool setup = false);
    void BuildUi();
    void BuildMenus();
    void InitializeWorkspace();
    void ResetWorkspaceLayout();
    void arrangeDefaultDocks();
    void applyDefaultDockSizes();
    void showToolWindow(QDockWidget* dock);
    void buildWindowMenu(QMenu* menu);
    void renderWindowDocuments(QMenu* menu);
    void openProjectItem(ProjectItemKind kind, const QString& path);
    void activateErrorEntry(const ErrorListEntry& entry);
    void renderActiveDocument();
    void renderDocumentTitles();
    void renderGraphCommands();
    void showOptions();
    void renderGraphErrors();
    void renderBuildErrors();
    void SaveWorkspaceLayout() const;
    [[nodiscard]] bool ConfirmProjectChange(const QString& action);
    void OpenProjectPath(const QString& path);
    [[nodiscard]] ProjectDescriptor DraftFromFields() const;
    void RenderFields();
    void RenderCapabilities();
    void RenderDocumentActions();
    /// Active document of the well, or null when no document is open.
    [[nodiscard]] QWidget* getActiveWorkspaceOrNull() const;
    void CommitProjectFields();
    [[nodiscard]] bool SaveProjectSettings();
    void RenderStatus();
    void RenderProperties();
    void RenderRecentProjects();

    WorldWorkspace* World_ = nullptr;
    ScriptWorkspace* Scripts_ = nullptr;
    // Created with the other work tabs in the constructor; never null afterwards.
    RendererWorkspace* Renderer_;
    ConfigurationWorkspace* Configuration_ = nullptr;
    QWidget* ProjectSettings_ = nullptr;
    AudioWorkspace* Audio_ = nullptr;
    ContentWorkspace* Content_ = nullptr;
    bool AudioClosing_ = false;
    bool AudioLaunchPending_ = false;
    EditorController* Controller_ = nullptr;
    bool Rendering_ = false;
    bool CloseConfirmed_ = false;
    QObject* LastEditedField_ = nullptr;
    QMetaObject::Connection FocusUndoConnection_;
    QMetaObject::Connection FocusRedoConnection_;
    uint64 RenderedProjectEpoch_ = ~uint64{0};
    QStringList RenderedTargets_;

    // Local presentation preferences; never part of project/controller state.
    QString WorkspaceSettingsFile_;
    QString ProjectLayoutRoot_;
    uint64 ProjectLayoutEpoch_ = ~uint64{0};
    uint64 ResumeViewportEpoch_ = 0;
    bool ResumeViewport_ = false;
    void SetProjectWorkspace(const QString& root, bool restore);
    void SaveProjectWorkspace() const;
    QByteArray DefaultLayout_;
    QDockWidget* InspectorDock_ = nullptr;
    QDockWidget* OutputDock_ = nullptr;
    // Constructed in the MainWindow member-initializer list, so none is ever
    // null (no nullable OrNull state); BuildUi only configures and places them.
    QDockWidget* TimingDock_;
    TimingPanel* Timing_;
    DocumentWell* mDocuments;
    QScrollArea* mAudioDocument;
    QDockWidget* mProjectExplorerDock;
    ProjectExplorer* mProjectExplorer;
    QDockWidget* mPropertiesDock;
    QStackedWidget* mProperties;
    QLabel* mNoProperties;
    QDockWidget* mErrorListDock;
    ErrorList* mErrorList;
    QDockWidget* mContentDock;
    QDockWidget* mRenderPreviewDock;
    QToolBar* mStandardToolbar;
    QToolBar* mRenderGraphToolbar;
    QAction* mNewGraphAction;
    QAction* mOpenGraphAction;
    QAction* mValidateGraphAction;
    QAction* mBuildGraphAction;
    QAction* mBuildPreviewGraphAction;
    QAction* mLivePreviewGraphAction;
    QAction* mCookGraphAction;
    QAction* mCancelGraphAction;
    QAction* mOptionsAction;
    QAction* mProjectSettingsAction;
    QAction* mResetLayoutAction;
    QToolBar* GameToolbar_ = nullptr;
    // Build output is parsed for the Error List after it stops changing briefly.
    QTimer mBuildErrorsTimer;
    QString mParsedBuildOutput;
    // Set when no saved layout was restored, so the first show sizes the default docks.
    bool mShouldSizeDefaultDocks = true;

    // Project lifecycle presentation; controller actions own all operations.
    QLabel* ProjectIdentity_ = nullptr;
    QLabel* ProjectSetupStatus_ = nullptr;
    QLabel* ProjectNextStep_ = nullptr;
    QPushButton* ProjectCheckButton_ = nullptr;
    QPushButton* ProjectRepairButton_ = nullptr;
    QPushButton* ProjectRunButton_ = nullptr;
    QPushButton* ProjectSaveButton_ = nullptr;
    QPushButton* ProjectStopButton_ = nullptr;
    QPushButton* mUseEditorEngineButtonOrNull = nullptr; // shown when the pinned engine is stale or unusable

    // Settings form controls.
    QLineEdit* NameEdit_ = nullptr;
    QComboBox* ProviderBox_ = nullptr;
    QLineEdit* SourceDirEdit_ = nullptr;
    QComboBox* PresetBox_ = nullptr;
    QComboBox* TargetBox_ = nullptr; // editable; executable dropdown after Configure
    QLineEdit* CwdEdit_ = nullptr;
    QListWidget* ArgsList_ = nullptr;

    // Actions / status.
    QWidget* Welcome_ = nullptr;
    QPushButton* WelcomeNewButton_ = nullptr;
    QAction* CloseProjectAction_ = nullptr;
    QListWidget* RecentList_ = nullptr;
    QLabel* RecentEmptyLabel_ = nullptr;
    QPushButton* RecentOpenButton_ = nullptr;
    QPushButton* BrowseProjectButton_ = nullptr;
    QMenu* RecentMenu_ = nullptr;
    QAction* ClearRecentAction_ = nullptr;
    QStringList RecentProjectsStamp_;
    QAction* NewProjectAction_ = nullptr;
    QAction* CheckSetupAction_ = nullptr;
    QAction* SetupProjectAction_ = nullptr;
    QAction* OpenAction_ = nullptr;
    QAction* SaveAction_ = nullptr;
    QAction* UndoAction_ = nullptr;
    QAction* RedoAction_ = nullptr;
#if defined(Q_OS_WASM)
    QAction* ExportProjectAction_ = nullptr;
#endif
    QAction* ReloadAction_ = nullptr;
    QAction* ConfigureAction_ = nullptr;
    QAction* BuildAction_ = nullptr;
    QAction* BuildRunAction_ = nullptr;
    QAction* BuildDebugAction_ = nullptr;
    QAction* SetupReleaseAction_ = nullptr;
    QAction* PackageReleaseAction_ = nullptr;
    QAction* StopAction_ = nullptr;
    QAction* ClearAction_ = nullptr;
    QAction* CopyAction_ = nullptr;
    QAction* PlayAction_ = nullptr;
    QAction* BuildReloadAction_ = nullptr;
    QAction* AutoReloadAction_ = nullptr;
    QAction* ReloadAssetAction_ = nullptr;
    QAction* PauseAction_ = nullptr;
    QAction* StepAction_ = nullptr;
    QAction* ResumeAction_ = nullptr;
    QAction* RefreshPropertiesAction_ = nullptr;
    QAction* UndoSessionAction_ = nullptr;
    QAction* RedoSessionAction_ = nullptr;
    QAction* UndoTuningAction_ = nullptr;
    QAction* RedoTuningAction_ = nullptr;
    QAction* SaveTuningAction_ = nullptr;
    QAction* DiscardTuningAction_ = nullptr;
    QTableWidget* Properties_ = nullptr;
    QPushButton* ApplySessionButton_ = nullptr;
    QPushButton* ApplyDocumentButton_ = nullptr;
    QString PropertiesStamp_;
    QPushButton* AddArgButton_ = nullptr;
    QPushButton* RemoveArgButton_ = nullptr;
    QLabel* StatusLabel_ = nullptr;
    QLabel* RuntimeLabel_ = nullptr;
    QPlainTextEdit* Output_ = nullptr;
};

} // namespace ludus::editor
