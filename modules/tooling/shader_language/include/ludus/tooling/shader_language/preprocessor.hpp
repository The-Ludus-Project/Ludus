#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/tooling/shader_compiler/compiler.hpp>
#include <ludus/tooling/shader_language/diagnostic.hpp>
#include <ludus/tooling/shader_language/token.hpp>

#include <string_view>

/// @file
/// The LUSL preprocessor: the first stage of the front end (docs/architecture/shading-language.md).
namespace ludus::tooling::shader_language
{
/// @brief One selected render-graph configuration value, visible to LUSL as an object-like macro.
///
/// Configuration values are the only macros that come from outside the source. Sources cannot
/// `#define` or `#undef` them.
struct ConfigurationValue final
{
    /// Macro name: an identifier, unique in the request, at most MAX_CONFIGURATION_TEXT_BYTES bytes.
    foundation::BorrowedView<const char> Name;
    /// Replacement text, lexed as LUSL tokens; may be empty. At most MAX_CONFIGURATION_TEXT_BYTES bytes.
    foundation::BorrowedView<const char> Value;
};

/// @brief An immutable source snapshot and the root file to preprocess.
///
/// Every view is borrowed for the duration of Preprocess. The file texts must also outlive the
/// result's tokens, whose spellings refer to them. No filesystem access occurs.
struct PreprocessRequest final
{
    /// The whole snapshot, which may also contain Slang files. Paths follow the shader compiler's
    /// rules: unique, relative, `/`-separated, without empty, `.` or `..` components. At most
    /// `shader_compiler::MAX_SOURCE_FILES` files and `shader_compiler::MAX_SOURCE_BYTES` bytes.
    foundation::BorrowedView<const shader_compiler::SourceFile> Files;
    /// Path of the root file in Files; it must end in `.lusl`.
    foundation::BorrowedView<const char> Source;
    /// Selected configuration values; at most MAX_CONFIGURATION_VALUES.
    foundation::BorrowedView<const ConfigurationValue> Configuration;
};

/// Outcome of a front-end stage.
enum class Status : foundation::uint8
{
    /// The stage succeeded; its output is complete. Warnings may be present.
    Ready,
    /// The source has errors; the diagnostics say which. No output tokens are published.
    Failed,
    /// The request itself is malformed; a DiagnosticCode::InvalidRequest diagnostic says why.
    InvalidRequest,
    /// Allocation failed. Diagnostics may be incomplete.
    OutOfMemory,
};

/// @brief Output of Preprocess.
///
/// On Ready, `Tokens` is the complete translation unit after includes, conditionals and macro
/// expansion, with comments removed. On any other outcome `Tokens`, `SynthesizedText` and
/// `Expansions` are empty, while the diagnostics and read masks describe what was found.
struct PreprocessResult final
{
    /// Final outcome.
    Status Outcome = Status::InvalidRequest;
    /// Preprocessed tokens in order.
    foundation::Array<Token> Tokens;
    /// Storage for spellings of tokens with Token::IsSynthesized.
    foundation::Array<char> SynthesizedText;
    /// Location of each outermost macro use, indexed by Token::ExpansionIndex.
    foundation::Array<SourceLocation> Expansions;
    /// One 0/1 byte per request file, set when the preprocessor read that file. Build identity
    /// records these files as dependencies.
    foundation::Array<foundation::uint8> SourceReadMask;
    /// One 0/1 byte per configuration value, set when the source expanded or tested it. Build
    /// identity records these values.
    foundation::Array<foundation::uint8> ConfigurationReadMask;
    /// Reported problems in the order found; at most MAX_DIAGNOSTICS.
    foundation::Array<Diagnostic> Diagnostics;
    /// Number of diagnostics found after `Diagnostics` reached MAX_DIAGNOSTICS.
    foundation::usize DroppedDiagnosticCount = 0;
};

/// @brief Preprocess the root file of a snapshot.
///
/// Supports `#include "path"` of `.luslh` headers, `#pragma once`, object-like and function-like
/// `#define` with `#` and `##`, `#undef`, `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`,
/// `#endif`, `defined` and `#error`. It departs from C where docs/modules/tooling/shader-language.md
/// says so: an undefined identifier in a condition and an unknown name tested by `defined` are
/// errors, unary `!` does not exist, macros are not variadic and `#line` is reserved.
///
/// The function is deterministic: equal requests produce equal results on every host. It is
/// reentrant and keeps no state between calls.
/// @param request Snapshot, root file and configuration values; borrowed for the call.
/// @param outResult Receives the result; any previous contents are replaced.
/// @return The same value as `outResult.Outcome`.
[[nodiscard]] Status Preprocess(const PreprocessRequest& request, PreprocessResult& outResult) noexcept;

/// @brief Return the spelling of a token from a successful Preprocess.
/// @param request The request that produced `result`; its file texts must still be alive.
/// @param result The result that holds `token`.
/// @param token A token of `result`.
/// @return The spelling, or an empty view when the token does not belong to this request and result.
[[nodiscard]] std::string_view
GetTokenSpelling(const PreprocessRequest& request, const PreprocessResult& result, const Token& token) noexcept;
} // namespace ludus::tooling::shader_language
