#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/strings/status.hpp>

#include <span>
#include <string_view>

namespace ludus::foundation
{
/// @brief Unique, move-only, zero-terminated byte storage.
///
/// Contents are counted bytes: sizes, capacities and offsets count bytes, not Unicode scalars, and no
/// operation validates UTF-8 or rejects embedded zero bytes (see ValidateUtf8 and ValidateCString).
/// Up to kInlineCapacity bytes are held without allocation; a longer value occupies one heap
/// allocation of capacity + 1 bytes from the owner's AllocationDomain. The domain is recorded even
/// while the value is inline and must outlive this owner.
///
/// Copies are explicit through CloneTo, so deep-copy allocation and failure stay visible. Moves carry
/// the source's domain; TryAssign and TryAppend keep the destination's domain. A failed operation
/// leaves bytes, size, capacity and domain unchanged. An owner is not internally synchronized:
/// concurrent const calls are safe, and any non-const call needs exclusive access.
class String final
{
public:
    /// @brief Bytes held inline before an operation must allocate.
    ///
    /// A capacity equal to this value selects inline storage; heap capacities are always larger. The
    /// value is the current layout baseline, not a stable ABI.
    static constexpr usize kInlineCapacity = 23;
    /// @brief Creates an empty inline owner that allocates from `domain`; does not allocate.
    /// @param domain Allocation domain for later growth. It is borrowed and must outlive this owner.
    explicit String(const AllocationDomain& domain = GetSystemAllocationDomain()) noexcept;
    /// Frees heap storage through the owner's domain; inline storage needs no domain call.
    ~String() noexcept;
    /// @brief Takes `other`'s bytes, storage and domain without allocating.
    ///
    /// `other` becomes empty and inline and keeps its domain, so it remains usable.
    String(String&& other) noexcept;
    /// @brief Releases this owner's storage, then takes `other`'s bytes, storage and domain.
    ///
    /// Does not allocate. `other` becomes empty and inline and keeps its domain. Self-move does nothing.
    String& operator=(String&& other) noexcept;
    /// Deleted: a deep copy can fail, so it is spelled CloneTo.
    String(const String&) = delete;
    /// Deleted: a deep copy can fail, so it is spelled CloneTo, and TryAssign copies bytes.
    String& operator=(const String&) = delete;

    /// Number of stored bytes, excluding the terminating zero.
    [[nodiscard]] usize GetSize() const noexcept
    {
        return mSize;
    }
    /// @brief Bytes the current storage holds without reallocating, excluding the terminating zero.
    ///
    /// kInlineCapacity while inline. Growth may reserve more than requested; do not depend on the
    /// exact value. Capacity never shrinks while the owner keeps its storage.
    [[nodiscard]] usize GetCapacity() const noexcept
    {
        return mCapacity;
    }
    /// True when GetSize is zero.
    [[nodiscard]] bool IsEmpty() const noexcept
    {
        return mSize == 0;
    }
    /// Domain that allocates and frees this owner's heap storage; borrowed, never null.
    [[nodiscard]] const AllocationDomain& GetDomain() const noexcept
    {
        return *mDomain;
    }
    /// @brief Borrowed counted view of the stored bytes, which may contain zero bytes.
    ///
    /// Valid until the next non-const call on this owner, a move from it, or its destruction.
    [[nodiscard]] std::string_view GetView() const& noexcept
    {
        return {GetData(), mSize};
    }
    /// Deleted so a view cannot outlive a temporary owner that is destroyed at the end of the expression.
    [[nodiscard]] std::string_view GetView() const&& = delete;
    /// @brief Borrowed pointer to the stored bytes; never null.
    ///
    /// `GetData()[GetSize()]` is always zero, including for an empty owner, but embedded zero bytes
    /// may end a C-string reading early. Valid until the next non-const call on this owner, a move
    /// from it, or its destruction.
    [[nodiscard]] const char* GetData() const& noexcept;
    /// Deleted so a pointer cannot outlive a temporary owner that is destroyed at the end of the expression.
    [[nodiscard]] const char* GetData() const&& = delete;

