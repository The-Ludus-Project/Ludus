#pragma once

// One hosted-window session on the endpoint side: admission of a connected
// peer, the Hello handshake, the mirrored window state and the bounded send
// queue. HostedWindowEndpoint adds the listening socket and session directory;
// tests drive a session directly over a socketpair.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/input/keyboard_event.h>
#include <ludus/platform/hosted_recording.h>
#include <ludus/platform/hosted_surface.h>
#include <ludus/platform/hosted_window.h>

#include "hosted_protocol.hpp"
#include "hosted_socket.hpp"
#if LUDUS_TARGET_OS == LUDUS_OS_MACOS
#    include "hosted_mach.hpp"
#elif LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
#    include "hosted_pipe.hpp"
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
/// Recording events queued for the editor. Samples beyond the room kept for
/// acknowledgements are dropped and counted; they never hold back surface events.
inline constexpr usize MAX_RECORDING_EVENTS = 128;
/// Queue entries kept for acknowledgements, one per request that may be outstanding.
inline constexpr usize RECORDING_ACKNOWLEDGEMENT_RESERVE = MAX_HOSTED_RECORDING_REQUESTS;
static_assert(RECORDING_ACKNOWLEDGEMENT_RESERVE < MAX_RECORDING_EVENTS);

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

    /// @brief Announce the consumer's adapter in the Welcome.
    ///
    /// Call after Configure() and before a window is admitted; Configure() forgets it.
    void SetConsumerAdapter(const HostedAdapterIdentity& adapter) noexcept;

#if LUDUS_TARGET_OS == LUDUS_OS_MACOS
    /// @brief Register the session's Mach rendezvous for IOSurface send rights.
    ///
    /// Call after Configure() and before a window is admitted; the Welcome then
    /// carries `nonce`. Without it, Mach-port images are a protocol violation.
    [[nodiscard]] bool TryStartRendezvous(const RendezvousNonce& nonce) noexcept;
#endif

    /// @brief Take the oldest received surface event.
    /// @return false when none is queued; `outEvent` is unchanged then.
    [[nodiscard]] bool TryTakeSurfaceEvent(HostedSurfaceEvent& outEvent) noexcept;

    /// @brief Take the oldest received recording event.
    ///
    /// Recording events outlive a disconnection, so a recording keeps what the window sent
    /// before it went away; Configure() discards them.
    /// @return false when none is queued; `outEvent` is unchanged then.
    [[nodiscard]] bool TryTakeRecordingEvent(HostedRecordingEvent& outEvent) noexcept;

    /// Recording events dropped because the editor had not taken the queue, since Configure().
    [[nodiscard]] LUDUS_INLINE foundation::uint64 GetDroppedRecordingEvents() const noexcept
    {
        return mDroppedRecordingEvents;
    }

    /// Process identifier of the admitted peer, read from the operating system at admission;
    /// zero before one is admitted. Kept after a disconnection until Configure().
    [[nodiscard]] LUDUS_INLINE foundation::uint64 GetPeerProcessId() const noexcept
    {
        return mAdmittedProcessId;
    }

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
    HostedEndpointStatus SendRecordingRequest(const HostedRecordingRequest& request) noexcept;

private:
    void refuse(HostedRefusal refusal) noexcept;
    void receiveHello(foundation::time::Timestamp now) noexcept;
    void receiveFromWindow() noexcept;
    [[nodiscard]] bool applyWindowMessage(const Message& message) noexcept;
    [[nodiscard]] bool applyRecordingMessage(const Message& message) noexcept;
    void pushRecordingEvent(const HostedRecordingEvent& event, bool isAcknowledgement) noexcept;
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
    bool mHasConsumerAdapter = false;
    HostedAdapterIdentity mConsumerAdapter = {};
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    PeerProcess mPeerProcess;
#else
    int32 mPeerProcessId = -1;
#endif
#if LUDUS_TARGET_OS == LUDUS_OS_MACOS
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
    foundation::uint64 mAdmittedProcessId = 0;
    /// Requests sent and not yet acknowledged, in send order.
    foundation::StaticArray<HostedRecordingRequest, MAX_HOSTED_RECORDING_REQUESTS> mOutstandingRequests = {};
    usize mOutstandingRequestCount = 0;
    foundation::uint64 mLastRequest = 0;
    /// Highest recording started this session: a sample of a later one violates the protocol.
    foundation::uint64 mLastStartedRecording = 0;
    foundation::StaticArray<HostedRecordingEvent, MAX_RECORDING_EVENTS> mRecordingEvents = {};
    usize mRecordingFirst = 0;
    usize mRecordingCount = 0;
    foundation::uint64 mDroppedRecordingEvents = 0;
};
} // namespace ludus::platform::hosted
