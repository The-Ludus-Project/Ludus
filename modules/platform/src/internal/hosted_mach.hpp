#pragma once

// Per-session Mach rendezvous for hosted surface images (macOS only).
//
// A Unix socket cannot carry Mach rights, so IOSurface send rights travel through
// a Mach port that the editor endpoint registers under a per-session bootstrap
// name. The name is not a secret: the endpoint accepts a message only from the
// process admitted on the hosted-window socket (its audit-token pid and euid) and
// only with the session token. Global IOSurface IDs are never used, because any
// process could look them up. See docs/architecture/editor-gui-systems.md,
// "Hosted surface channel", and docs/decisions/0003-standard-library-usage-policy.md,
// "Platform hosted-window channel boundary".

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/platform/hosted_surface.h>

#include "hosted_protocol.hpp"

#include <string_view>

namespace ludus::platform::hosted
{
using foundation::int32;

/// Random bytes that make one session's bootstrap name unique.
inline constexpr usize RENDEZVOUS_NONCE_BYTES = 16;
/// Bootstrap name prefix; the nonce follows as lowercase hexadecimal.
inline constexpr std::string_view RENDEZVOUS_NAME_PREFIX = "org.ludus.hosted-window.";
/// Longest bootstrap name this protocol forms.
inline constexpr usize MAX_RENDEZVOUS_NAME_BYTES = RENDEZVOUS_NAME_PREFIX.size() + RENDEZVOUS_NONCE_BYTES * 2;
/// Messages the rendezvous port queues before a sender sees Busy.
inline constexpr uint32 RENDEZVOUS_QUEUE_LIMIT = 16;

/// Per-session rendezvous nonce, carried by the Welcome.
using RendezvousNonce = foundation::StaticArray<uint8, RENDEZVOUS_NONCE_BYTES>;
/// A bootstrap service name.
using RendezvousName = foundation::StaticString<MAX_RENDEZVOUS_NAME_BYTES>;

/// @brief Form the bootstrap name for a nonce. An all-zero nonce has no rendezvous.
/// @return false for an all-zero nonce; `outName` is unchanged then.
[[nodiscard]] bool TryFormatRendezvousName(const RendezvousNonce& nonce, RendezvousName& outName) noexcept;

/// Images of one generation received through the rendezvous.
struct RendezvousImages final
{
    uint32 Generation = 0;
    foundation::StaticArray<HostedImageHandle, HOSTED_SURFACE_IMAGE_COUNT> Handles{};
};

/// Outcome of a nonblocking rendezvous receive.
enum class RendezvousReceiveStatus : uint8
{
    /// One authenticated message was received.
    Ok,
    /// No message is queued.
    Empty,
    /// A message from another process, with another token or of the wrong shape was
    /// received and destroyed, releasing every right it carried.
    Rejected,
    /// The port is not registered.
    Failed,
};

/// Outcome of a nonblocking rendezvous send.
enum class RendezvousSendStatus : uint8
{
    /// The message is queued on the endpoint's port.
    Ok,
    /// The port's queue is full; nothing was sent.
    Busy,
    /// The service is unknown, the endpoint is gone or the kernel refused the message.
    Failed,
};

/// @brief Endpoint side: owns the receive right registered under the session's name.
class RendezvousServer final
{
public:
    RendezvousServer() noexcept = default;
    ~RendezvousServer() noexcept
    {
        Stop();
    }
    RendezvousServer(const RendezvousServer&) = delete;
    RendezvousServer& operator=(const RendezvousServer&) = delete;
    RendezvousServer(RendezvousServer&&) = delete;
    RendezvousServer& operator=(RendezvousServer&&) = delete;

    /// @brief Register `name` with the bootstrap server and own its receive right.
    /// @return false when the name is taken or the kernel refuses; the server stays stopped.
    [[nodiscard]] bool TryStart(const RendezvousName& name) noexcept;
    /// Destroy the receive right, which removes the registration.
    void Stop() noexcept;
    [[nodiscard]] LUDUS_INLINE bool IsStarted() const noexcept
    {
        return mReceiveRight != 0;
    }

    /// @brief Receive one queued message without blocking.
    /// @param expectedProcessId pid of the admitted window; any other sender is rejected.
    /// @param expectedUserId euid the sender must run under.
    /// @param token Session token the message must carry.
    /// @param outImages Received generation; unchanged unless Ok.
    [[nodiscard]] RendezvousReceiveStatus TryReceive(int32 expectedProcessId,
                                                     uint32 expectedUserId,
                                                     const Token& token,
                                                     RendezvousImages& outImages) noexcept;

private:
    uint32 mReceiveRight = 0;
};

/// @brief Window side: a send right to the endpoint's rendezvous port.
class RendezvousClient final
{
public:
    RendezvousClient() noexcept = default;
    ~RendezvousClient() noexcept
    {
        Reset();
    }
    RendezvousClient(const RendezvousClient&) = delete;
    RendezvousClient& operator=(const RendezvousClient&) = delete;
    RendezvousClient(RendezvousClient&&) = delete;
    RendezvousClient& operator=(RendezvousClient&&) = delete;

    /// @brief Look up the endpoint's port by name.
    /// @return false when the service is not registered.
    [[nodiscard]] bool TryConnect(const RendezvousName& name) noexcept;
    /// Release the send right.
    void Reset() noexcept;
    [[nodiscard]] LUDUS_INLINE bool IsConnected() const noexcept
    {
        return mSendRight != 0;
    }

    /// @brief Send one generation's Mach send rights with the session token, without blocking.
    ///
    /// The rights are copied into the message, so `images` keeps its handles whatever the outcome.
    [[nodiscard]] RendezvousSendStatus
    TrySend(const Token& token,
            uint32 generation,
            const foundation::StaticArray<HostedImageHandle, HOSTED_SURFACE_IMAGE_COUNT>& images) noexcept;

private:
    uint32 mSendRight = 0;
};
} // namespace ludus::platform::hosted
