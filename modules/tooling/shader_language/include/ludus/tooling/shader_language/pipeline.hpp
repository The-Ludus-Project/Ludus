#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/tooling/shader_language/diagnostic.hpp>
#include <ludus/tooling/shader_language/emitter.hpp>
#include <ludus/tooling/shader_language/preprocessor.hpp>
#include <ludus/tooling/shader_language/semantic.hpp>
#include <ludus/tooling/shader_language/status.hpp>

/// @file
/// The whole LUSL front end in one call, from a source snapshot to a Slang module
/// (docs/modules/tooling/shader-language.md, "Lowering to Slang").
namespace ludus::tooling::shader_language
{
/// Options of LowerToSlang.
struct LoweringOptions final
{
    /// Style setting of the checker.
    CheckOptions Check;
    /// Development bounds checks of the emitter.
    EmitOptions Emit;
};

/// @brief Output of LowerToSlang.
///
/// On Ready, `Text` is the generated Slang module. On any other outcome `Text`, `SourceMap` and
/// `BoundsChecks` are empty; the diagnostics and read masks still describe what was found.
struct LoweringResult final
{
    /// Outcome of the first stage that did not succeed, or Ready.
    Status Outcome = Status::InvalidRequest;
    /// The generated Slang module; see EmitResult::Text.
    foundation::Array<char> Text;
    /// See EmitResult::SourceMap.
    foundation::Array<SourceMapEntry> SourceMap;
    /// See EmitResult::BoundsChecks.
    foundation::Array<BoundsCheckSite> BoundsChecks;
    /// See EmitResult::Specializations.
    foundation::Array<SpecializationRecord> Specializations;
    /// See EmitResult::GroupsharedClears.
    foundation::Array<GroupsharedClearRecord> GroupsharedClears;
    /// See EmitResult::Bindings.
    foundation::Array<BindingRecord> Bindings;
    /// See EmitResult::VertexInputs.
    foundation::Array<VertexInputRecord> VertexInputs;
    /// The preprocessor's file read mask; see PreprocessResult::SourceReadMask.
    foundation::Array<foundation::uint8> SourceReadMask;
    /// The preprocessor's configuration read mask; see PreprocessResult::ConfigurationReadMask.
    foundation::Array<foundation::uint8> ConfigurationReadMask;
    /// Diagnostics of every stage that ran, in stage order; at most MAX_DIAGNOSTICS.
    foundation::Array<Diagnostic> Diagnostics;
    /// Number of diagnostics found after `Diagnostics` reached MAX_DIAGNOSTICS, by any stage.
    foundation::usize DroppedDiagnosticCount = 0;
};

/// @brief Preprocess, parse, check and lower one LUSL root to Slang.
///
/// Runs Preprocess, Parse, Check and EmitSlang in order and stops after the first stage that does
/// not succeed. Warnings do not stop it. No file access occurs: every file, including `extern
/// slang` files, comes from the request's snapshot.
///
/// Deterministic and reentrant; keeps no state between calls.
/// @param request Snapshot, root file and configuration values; borrowed for the call.
/// @param options Style setting and development bounds checks.
/// @param outResult Receives the result; any previous contents are replaced. It owns its text and
/// does not borrow the request.
/// @return The same value as `outResult.Outcome`.
[[nodiscard]] Status
LowerToSlang(const PreprocessRequest& request, const LoweringOptions& options, LoweringResult& outResult) noexcept;
} // namespace ludus::tooling::shader_language