    /// @brief Replaces the contents with a copy of `value`.
    ///
    /// Copies in place, overlap-safe, when `value` fits the current capacity. Otherwise allocates
    /// grown storage (the larger of the request and twice the current capacity, clamped to the
    /// maximum), copies `value` into it and only then frees the old storage. `value` may therefore borrow this owner's
    /// own bytes.
    /// @param value Bytes to copy; it may alias this owner's contents.
    /// @return Ok; TooLarge when `value` exceeds the maximum capacity (just under half the address
    /// space); OutOfMemory when the domain cannot supply the storage. Failure leaves the owner unchanged.
    [[nodiscard]] StringStatus TryAssign(std::string_view value) noexcept;
    /// @brief Appends a copy of `value`; equivalent to TryAppendMany with one view.
    /// @param value Bytes to append; it may alias this owner's contents.
    /// @return Ok, TooLarge or OutOfMemory as for TryAppendMany. Failure leaves the owner unchanged.
    [[nodiscard]] StringStatus TryAppend(std::string_view value) noexcept;
    /// @brief Appends every view in order, with one reserve and one commit for the batch.
    ///
    /// The checked total size is computed before anything changes, and the call allocates at most once.
    /// When it grows, all views are read while the old storage is still live, so slices of this owner's
    /// own bytes are valid input. Views into unused capacity beyond GetSize are not valid input.
    /// @param values Views to append; each may alias this owner's contents.
    /// @return Ok; TooLarge when the total size overflows or exceeds the maximum capacity; OutOfMemory
    /// when the domain cannot supply the storage. Failure leaves the owner unchanged.
    [[nodiscard]] StringStatus TryAppendMany(std::span<const std::string_view> values) noexcept;
    /// @brief Ensures room for at least `capacity` bytes, excluding the terminating zero.
    ///
    /// Does nothing when the current capacity suffices. Otherwise moves the contents to grown storage
    /// (the larger of the request and twice the current capacity), so later appends up to that capacity do not
    /// allocate.
    /// @param capacity Requested capacity in bytes.
    /// @return Ok; TooLarge when `capacity` exceeds the maximum capacity; OutOfMemory when the domain
    /// cannot supply the storage. Failure leaves the owner unchanged.
    [[nodiscard]] StringStatus TryEnsureCapacity(usize capacity) noexcept;
    /// @brief Deep-copies these bytes into a new owner that allocates from `domain`, then moves it into
    /// `output`.
    ///
    /// On success `output`'s previous storage is released and `output` takes `domain`. `output` may be
    /// this owner.
    /// @param domain Domain for the copy; borrowed and must outlive `output`.
    /// @param output Receives the copy.
    /// @return Ok; OutOfMemory when `domain` cannot supply the storage. Failure leaves `output` unchanged.
    [[nodiscard]] StringStatus CloneTo(const AllocationDomain& domain, String& output) const noexcept;
    /// Sets the size to zero and keeps the current storage and capacity; never allocates or frees.
    void Clear() noexcept;

private:
    [[nodiscard]] char* MutableData() noexcept;
    void Release() noexcept;
    const AllocationDomain* mDomain;
    usize mSize{};
    usize mCapacity{kInlineCapacity};
    /// Inline bytes or heap pointer, selected by whether the capacity equals kInlineCapacity.
    union Storage
    {
        /// Inline bytes plus terminator; active while the capacity equals kInlineCapacity.
        char Inline[kInlineCapacity + 1];
        /// Heap block of capacity + 1 bytes from the owner's domain; active for larger capacities.
        char* Heap;
        /// Starts with zeroed inline bytes, which is the empty value.
        constexpr Storage() noexcept : Inline{} {}
    } mStorage;
};

/// @brief Reusable append buffer around one String.
///
/// Appends report failure explicitly and leave the contents unchanged; there are no expression
/// templates, allocating `operator+` chains or hidden copies. Clear keeps the storage for reuse. Not
/// internally synchronized, like String.
class StringBuilder final
{
public:
    /// @brief Creates an empty builder whose string allocates from `domain`; does not allocate.
    /// @param domain Borrowed allocation domain; it must outlive the builder and any taken string.
    explicit StringBuilder(const AllocationDomain& domain = GetSystemAllocationDomain()) noexcept : mString(domain) {}
    /// Appends `value`, as String::TryAppend; `value` may alias the builder's contents.
    [[nodiscard]] StringStatus TryAppend(std::string_view value) noexcept
    {
        return mString.TryAppend(value);
    }
    /// Appends every view with one reserve and one commit, as String::TryAppendMany.
    [[nodiscard]] StringStatus TryAppendMany(std::span<const std::string_view> values) noexcept
    {
        return mString.TryAppendMany(values);
    }
    /// Reserves room for `capacity` bytes, as String::TryEnsureCapacity.
    [[nodiscard]] StringStatus TryEnsureCapacity(usize capacity) noexcept
    {
        return mString.TryEnsureCapacity(capacity);
    }
    /// Borrowed view of the bytes built so far; valid until the next non-const call or destruction.
    [[nodiscard]] std::string_view GetView() const& noexcept
    {
        return mString.GetView();
    }
    /// Deleted so a view cannot outlive a temporary builder that is destroyed at the end of the expression.
    [[nodiscard]] std::string_view GetView() const&& = delete;
    /// @brief Moves the built string out without allocating.
    ///
    /// The result keeps the builder's domain and storage. The builder becomes empty and inline with the
    /// same domain, so growing it beyond String::kInlineCapacity allocates again.
    [[nodiscard]] String TakeString() noexcept;
    /// Empties the builder and keeps its storage; never allocates or frees.
    void Clear() noexcept
    {
        mString.Clear();
    }

private:
    String mString;
};
} // namespace ludus::foundation
