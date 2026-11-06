#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/tooling/shader_language/diagnostic.hpp>
#include <ludus/tooling/shader_language/preprocessor.hpp>
#include <ludus/tooling/shader_language/semantic.hpp>
#include <ludus/tooling/shader_language/status.hpp>
#include <ludus/tooling/shader_language/syntax.hpp>

#include <string_view>

/// @file
/// Lowering a checked LUSL translation unit to one Slang module: the fourth front-end stage
/// (docs/modules/tooling/shader-language.md, "Lowering to Slang").
namespace ludus::tooling::shader_language
{
/// @brief Suffix appended to a LUSL root's path to name its generated Slang module.
///
/// `main_shaders/shade.lusl` lowers to `main_shaders/shade.lusl.slang`, beside the source, so the
/// generated `#include` paths of `extern slang` files resolve from that directory.
inline constexpr std::string_view GENERATED_SLANG_SUFFIX = ".slang";

/// @brief Name of the private per-invocation `uint` that development bounds checks latch.
///
/// Zero means no check failed. After a clamped access it holds that access's
/// BoundsCheckSite::FailureCode; later failures keep the first code. Code that includes the
/// generated module, such as a render-graph wrapper, reads it to report the failure.
inline constexpr std::string_view FAILURE_CODE_VARIABLE = "_luslFailureCode";

/// Options of EmitSlang.
struct EmitOptions final
{
    /// @brief Development builds: check runtime indices into fixed-size arrays, vectors and matrices.
    ///
    /// An index outside its object is clamped to the last element and latches the access's failure
    /// code in FAILURE_CODE_VARIABLE. Constant indices are checked by Check in every build. Release
    /// builds leave this false and emit no checks.
    bool ShouldCheckBounds = false;
};

/// @brief One line of the generated Slang that starts a LUSL construct.
///
/// The generated text also carries `#line` directives, so Slang's own diagnostics already name
/// LUSL lines; this map is for tools that need columns or the generated position.
struct SourceMapEntry final
{
    /// One-based line in the generated text.
    foundation::uint32 GeneratedLine = 0;
    /// The LUSL position: where the construct is spelled, or the outermost macro use for a
    /// construct produced by a macro.
    SourceLocation Location;
};

/// @brief A runtime index that a development build checks.
struct BoundsCheckSite final
{
    /// The value FAILURE_CODE_VARIABLE holds after this access was clamped; codes start at one.
    foundation::uint32 FailureCode = 0;
    /// The index expression, mapped like SourceMapEntry::Location.
    SourceLocation Location;
};

/// @brief Output of EmitSlang.
///
/// On Ready, `Text` is one complete Slang module. On any other outcome `Text`, `SourceMap` and
/// `BoundsChecks` are empty and the diagnostics say why.
struct EmitResult final
{
    /// Final outcome.
    Status Outcome = Status::InvalidRequest;
    /// The generated Slang module, UTF-8 without a terminator; at most
    /// `shader_compiler::MAX_SOURCE_BYTES` bytes, so it fits a compiler snapshot.
    foundation::Array<char> Text;
    /// Mapped lines in increasing GeneratedLine order.
    foundation::Array<SourceMapEntry> SourceMap;
    /// Checked accesses in failure-code order; empty unless EmitOptions::ShouldCheckBounds.
    foundation::Array<BoundsCheckSite> BoundsChecks;
    /// Reported problems in the order found; at most MAX_DIAGNOSTICS.
    foundation::Array<Diagnostic> Diagnostics;
    /// Number of diagnostics found after `Diagnostics` reached MAX_DIAGNOSTICS.
    foundation::usize DroppedDiagnosticCount = 0;
};

/// @brief Lower a checked translation unit to one Slang module.
///
/// The module keeps LUSL's semantics on every target that Slang compiles it for:
/// - each value has an explicit Slang type; untyped constants take the type the checker gave them;
/// - the literal `0` and omitted designated-initializer fields become explicit zero values;
/// - named arguments and defaults become positional arguments, evaluated in written order;
/// - `M * v` and `A * B` on matrices become `mul`;
/// - signed `+`, `-`, `*`, negation, compound assignments and `++`/`--` wrap on every target,
///   through unsigned arithmetic on Metal;
/// - shift counts are masked to the operand's bit width;
/// - `groupshared` variables declared `= 0` are cleared by every compute entry point that reaches
///   them, followed by a group barrier;
/// - `[[fallthrough]]` copies the next case's statements, so no target needs fallthrough;
/// - `extern slang` blocks become `#include` directives relative to the generated module.
///
/// Public declarations keep their LUSL names. Generated helpers start with `_lusl`, which Check
/// reserves, and are named in first-use order, so equal inputs give byte-identical text.
///
/// Deterministic and reentrant; keeps no state between calls.
/// @param request The request given to Preprocess; its file texts must still be alive.
/// @param preprocessed The Ready Preprocess result the tree was parsed from.
/// @param tree The Ready Parse result the model was checked from.
/// @param model A Ready result of Check for `tree`.
/// @param options Development bounds checks.
/// @param outResult Receives the module; any previous contents are replaced.
/// @return The same value as `outResult.Outcome`; InvalidRequest when an input is not Ready.
[[nodiscard]] Status EmitSlang(const PreprocessRequest& request,
                               const PreprocessResult& preprocessed,
                               const SyntaxTree& tree,
                               const SemanticModel& model,
                               const EmitOptions& options,
                               EmitResult& outResult) noexcept;
} // namespace ludus::tooling::shader_language
