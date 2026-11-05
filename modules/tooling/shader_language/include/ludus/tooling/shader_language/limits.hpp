#pragma once

#include <ludus/foundation/base/types.h>

/// @file
/// Admission limits owned by the LUSL front end (docs/architecture/shading-language.md).
///
/// The number of files and total source bytes in one snapshot are owned by the shader compiler
/// (`shader_compiler::MAX_SOURCE_FILES` and `shader_compiler::MAX_SOURCE_BYTES`), because LUSL
/// sources travel in the same snapshot as the Slang sources they lower to. Exceeding a limit below
/// produces a diagnostic and a failed result; it never truncates output.
namespace ludus::tooling::shader_language
{
/// Most tokens lexed from one file or configuration value.
inline constexpr foundation::usize MAX_FILE_TOKENS = foundation::usize{1} << 20;
/// Most tokens in one preprocessed translation unit.
inline constexpr foundation::usize MAX_OUTPUT_TOKENS = foundation::usize{1} << 20;
/// Deepest `#include` nesting; the root file is depth one.
inline constexpr foundation::usize MAX_INCLUDE_DEPTH = 32;
/// Deepest nesting of `#if` groups in one translation unit.
inline constexpr foundation::usize MAX_CONDITIONAL_DEPTH = 64;
/// Deepest parenthesis and operator nesting inside one `#if` or `#elif` expression.
inline constexpr foundation::usize MAX_CONDITION_NESTING = 64;
/// Most distinct macro names, defined or undefined, including configuration values.
inline constexpr foundation::usize MAX_MACROS = 4096;
/// Most parameters of one function-like macro.
inline constexpr foundation::usize MAX_MACRO_PARAMETERS = 32;
/// Deepest nesting of macro-argument expansion, such as `F(G(H(x)))`.
inline constexpr foundation::usize MAX_MACRO_EXPANSION_DEPTH = 32;
/// Most macro expansions performed while preprocessing one translation unit.
inline constexpr foundation::usize MAX_MACRO_EXPANSIONS = foundation::usize{1} << 18;
/// Most bytes of token spellings created by `#`, `##` and configuration values.
inline constexpr foundation::usize MAX_SYNTHESIZED_BYTES = foundation::usize{1} << 20;
/// Most configuration values in one request.
inline constexpr foundation::usize MAX_CONFIGURATION_VALUES = 256;
/// Longest configuration value name or value text, in bytes.
inline constexpr foundation::usize MAX_CONFIGURATION_TEXT_BYTES = 1024;
/// Most diagnostics retained for one request; later ones are counted as dropped.
inline constexpr foundation::usize MAX_DIAGNOSTICS = 256;
/// Longest retained diagnostic message, in bytes; longer messages are clipped.
inline constexpr foundation::usize MAX_DIAGNOSTIC_MESSAGE_BYTES = 256;
} // namespace ludus::tooling::shader_language
