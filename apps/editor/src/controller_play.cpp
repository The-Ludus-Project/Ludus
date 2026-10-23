#include "internal/browser_player.h"
#include "internal/controller.h"

#include <ludus/foundation/base/types.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstring>

#if defined(Q_OS_WASM)
extern "C" void LudusBrowserPlaySample(const char* player);
#endif

namespace ludus::editor
{
namespace
{
/// Largest UTF-8 encoding accepted for a typed string property edit.
constexpr qsizetype MAX_STRING_PROPERTY_BYTES = 256;

bool DebugId(const QJsonValue& value)
{
    const auto text = value.toString();
    if (text.size() != 16)
    {
        return false;
    }
    for (const auto c : text)
    {
        if ((c < QLatin1Char('0') || c > QLatin1Char('9')) && (c < QLatin1Char('a') || c > QLatin1Char('f')))
        {
            return false;
        }
    }
    return true;
}
bool DebugReply(const QJsonObject& reply)
{
    return reply.value(QStringLiteral("version")).toInt() == 1 && reply.value(QStringLiteral("partial")).isBool() &&
           DebugId(reply.value(QStringLiteral("session"))) && DebugId(reply.value(QStringLiteral("execution"))) &&
           DebugId(reply.value(QStringLiteral("stop"))) && reply.value(QStringLiteral("frames")).isArray() &&
           reply.value(QStringLiteral("frames")).toArray().size() <= 8 &&
           reply.value(QStringLiteral("locals")).isArray() &&
           reply.value(QStringLiteral("locals")).toArray().size() <= 16;
}
} // namespace
Capabilities EditorController::Caps() const
{
    auto caps = ComputeCapabilities(State_);
    const bool owned = Play_.Active() || GenerationJob_ || PlayState_.Phase != PlayPhase::Stopped;
    if (owned)
    {
        caps.CanCloseProject = caps.CanOpen = caps.CanReload = caps.CanConfigure = caps.CanBuild = caps.CanBuildRun =
            caps.CanBuildDebug = false;
        caps.CanReleaseInit = caps.CanPackage = false;
        caps.CanProjectCheck = caps.CanProjectSetup = caps.CanProjectCreate = false;
        caps.CanCloseImmediately = false;
        caps.CanStop = (PlayState_.Phase != PlayPhase::CleanupUnknown || Play_.CanBeginSession()) &&
                       (PlayState_.Phase != PlayPhase::Stopped || TuningDocumentDirty_ == false);
        caps.CanCopyJobDetails = true;
    }
    // A result is provisional until the one-shot adapter has exited and drained.
    if (Tool_.Active())
    {
        caps.CanCloseImmediately = false;
        if (State_.Busy() == false)
        {
            caps.CanCloseProject = caps.CanOpen = caps.CanEdit = caps.CanSave = caps.CanReload = false;
            caps.CanConfigure = caps.CanBuild = caps.CanBuildRun = caps.CanBuildDebug = false;
            caps.CanProjectCheck = caps.CanProjectSetup = caps.CanProjectCreate = false;
            caps.CanReleaseInit = caps.CanPackage = false;
        }
    }
#if defined(Q_OS_MACOS)
    caps.CanBuildDebug = false;
#endif
#if defined(Q_OS_WASM)
    caps.CanConfigure = caps.CanBuild = caps.CanBuildRun = caps.CanBuildDebug = false;
    caps.CanProjectCheck = caps.CanProjectSetup = caps.CanProjectCreate = false;
    caps.CanReleaseInit = caps.CanPackage = false;
#endif
    return caps;
}

bool EditorController::CanPlay() const
{
#if defined(Q_OS_WASM)
    return State_.Document == DocumentState::ProjectLoaded && State_.Busy() == false &&
           BrowserPlayer(State_.DescriptorPath).isEmpty() == false;
#else
    return QFileInfo(Tooling_.PythonPath).isExecutable() && QFileInfo::exists(Tooling_.AdapterPath) &&
           QFileInfo::exists(QDir(Tooling_.ToolingRoot).filePath(QStringLiteral("scripts/python/play_tool.py"))) &&
           State_.Document == DocumentState::ProjectLoaded && State_.HasSaved && SetupCheckPending_ == false &&
           State_.Dirty() == false && State_.Busy() == false && State_.OperationPhase != Phase::CleanupUnknown &&
           Tool_.Active() == false && (Play_.Active() == false || Play_.CanBeginSession()) &&
           TuningDocumentDirty_ == false && PlayState_.Phase == PlayPhase::Stopped &&
           QFileInfo::exists(
               QDir(QFileInfo(State_.DescriptorPath).absolutePath()).filePath(QStringLiteral("ludus.play.json"))) &&
           (State_.Saved.Preset == QStringLiteral("linux-clang-debug") ||
            State_.Saved.Preset == QStringLiteral("linux-clang-development") ||
            State_.Saved.Preset == QStringLiteral("macos-clang-debug") ||
            State_.Saved.Preset == QStringLiteral("macos-clang-development"));
#endif
}

bool EditorController::CanBuildReload() const
{
    return PlayState_.ScriptPaused == false && State_.Dirty() == false && State_.Busy() == false &&
           Tool_.Active() == false && GenerationJob_ == false && PlayState_.DebuggerStopped == false &&
           (PlayState_.Phase == PlayPhase::Running || PlayState_.Phase == PlayPhase::Paused);
}

QString EditorController::NextPlayRequest()
{
    if (NextPlayRequest_ >= (uint64{1} << 63U))
    {
        return {};
    }
    return QStringLiteral("%1").arg(NextPlayRequest_++, 16, 16, QLatin1Char('0'));
}

void EditorController::Play()
{
    if (CanPlay() == false)
    {
        return;
    }
#if defined(Q_OS_WASM)
    const auto player = BrowserPlayer(State_.DescriptorPath).toUtf8();
    LudusBrowserPlaySample(player.constData());
    return;
#endif
    if (State_.ProjectEpoch == ~uint64{0})
    {
        return;
    }
    PlayState_ = {};
    ScriptRequest_.clear();
    TimingRequest_.clear();
    SessionUndo_.clear();
    SessionRedo_.clear();
    EditRequest_.clear();
    PlayState_.Epoch = ++State_.ProjectEpoch;
    PlayState_.Phase = PlayPhase::Starting;
    GenerationJob_ = true;
    PublishedGeneration_.clear();
    StartJob(ActionKind::Build, ToolOperation::BuildGeneration);
}

void EditorController::BuildReload()
{
    if (CanBuildReload() == false)
    {
        return;
    }
    GenerationJob_ = true;
    PublishedGeneration_.clear();
    StartJob(ActionKind::Build, ToolOperation::BuildGeneration);
}

void EditorController::SetAutoReload(bool enabled)
{
    if (enabled == false)
    {
        Watch_.Stop();
    }
    else if ((PlayState_.Phase == PlayPhase::Running || PlayState_.Phase == PlayPhase::Paused) &&
             Watch_.Start(QFileInfo(State_.DescriptorPath).absolutePath()) == false)
    {
        PlayState_.Message = QStringLiteral("Source watch setup failed; use manual build/reload");
    }
    Publish();
}

void EditorController::ReloadClearConfiguration(const QString& source)
{
    if (PlayState_.DebuggerStopped || (PlayState_.Phase != PlayPhase::Running && PlayState_.Phase != PlayPhase::Paused))
    {
        return;
    }
    const auto relative = QDir(QFileInfo(State_.DescriptorPath).absolutePath()).relativeFilePath(source);
    if (Play_.Send({{QStringLiteral("type"), QStringLiteral("asset")},
                    {QStringLiteral("request"), NextPlayRequest()},
                    {QStringLiteral("source"), relative},
                    {QStringLiteral("expected_generation"), PlayState_.Generation}}) == false)
    {
        PlayState_.Message = QStringLiteral("Configuration import could not be queued");
        Publish();
    }
}

void EditorController::ActivateGeneration(const QString& path)
{
    const auto request = NextPlayRequest();
    if (request.isEmpty())
    {
        PlayState_.Message = QStringLiteral("Request namespace exhausted; restart the editor");
        return;
    }
    if (Play_.Active() && (PlayState_.Phase == PlayPhase::Running || PlayState_.Phase == PlayPhase::Paused))
    {
        if (Play_.Send({{QStringLiteral("type"), QStringLiteral("reload")},
                        {QStringLiteral("request"), request},
                        {QStringLiteral("generation_path"), path},
                        {QStringLiteral("expected_generation"), PlayState_.Generation}}) == false)
        {
            PlayState_.Message = QStringLiteral("Reload command could not be queued; inspect Play status");
        }
    }
    else
    {
        QJsonObject start{{QStringLiteral("type"), QStringLiteral("start")},
                          {QStringLiteral("request"), request},
                          {QStringLiteral("project"), State_.DescriptorPath},
                          {QStringLiteral("expected_sha256"), State_.SavedDigest},
                          {QStringLiteral("generation_path"), path}};
        const bool sent = Play_.Active() ? Play_.BeginSession(PlayState_.Epoch, start)
                                         : Play_.Start(
                                         {
                                             .Python = Tooling_.PythonPath,
                                             .ToolingRoot = Tooling_.ToolingRoot,
                                             .Epoch = PlayState_.Epoch,
                                             .StartRequest = start,
                                         });
        if (sent == false)
        {
            PlayState_.Phase = Play_.Active() ? PlayPhase::CleanupUnknown : PlayPhase::Stopped;
            PlayState_.Message = QStringLiteral("Play supervisor could not start the session");
        }
    }
}

void EditorController::CookScripts()
{
    if (State_.HasSaved && State_.Dirty() == false && State_.Busy() == false && Tool_.Active() == false &&
        GenerationJob_ == false && PlayState_.Phase != PlayPhase::Starting && PlayState_.Phase != PlayPhase::Stopping &&
        PlayState_.Phase != PlayPhase::CleanupUnknown)
    {
        StartJob(ActionKind::Build, ToolOperation::CookScripts);
    }
}
void EditorController::ScriptCommand(const QJsonObject& command)
{
    if (ScriptRequest_.isEmpty() == false || PlayState_.DebuggerStopped || PlayState_.Phase != PlayPhase::Running ||
        command.value(QStringLiteral("version")).toInt() != 1)
    {
        return;
    }
    const auto payload = QJsonDocument(command).toJson(QJsonDocument::Compact);
    if (payload.isEmpty() || payload.size() > 4096)
    {
        return;
    }
    ScriptRequest_ = NextPlayRequest();
    PlayState_.ScriptBusy = true;
    if (Play_.Send({{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("request"), ScriptRequest_},
                    {QStringLiteral("command"),
                     QJsonObject{{QStringLiteral("command"), QStringLiteral("ScriptDebug")},
                                 {QStringLiteral("expected_generation"), PlayState_.Generation},
                                 {QStringLiteral("extension"), 1},
                                 {QStringLiteral("payload"), QString::fromUtf8(payload)}}}}) == false)
    {
        ScriptRequest_.clear();
        PlayState_.ScriptBusy = false;
        PlayState_.ScriptMessage = QStringLiteral("Debugger command could not be queued");
    }
    Publish();
}

void EditorController::PlayCommand(const QString& command)
{
    if (PlayState_.DebuggerStopped || (PlayState_.Phase != PlayPhase::Running && PlayState_.Phase != PlayPhase::Paused))
    {
        return;
    }
    if (command != QStringLiteral("Pause") && command != QStringLiteral("Resume") && command != QStringLiteral("Step"))
    {
        return;
    }
    (void)Play_.Send({{QStringLiteral("type"), QStringLiteral("command")},
                      {QStringLiteral("request"), NextPlayRequest()},
                      {QStringLiteral("command"),
                       QJsonObject{{QStringLiteral("command"), command},
                                   {QStringLiteral("expected_generation"), PlayState_.Generation}}}});
}

void EditorController::RefreshProperties()
{
    if (PropertyRequest_.isEmpty() == false ||
        (PlayState_.Phase != PlayPhase::Running && PlayState_.Phase != PlayPhase::Paused))
    {
        return;
    }
    PropertyChunks_ = {};
    PropertyCount_ = 0;
    PropertyRequest_ = NextPlayRequest();
    if (Play_.Send({{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("request"), PropertyRequest_},
                    {QStringLiteral("command"),
                     QJsonObject{{QStringLiteral("command"), QStringLiteral("ReadProperties")},
                                 {QStringLiteral("expected_generation"), PlayState_.Generation}}}}) == false)
    {
        PropertyRequest_.clear();
    }
}

void EditorController::RefreshSessionDetails()
{
    if (Play_.Active())
    {
        (void)Play_.Send(
            {{QStringLiteral("type"), QStringLiteral("command")},
             {QStringLiteral("request"), NextPlayRequest()},
             {QStringLiteral("command"), QJsonObject{{QStringLiteral("command"), QStringLiteral("Status")}}}});
    }
}

void EditorController::RefreshTiming()
{
    if (TimingRequest_.isEmpty() == false || Play_.Active() == false ||
        (PlayState_.Phase != PlayPhase::Running && PlayState_.Phase != PlayPhase::Paused))
    {
        return;
    }
    TimingRequest_ = NextPlayRequest();
    const QJsonObject status{
        {QStringLiteral("type"), QStringLiteral("command")},
        {QStringLiteral("request"), TimingRequest_},
        {QStringLiteral("command"), QJsonObject{{QStringLiteral("command"), QStringLiteral("Status")}}}};
    if (Play_.Send(status) == false)
    {
        TimingRequest_.clear();
    }
}

void EditorController::OnPlayEvent(const QJsonObject& event)
{
    const auto type = event.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("output"))
    {
        Log_.Append(event.value(QStringLiteral("stream")) == QStringLiteral("stderr") ? OutputStream::Stderr
                                                                                      : OutputStream::Stdout,
                    event.value(QStringLiteral("text")).toString());
    }
    else if (type == QStringLiteral("fatal"))
    {
        TimingRequest_.clear();
        PlayState_.Phase = PlayPhase::CleanupUnknown;
        PlayState_.Message = event.value(QStringLiteral("message")).toString();
    }
    else if (type == QStringLiteral("diagnostic"))
    {
        PlayState_.Message = event.value(QStringLiteral("message")).toString();
    }
    else if (type == QStringLiteral("debugger_state"))
    {
        PlayState_.DebuggerStopped = event.value(QStringLiteral("stopped")).toBool();
        if (PlayState_.DebuggerStopped)
        {
            PlayState_.Message =
                QStringLiteral("Debugger stopped the host; continue in the debugger before editing/reload");
        }
    }
    else if (type == QStringLiteral("host_started"))
    {
        PlayState_.HostPid = static_cast<foundation::int64>(event.value(QStringLiteral("pid")).toInteger());
        PlayState_.HostCwd = event.value(QStringLiteral("cwd")).toString();
        PlayState_.HostArgv.clear();
        for (const auto& argument : event.value(QStringLiteral("argv")).toArray())
        {
            PlayState_.HostArgv.append(argument.toString());
        }
        PlayState_.SdkIdentity =
            event.value(QStringLiteral("manifest")).toObject().value(QStringLiteral("sdk_identity")).toString();
    }
    else if (type == QStringLiteral("asset"))
    {
        PlayState_.Message = QStringLiteral("Configuration source %1 · cooked %2")
                                 .arg(event.value(QStringLiteral("source_sha256")).toString(),
                                      event.value(QStringLiteral("cooked_sha256")).toString());
    }
    else if (type == QStringLiteral("ended"))
    {
        PlayState_.DebuggerStopped = false;
        PlayState_.ScriptStatus = {};
        PlayState_.ScriptPaused = false;
        PlayState_.ScriptBusy = false;
        PlayState_.ScriptMessage.clear();
        ScriptRequest_.clear();
        TimingRequest_.clear();
        PlayState_.Phase =
            event.value(QStringLiteral("cleanup_confirmed")).toBool() ? PlayPhase::Stopped : PlayPhase::CleanupUnknown;
        PlayState_.Message = event.value(QStringLiteral("reason")).toString();
        PlayState_.Properties = {};
        // A dirty tuning draft belongs to the editor session and survives a
        // stopped game. Keep the actor alive until Save or explicit Discard.
        if (TuningDocumentDirty_ == false)
        {
            Play_.Close();
        }
    }
    else if (type == QStringLiteral("result"))
    {
        const auto request = event.value(QStringLiteral("request")).toString();
        const auto status = event.value(QStringLiteral("status")).toString();
        if (request == TuningDocumentRequest_)
        {
            if (status == QStringLiteral("Ok") || status == QStringLiteral("Conflict"))
            {
                TuningDocumentDirty_ = event.value(QStringLiteral("document_dirty")).toBool();
                TuningDocumentDigest_ = event.value(QStringLiteral("document_digest")).toString();
                TuningCanUndo_ = event.value(QStringLiteral("can_undo")).toBool();
                TuningCanRedo_ = event.value(QStringLiteral("can_redo")).toBool();
                PlayState_.TuningDocumentAvailable = TuningDocumentAvailable_;
                PlayState_.TuningDocumentDirty = TuningDocumentDirty_;
                PlayState_.TuningCanUndo = TuningCanUndo_;
                PlayState_.TuningCanRedo = TuningCanRedo_;
            }
            if (TuningDocumentCloseAfterRequest_ && status == QStringLiteral("Ok") && TuningDocumentDirty_ == false &&
                PlayState_.Phase == PlayPhase::Stopped)
            {
                Play_.Close();
            }
            TuningDocumentRequest_.clear();
            TuningDocumentCloseAfterRequest_ = false;
        }
        if (request == EditRequest_)
        {
            if (status == QStringLiteral("Ok") && EditReply_.isEmpty() == false)
            {
                PendingEdit_.Expected = EditReply_;
                if (EditIntent_ == EditIntent::Undo)
                {
                    SessionUndo_.removeLast();
                    SessionRedo_.append(PendingEdit_);
                }
                else if (EditIntent_ == EditIntent::Redo)
                {
                    SessionRedo_.removeLast();
                    SessionUndo_.append(PendingEdit_);
                }
                else
                {
                    SessionUndo_.append(PendingEdit_);
                    if (SessionUndo_.size() > 128)
                    {
                        SessionUndo_.removeFirst();
                    }
                    SessionRedo_.clear();
                }
            }
            EditRequest_.clear();
            RefreshProperties();
        }
        PlayState_.Message = status + QStringLiteral(": ") + event.value(QStringLiteral("message")).toString();
        const auto host = event.value(QStringLiteral("host")).toObject();
        if (ScriptRequest_.isEmpty() == false && request == ScriptRequest_)
        {
            ScriptRequest_.clear();
            PlayState_.ScriptBusy = false;
            PlayState_.ScriptMessage =
                status == QStringLiteral("Ok")
                    ? QString()
                    : status + QStringLiteral(": ") + event.value(QStringLiteral("message")).toString();
            if (status == QStringLiteral("Ok"))
            {
                QJsonParseError error{};
                const auto reply =
                    QJsonDocument::fromJson(host.value(QStringLiteral("message")).toString().toUtf8(), &error);
                if (error.error == QJsonParseError::NoError && reply.isObject() && DebugReply(reply.object()))
                {
                    PlayState_.ScriptStatus = reply.object();
                    PlayState_.ScriptPaused = reply.object().value(QStringLiteral("partial")).toBool();
                }
                else
                {
                    PlayState_.ScriptStatus = {};
                    PlayState_.ScriptMessage = QStringLiteral("Invalid debugger reply; inspect again");
                }
            }
        }
        if (request == TimingRequest_)
        {
            TimingRequest_.clear();
        }
        if (host.contains(QStringLiteral("presented_frames")))
        {
            PlayState_.HostStatus = host;
            // Runtime-computed intervals only; no editor clock reading is involved.
            QString timingError;
            PlayState_.HasTiming = ReadTimingSnapshot(host, PlayState_.Timing, timingError);
            PlayState_.TimingMessage = PlayState_.HasTiming == true ? QString() : timingError;
            if (host.contains(QStringLiteral("script_paused")))
            {
                PlayState_.ScriptPaused = host.value(QStringLiteral("script_paused")).toBool();
            }
        }
        if (PlayState_.Phase != PlayPhase::Stopping && PlayState_.Phase != PlayPhase::CleanupUnknown &&
            host.isEmpty() == false)
        {
            const auto state = host.value(QStringLiteral("state")).toString();
            if (state == QStringLiteral("Running"))
            {
                PlayState_.Phase = PlayPhase::Running;
            }
            if (state == QStringLiteral("Paused"))
            {
                PlayState_.Phase = PlayPhase::Paused;
            }
            if (state == QStringLiteral("CleanupUnknown"))
            {
                PlayState_.Phase = PlayPhase::CleanupUnknown;
            }
            const auto generation = host.value(QStringLiteral("generation")).toString();
            if (generation.isEmpty() == false && generation != PlayState_.Generation)
            {
                PlayState_.Generation = generation;
                PlayState_.ScriptStatus = {};
                PlayState_.ScriptPaused = false;
                PlayState_.ScriptBusy = false;
                PlayState_.ScriptMessage.clear();
                ScriptRequest_.clear();
                PlayState_.Properties = {};
                PropertyRequest_.clear();
                RefreshProperties();
            }
        }
        if (request == PropertyRequest_)
        {
            if (status == QStringLiteral("Ok") && static_cast<uint64>(PropertyChunks_.size()) == PropertyCount_)
            {
                PlayState_.Properties = PropertyChunks_;
            }
            PropertyRequest_.clear();
            PropertyChunks_ = {};
        }
        if (PlayState_.Phase == PlayPhase::Starting && status != QStringLiteral("Ok"))
        {
            Play_.Close();
        }
    }
    else if (type == QStringLiteral("document"))
    {
        TuningDocumentAvailable_ = event.value(QStringLiteral("available")).toBool();
        TuningDocumentDirty_ = event.value(QStringLiteral("dirty")).toBool();
        TuningDocumentDigest_ = event.value(QStringLiteral("digest")).toString();
        TuningCanUndo_ = event.value(QStringLiteral("can_undo")).toBool();
        TuningCanRedo_ = event.value(QStringLiteral("can_redo")).toBool();
        PlayState_.TuningDocumentAvailable = TuningDocumentAvailable_;
        PlayState_.TuningDocumentDirty = TuningDocumentDirty_;
        PlayState_.TuningCanUndo = TuningCanUndo_;
        PlayState_.TuningCanRedo = TuningCanRedo_;
    }
    else if (type == QStringLiteral("host"))
    {
        const auto host = event.value(QStringLiteral("event")).toObject();
        const auto kind = host.value(QStringLiteral("event")).toString();
        if (kind == QStringLiteral("SessionReady"))
        {
            PlayState_.Session = host.value(QStringLiteral("session")).toString();
        }
        if (kind == QStringLiteral("ModuleReady") && PlayState_.Phase == PlayPhase::Starting)
        {
            PlayState_.Generation = host.value(QStringLiteral("generation")).toString();
            PlayState_.Phase = PlayPhase::Running;
            RefreshProperties();
        }
        if (kind == QStringLiteral("PropertiesChanged") &&
            host.value(QStringLiteral("request")).toString() == PropertyRequest_)
        {
            const auto rows = host.value(QStringLiteral("properties")).toArray();
            const auto offset = host.value(QStringLiteral("offset")).toInteger(-1);
            const auto count = host.value(QStringLiteral("count")).toInteger(-1);
            if (offset != PropertyChunks_.size() || count < 0 || count > 4096 || rows.size() > 64 ||
                offset + rows.size() > count ||
                host.value(QStringLiteral("generation")).toString() != PlayState_.Generation)
            {
                PlayState_.Message = QStringLiteral("Invalid property transfer; refresh explicitly");
                PropertyRequest_.clear();
                PropertyChunks_ = {};
            }
            else
            {
                if (offset == 0)
                {
                    PropertyCount_ = static_cast<uint64>(count);
                    PlayState_.SchemaEpoch =
                        static_cast<uint64>(host.value(QStringLiteral("schema_epoch")).toInteger());
                    PlayState_.SchemaVersion =
                        static_cast<uint64>(host.value(QStringLiteral("schema_version")).toInteger());
                }
                for (const auto& row : rows)
                {
                    PropertyChunks_.append(row);
                }
            }
        }
        if (kind == QStringLiteral("PropertiesChanged") &&
            host.value(QStringLiteral("request")).toString() == EditRequest_ &&
            host.value(QStringLiteral("generation")).toString() == PendingEdit_.Generation)
        {
            for (const auto& row : host.value(QStringLiteral("properties")).toArray())
            {
                const auto value = row.toObject();
                if (value.value(QStringLiteral("object")) == PendingEdit_.Before.value(QStringLiteral("object")) &&
                    value.value(QStringLiteral("property")) == PendingEdit_.Before.value(QStringLiteral("property")))
                {
                    EditReply_ = value;
                }
            }
        }
    }
    Publish();
}

