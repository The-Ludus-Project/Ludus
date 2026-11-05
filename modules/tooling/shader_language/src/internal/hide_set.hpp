#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>

namespace ludus::tooling::shader_language::internal
{
/// Result of a hide-set operation.
enum class HideSetOutcome : foundation::uint8
{
    Ok,          ///< The resulting set is available.
    OutOfMemory, ///< Allocation failed.
    Limit,       ///< MAX_HIDE_SET_ELEMENTS would be exceeded; reported as a macro expansion limit.
};

/// @brief Immutable sets of macro indices that may not expand a token again.
///
/// Implements the hide sets of Dave Prosser's macro expansion algorithm (see macro_expander.cpp).
/// Every operation creates a new set, so set identifiers stay valid for the table's lifetime. Set
/// EMPTY_HIDE_SET always exists. Elements of a set are sorted and unique.
class HideSetTable final
{
public:
    /// Identifier of the empty set.
    static constexpr foundation::uint32 EMPTY_HIDE_SET = 0;
    /// Bound on stored elements across every set of one request.
    static constexpr foundation::usize MAX_HIDE_SET_ELEMENTS = foundation::usize{1} << 22;

    /// True when `macroIndex` is an element of `set`.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a set and an element, as in every set API.
    [[nodiscard]] bool Contains(foundation::uint32 set, foundation::uint32 macroIndex) const noexcept;
    /// `outSet` receives `set` with `macroIndex` added.
    [[nodiscard]] HideSetOutcome
    TryInsert(foundation::uint32 set, foundation::uint32 macroIndex, foundation::uint32& outSet) noexcept;
    /// `outSet` receives the union of `left` and `right`.
    [[nodiscard]] HideSetOutcome
    TryUnite(foundation::uint32 left, foundation::uint32 right, foundation::uint32& outSet) noexcept;
    /// `outSet` receives the intersection of `left` and `right`.
    [[nodiscard]] HideSetOutcome
    TryIntersect(foundation::uint32 left, foundation::uint32 right, foundation::uint32& outSet) noexcept;

private:
    struct Range final
    {
        foundation::uint32 First = 0;
        foundation::uint32 Count = 0;
    };

    enum class Combination : foundation::uint8
    {
        Union,
        Intersection,
    };

    [[nodiscard]] HideSetOutcome combine(foundation::uint32 left,
                                         foundation::uint32 right,
                                         Combination combination,
                                         foundation::uint32& outSet) noexcept;
    [[nodiscard]] Range getRange(foundation::uint32 set) const noexcept;

    foundation::Array<foundation::uint32> mElements;
    foundation::Array<Range> mSets;
};
} // namespace ludus::tooling::shader_language::internal
