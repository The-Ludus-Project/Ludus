#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>

#include <string_view>

// Windows worker endpoint vocabulary shared by the client and ludus-shader-worker.
namespace ludus::tooling::shader_compiler::internal
{
/// Local pipe namespace prefix every Windows worker endpoint uses; remote names are refused.
inline constexpr std::string_view PIPE_PREFIX = "\\\\.\\pipe\\";
/// Pipe names are limited to 256 UTF-16 code units, including the prefix.
inline constexpr foundation::usize MAX_PIPE_NAME_CHARACTERS = 256;
/// NUL-terminated UTF-16 pipe name.
using PipeName = foundation::StaticArray<wchar_t, MAX_PIPE_NAME_CHARACTERS + 1>;
/// Convert a UTF-8 endpoint `\\.\pipe\<name>` to a NUL-terminated UTF-16 name. The name must
/// be non-empty and free of NUL and backslashes. Returns false and leaves outName unspecified.
[[nodiscard]] bool TryConvertPipeName(std::string_view endpoint, PipeName& outName) noexcept;
} // namespace ludus::tooling::shader_compiler::internal