bool EditorController::CanEditProperties() const
{
    return PlayState_.DebuggerStopped == false && EditRequest_.isEmpty() && PropertyRequest_.isEmpty() &&
           PlayState_.Properties.isEmpty() == false &&
           (PlayState_.Phase == PlayPhase::Running || PlayState_.Phase == PlayPhase::Paused);
}

void EditorController::EditProperty(int row, const QString& text)
{
    if (CanEditProperties() == false || row < 0 || row >= PlayState_.Properties.size())
    {
        return;
    }
    const auto property = PlayState_.Properties.at(row).toObject();
    if (property.value(QStringLiteral("writable")).toBool() == false)
    {
        return;
    }
    QJsonObject edit{{QStringLiteral("object"), property.value(QStringLiteral("object"))},
                     {QStringLiteral("property"), property.value(QStringLiteral("property"))},
                     {QStringLiteral("revision"), property.value(QStringLiteral("revision"))},
                     {QStringLiteral("kind"), property.value(QStringLiteral("kind"))}};
    const int kind = property.value(QStringLiteral("kind")).toInt(-1);
    bool valid = false;
    if (kind == 0)
    {
        valid = text == QStringLiteral("true") || text == QStringLiteral("false");
        edit.insert(QStringLiteral("value"), text == QStringLiteral("true"));
    }
    else if (kind == 1 || kind == 3)
    {
        const foundation::int32 value = text.toInt(&valid);
        valid = valid && (kind == 1 || value >= 0);
        edit.insert(QStringLiteral("value"), value);
    }
    else if (kind == 2)
    {
        const foundation::float32 value = text.toFloat(&valid);
        foundation::uint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        valid = valid && (bits & 0x7F800000U) != 0x7F800000U;
        edit.insert(QStringLiteral("bits"), static_cast<qint64>(bits));
    }
    else if (kind == 4)
    {
        valid = text.toUtf8().size() <= MAX_STRING_PROPERTY_BYTES && text.contains(QChar(u'\0')) == false;
        edit.insert(QStringLiteral("value"), text);
    }
    if (valid == false)
    {
        PlayState_.Message = QStringLiteral("Invalid typed value");
        Publish();
        return;
    }
    EditIntent_ = EditIntent::New;
    PendingEdit_ = {property, edit, {}, PlayState_.Generation, PlayState_.SchemaEpoch, PlayState_.SchemaVersion};
    SubmitEdit(edit);
}

