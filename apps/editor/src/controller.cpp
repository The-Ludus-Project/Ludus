#include "internal/controller.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log.hpp>

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTimer>

namespace ludus::editor
{
namespace
{
LUDUS_DEFINE_LOG_CATEGORY(LOG_EDITOR_CTRL, "Editor");

Phase PhaseFromStage(const QString& stage)
{
    if (stage == QStringLiteral("configuring"))
    {
        return Phase::Configuring;
    }
    if (stage == QStringLiteral("building"))
    {
        return Phase::Building;
    }
    if (stage == QStringLiteral("publishing"))
    {
        return Phase::Publishing;
    }
    if (stage == QStringLiteral("launching"))
    {
        return Phase::Launching;
    }
    if (stage == QStringLiteral("running"))
    {
        return Phase::Running;
    }
    if (stage == QStringLiteral("stopping"))
    {
        return Phase::Stopping;
    }
    return Phase::Starting;
}

const char* PhaseName(Phase phase)
{
    switch (phase)
    {
        case Phase::Idle:
            return "Idle";
        case Phase::Starting:
            return "Starting";
        case Phase::Configuring:
            return "Configuring";
        case Phase::Building:
            return "Building";
        case Phase::Publishing:
            return "Publishing";
        case Phase::Launching:
            return "Launching";
        case Phase::Debugging:
            return "Debugging";
        case Phase::Running:
            return "Running";
        case Phase::Stopping:
            return "Stopping";
        case Phase::CleanupUnknown:
            return "CleanupUnknown";
    }
    return "Idle";
}
} // namespace

EditorController::EditorController(ToolingPaths tooling, QObject* parent, const QString& recentProjectsPath)
    : QObject(parent), Tooling_(std::move(tooling)), RecentProjects_(recentProjectsPath), Tool_(this), Play_(this)
{
    if (!RecentProjects_.Load())
    {
        LUDUS_LOG_TEXT(LOG_EDITOR_CTRL, Warning, "could not load recent projects; starting with an empty history");
    }
    // Bind tool events to this controller's lifetime; the queued connection
    // defers delivery to the next event-loop turn so a reentrant mutation from
    // inside a notification cannot corrupt state.
    connect(&Tool_, &ToolProcess::Event, this, &EditorController::OnToolEvent, Qt::QueuedConnection);
    connect(&Play_, &PlayProcess::Event, this, &EditorController::OnPlayEvent);
    connect(&Watch_, &SourceWatch::BuildRequested, this, &EditorController::BuildReload);
    connect(&Watch_, &SourceWatch::Failed, this, [this](const QString& message) {
        PlayState_.Message = message;
        Publish();
    });
    connect(&Play_, &PlayProcess::Finished, this, [this](bool confirmed) {
        PlayState_.Phase = confirmed ? PlayPhase::Stopped : PlayPhase::CleanupUnknown;
        PlayState_.Properties = {};
        PropertyRequest_.clear();
        if (confirmed && !TuningDocumentDirty_)
        {
            TuningDocumentAvailable_ = false;
            TuningDocumentDigest_.clear();
            TuningCanUndo_ = TuningCanRedo_ = false;
            PlayState_.TuningDocumentAvailable = false;
            PlayState_.TuningDocumentDirty = false;
            PlayState_.TuningCanUndo = PlayState_.TuningCanRedo = false;
        }
        else if (!confirmed && TuningDocumentDirty_)
        {
            PlayState_.Message =
                QStringLiteral("Play supervisor exited with an unsaved tuning draft; recovery is unknown");
        }
        Publish();
    });
    connect(
        &Tool_,
        &ToolProcess::Finished,
        this,
        [this]() {
            const QString created = PendingCreatedProject_;
            PendingCreatedProject_.clear();
            if (!created.isEmpty() && State_.Result.Kind == Outcome::Success)
            {
                OpenProject(QDir(created).filePath(QStringLiteral("ludus.project.json")));
            }
            const bool prompt = PendingDebuggerPrompt_;
            PendingDebuggerPrompt_ = false;
            Publish();
            if (prompt && Caps().CanBuildDebug && State_.Result.Code == ResultCode::MissingDebugger)
            {
                Q_EMIT DebuggerSetupRequested();
            }
        },
        Qt::QueuedConnection);
}

void EditorController::Publish()
{
    if (PlayState_.Phase == PlayPhase::Stopped || PlayState_.Phase == PlayPhase::Stopping ||
        PlayState_.Phase == PlayPhase::CleanupUnknown)
    {
        Watch_.Stop();
    }
    Watch_.SetBusy(!CanBuildReload());
    Q_EMIT StateChanged();
}

QString EditorController::ResolveDescriptorPath() const
{
    return State_.DescriptorPath;
}

void EditorController::OpenProject(const QString& descriptorPath)
{
    if (!Caps().CanOpen)
    {
        return; // a failed attempt leaves the existing workspace intact
    }
    const QString absolute = QFileInfo(descriptorPath).absoluteFilePath();
    const LoadOutcome outcome = Store_.Load(absolute);
    if (!outcome.Ok())
    {
        // Invalid/missing file leaves the previous workspace unchanged and
        // records a useful error as the last result.
        LastResult result;
        result.Kind = Outcome::Failed;
        result.Stage = Phase::Idle;
        result.Code = outcome.Parse.Code;
        result.Message = outcome.Parse.Message;
        State_.Result = result;
        LUDUS_LOG_TEXT(LOG_EDITOR_CTRL, Warning, "open project failed");
        Publish();
        return;
    }

    State_.Document = DocumentState::ProjectLoaded;
    State_.DescriptorPath = absolute;
    State_.SavedDigest = outcome.Digest;
    State_.Saved = outcome.Parse.Descriptor;
    State_.Draft = outcome.Parse.Descriptor;
    State_.HasSaved = true;
    State_.ProjectEpoch += 1;
    ProjectHistory_.Reset();
    State_.DiscoveredTargets.clear();
    State_.DiscoveredPreset.clear();
    State_.Result = LastResult{};
    LUDUS_LOG_TEXT(LOG_EDITOR_CTRL, Info, "opened project (no project code executed)");
    RememberProject();
    ScheduleSetupCheck();
    Publish();
}

bool EditorController::CloseProject()
{
    if (!Caps().CanCloseProject || State_.Dirty() || TuningDocumentDirty_)
    {
        return false;
    }
    // Retire project callbacks without recycling job ids or retaining drafts.
    const auto epoch = State_.ProjectEpoch + 1;
    const auto nextJob = State_.NextJob;
    State_ = WorkspaceState{};
    ProjectHistory_.Reset();
    State_.ProjectEpoch = epoch;
    State_.NextJob = nextJob;
    SetupCheckPending_ = false;
    SetupCheckJob_ = 0;
    PendingSetupDetails_.clear();
    PendingCreatedProject_.clear();
    PlayState_ = PlaySnapshot{};
    Watch_.Stop();
    Publish();
    return true;
}

void EditorController::RememberProject()
{
    if (!RecentProjects_.Remember({ .DescriptorPath = State_.DescriptorPath, .Name = State_.Saved.Name }))
    {
        LUDUS_LOG_TEXT(LOG_EDITOR_CTRL,
                       Warning,
                       "could not save recent projects; history remains available this session");
    }
}

void EditorController::ClearRecentProjects()
{
    if (!Caps().CanOpen)
    {
        return;
    }
    if (!RecentProjects_.Clear())
    {
        LUDUS_LOG_TEXT(LOG_EDITOR_CTRL, Warning, "could not persist cleared recent projects");
    }
    Publish();
}

void EditorController::EditDraft(const ProjectDescriptor& draft, bool merge)
{
    if (!Caps().CanEdit)
    {
        return;
    }
    if (!ProjectHistory_.Commit(State_.Draft, draft, merge))
    {
        return;
    }
    State_.Draft = draft;
    InvalidateDraftTargets();
    Publish();
}

void EditorController::InvalidateDraftTargets()
{
    // A preset change invalidates the target discovery cache.
    if (State_.DiscoveredPreset != State_.Draft.Preset)
    {
        State_.DiscoveredTargets.clear();
        State_.DiscoveredPreset.clear();
    }
}

bool EditorController::CanUndoProject() const
{
    return Caps().CanEdit && ProjectHistory_.CanUndo();
}

bool EditorController::CanRedoProject() const
{
    return Caps().CanEdit && ProjectHistory_.CanRedo();
}

void EditorController::UndoProjectEdit()
{
    if (CanUndoProject() && ProjectHistory_.Undo(State_.Draft))
    {
        InvalidateDraftTargets();
        Publish();
    }
}

void EditorController::RedoProjectEdit()
{
    if (CanRedoProject() && ProjectHistory_.Redo(State_.Draft))
    {
        InvalidateDraftTargets();
        Publish();
    }
}

void EditorController::Save()
{
    if (Saving_ || !Caps().CanSave)
    {
        return;
    }
    // A save acknowledges only its captured snapshot, including when a storage
    // callback causes another edit. Failure never clears history or the draft.
    ProjectHistory_.BreakGroup();
    const auto snapshot = State_.Draft;
    const auto epoch = State_.ProjectEpoch;
    const auto path = State_.DescriptorPath;
    const auto digest = State_.SavedDigest;
    Saving_ = true;
    const SaveOutcome outcome = Store_.Save(path, snapshot, digest);
    Saving_ = false;
    if (epoch != State_.ProjectEpoch || path != State_.DescriptorPath)
    {
        return;
    }
    if (!outcome.Ok())
    {
        LastResult result;
        result.Kind = Outcome::Failed;
        result.Code = outcome.Code;
        result.Message = outcome.Message;
        State_.Result = result;
        LUDUS_LOG_TEXT(LOG_EDITOR_CTRL, Warning, "save failed; keeping dirty draft and previous saved file");
        Publish();
        return;
    }
    State_.Saved = snapshot;
    State_.SavedDigest = outcome.Digest;
    State_.HasSaved = true;
    State_.Result = LastResult{};
    LUDUS_LOG_TEXT(LOG_EDITOR_CTRL, Info, "saved project");
    RememberProject();
    ScheduleSetupCheck();
    Publish();
}

void EditorController::Reload()
{
    if (!Caps().CanReload)
    {
        return;
    }
    const LoadOutcome outcome = Store_.Load(State_.DescriptorPath);
    if (!outcome.Ok())
    {
        LastResult result;
        result.Kind = Outcome::Failed;
        result.Code = outcome.Parse.Code;
        result.Message = outcome.Parse.Message;
        State_.Result = foundation::Move(result);
        Publish();
        return;
    }
    State_.SavedDigest = outcome.Digest;
    State_.Saved = outcome.Parse.Descriptor;
    State_.Draft = outcome.Parse.Descriptor;
    State_.HasSaved = true;
    State_.ProjectEpoch += 1;
    ProjectHistory_.Reset();
    State_.DiscoveredTargets.clear();
    State_.DiscoveredPreset.clear();
    RememberProject();
    ScheduleSetupCheck();
    Publish();
}

void EditorController::StartJob(ActionKind kind, ToolOperation operation, const ToolLaunch& options)
{
    if (Tool_.Active() || !CanStartJob(State_, kind))
    {
        return; // duplicate/invalid starts cannot create a second job
    }
    PendingDebuggerPrompt_ = false;
    PendingSetupDetails_.clear();
    State_ = BeginJob(State_, kind);
    SetupCheckJob_ = operation == ToolOperation::ProjectCheck || operation == ToolOperation::ProjectSetup ||
                             operation == ToolOperation::InspectSetup
                         ? State_.ActiveJob
                         : 0;
    if (SetupCheckJob_ != 0)
    {
        State_.SetupStatus = QStringLiteral("Checking selectable CMake presets and local toolchain…");
    }
    Commands_.clear();
    Transitions_.clear();
    LastPid_ = 0;
    Transitions_.append(QString::fromLatin1(PhaseName(State_.OperationPhase)));

    ToolLaunch launch = options;
    launch.PythonPath = Tooling_.PythonPath;
    launch.AdapterPath = Tooling_.AdapterPath;
    launch.ToolingRoot = Tooling_.ToolingRoot;
    if (operation != ToolOperation::ProjectCreate)
    {
        launch.ProjectPath = State_.DescriptorPath;
        launch.ExpectedSha256 = State_.SavedDigest;
    }
    launch.Operation = operation;
    launch.Job = State_.ActiveJob;

    if (!Tool_.Start(launch))
    {
        LastResult result;
        result.Kind = Outcome::Failed;
        result.Code = ResultCode::Busy;
        result.Message = QStringLiteral("a tool process is already active");
        State_ = ApplyResult(State_, State_.ActiveJob, result);
    }
    Publish();
}

void EditorController::ScheduleSetupCheck()
{
#if defined(Q_OS_WASM)
    SetupCheckPending_ = false;
    State_.SetupStatus = QStringLiteral(
        "Browser workspace: Play Web Sample runs the shipped build. Download edits for a desktop rebuild.");
    return;
#endif
    // Open/Reload remain metadata operations. Queue the trusted read-only check
    // with an epoch guard so a later project switch cannot check the old project.
    SetupCheckPending_ = QFileInfo::exists(Tooling_.AdapterPath);
    State_.SetupStatus = SetupCheckPending_
                             ? QStringLiteral("CMake setup check pending")
                             : QStringLiteral("Setup validation unavailable: managed editor tooling is missing");
    const uint64 epoch = State_.ProjectEpoch;
    QTimer::singleShot(0, this, [this, epoch]() {
        if (State_.ProjectEpoch == epoch)
        {
            const bool pending = SetupCheckPending_;
            SetupCheckPending_ = false;
            if (pending && State_.Saved.Version == 2)
            {
                CheckProjectSetup();
            }
            else if (pending && State_.Saved.ProviderKind == Provider::Cmake && Caps().CanConfigure)
            {
                StartJob(ActionKind::Configure, ToolOperation::InspectSetup);
            }
            Publish();
        }
    });
}

void EditorController::CheckProjectSetup()
{
    if (Caps().CanProjectCheck)
    {
        StartJob(ActionKind::ProjectCheck, ToolOperation::ProjectCheck);
    }
}

void EditorController::SetupProject(const QString& sdk, const QString& webSdk, bool prepareEngine, bool disableWeb)
{
    if (!Caps().CanProjectSetup)
    {
        return;
    }
    ToolLaunch options;
    options.SetupSdk = sdk;
    options.SetupWebSdk = webSdk;
    options.PrepareEngine = prepareEngine;
    options.DisableWeb = disableWeb;
    StartJob(ActionKind::ProjectSetup, ToolOperation::ProjectSetup, options);
}

void EditorController::CreateProject(const ProjectCreationOptions& creation)
{
    if (!Caps().CanProjectCreate)
    {
        return;
    }
    ToolLaunch options;
    options.ProjectPath = QFileInfo(creation.Destination).absoluteFilePath();
    options.ProjectName = creation.Name;
    options.SetupSdk = creation.Sdk;
    options.PrepareEngine = creation.PrepareEngine;
    PendingCreatedProject_ = options.ProjectPath;
    StartJob(ActionKind::ProjectCreate, ToolOperation::ProjectCreate, options);
}

void EditorController::Configure()
{
    if (Caps().CanConfigure)
    {
        StartJob(ActionKind::Configure, ToolOperation::Configure);
    }
}
void EditorController::Build()
{
    if (Caps().CanBuild)
    {
        StartJob(ActionKind::Build, ToolOperation::Build);
    }
}
void EditorController::BuildRun()
{
    if (Caps().CanBuildRun)
    {
        StartJob(ActionKind::BuildRun, ToolOperation::BuildRun);
    }
}

void EditorController::BuildDebug(const QString& debugger, bool setup)
{
    if (!Caps().CanBuildDebug)
    {
        return;
    }
    ToolLaunch options;
    options.DebuggerPath = debugger;
    options.SetupDebugger = setup;
    StartJob(ActionKind::BuildDebug, ToolOperation::BuildDebug, options);
}

void EditorController::SetupRelease(const QString& platform, const QString& itchTarget)
{
    if (!Caps().CanReleaseInit)
    {
        return;
    }
    ToolLaunch options;
    options.ReleasePlatform = platform;
    options.ItchTarget = itchTarget;
    StartJob(ActionKind::ReleaseInit, ToolOperation::ReleaseInit, options);
}

void EditorController::PackageRelease(const QString& profile, const QString& version, const QString& sdk)
{
    if (!Caps().CanPackage)
    {
        return;
    }
    ToolLaunch options;
    options.ReleaseProfile = profile;
    options.ReleaseVersion = version;
    options.ReleaseSdk = sdk;
    StartJob(ActionKind::Package, ToolOperation::Package, options);
}

void EditorController::Stop()
{
    Watch_.Stop();
    if (Play_.Active() && TuningDocumentDirty_ &&
        (PlayState_.Phase == PlayPhase::Stopped || PlayState_.Phase == PlayPhase::Starting))
    {
        PlayState_.Message = QStringLiteral("Save or discard the tuning draft before closing Play");
        Publish();
        return;
    }
    if (Play_.Active() && (PlayState_.Phase == PlayPhase::Running || PlayState_.Phase == PlayPhase::Paused ||
                           (PlayState_.Phase == PlayPhase::CleanupUnknown && Play_.CanBeginSession())))
    {
        PlayState_.Phase = PlayPhase::Stopping;
        const auto request = NextPlayRequest();
        if (!Play_.Send(
                {{QStringLiteral("type"), QStringLiteral("command")},
                 {QStringLiteral("request"), request},
                 {QStringLiteral("command"), QJsonObject{{QStringLiteral("command"), QStringLiteral("Stop")}}}}))
        {
            PlayState_.Phase = PlayPhase::CleanupUnknown;
            PlayState_.Message = QStringLiteral("Stop could not be queued; inspect Play status");
        }
    }
    else if (GenerationJob_ || Play_.Active())
    {
        PlayState_.Phase = PlayPhase::Stopping;
        Play_.Close();
    }
    if (State_.ActiveJob == 0)
    {
        Publish();
        return;
    }
    // Latch stop first so a late success cannot launch; the latched state wins
    // even before the adapter acknowledges.
    State_ = LatchStop(State_);
    Tool_.Cancel();
    Publish();
}

void EditorController::ClearOutput()
{
    Log_.Clear();
    Publish();
}

bool EditorController::RequestClose()
{
    if (Caps().CanCloseImmediately)
    {
        return true;
    }
    // Busy: begin asynchronous cancellation; the window closes only after the
    // controller confirms cleanup (handled when the terminal result arrives).
    Stop();
    return false;
}

void EditorController::RecordCommand(const QString& stage, const QStringList& argv, const QString& cwd)
{
    if (Commands_.size() >= 8)
    {
        return; // bounded: retain at most eight command records per job
    }
    CommandRecord record;
    record.Stage = stage;
    record.Argv = argv;
    record.Cwd = cwd;
    Commands_.append(record);
}

void EditorController::OnToolEvent(const ProtocolEvent& event)
{
    // Ignore events for a retired/non-current job (stale callbacks).
    if (event.Job != State_.ActiveJob && event.Kind != ProtocolEvent::Type::Error)
    {
        return;
    }

    switch (event.Kind)
    {
        case ProtocolEvent::Type::Ready:
            break;
        case ProtocolEvent::Type::Phase: {
            const Phase before = State_.OperationPhase;
            State_ = ApplyPhaseEvent(State_, event.Job, PhaseFromStage(event.Stage), /*runtimeConfirmed=*/false);
            if (State_.OperationPhase != before && Transitions_.size() < 256)
            {
                Transitions_.append(QString::fromLatin1(PhaseName(State_.OperationPhase)));
            }
            break;
        }
        case ProtocolEvent::Type::Command:
            RecordCommand(event.Stage, event.Argv, event.Cwd);
            break;
        case ProtocolEvent::Type::Targets:
            // Target discovery never changes the document silently; it only updates
            // the discovery cache for the current preset.
            State_.DiscoveredTargets = event.Targets;
            State_.DiscoveredPreset = event.Preset;
            break;
        case ProtocolEvent::Type::Output:
            Log_.Append(event.Stream == ProtocolEvent::OutputStreamTag::Stderr ? OutputStream::Stderr
                                                                               : OutputStream::Stdout,
                        event.Text);
            break;
        case ProtocolEvent::Type::RuntimeStarted: {
            LastPid_ = event.Pid;
            const Phase before = State_.OperationPhase;
            State_ = ApplyPhaseEvent(State_, event.Job, Phase::Running, /*runtimeConfirmed=*/true);
            if (State_.OperationPhase != before && Transitions_.size() < 256)
            {
                Transitions_.append(QString::fromLatin1(PhaseName(State_.OperationPhase)));
            }
            break;
        }
        case ProtocolEvent::Type::DebuggerStarted: {
            LastPid_ = event.Pid;
            State_ = ApplyDebuggerStarted(State_, event.Job);
            if (Transitions_.size() < 256)
            {
                Transitions_.append(QString::fromLatin1(PhaseName(State_.OperationPhase)));
            }
            break;
        }
        case ProtocolEvent::Type::SetupDetails:
            if (event.Job == SetupCheckJob_ && !State_.StopLatched)
            {
                PendingSetupDetails_ = event.Text;
            }
            break;
        case ProtocolEvent::Type::Generation:
            if (GenerationJob_ && !State_.StopLatched)
            {
                PublishedGeneration_ = event.GenerationPath;
            }
            break;
        case ProtocolEvent::Type::Result: {
            LastResult result;
            result.Stage = State_.OperationPhase;
            if (event.Outcome == QStringLiteral("success"))
            {
                result.Kind = Outcome::Success;
            }
            else if (event.Outcome == QStringLiteral("cancelled"))
            {
                result.Kind = Outcome::Cancelled;
            }
            else
            {
                result.Kind = Outcome::Failed;
            }
            result.Code = event.Code;
            result.Message = event.Message;
            result.ExitCode = event.ExitCode;
            result.Signal = event.Signal;
            result.CleanupConfirmed = event.CleanupConfirmed;
            if (!event.CleanupConfirmed)
            {
                result.Kind = Outcome::CleanupUnknown;
            }
            PendingDebuggerPrompt_ = State_.ActiveAction == ActionKind::BuildDebug && !State_.StopLatched &&
                                     result.Kind == Outcome::Failed && result.Code == ResultCode::MissingDebugger &&
                                     result.CleanupConfirmed;
            if (event.Job == SetupCheckJob_)
            {
                State_.SetupStatus =
                    result.Kind == Outcome::Success
                        ? QStringLiteral("CMake setup ready: %1")
                              .arg(PendingSetupDetails_.isEmpty() ? result.Message : PendingSetupDetails_)
                        : QStringLiteral("CMake setup needs attention: %1").arg(result.Message);
                SetupCheckJob_ = 0;
                PendingSetupDetails_.clear();
            }
            State_ = ApplyResult(State_, event.Job, result);
            if (GenerationJob_)
            {
                GenerationJob_ = false;
                if (result.Kind == Outcome::Success && PlayState_.Phase != PlayPhase::Stopping &&
                    !PublishedGeneration_.isEmpty())
                {
                    ActivateGeneration(PublishedGeneration_);
                }
                else if (!Play_.Active())
                {
                    PlayState_.Phase = result.CleanupConfirmed ? PlayPhase::Stopped : PlayPhase::CleanupUnknown;
                }
                PublishedGeneration_.clear();
            }
            break;
        }
        case ProtocolEvent::Type::Error: {
            LastResult result;
            result.Stage = State_.OperationPhase;
            result.Kind = event.Code == ResultCode::CleanupUnknown ? Outcome::CleanupUnknown : Outcome::Failed;
            result.Code = event.Code;
            result.Message = event.Message;
            result.CleanupConfirmed = event.Code != ResultCode::CleanupUnknown;
            if (SetupCheckJob_ != 0)
            {
                State_.SetupStatus = QStringLiteral("CMake setup validation unavailable: %1").arg(event.Message);
                SetupCheckJob_ = 0;
                PendingSetupDetails_.clear();
            }
            if (State_.ActiveJob != 0)
            {
                State_ = ApplyResult(State_, State_.ActiveJob, result);
            }
            else
            {
                State_.Result = foundation::Move(result);
            }
            GenerationJob_ = false;
            if (!Play_.Active() && PlayState_.Phase != PlayPhase::Stopped)
            {
                PlayState_.Phase = event.CleanupConfirmed ? PlayPhase::Stopped : PlayPhase::CleanupUnknown;
            }
            break;
        }
    }
    Publish();
}

QString EditorController::JobDetails() const
{
    // Bounded, telemetry-free record: descriptor digest, provider/preset/target,
    // argv/cwd per stage, job id, transitions, last exit/signal/result, cleanup
    // status. No environment dump or upload (design section 10).
    QString out;
    out += QStringLiteral("job: %1\n").arg(State_.ActiveJob, 16, 16, QLatin1Char('0'));
    out += QStringLiteral("descriptor_digest: %1\n").arg(State_.SavedDigest);
    out += QStringLiteral("play_epoch: %1\nplay_session: %2\nplay_generation: %3\nplay_status: %4\n")
               .arg(PlayState_.Epoch, 16, 16, QLatin1Char('0'))
               .arg(PlayState_.Session, PlayState_.Generation, PlayState_.Message);
    out += QStringLiteral("play_details: %1\n")
               .arg(QString::fromUtf8(QJsonDocument(PlayState_.HostStatus).toJson(QJsonDocument::Compact)));
    out += QStringLiteral("play_host_pid: %1\nplay_sdk_identity: %2\nplay_host_cwd: %3\n")
               .arg(PlayState_.HostPid)
               .arg(PlayState_.SdkIdentity, PlayState_.HostCwd);
    for (const auto& argument : PlayState_.HostArgv)
    {
        out += QStringLiteral("play_host_argv: %1\n").arg(argument);
    }
    out += QStringLiteral("provider: %1\n")
               .arg(State_.Saved.ProviderKind == Provider::Ludus ? QStringLiteral("ludus") : QStringLiteral("cmake"));
    out += QStringLiteral("preset: %1\n").arg(State_.Saved.Preset);
    out += QStringLiteral("target: %1\n").arg(State_.Saved.Target);
    out += QStringLiteral("transitions: %1\n").arg(Transitions_.join(QStringLiteral(" -> ")));
    for (const CommandRecord& record : Commands_)
    {
        out += QStringLiteral("command[%1] cwd=%2\n").arg(record.Stage, record.Cwd);
        for (const QString& arg : record.Argv)
        {
            out += QStringLiteral("  argv: %1\n").arg(arg);
        }
    }
    if (LastPid_ != 0)
    {
        out += QStringLiteral("runtime_pid: %1\n").arg(LastPid_);
    }
    out += QStringLiteral("last_result: %1 (%2)\n")
               .arg(QString::fromLatin1(ResultCodeName(State_.Result.Code)), State_.Result.Message);
    if (State_.Result.ExitCode.has_value())
    {
        out += QStringLiteral("exit_code: %1\n").arg(State_.Result.ExitCode.value());
    }
    if (State_.Result.Signal.has_value())
    {
        out += QStringLiteral("signal: %1\n").arg(State_.Result.Signal.value());
    }
    out += QStringLiteral("cleanup_confirmed: %1\n")
               .arg(State_.Result.CleanupConfirmed ? QStringLiteral("true") : QStringLiteral("false"));
    out += QStringLiteral("dropped_output_blocks: %1\n").arg(Log_.DroppedBlocks());
    out += QStringLiteral("dropped_output_bytes: %1\n").arg(Log_.DroppedBytes());
    // Bound the whole record to 1 MiB.
    if (static_cast<usize>(out.toUtf8().size()) > usize{1} * 1024u * 1024u)
    {
        out.truncate(foundation::isize{1024} * 1024);
        out += QStringLiteral("\n[... job details truncated ...]\n");
    }
    return out;
}

} // namespace ludus::editor
