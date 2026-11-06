#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/strings/string_table.hpp>

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// Result of a ScopeTable operation that may allocate.
enum class ScopeOutcome : foundation::uint8
{
    Ok,          ///< The operation completed.
    OutOfMemory, ///< Allocation failed.
    Limit,       ///< MAX_SCOPE_NAMES distinct names already exist.
};

/// @brief Nested name bindings with constant-time lookup.
///
/// Names are interned once; each name keeps a chain of bindings, innermost first, so Find reads one
/// entry and Leave restores the outer bindings.
class ScopeTable final
{
public:
    /// Most distinct names in one translation unit.
    static constexpr foundation::usize MAX_SCOPE_NAMES = foundation::usize{1} << 18;
    /// Find result for an unbound name.
    static constexpr foundation::uint32 UNBOUND = 0xffffffffU;

    /// Create the name table; must succeed before any other call.
    [[nodiscard]] ScopeOutcome Initialize() noexcept;

    /// Open a nested scope.
    [[nodiscard]] ScopeOutcome Enter() noexcept;
    /// Close the innermost scope, restoring the bindings it hid.
    void Leave() noexcept;

    /// @brief Bind `name` to `symbol` in the innermost scope.
    /// @param outPrevious Receives the symbol `name` already had in this same scope, or UNBOUND.
    [[nodiscard]] ScopeOutcome
    TryBind(std::string_view name, foundation::uint32 symbol, foundation::uint32& outPrevious) noexcept;

    /// The innermost symbol bound to `name`, or UNBOUND.
    [[nodiscard]] foundation::uint32 Find(std::string_view name) const noexcept;

private:
    struct Binding final
    {
        foundation::uint32 Symbol = UNBOUND;
        foundation::uint32 Name = 0;
        foundation::uint32 Depth = 0;
        foundation::uint32 Previous = UNBOUND;
    };

    foundation::StringTable mNames;
    foundation::Array<foundation::uint32> mHeads;
    foundation::Array<Binding> mBindings;
    foundation::Array<foundation::usize> mScopeStarts;
};
} // namespace ludus::tooling::shader_language::internal
