#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/strings/status.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include <string_view>

namespace ludus::foundation
{
namespace detail
{
[[nodiscard]] StringStatus
AssignBounded(char* destination, usize& size, usize capacity, std::string_view source) noexcept;
[[nodiscard]] StringStatus
AppendBounded(char* destination, usize& size, usize capacity, std::string_view source) noexcept;
[[nodiscard]] StringStatus EraseBounded(char* inoutBytes, usize& inoutSize, usize offset, usize count) noexcept;
} // namespace detail

/// @brief Fixed-capacity inline bytes; Capacity excludes the extra terminator.
///
/// Holds up to `Capacity` bytes plus a terminating zero in `Capacity + 1` inline bytes and never
/// allocates; `Capacity` zero is valid and holds only empty text. Sizes count bytes, not Unicode
/// scalars, and contents are not validated as UTF-8.
/// Counted operations preserve embedded zero bytes. Overflow preserves the entire old value; no silent truncation.
/// Copies are plain value copies. Not internally synchronized: concurrent const calls are safe, and
/// any non-const call needs exclusive access.
/// @tparam Capacity Maximum number of stored bytes, excluding the terminator.
template <usize Capacity>
class StaticString final
{
    static_assert(Capacity < static_cast<usize>(-1));

public:
    /// Number of stored bytes, excluding the terminating zero.
    [[nodiscard]] constexpr usize GetSize() const noexcept
    {
        return mSize;
    }
    /// Maximum number of stored bytes, `Capacity`, excluding the terminating zero.
    [[nodiscard]] static constexpr usize GetCapacity() noexcept
    {
        return Capacity;
    }
    /// True when GetSize is zero.
    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        return mSize == 0;
    }
    /// @brief Borrowed counted view of the stored bytes, which may contain zero bytes.
    ///
    /// Points into this object: valid until the next non-const call on it or its destruction.
    [[nodiscard]] std::string_view GetView() const& noexcept
    {
        return {mBytes, mSize};
    }
    /// Deleted so a view cannot outlive a temporary that is destroyed at the end of the expression.
    [[nodiscard]] std::string_view GetView() const&& = delete;
    /// @brief Borrowed pointer to the inline bytes; never null.
    ///
    /// `GetData()[GetSize()]` is always zero, but embedded zero bytes may end a C-string reading
    /// early. Lifetime is the same as GetView.
    [[nodiscard]] const char* GetData() const& noexcept
    {
        return mBytes;
    }
    /// Deleted so a pointer cannot outlive a temporary that is destroyed at the end of the expression.
    [[nodiscard]] const char* GetData() const&& = delete;
    /// @brief Replaces the contents with a copy of `value`, overlap-safe.
    /// @param value Bytes to copy; it may borrow this object's own bytes.
    /// @return Ok; CapacityExceeded when `value` is longer than `Capacity`, leaving the old value
    /// unchanged.
    [[nodiscard]] StringStatus TryAssign(std::string_view value) noexcept
    {
        return detail::AssignBounded(mBytes, mSize, Capacity, value);
    }
    /// @brief Appends a copy of `value`, overlap-safe.
    /// @param value Bytes to append; it may borrow this object's own bytes.
    /// @return Ok; CapacityExceeded when `value` does not fit the remaining capacity, leaving the old
    /// value unchanged.
    [[nodiscard]] StringStatus TryAppend(std::string_view value) noexcept
    {
        return detail::AppendBounded(mBytes, mSize, Capacity, value);
    }
    /// @brief Display assignment: copies the longest prefix of `value` that fits and ends on a UTF-8
    /// code point boundary, overlap-safe.
    ///
    /// The only truncating assignment, named so that shortened text is a visible decision. Because the
    /// cut never splits a sequence of the validated input, the stored bytes stay well-formed UTF-8.
    /// Never fails; truncation keeps whole scalars and may leave up to three bytes of capacity unused.
    /// Not a grapheme or display-width operation: a combining mark can be separated from its base.
    /// @param value Validated text to copy; it may borrow this object's own bytes.
    /// @param outIsTruncated Set to true when bytes of `value` were dropped, otherwise false.
    void AssignTruncatedUtf8(Utf8View value, bool& outIsTruncated) noexcept
    {
        std::string_view kept = value.GetView();
        const usize size = FindPrecedingUtf8Boundary(value, Capacity);
        outIsTruncated = size < kept.size();
        kept.remove_suffix(kept.size() - size);
        const StringStatus status = detail::AssignBounded(mBytes, mSize, Capacity, kept);
        static_cast<void>(status);
    }
    /// @brief Removes `count` bytes starting at byte `offset` and closes the gap, as String::TryErase.
    /// @return Ok; OutOfRange when the range extends beyond GetSize(), leaving the value unchanged.
    [[nodiscard]] StringStatus TryErase(usize offset, usize count) noexcept
    {
        return detail::EraseBounded(mBytes, mSize, offset, count);
    }
    /// Sets the size to zero and writes the terminating zero at the start.
    void Clear() noexcept
    {
        mSize = 0;
        mBytes[0] = '\0';
    }

private:
    usize mSize{};
    char mBytes[Capacity + 1]{};
};
} // namespace ludus::foundation
