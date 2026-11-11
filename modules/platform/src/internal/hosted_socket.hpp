#pragma once

// Local byte stream operations shared by the hosted window and endpoint. The approved operations are listed in
// docs/decisions/0003-standard-library-usage-policy.md, "Platform hosted-window
// channel boundary".

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/time/time.hpp>

namespace ludus::platform::hosted
{
using foundation::int32;
using foundation::uint32;
using foundation::uint8;
using foundation::usize;

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
using NativeStream = foundation::isize;
#else
using NativeStream = int32;
#endif
inline constexpr NativeStream CLOSED_STREAM = -1;

/// Owns one socket or pipe and closes it on destruction.
class SocketDescriptor final
{
public:
    SocketDescriptor() noexcept = default;
    explicit SocketDescriptor(NativeStream descriptor) noexcept : mDescriptor(descriptor) {}
    ~SocketDescriptor() noexcept
    {
        Reset();
    }
    SocketDescriptor(const SocketDescriptor&) = delete;
    SocketDescriptor& operator=(const SocketDescriptor&) = delete;
    SocketDescriptor(SocketDescriptor&& other) noexcept : mDescriptor(other.Release()) {}
    SocketDescriptor& operator=(SocketDescriptor&& other) noexcept
    {
        if (this != &other)
        {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] LUDUS_INLINE NativeStream Get() const noexcept
    {
        return mDescriptor;
    }
    [[nodiscard]] LUDUS_INLINE bool IsOpen() const noexcept
    {
        return mDescriptor != CLOSED_STREAM;
    }
    /// Give up ownership without closing.
    [[nodiscard]] LUDUS_INLINE NativeStream Release() noexcept
    {
        const NativeStream descriptor = mDescriptor;
        mDescriptor = CLOSED_STREAM;
        return descriptor;
    }
    /// Close the owned descriptor, if any, and take `descriptor`.
    void Reset(NativeStream descriptor = CLOSED_STREAM) noexcept;

private:
    NativeStream mDescriptor = CLOSED_STREAM;
};

/// Outcome of one nonblocking transfer.
enum class TransferStatus : uint8
{
    /// Some bytes moved; the count may be less than requested.
    Progress,
    /// The socket would block; nothing moved.
    WouldBlock,
    /// The peer closed the connection.
    Closed,
    /// The operating system reported an error.
    Failed,
};

/// @brief Make a socket nonblocking and close-on-exec, and keep writes from raising SIGPIPE.
[[nodiscard]] bool TryConfigureSocket(NativeStream descriptor) noexcept;

/// @brief Send as many bytes as the socket accepts without blocking, retrying EINTR.
[[nodiscard]] TransferStatus
SendSome(NativeStream descriptor, foundation::BorrowedView<const uint8> bytes, usize& outSent) noexcept;

/// @brief Receive as many bytes as are available without blocking, retrying EINTR.
[[nodiscard]] TransferStatus
ReceiveSome(NativeStream descriptor, foundation::BorrowedView<uint8> outBytes, usize& outReceived) noexcept;

/// Most file descriptors one transfer carries: one hosted surface generation.
inline constexpr usize MAX_TRANSFER_DESCRIPTORS = 3;
/// Most file descriptors one receive accepts: four generations in flight.
inline constexpr usize MAX_RECEIVE_DESCRIPTORS = 4 * MAX_TRANSFER_DESCRIPTORS;

/// @brief Send bytes with file descriptors attached (SCM_RIGHTS), without blocking.
///
/// The descriptors travel with the first byte. On Progress they were sent, even when
/// fewer bytes than requested moved; the caller still owns and must close its copies.
/// On WouldBlock nothing moved.
/// @pre `descriptorCount` is 1 to MAX_TRANSFER_DESCRIPTORS and `bytes` is nonempty.
[[nodiscard]] TransferStatus SendWithDescriptors(NativeStream descriptor,
                                                 foundation::BorrowedView<const uint8> bytes,
                                                 const int32* descriptors,
                                                 usize descriptorCount,
                                                 usize& outSent) noexcept;

/// @brief Receive bytes and any attached file descriptors, without blocking.
///
/// Received descriptors are close-on-exec and owned by the caller. More descriptors
/// than `descriptorCapacity`, or a truncated control message, is Failed; every
/// descriptor received by that call is closed then.
/// @pre `descriptorCapacity` is at most MAX_RECEIVE_DESCRIPTORS.
/// @param outDescriptors Room for `descriptorCapacity` descriptors.
/// @param outDescriptorCount Descriptors written to `outDescriptors`; zero unless Progress.
[[nodiscard]] TransferStatus ReceiveWithDescriptors(NativeStream descriptor,
                                                    foundation::BorrowedView<uint8> outBytes,
                                                    usize& outReceived,
                                                    int32* outDescriptors,
                                                    usize descriptorCapacity,
                                                    usize& outDescriptorCount) noexcept;

/// @brief Wait until a socket is readable, closed or failed.
/// @param timeout Longest wait, rounded up to whole milliseconds; zero polls once.
/// @return false on timeout or a poll failure.
[[nodiscard]] bool WaitUntilReadable(NativeStream descriptor, foundation::time::Duration timeout) noexcept;

/// @brief Read the effective user id of a connected Unix-socket peer.
///
/// macOS uses getpeereid(3); Linux uses SO_PEERCRED. The kernel records the
/// credentials at connect() time, so the peer cannot forge them.
[[nodiscard]] bool TryReadPeerUserId(NativeStream descriptor, uint32& outUserId) noexcept;

/// @brief Read the process id of a connected Unix-socket peer.
///
/// macOS uses LOCAL_PEERPID; Linux uses SO_PEERCRED. Like the user id, the kernel
/// records it at connect() time.
[[nodiscard]] bool TryReadPeerProcessId(NativeStream descriptor, int32& outProcessId) noexcept;

/// The effective user id of this process.
[[nodiscard]] uint32 GetCurrentUserId() noexcept;
} // namespace ludus::platform::hosted