void EditorController::SubmitEdit(const QJsonObject& edit)
{
    EditReply_ = {};
    EditRequest_ = NextPlayRequest();
    const QJsonObject command{{QStringLiteral("command"), QStringLiteral("ApplyEdits")},
                              {QStringLiteral("expected_generation"), PlayState_.Generation},
                              {QStringLiteral("schema_epoch"), static_cast<qint64>(PlayState_.SchemaEpoch)},
                              {QStringLiteral("schema_version"), static_cast<qint64>(PlayState_.SchemaVersion)},
                              {QStringLiteral("edits"), QJsonArray{edit}}};
    if (Play_.Send({{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("request"), EditRequest_},
                    {QStringLiteral("command"), command}}) == false)
    {
        EditRequest_.clear();
    }
    Publish();
}

bool EditorController::CanUndoSession() const
{
    return CanEditProperties() && SessionUndo_.isEmpty() == false;
}
bool EditorController::CanRedoSession() const
{
    return CanEditProperties() && SessionRedo_.isEmpty() == false;
}

bool EditorController::CanApplyToTuningDocument(int row) const
{
    if (TuningDocumentAvailable_ == false || TuningDocumentRequest_.isEmpty() == false ||
        CanEditProperties() == false || row < 0 || row >= PlayState_.Properties.size())
    {
        return false;
    }
    const auto property = PlayState_.Properties.at(row).toObject();
    return property.value(QStringLiteral("writable")).toBool() && property.value(QStringLiteral("scope")).toInt() == 1;
}

