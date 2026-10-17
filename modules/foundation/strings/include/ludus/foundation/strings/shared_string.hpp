#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/strings/status.hpp>

#include <string_view>

namespace ludus::foundation
{
class String;
// Thanks to The Qt Company, "Implicit Sharing", Qt 6 documentation,
// https://doc.qt.io/qt-6/implicit-sharing.html: adapt cheap shared copies, with
// an immutable API instead of mutation/detachment. See docs/architecture/strings.md.
/// @brief Immutable bytes shared by copies, without copy-on-write.
///
/// A handle is one pointer to a private block that holds an atomic retain count, the allocation domain,
/// the byte count and the zero-terminated bytes. Empty has no allocation. Copies retain the same block;
/// the bytes never change while any handle refers to them. CreateShared and CopyToString are the
/// explicit, fallible copies in and out.
///
/// Separate handles may be copied/destroyed on different threads after publication;
/// synchronize access to the same handle externally. Publish a handle to another thread through a
/// queue, mutex or release/acquire handoff. The allocation domain must outlive all shared owners
/// and must accept the final Free from whichever thread releases the last handle.
class SharedString final
{
public:
    /// Creates an empty handle; does not allocate.
    SharedString() noexcept = default;
    /// Releases this handle's reference; the last release frees the block through its domain.
    ~SharedString() noexcept;
    /// @brief Shares `other`'s block without allocating or copying bytes.
    ///
    /// If the retain count saturates, the block is pinned (never freed) and a check failure is
    /// reported instead of wrapping the count.
    SharedString(const SharedString& other) noexcept;
    /// Retains `other`'s block before releasing this handle's, so self-assignment is safe.
    SharedString& operator=(const SharedString& other) noexcept;
    /// Takes `other`'s block without touching the retain count; `other` becomes empty.
    SharedString(SharedString&& other) noexcept;
    /// Releases this handle's block, then takes `other`'s; `other` becomes empty. Self-move does nothing.
    SharedString& operator=(SharedString&& other) noexcept;
    /// Number of shared bytes, excluding the terminating zero; zero for an empty handle.
    [[nodiscard]] usize GetSize() const noexcept;
    /// True when GetSize is zero.
    [[nodiscard]] bool IsEmpty() const noexcept
    {
        return GetSize() == 0;
    }
    /// @brief Borrowed counted view of the shared bytes, which may contain zero bytes.
    ///
    /// Valid while some handle keeps the block alive; a view taken from a handle that is then
    /// reassigned, moved from or destroyed may dangle once that was the last reference.
    [[nodiscard]] std::string_view GetView() const& noexcept
    {
        return {GetData(), GetSize()};
    }
    /// Deleted so a view cannot outlive a temporary handle that may hold the last reference.
    [[nodiscard]] std::string_view GetView() const&& = delete;
    /// @brief Borrowed pointer to the shared bytes; never null.
    ///
    /// `GetData()[GetSize()]` is always zero; an empty handle returns a static empty string. Lifetime
    /// is the same as GetView.
    [[nodiscard]] const char* GetData() const& noexcept;
    /// Deleted so a pointer cannot outlive a temporary handle that may hold the last reference.
    [[nodiscard]] const char* GetData() const&& = delete;
    /// @brief Text equality: the same block, or equal bytes in different blocks.
    ///
    /// Two independently created handles with equal bytes compare equal; equality is not storage
    /// identity. Empty handles are equal.
    friend bool operator==(const SharedString& left, const SharedString& right) noexcept;

private:
    struct Block;
    Block* mBlock{};
    void Retain() const noexcept;
    void Release() noexcept;
    friend StringStatus CreateShared(std::string_view, const AllocationDomain&, SharedString&) noexcept;
};
/// @brief Copies `value` once into a new shared block allocated from `domain` and stores it in `output`.
///
/// An empty `value` makes `output` empty without allocating or recording `domain`. Otherwise the header
/// and bytes are one allocation. On success `output` releases its previous block.
/// Output unchanged on failure, even when source borrows the old output.
/// @param value Bytes to copy; it may borrow `output`'s current bytes.
/// @param domain Domain for the block; borrowed and must outlive every handle that shares it.
/// @param output Receives the new handle.
/// @return Ok; TooLarge when the block size overflows or exceeds half the address space; OutOfMemory
/// when `domain` cannot supply the block.
[[nodiscard]] StringStatus
CreateShared(std::string_view value, const AllocationDomain& domain, SharedString& output) noexcept;
/// @brief Deep-copies the shared bytes into `output`, as String::TryAssign.
///
/// `output` keeps its own allocation domain, so the copy is independent of the shared block's domain.
/// @return Ok; OutOfMemory when `output`'s domain cannot supply the storage. Failure leaves `output`
/// unchanged.
[[nodiscard]] StringStatus CopyToString(const SharedString& value, String& output) noexcept;
} // namespace ludus::foundation
