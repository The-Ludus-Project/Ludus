#pragma once

// POSIX Unix-stream socket operations shared by the hosted window and its
// endpoint (macOS and Linux only). The approved operations are listed in
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

/// Owns one file descriptor and closes it on destruction.
class SocketDescriptor final
{
public:
    SocketDescriptor() noexcept = default;
    explicit SocketDescriptor(int32 descriptor) noexcept : mDescriptor(descriptor) {}
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

    [[nodiscard]] LUDUS_INLINE int32 Get() const noexcept
    {
        return mDescriptor;
    }
    [[nodiscard]] LUDUS_INLINE bool IsOpen() const noexcept
    {
        return mDescriptor >= 0;
    }
    /// Give up ownership without closing.
    [[nodiscard]] LUDUS_INLINE int32 Release() noexcept
    {
        const int32 descriptor = mDescriptor;
        mDescriptor = -1;
        return descriptor;
    }
    /// Close the owned descriptor, if any, and take `descriptor`.
    void Reset(int32 descriptor = -1) noexcept;

private:
    int32 mDescriptor = -1;
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
[[nodiscard]] bool TryConfigureSocket(int32 descriptor) noexcept;

/// @brief Send as many bytes as the socket accepts without blocking, retrying EINTR.
[[nodiscard]] TransferStatus
SendSome(int32 descriptor, foundation::BorrowedView<const uint8> bytes, usize& outSent) noexcept;

/// @brief Receive as many bytes as are available without blocking, retrying EINTR.
[[nodiscard]] TransferStatus
ReceiveSome(int32 descriptor, foundation::BorrowedView<uint8> outBytes, usize& outReceived) noexcept;

/// @brief Wait until a socket is readable, closed or failed.
/// @param timeout Longest wait, rounded up to whole milliseconds; zero polls once.
/// @return false on timeout or a poll failure.
[[nodiscard]] bool WaitUntilReadable(int32 descriptor, foundation::time::Duration timeout) noexcept;

/// @brief Read the effective user id of a connected Unix-socket peer.
///
/// macOS uses getpeereid(3); Linux uses SO_PEERCRED. The kernel records the
/// credentials at connect() time, so the peer cannot forge them.
[[nodiscard]] bool TryReadPeerUserId(int32 descriptor, uint32& outUserId) noexcept;

/// The effective user id of this process.
[[nodiscard]] uint32 GetCurrentUserId() noexcept;
} // namespace ludus::platform::hosted