bool EditorController::CanSaveTuningDocument() const noexcept
{
    return TuningDocumentAvailable_ && TuningDocumentDirty_ && TuningDocumentRequest_.isEmpty() &&
           PlayState_.Phase != PlayPhase::CleanupUnknown && Play_.Active();
}

void EditorController::ApplyLivePropertyToTuningDocument(int row)
{
    if (CanApplyToTuningDocument(row) == false)
    {
        return;
    }
    const auto property = PlayState_.Properties.at(row).toObject();
    const int kind = property.value(QStringLiteral("kind")).toInt(-1);
    const QString kindName = kind == 0   ? QStringLiteral("bool")
                             : kind == 1 ? QStringLiteral("int32")
                             : kind == 2 ? QStringLiteral("float32")
                             : kind == 3 ? QStringLiteral("enum")
                             : kind == 4 ? QStringLiteral("string")
                                         : QString();
    if (kindName.isEmpty())
    {
        PlayState_.Message = QStringLiteral("Unsupported tuning value kind");
        Publish();
        return;
    }
    QJsonValue value = property.value(QStringLiteral("value"));
    if (kind == 2)
    {
        const auto bits = static_cast<foundation::uint32>(property.value(QStringLiteral("bits")).toInteger());
        foundation::float32 scalar = 0.0F;
        std::memcpy(&scalar, &bits, sizeof(bits));
        value = static_cast<foundation::float64>(scalar);
    }
    const QJsonObject edit{{QStringLiteral("object"), property.value(QStringLiteral("object"))},
                           {QStringLiteral("property"), property.value(QStringLiteral("property"))},
                           {QStringLiteral("kind"), kindName},
                           {QStringLiteral("value"), value}};
    SendTuningDocumentCommand({{QStringLiteral("command"), QStringLiteral("ApplyToDocument")},
                               {QStringLiteral("expected_digest"), TuningDocumentDigest_},
                               {QStringLiteral("edits"), QJsonArray{edit}}});
}

