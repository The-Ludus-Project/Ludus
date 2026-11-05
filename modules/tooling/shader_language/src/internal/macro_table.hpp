#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/strings/string_table.hpp>
#include <ludus/tooling/shader_language/token.hpp>

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// MacroTable::Find result for a name that was never defined, undefined or configured.
inline constexpr foundation::uint32 NO_MACRO = 0xffffffffU;
/// BodyToken::ParameterIndex of a body token that is not a parameter.
inline constexpr foundation::uint32 NO_PARAMETER = 0xffffffffU;
/// MacroDefinition::ConfigurationIndex of a macro defined by the source.
inline constexpr foundation::uint32 NO_CONFIGURATION = 0xffffffffU;

/// One replacement-list token of a macro, with its parameter resolved at definition time.
struct BodyToken final
{
    /// The token as spelled in the definition.
    Token Value;
    /// Index of the parameter this token names, or NO_PARAMETER.
    foundation::uint32 ParameterIndex = NO_PARAMETER;
};

/// A macro name and its current definition.
struct MacroDefinition final
{
    /// Name token of the latest `#define`, or of the configuration value.
    Token Name;
    /// False after `#undef`; the name stays known to `defined`.
    bool IsDefined = false;
    /// True for a `NAME(parameters)` definition.
    bool IsFunctionLike = false;
    /// Index of the configuration value this macro represents, or NO_CONFIGURATION.
    foundation::uint32 ConfigurationIndex = NO_CONFIGURATION;
    /// Number of parameters of a function-like macro.
    foundation::uint32 ParameterCount = 0;
    /// First replacement token in MacroTable::GetBody storage.
    foundation::uint32 FirstBodyToken = 0;
    /// Number of replacement tokens.
    foundation::uint32 BodyTokenCount = 0;
};

/// Result of a MacroTable operation that may allocate.
enum class MacroTableOutcome : foundation::uint8
{
    Ok,          ///< The operation completed.
    OutOfMemory, ///< Allocation failed.
    Limit,       ///< MAX_MACROS names already exist.
};

/// @brief Every macro name seen in one translation unit, looked up by spelling.
///
/// Names are interned in a FoundationStrings table, so a macro's index is its name's table index
/// minus one (index zero is the table's empty spelling).
class MacroTable final
{
public:
    /// Create the name table; must succeed before any other call.
    [[nodiscard]] MacroTableOutcome Initialize() noexcept;

    /// Index of the macro named `name`, or NO_MACRO.
    [[nodiscard]] foundation::uint32 Find(std::string_view name) const noexcept;

    /// Index of the macro named `name`, creating an undefined entry if it is new.
    [[nodiscard]] MacroTableOutcome
    TryIntern(std::string_view name, const Token& nameToken, foundation::uint32& outIndex) noexcept;

    /// Replace the definition at `index`; `body` is copied.
    [[nodiscard]] MacroTableOutcome TryDefine(foundation::uint32 index,
                                              const MacroDefinition& definition,
                                              const foundation::Array<BodyToken>& body) noexcept;

    /// The macro at `index`, which must be valid.
    [[nodiscard]] const MacroDefinition& Get(foundation::uint32 index) const noexcept
    {
        return mMacros[index];
    }

    /// Mark the macro at `index` undefined.
    void Undefine(foundation::uint32 index) noexcept
    {
        mMacros[index].IsDefined = false;
    }

    /// Replacement token `offset` of the macro at `index`.
    [[nodiscard]] const BodyToken& GetBodyToken(foundation::uint32 index, foundation::uint32 offset) const noexcept
    {
        return mBodies[mMacros[index].FirstBodyToken + offset];
    }

private:
    foundation::StringTable mNames;
    foundation::Array<MacroDefinition> mMacros;
    foundation::Array<BodyToken> mBodies;
};
} // namespace ludus::tooling::shader_language::internal
