#pragma once

// One hosted-window session on the endpoint side: admission of a connected
// peer, the Hello handshake, the mirrored window state and the bounded send
// queue. HostedWindowEndpoint adds the listening socket and session directory;
// tests drive a session directly over a socketpair.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/input/keyboard_event.h>
#include <ludus/platform/hosted_surface.h>
#include <ludus/platform/hosted_window.h>

#include "hosted_protocol.hpp"
#include "hosted_socket.hpp"
#if defined(__APPLE__)
#    include "hosted_mach.hpp"
#endif

namespace ludus::platform::hosted
{
/// Bytes the endpoint queues for a window that has not drained its socket.
inline constexpr usize MAX_PENDING_SEND_BYTES = static_cast<usize>(16) * 1024;
/// Time a connected peer has to deliver a complete Hello.
inline constexpr foundation::uint64 HANDSHAKE_TIMEOUT_NANOSECONDS = static_cast<foundation::uint64>(5) * 1'000'000'000;
/// Bytes buffered from the window between surface event takes.
inline constexpr usize ENDPOINT_RECEIVE_BUFFER_BYTES = 1024;
static_assert(ENDPOINT_RECEIVE_BUFFER_BYTES >= MAX_FRAME_BYTES);
/// Surface events queued for the consumer. When full, the endpoint stops reading
/// the socket, so a producer that outruns the consumer sees Busy.
inline constexpr usize MAX_SURFACE_EVENTS = 8;
/// Received file descriptors not yet matched to an Images message.
inline constexpr usize MAX_PENDING_DESCRIPTORS = MAX_RECEIVE_DESCRIPTORS;
/// Releases held back while the send queue is full; at most every image of four generations.
inline constexpr usize MAX_DEFERRED_RELEASES = 4 * HOSTED_SURFACE_IMAGE_COUNT;

/// Lifecycle of a session.
enum class SessionPhase : uint8
{
    /// No peer is admitted.
    Idle,
    /// A same-user peer is connected and its Hello has not completed.
    Handshaking,
    /// The peer is authenticated.
    Connected,
    /// The authenticated peer is gone. Terminal until Configure().
    Disconnected,
};

/// Window state the endpoint reproduces in a Welcome and after an overflow.
struct MirroredWindowState final
{
    uint32 Width = 0;
    uint32 Height = 0;
    float32 Scale = 1.0F;
    WindowVisibility Visibility = WindowVisibility::Visible;
};

class EndpointSession final
{
public:
    /// @brief Reset to Idle with a token, the accepted user id and the initial window state.
    /// @pre The state is valid for the protocol (extent, scale, visibility).
    void Configure(const Token& token, uint32 expectedUserId, const MirroredWindowState& state) noexcept;

#if defined(__APPLE__)
    /// @brief Register the session's Mach rendezvous for IOSurface send rights.
    ///
    /// Call after Configure() and before a window is admitted; the Welcome then
    /// carries `nonce`. Without it, Mach-port images are a protocol violation.
    [[nodiscard]] bool TryStartRendezvous(const RendezvousNonce& nonce) noexcept;
#endif

    /// @brief Take the oldest received surface event.
    /// @return false when none is queued; `outEvent` is unchanged then.
    [[nodiscard]] bool TryTakeSurfaceEvent(HostedSurfaceEvent& outEvent) noexcept;

    /// @brief Take ownership of a newly connected peer.
    ///
    /// A peer under another user id is refused at once. Otherwise the session enters
    /// Handshaking. Ignored (and the peer closed) unless the session is Idle.
    void Admit(SocketDescriptor&& connection, foundation::time::Timestamp now) noexcept;

    /// Progress the handshake, flush queued bytes and detect disconnection.
    void Pump(foundation::time::Timestamp now) noexcept;

    /// Close any connection. A connected session becomes Disconnected; otherwise Idle.
    void Close() noexcept;

    [[nodiscard]] LUDUS_INLINE SessionPhase GetPhase() const noexcept
    {
        return mPhase;
    }
    [[nodiscard]] LUDUS_INLINE HostedRefusal GetLastRefusal() const noexcept
    {
        return mLastRefusal;
    }
    [[nodiscard]] LUDUS_INLINE uint32 GetRefusalCount() const noexcept
    {
        return mRefusalCount;
    }

    HostedEndpointStatus SendResize(uint32 width, uint32 height, float32 scale) noexcept;
    HostedEndpointStatus SendVisibility(WindowVisibility visibility) noexcept;
    HostedEndpointStatus SendFocusEntered(const input::FocusBaseline& baseline) noexcept;
    HostedEndpointStatus SendFocusLost() noexcept;
    HostedEndpointStatus SendKey(input::Key key, input::KeyTransition transition, bool isRepeat) noexcept;
    HostedEndpointStatus SendClose() noexcept;
    HostedEndpointStatus SendRelease(uint32 generation, uint32 image) noexcept;

private:
    void refuse(HostedRefusal refusal) noexcept;
    void receiveHello(foundation::time::Timestamp now) noexcept;
    void receiveFromWindow() noexcept;
    [[nodiscard]] bool applyWindowMessage(const Message& message) noexcept;
    [[nodiscard]] bool tryAttachHandles(const Message& message, HostedSurfaceImages& inoutImages) noexcept;
    void pushEvent(HostedSurfaceEvent&& event) noexcept;
    void clearSurfaceState() noexcept;
    void disconnect() noexcept;
    [[nodiscard]] bool enqueue(const Message& message) noexcept;
    [[nodiscard]] bool enqueueSynchronization(bool isOverflow) noexcept;
    [[nodiscard]] Message makeFocusMessage() const noexcept;
    [[nodiscard]] HostedEndpointStatus send(const Message& message) noexcept;
    void flush() noexcept;
    [[nodiscard]] bool flushDeferredReleases() noexcept;

private:
    Token mToken = {};
    uint32 mExpectedUserId = 0;
    MirroredWindowState mState = {};
    bool mIsFocused = false;
    input::FocusBaseline mHeldKeys = {};
    bool mIsCloseRequested = false;
    bool mIsResynchronizationPending = false;

    SessionPhase mPhase = SessionPhase::Idle;
    SocketDescriptor mConnection;
    foundation::time::Timestamp mConnectedAt = {};
    foundation::StaticArray<uint8, MAX_FRAME_BYTES> mHelloBytes = {};
    usize mHelloSize = 0;
    foundation::StaticArray<uint8, MAX_PENDING_SEND_BYTES> mPending = {};
    usize mPendingSize = 0;

    HostedRefusal mLastRefusal = HostedRefusal::None;
    uint32 mRefusalCount = 0;

    RendezvousNonce mNonce = {};
    int32 mPeerProcessId = -1;
#if defined(__APPLE__)
    RendezvousServer mRendezvous;
#endif
    foundation::StaticArray<uint8, ENDPOINT_RECEIVE_BUFFER_BYTES> mReceived = {};
    usize mReceivedSize = 0;
    foundation::StaticArray<HostedImageHandle, MAX_PENDING_DESCRIPTORS> mDescriptors = {};
    usize mDescriptorCount = 0;
    foundation::StaticArray<HostedSurfaceEvent, MAX_SURFACE_EVENTS> mEvents = {};
    usize mEventFirst = 0;
    usize mEventCount = 0;
    uint32 mLastGeneration = 0;
    foundation::StaticArray<HostedImageRelease, MAX_DEFERRED_RELEASES> mDeferredReleases = {};
    usize mDeferredReleaseCount = 0;
};
} // namespace ludus::platform::hosted
