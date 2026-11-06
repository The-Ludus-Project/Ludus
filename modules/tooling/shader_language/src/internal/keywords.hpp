#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// Number of LUSL keywords, including those reserved for later phases.
inline constexpr foundation::usize KEYWORD_COUNT = 39;
/// Number of keywords that begin later-phase constructs.
inline constexpr foundation::usize LATER_PHASE_KEYWORD_COUNT = 1;

/// Identifiers that cannot be names. `slang` is contextual after `extern` and stays a name.
inline constexpr foundation::StaticArray<std::string_view, KEYWORD_COUNT> KEYWORDS = {{
    "break",   "case",      "class",    "concept",   "const",   "continue", "default", "discard", "do",
    "dynamic", "else",      "enum",     "extension", "extern",  "false",    "for",     "goto",    "groupshared",
    "if",      "in",        "inout",    "interface", "out",     "requires", "return",  "static",  "static_assert",
    "struct",  "switch",    "template", "true",      "typedef", "typename", "union",   "void",    "while",
    "sizeof",  "constexpr", "uniform",
}};

/// Keywords whose constructs arrive in later phases: dynamic dispatch (L4).
inline constexpr foundation::StaticArray<std::string_view, LATER_PHASE_KEYWORD_COUNT> LATER_PHASE_KEYWORDS = {{
    "dynamic",
}};

/// True when `spelling` is a LUSL keyword.
[[nodiscard]] constexpr bool IsKeyword(std::string_view spelling) noexcept
{
    for (const std::string_view keyword : KEYWORDS)
    {
        if (keyword == spelling)
        {
            return true;
        }
    }
    return false;
}
} // namespace ludus::tooling::shader_language::internal
