#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// Prefix of every name the Slang emitter generates; LUSL declarations cannot use it.
inline constexpr std::string_view GENERATED_NAME_PREFIX = "_lusl";
/// Prefix Slang reserves for its own declarations.
inline constexpr std::string_view SLANG_RESERVED_PREFIX = "__";
/// Number of entries in RESERVED_SLANG_NAMES.
inline constexpr foundation::usize RESERVED_SLANG_NAME_COUNT = 24;

/// @brief Identifiers LUSL accepts but the generated Slang cannot declare.
///
/// Found by compiling each candidate with the pinned slangc as a struct, field, parameter, local
/// and function name; the statement-starting words (`var`, `import`, ...) are added because Slang
/// parses a statement that begins with them differently. LUSL keywords are not repeated here.
inline constexpr foundation::StaticArray<std::string_view, RESERVED_SLANG_NAME_COUNT> RESERVED_SLANG_NAMES = {{
    "This",   "catch",     "defer",     "func",    "implementing", "import",    "int",    "let",
    "module", "namespace", "new",       "no_diff", "operator",     "spirv_asm", "this",   "throw",
    "throws", "try",       "typealias", "using",   "var",          "where",     "export", "associatedtype",
}};

/// True when `name` cannot be declared in the generated Slang.
[[nodiscard]] constexpr bool IsReservedSlangName(std::string_view name) noexcept
{
    if (name.starts_with(GENERATED_NAME_PREFIX) == true || name.starts_with(SLANG_RESERVED_PREFIX) == true)
    {
        return true;
    }
    for (const std::string_view reserved : RESERVED_SLANG_NAMES)
    {
        if (reserved == name)
        {
            return true;
        }
    }
    return false;
}
} // namespace ludus::tooling::shader_language::internal
