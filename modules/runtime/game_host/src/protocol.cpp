#include <ludus/runtime/game_host/protocol.h>

namespace ludus::runtime::game_host::protocol
{
std::string_view CommandKindName(CommandKind kind) noexcept
{
    switch (kind)
    {
        case CommandKind::Hello:
            return "Hello";
        case CommandKind::Status:
            return "Status";
        case CommandKind::Load:
            return "Load";
        case CommandKind::Pause:
            return "Pause";
        case CommandKind::Resume:
            return "Resume";
        case CommandKind::Step:
            return "Step";
        case CommandKind::Reload:
            return "Reload";
        case CommandKind::ReadProperties:
            return "ReadProperties";
        case CommandKind::ApplyEdits:
            return "ApplyEdits";
        case CommandKind::ReloadAsset:
            return "ReloadAsset";
        case CommandKind::Stop:
            return "Stop";
        case CommandKind::ScriptDebug:
            return "ScriptDebug";
        case CommandKind::SetFastForward:
            return "SetFastForward";
    }
    return "Unknown";
}

std::string_view EventKindName(EventKind kind) noexcept
{
    switch (kind)
    {
        case EventKind::SessionReady:
            return "SessionReady";
        case EventKind::ModuleReady:
            return "ModuleReady";
        case EventKind::CommandResult:
            return "CommandResult";
        case EventKind::PropertiesChanged:
            return "PropertiesChanged";
        case EventKind::ReloadPhase:
            return "ReloadPhase";
        case EventKind::FramePresented:
            return "FramePresented";
        case EventKind::SessionEnded:
            return "SessionEnded";
    }
    return "Unknown";
}

std::string_view CommandStatusName(CommandStatus status) noexcept
{
    switch (status)
    {
        case CommandStatus::Ok:
            return "Ok";
        case CommandStatus::InvalidRequest:
            return "InvalidRequest";
        case CommandStatus::Busy:
            return "Busy";
        case CommandStatus::IncompatibleModule:
            return "IncompatibleModule";
        case CommandStatus::Superseded:
            return "Superseded";
        case CommandStatus::ReloadRejected:
            return "ReloadRejected";
        case CommandStatus::RestartRequired:
            return "RestartRequired";
        case CommandStatus::StaleRevision:
            return "StaleRevision";
        case CommandStatus::SchemaChanged:
            return "SchemaChanged";
        case CommandStatus::ProtocolError:
            return "ProtocolError";
        case CommandStatus::HostFailed:
            return "HostFailed";
        case CommandStatus::CleanupUnknown:
            return "CleanupUnknown";
    }
    return "Unknown";
}

std::string_view ReloadPhaseName(ReloadPhase phase) noexcept
{
    switch (phase)
    {
        case ReloadPhase::Validate:
            return "Validate";
        case ReloadPhase::Quiesce:
            return "Quiesce";
        case ReloadPhase::Snapshot:
            return "Snapshot";
        case ReloadPhase::Stage:
            return "Stage";
        case ReloadPhase::Commit:
            return "Commit";
        case ReloadPhase::Retire:
            return "Retire";
        case ReloadPhase::Rejected:
            return "Rejected";
        case ReloadPhase::Done:
            return "Done";
    }
    return "Unknown";
}
} // namespace ludus::runtime::game_host::protocol