void EditorController::SendTuningDocumentCommand(QJsonObject command, bool closeWhenClean)
{
    if (TuningDocumentAvailable_ == false || TuningDocumentRequest_.isEmpty() == false || Play_.Active() == false)
    {
        return;
    }
    TuningDocumentRequest_ = NextPlayRequest();
    TuningDocumentCloseAfterRequest_ = closeWhenClean;
    if (Play_.Send({{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("request"), TuningDocumentRequest_},
                    {QStringLiteral("command"), command}}) == false)
    {
        TuningDocumentRequest_.clear();
        TuningDocumentCloseAfterRequest_ = false;
        PlayState_.Message = QStringLiteral("Tuning document command could not be queued");
    }
    Publish();
}

void EditorController::SaveTuningDocument()
{
    if (CanSaveTuningDocument())
    {
        SendTuningDocumentCommand({{QStringLiteral("command"), QStringLiteral("SaveDocument")}}, true);
    }
}

void EditorController::DiscardTuningDocumentDraft()
{
    if (TuningDocumentAvailable_ && TuningDocumentDirty_ && TuningDocumentRequest_.isEmpty() && Play_.Active())
    {
        SendTuningDocumentCommand({{QStringLiteral("command"), QStringLiteral("DiscardDocument")}}, true);
    }
}

