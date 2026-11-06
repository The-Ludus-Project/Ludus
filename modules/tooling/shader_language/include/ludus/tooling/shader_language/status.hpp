#pragma once

#include <ludus/foundation/base/types.h>

/// @file
/// The outcome shared by every LUSL front-end stage.
namespace ludus::tooling::shader_language
{
/// Outcome of a front-end stage.
enum class Status : foundation::uint8
{
    /// The stage succeeded; its output is complete. Warnings may be present.
    Ready,
    /// The source has errors; the diagnostics say which. The stage publishes no output.
    Failed,
    /// The request itself is malformed, or depends on a stage that did not succeed; a
    /// DiagnosticCode::InvalidRequest diagnostic says why.
    InvalidRequest,
    /// Allocation failed. Diagnostics may be incomplete.
    OutOfMemory,
};
} // namespace ludus::tooling::shader_language
