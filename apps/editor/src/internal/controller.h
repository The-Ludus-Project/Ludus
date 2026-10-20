#pragma once

// EditorController: validates actions, advances WorkspaceState, executes effects
// (project load/save, tool start/cancel) and publishes exactly one state-change
// notification per step (design.md sections 3, 5, 6, 9).
//
// Private editor header (not installed). The controller is the only owner of
// WorkspaceState, project/history stores, ToolProcess and LogBuffer; MainWindow
// renders controller state and emits actions. There is no global event bus or singleton.

#include "internal/document_history.h"
#include "internal/log_buffer.h"
#include "internal/play_process.h"
#include "internal/project_store.h"
#include "internal/recent_projects.h"
#include "internal/source_watch.h"
#include "internal/timing_view.h"
#include "internal/tool_process.h"
#include "internal/workspace.h"

#include <ludus/foundation/base/types.h>

#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace ludus::editor
{

// Where to find the trusted tooling adapter/interpreter (resolved by
// scripts/editor and passed to the application). The controller never imports a
// Python adapter named in the descriptor.
struct ToolingPaths
{
    QString PythonPath;  // absolute managed interpreter
    QString AdapterPath; // absolute editor_tool.py
    QString ToolingRoot; // absolute tooling checkout
};

struct ProjectCreationOptions
{
    QString Destination;
    QString Name;
    QString Sdk;
    bool PrepareEngine = false;
};

// One recorded command invocation for Copy Job Details (bounded).
struct CommandRecord
{
    QString Stage;
    QStringList Argv;
    QString Cwd;
};

enum class PlayPhase : foundation::uint8
{
    Stopped,
    Starting,
    Running,
    Paused,
    Stopping,
    CleanupUnknown
};
struct PlaySnapshot
{
    PlayPhase Phase = PlayPhase::Stopped;
    uint64 Epoch = 0;
    QString Session;
    QString Generation;
    QString Message;
    foundation::int64 HostPid = 0;
    QStringList HostArgv;
    QString HostCwd;
    QString SdkIdentity;
    bool DebuggerStopped = false;
    uint64 SchemaEpoch = 0;
    uint64 SchemaVersion = 0;
    QJsonArray Properties;
    QJsonObject HostStatus;
    /// Runtime timing from the latest Status result; meaningful while HasTiming.
    TimingSnapshot Timing;
    /// Whether Timing holds a validated runtime report for this session.
    bool HasTiming = false;
    /// Why the latest timing record was rejected; empty when it was accepted.
    QString TimingMessage;
    QJsonObject ScriptStatus;
    bool ScriptPaused = false;
    bool ScriptBusy = false;
    QString ScriptMessage;
    bool TuningDocumentAvailable = false;
    bool TuningDocumentDirty = false;
    bool TuningCanUndo = false;
    bool TuningCanRedo = false;
};

class EditorController : public QObject
{
    Q_OBJECT
public:
    explicit EditorController(ToolingPaths tooling, QObject* parent = nullptr, const QString& recentProjectsPath = {});

    [[nodiscard]] const ToolingPaths& Tooling() const noexcept
    {
        return Tooling_;
    }
    [[nodiscard]] const WorkspaceState& State() const noexcept
    {
        return State_;
    }
    [[nodiscard]] const LogBuffer& Log() const noexcept
    {
        return Log_;
    }
    [[nodiscard]] const QList<RecentProject>& RecentProjects() const noexcept
    {
        return RecentProjects_.Entries();
    }
    [[nodiscard]] Capabilities Caps() const;
    [[nodiscard]] const PlaySnapshot& PlayState() const noexcept
    {
        return PlayState_;
    }
    [[nodiscard]] bool CanPlay() const;
    [[nodiscard]] bool CanBuildReload() const;

    // Expose the store so tests can inject fault hooks.
    [[nodiscard]] ProjectStore& Store() noexcept
    {
        return Store_;
    }

    // Actions (validated centrally). Each applies at most one transition and
    // emits StateChanged exactly once when it changes state.
    [[nodiscard]] bool CloseProject();
    void OpenProject(const QString& descriptorPath);
    void ClearRecentProjects();
    void EditDraft(const ProjectDescriptor& draft, bool merge = false);
    void UndoProjectEdit();
    void RedoProjectEdit();
    [[nodiscard]] bool CanUndoProject() const;
    [[nodiscard]] bool CanRedoProject() const;
    [[nodiscard]] uint64 ProjectRevision() const noexcept
    {
        return ProjectHistory_.Revision();
    }
    void Save();
    void Reload();
    void Configure();
    void Build();
    /// Build and run the configured target. hostedWindowAddress, when set, is the game's
    /// hosted-window channel: the game then opens in the editor's viewport (Play in the viewport).
    void BuildRun(const QString& hostedWindowAddress = {});
    void BuildDebug(const QString& debugger = {}, bool setup = false);
    void CheckProjectSetup();
    void SetupProject(const QString& sdk, const QString& webSdk, bool prepareEngine, bool disableWeb = false);
    /// Rebuild and install this editor's engine, select it for the project and verify the
    /// project build. Offered when the last check reported a stale or unusable engine pin.
    void UseEditorEngine();
    void CreateProject(const ProjectCreationOptions& creation);
    void SetupRelease(const QString& platform, const QString& itchTarget);
    void PackageRelease(const QString& profile, const QString& version, const QString& sdk);
    void Play();
    void BuildReload();
    void ReloadClearConfiguration(const QString& source);
    void SetAutoReload(bool enabled);
    [[nodiscard]] bool AutoReloadEnabled() const noexcept
    {
        return Watch_.Active();
    }
    void PlayCommand(const QString& command);
    void CookScripts();
    void ScriptCommand(const QJsonObject& command);
    void RefreshProperties();
    void RefreshSessionDetails();
    /// Request runtime timing (a Status command) while a session runs. At most
    /// one timing request is outstanding; the Timing panel calls this on its own
    /// refresh timer only while it is visible.
    void RefreshTiming();
    [[nodiscard]] bool CanEditProperties() const;
    void EditProperty(int row, const QString& text);
    void UndoSessionEdit();
    void RedoSessionEdit();
    [[nodiscard]] bool CanUndoSession() const;
    [[nodiscard]] bool CanRedoSession() const;
    [[nodiscard]] bool CanApplyToTuningDocument(int row) const;
    [[nodiscard]] bool CanSaveTuningDocument() const noexcept;
    void ApplyLivePropertyToTuningDocument(int row);
    void SaveTuningDocument();
    void DiscardTuningDocumentDraft();
    void UndoTuningDocument();
    void RedoTuningDocument();
    void Stop();
    void ClearOutput();

    // A bounded text record for Copy Job Details (design section 10).
    [[nodiscard]] QString JobDetails() const;

    // Request to close. Returns true if the application may close immediately;
    // false if a job is active (caller must offer Stop and Close).
    [[nodiscard]] bool RequestClose();

Q_SIGNALS:
    // Published after any state/log change. Rendering reads State()/Log(); it
    // must not trigger new edit actions (reentrancy is deferred by the window).
    void StateChanged();
    void DebuggerSetupRequested();

private Q_SLOTS:
    void OnToolEvent(const ProtocolEvent& event);
    void OnPlayEvent(const QJsonObject& event);

private:
    void StartJob(ActionKind kind, ToolOperation operation, const ToolLaunch& options = {});
    void ScheduleSetupCheck();
    void RememberProject();
    void Publish();
    void InvalidateDraftTargets();
    void RecordCommand(const QString& stage, const QStringList& argv, const QString& cwd);
    [[nodiscard]] QString ResolveDescriptorPath() const;
    void ActivateGeneration(const QString& path);
    [[nodiscard]] QString NextPlayRequest();
    void SubmitEdit(const QJsonObject& edit);
    void ReplaySessionEdit(bool undo);
    void SendTuningDocumentCommand(QJsonObject command, bool closeWhenClean = false);

    ToolingPaths Tooling_;
    WorkspaceState State_;
    DocumentHistory<ProjectDescriptor> ProjectHistory_;
    bool Saving_ = false;
    ProjectStore Store_;
    RecentProjectStore RecentProjects_;
    LogBuffer Log_;
    ToolProcess Tool_;
    PlayProcess Play_;
    SourceWatch Watch_;
    uint64 SetupCheckJob_ = 0;
    QString PendingSetupDetails_;
    bool SetupCheckPending_ = false;
    PlaySnapshot PlayState_;
    uint64 NextPlayRequest_ = 1;
    bool GenerationJob_ = false;
    QString PublishedGeneration_;
    QString ScriptRequest_;
    QString PropertyRequest_;
    QString TimingRequest_;
    QJsonArray PropertyChunks_;
    uint64 PropertyCount_ = 0;
    QString EditRequest_;
    struct SessionEdit
    {
        QJsonObject Before;
        QJsonObject After;
        QJsonObject Expected;
        QString Generation;
        uint64 SchemaEpoch = 0;
        uint64 SchemaVersion = 0;
    } PendingEdit_;
    QList<SessionEdit> SessionUndo_;
    QList<SessionEdit> SessionRedo_;
    QJsonObject EditReply_;
    enum class EditIntent : foundation::uint8
    {
        New,
        Undo,
        Redo
    };
    EditIntent EditIntent_ = EditIntent::New;
    QString TuningDocumentRequest_;
    QString TuningDocumentDigest_;
    bool TuningDocumentDirty_ = false;
    bool TuningDocumentAvailable_ = false;
    bool TuningCanUndo_ = false;
    bool TuningCanRedo_ = false;
    bool TuningDocumentCloseAfterRequest_ = false;

    // Bounded Copy Job Details storage for the current/last job.
    QList<CommandRecord> Commands_;
    QStringList Transitions_;
    qint64 LastPid_ = 0;
    QString PendingCreatedProject_;
    bool PendingDebuggerPrompt_ = false;
};

} // namespace ludus::editor
