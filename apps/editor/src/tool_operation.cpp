#include "internal/tool_process.h"

namespace ludus::editor
{
const char* ToolOperationName(ToolOperation op) noexcept
{
    switch (op)
    {
        case ToolOperation::Configure:
            return "configure";
        case ToolOperation::Build:
            return "build";
        case ToolOperation::BuildDebug:
            return "build_debug";
        case ToolOperation::PlayWorld:
            return "play_world";
        case ToolOperation::BuildRun:
            return "build_run";
        case ToolOperation::ProjectCheck:
            return "project_check";
        case ToolOperation::ProjectSetup:
            return "project_setup";
        case ToolOperation::ProjectCreate:
            return "project_create";
        case ToolOperation::ReleaseInit:
            return "release_init";
        case ToolOperation::Package:
            return "package";
        case ToolOperation::BuildGeneration:
            return "build_generation";
        case ToolOperation::CookScripts:
            return "cook_scripts";
        case ToolOperation::InspectSetup:
            return "inspect_setup";
    }
    return "configure";
}

} // namespace ludus::editor