void EditorController::UndoTuningDocument()
{
    if (TuningDocumentAvailable_ && TuningCanUndo_ && TuningDocumentRequest_.isEmpty() && Play_.Active())
    {
        SendTuningDocumentCommand({{QStringLiteral("command"), QStringLiteral("UndoDocument")}});
    }
}

void EditorController::RedoTuningDocument()
{
    if (TuningDocumentAvailable_ && TuningCanRedo_ && TuningDocumentRequest_.isEmpty() && Play_.Active())
    {
        SendTuningDocumentCommand({{QStringLiteral("command"), QStringLiteral("RedoDocument")}});
    }
}
void EditorController::UndoSessionEdit()
{
    ReplaySessionEdit(true);
}
void EditorController::RedoSessionEdit()
{
    ReplaySessionEdit(false);
}
void EditorController::ReplaySessionEdit(bool undo)
{
    if ((undo ? CanUndoSession() : CanRedoSession()) == false)
    {
        return;
    }
    PendingEdit_ = undo ? SessionUndo_.last() : SessionRedo_.last();
    if (PendingEdit_.Generation != PlayState_.Generation || PendingEdit_.SchemaEpoch != PlayState_.SchemaEpoch)
    {
        PlayState_.Message =
            QStringLiteral("Undo/redo belongs to an earlier code/schema generation; refresh and edit explicitly");
        Publish();
        return;
    }
    EditIntent_ = undo ? EditIntent::Undo : EditIntent::Redo;
    const auto target = undo ? PendingEdit_.Before : PendingEdit_.After;
    QJsonObject edit{{QStringLiteral("object"), target.value(QStringLiteral("object"))},
                     {QStringLiteral("property"), target.value(QStringLiteral("property"))},
                     {QStringLiteral("kind"), target.value(QStringLiteral("kind"))},
                     {QStringLiteral("revision"), PendingEdit_.Expected.value(QStringLiteral("revision"))}};
    const auto field =
        target.value(QStringLiteral("kind")).toInt() == 2 ? QStringLiteral("bits") : QStringLiteral("value");
    edit.insert(field, target.value(field));
    SubmitEdit(edit);
}
} // namespace ludus::editor
