#include "internal/hosted_endpoint_session.hpp"

#include <cstring>

namespace ludus::platform::hosted
{
namespace
{
/// Welcome or overflow resize, visibility, overflow reset, focus and close.
constexpr usize MAX_SYNCHRONIZATION_MESSAGES = 5;
/// Rendezvous messages from other processes skipped while matching one Images message.
constexpr usize MAX_REJECTED_RENDEZVOUS_MESSAGES = 16;

bool IsValidVisibility(WindowVisibility visibility) noexcept
{
    return static_cast<uint8>(visibility) <= static_cast<uint8>(WindowVisibility::Minimized);
}
} // namespace

void EndpointSession::Configure(const Token& token, uint32 expectedUserId, const MirroredWindowState& state) noexcept
{
    mConnection.Reset();
    mToken = token;
    mExpectedUserId = expectedUserId;
    mState = state;
    mIsFocused = false;
    mHeldKeys = {};
    mIsCloseRequested = false;
    mIsResynchronizationPending = false;
    mPhase = SessionPhase::Idle;
    mConnectedAt = {};
    mHelloSize = 0;
    mPendingSize = 0;
    mLastRefusal = HostedRefusal::None;
    mRefusalCount = 0;
    mNonce = {};
    mPeerProcessId = -1;
#if defined(__APPLE__)
    mRendezvous.Stop();
#endif
    clearSurfaceState();
}

#if defined(__APPLE__)
bool EndpointSession::TryStartRendezvous(const RendezvousNonce& nonce) noexcept
{
    RendezvousName name;
    if (mPhase != SessionPhase::Idle || mRendezvous.IsStarted() == true ||
        TryFormatRendezvousName(nonce, name) == false || mRendezvous.TryStart(name) == false)
    {
        return false;
    }
    mNonce = nonce;
    return true;
}
#endif

void EndpointSession::clearSurfaceState() noexcept
{
    mReceivedSize = 0;
    for (HostedImageHandle& handle : mDescriptors)
    {
        handle.Reset();
    }
    mDescriptorCount = 0;
    for (HostedSurfaceEvent& event : mEvents)
    {
        event = {};
    }
    mEventFirst = 0;
    mEventCount = 0;
    mLastGeneration = 0;
    mDeferredReleaseCount = 0;
}

bool EndpointSession::TryTakeSurfaceEvent(HostedSurfaceEvent& outEvent) noexcept
{
    if (mEventCount == 0)
    {
        return false;
    }
    outEvent = foundation::Move(mEvents[mEventFirst]);
    mEvents[mEventFirst] = {};
    mEventFirst = (mEventFirst + 1) % MAX_SURFACE_EVENTS;
    --mEventCount;
    return true;
}

void EndpointSession::pushEvent(HostedSurfaceEvent&& event) noexcept
{
    mEvents[(mEventFirst + mEventCount) % MAX_SURFACE_EVENTS] = foundation::Move(event);
    ++mEventCount;
}

void EndpointSession::Admit(SocketDescriptor&& connection, foundation::time::Timestamp now) noexcept
{
    SocketDescriptor peer(foundation::Move(connection));
    if (mPhase != SessionPhase::Idle || peer.IsOpen() == false || TryConfigureSocket(peer.Get()) == false)
    {
        return;
    }
    mConnection = foundation::Move(peer);
    uint32 peerUserId = 0;
    int32 peerProcessId = -1;
    if (TryReadPeerUserId(mConnection.Get(), peerUserId) == false || peerUserId != mExpectedUserId ||
        TryReadPeerProcessId(mConnection.Get(), peerProcessId) == false)
    {
        refuse(HostedRefusal::WrongUser);
        return;
    }
    mPeerProcessId = peerProcessId;
    mPhase = SessionPhase::Handshaking;
    mConnectedAt = now;
    mHelloSize = 0;
}

void EndpointSession::Pump(foundation::time::Timestamp now) noexcept
{
    if (mPhase == SessionPhase::Handshaking)
    {
        receiveHello(now);
    }
    if (mPhase == SessionPhase::Connected)
    {
        flush();
    }
    if (mPhase == SessionPhase::Connected)
    {
        receiveFromWindow();
    }
}

void EndpointSession::Close() noexcept
{
    if (mPhase == SessionPhase::Connected)
    {
        disconnect();
        return;
    }
    mConnection.Reset();
    if (mPhase == SessionPhase::Handshaking)
    {
        mPhase = SessionPhase::Idle;
    }
}

void EndpointSession::refuse(HostedRefusal refusal) noexcept
{
    Message message;
    message.Kind = MessageKind::Refused;
    message.Refusal = refusal;
    foundation::StaticArray<uint8, MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    usize sent = 0;
    // Best effort: the refused peer learns why unless its socket is already full.
    if (TryEncodeMessage(message, {bytes.GetData(), bytes.GetSize()}, size) == true)
    {
        static_cast<void>(SendSome(mConnection.Get(), {bytes.GetData(), size}, sent));
    }
    mConnection.Reset();
    mPhase = SessionPhase::Idle;
    mHelloSize = 0;
    mLastRefusal = refusal;
    ++mRefusalCount;
}

void EndpointSession::receiveHello(foundation::time::Timestamp now) noexcept
{
    while (mHelloSize < mHelloBytes.GetSize())
    {
        usize received = 0;
        const TransferStatus status =
            ReceiveSome(mConnection.Get(),
                        {mHelloBytes.GetData() + mHelloSize, mHelloBytes.GetSize() - mHelloSize},
                        received);
        if (status == TransferStatus::WouldBlock)
        {
            break;
        }
        if (status != TransferStatus::Progress)
        {
            // The peer left before authenticating; nothing was admitted.
            mConnection.Reset();
            mPhase = SessionPhase::Idle;
            mHelloSize = 0;
            return;
        }
        mHelloSize += received;
    }

    Message hello;
    usize consumed = 0;
    const DecodeStatus decoded = TryDecodeMessage({mHelloBytes.GetData(), mHelloSize}, hello, consumed);
    if (decoded == DecodeStatus::NeedMore)
    {
        if (now.Nanoseconds >= mConnectedAt.Nanoseconds &&
            now.Nanoseconds - mConnectedAt.Nanoseconds > HANDSHAKE_TIMEOUT_NANOSECONDS)
        {
            refuse(HostedRefusal::HandshakeTimeout);
        }
        return;
    }
    if (decoded == DecodeStatus::Malformed || hello.Kind != MessageKind::Hello)
    {
        refuse(HostedRefusal::MalformedHello);
        return;
    }
    if (hello.Version != PROTOCOL_VERSION || hello.KeyCount != input::KEY_COUNT)
    {
        refuse(HostedRefusal::IncompatibleVersion);
        return;
    }
    if (AreTokensEqual(hello.SessionToken, mToken) == false)
    {
        refuse(HostedRefusal::BadToken);
        return;
    }
    if (consumed != mHelloSize)
    {
        // A version-1 window sends nothing until it has read the Welcome.
        refuse(HostedRefusal::MalformedHello);
        return;
    }
    mPhase = SessionPhase::Connected;
    mPendingSize = 0;
    mIsResynchronizationPending = false;
    if (enqueueSynchronization(false) == false)
    {
        disconnect();
        return;
    }
    flush();
}

void EndpointSession::receiveFromWindow() noexcept
{
    while (mPhase == SessionPhase::Connected)
    {
        // Decode what is buffered first; a full event queue holds the rest back.
        while (mEventCount < MAX_SURFACE_EVENTS)
        {
            Message message;
            usize consumed = 0;
            const DecodeStatus decoded = TryDecodeMessage({mReceived.GetData(), mReceivedSize}, message, consumed);
            if (decoded == DecodeStatus::NeedMore)
            {
                break;
            }
            if (decoded == DecodeStatus::Malformed || applyWindowMessage(message) == false)
            {
                disconnect();
                return;
            }
            std::memmove(mReceived.GetData(), mReceived.GetData() + consumed, mReceivedSize - consumed);
            mReceivedSize -= consumed;
        }
        if (mEventCount == MAX_SURFACE_EVENTS)
        {
            return;
        }
        foundation::StaticArray<int32, MAX_PENDING_DESCRIPTORS> descriptors{};
        usize received = 0;
        usize descriptorCount = 0;
        const TransferStatus status =
            ReceiveWithDescriptors(mConnection.Get(),
                                   {mReceived.GetData() + mReceivedSize, mReceived.GetSize() - mReceivedSize},
                                   received,
                                   descriptors.GetData(),
                                   MAX_PENDING_DESCRIPTORS - mDescriptorCount,
                                   descriptorCount);
        if (status == TransferStatus::WouldBlock)
        {
            return;
        }
        if (status != TransferStatus::Progress)
        {
            disconnect();
            return;
        }
        mReceivedSize += received;
        for (usize i = 0; i < descriptorCount; ++i)
        {
            mDescriptors[mDescriptorCount] = HostedImageHandle::FromFileDescriptor(descriptors[i]);
            ++mDescriptorCount;
        }
    }
}

bool EndpointSession::applyWindowMessage(const Message& message) noexcept
{
    if (message.Kind == MessageKind::Present)
    {
        // A frame of a later generation than any published one violates the protocol.
        if (message.Generation > mLastGeneration)
        {
            return false;
        }
        HostedSurfaceEvent event;
        event.Kind = HostedSurfaceEventKind::Present;
        event.Generation = message.Generation;
        event.Image = message.Image;
        event.Sequence = message.Sequence;
        pushEvent(foundation::Move(event));
        return true;
    }
    if (message.Kind != MessageKind::Images || message.Generation <= mLastGeneration)
    {
        // A window sends only Images and Present after its Hello.
        return false;
    }
    HostedSurfaceEvent event;
    event.Kind = HostedSurfaceEventKind::Images;
    HostedSurfaceImages& images = event.Images;
    images.Generation = message.Generation;
    images.Width = message.Width;
    images.Height = message.Height;
    images.Format = message.Format;
    images.AllocationSizes = message.AllocationSizes;
    images.MemoryTypeIndex = message.MemoryTypeIndex;
    images.DeviceUuid = message.DeviceUuid;
    images.DriverUuid = message.DriverUuid;
    if (tryAttachHandles(message, images) == false)
    {
        return false;
    }
    mLastGeneration = message.Generation;
    pushEvent(foundation::Move(event));
    return true;
}

bool EndpointSession::tryAttachHandles(const Message& message, HostedSurfaceImages& inoutImages) noexcept
{
    if (message.HandleKind == HostedHandleKind::OpaqueFileDescriptor)
    {
        // The descriptors arrived with the first byte of this message, in send order.
        if (mDescriptorCount < HOSTED_SURFACE_IMAGE_COUNT)
        {
            return false;
        }
        for (usize i = 0; i < HOSTED_SURFACE_IMAGE_COUNT; ++i)
        {
            inoutImages.Handles[i] = foundation::Move(mDescriptors[i]);
        }
        for (usize i = HOSTED_SURFACE_IMAGE_COUNT; i < mDescriptorCount; ++i)
        {
            mDescriptors[i - HOSTED_SURFACE_IMAGE_COUNT] = foundation::Move(mDescriptors[i]);
        }
        mDescriptorCount -= HOSTED_SURFACE_IMAGE_COUNT;
        return true;
    }
#if defined(__APPLE__)
    // The window sends the Mach message before the socket message, so it is queued by now.
    for (usize attempt = 0; attempt < MAX_REJECTED_RENDEZVOUS_MESSAGES; ++attempt)
    {
        RendezvousImages received;
        const RendezvousReceiveStatus status =
            mRendezvous.TryReceive(mPeerProcessId, mExpectedUserId, mToken, received);
        if (status == RendezvousReceiveStatus::Rejected)
        {
            continue;
        }
        if (status != RendezvousReceiveStatus::Ok || received.Generation != message.Generation)
        {
            return false;
        }
        for (usize i = 0; i < HOSTED_SURFACE_IMAGE_COUNT; ++i)
        {
            inoutImages.Handles[i] = foundation::Move(received.Handles[i]);
        }
        return true;
    }
#endif
    return false;
}

void EndpointSession::disconnect() noexcept
{
    mConnection.Reset();
    mPhase = SessionPhase::Disconnected;
    mPendingSize = 0;
    mIsResynchronizationPending = false;
    clearSurfaceState();
}

bool EndpointSession::enqueue(const Message& message) noexcept
{
    usize size = 0;
    if (TryEncodeMessage(message, {mPending.GetData() + mPendingSize, mPending.GetSize() - mPendingSize}, size) ==
        false)
    {
        return false;
    }
    mPendingSize += size;
    return true;
}

Message EndpointSession::makeFocusMessage() const noexcept
{
    Message message;
    message.Kind = MessageKind::Reset;
    message.Reason = input::ResetReason::FocusEntered;
    message.Baseline = mHeldKeys;
    message.Baseline.Focused = true;
    return message;
}

bool EndpointSession::enqueueSynchronization(bool isOverflow) noexcept
{
    // Default-initialized: every Message member has a default member initializer.
    foundation::StaticArray<Message, MAX_SYNCHRONIZATION_MESSAGES> messages;
    usize count = 0;
    if (isOverflow == true)
    {
        messages[count].Kind = MessageKind::Resize;
        messages[count].Width = mState.Width;
        messages[count].Height = mState.Height;
        messages[count].Scale = mState.Scale;
        ++count;
        messages[count].Kind = MessageKind::Visibility;
        messages[count].Visibility = mState.Visibility;
        ++count;
        messages[count].Kind = MessageKind::Reset;
        messages[count].Reason = input::ResetReason::Overflow;
        ++count;
    }
    else
    {
        messages[count].Kind = MessageKind::Welcome;
        messages[count].Width = mState.Width;
        messages[count].Height = mState.Height;
        messages[count].Scale = mState.Scale;
        messages[count].Visibility = mState.Visibility;
        messages[count].Nonce = mNonce;
        ++count;
    }
    if (mIsFocused == true)
    {
        messages[count] = makeFocusMessage();
        ++count;
    }
    if (mIsCloseRequested == true)
    {
        messages[count].Kind = MessageKind::Close;
        ++count;
    }
    // All or nothing, so the window never sees half a synchronization.
    const usize start = mPendingSize;
    for (usize i = 0; i < count; ++i)
    {
        if (enqueue(messages[i]) == false)
        {
            mPendingSize = start;
            return false;
        }
    }
    return true;
}

HostedEndpointStatus EndpointSession::send(const Message& message) noexcept
{
    if (mPhase != SessionPhase::Connected)
    {
        return HostedEndpointStatus::NotConnected;
    }
    if (mIsResynchronizationPending == true || enqueue(message) == false)
    {
        mIsResynchronizationPending = true;
        return HostedEndpointStatus::Overflow;
    }
    flush();
    return mPhase == SessionPhase::Connected ? HostedEndpointStatus::Ok : HostedEndpointStatus::NotConnected;
}

void EndpointSession::flush() noexcept
{
    while (mPhase == SessionPhase::Connected)
    {
        while (mPendingSize > 0)
        {
            usize sent = 0;
            const TransferStatus status = SendSome(mConnection.Get(), {mPending.GetData(), mPendingSize}, sent);
            if (status == TransferStatus::WouldBlock)
            {
                return;
            }
            if (status != TransferStatus::Progress)
            {
                disconnect();
                return;
            }
            std::memmove(mPending.GetData(), mPending.GetData() + sent, mPendingSize - sent);
            mPendingSize -= sent;
        }
        bool hasQueued = false;
        if (mIsResynchronizationPending == true)
        {
            if (enqueueSynchronization(true) == false)
            {
                return;
            }
            mIsResynchronizationPending = false;
            hasQueued = true;
        }
        hasQueued = flushDeferredReleases() == true || hasQueued == true;
        if (hasQueued == false)
        {
            return;
        }
    }
}

bool EndpointSession::flushDeferredReleases() noexcept
{
    usize queued = 0;
    while (queued < mDeferredReleaseCount)
    {
        Message message;
        message.Kind = MessageKind::Release;
        message.Generation = mDeferredReleases[queued].Generation;
        message.Image = mDeferredReleases[queued].Image;
        if (enqueue(message) == false)
        {
            break;
        }
        ++queued;
    }
    for (usize i = queued; i < mDeferredReleaseCount; ++i)
    {
        mDeferredReleases[i - queued] = mDeferredReleases[i];
    }
    mDeferredReleaseCount -= queued;
    return queued > 0;
}

HostedEndpointStatus EndpointSession::SendResize(uint32 width, uint32 height, float32 scale) noexcept
{
    if (mIsCloseRequested == true)
    {
        return HostedEndpointStatus::InvalidState;
    }
    if (width > MAX_EXTENT_PIXELS || height > MAX_EXTENT_PIXELS || IsValidScale(scale) == false)
    {
        return HostedEndpointStatus::InvalidArgument;
    }
    mState.Width = width;
    mState.Height = height;
    mState.Scale = scale;
    Message message;
    message.Kind = MessageKind::Resize;
    message.Width = width;
    message.Height = height;
    message.Scale = scale;
    return send(message);
}

HostedEndpointStatus EndpointSession::SendVisibility(WindowVisibility visibility) noexcept
{
    if (mIsCloseRequested == true)
    {
        return HostedEndpointStatus::InvalidState;
    }
    if (IsValidVisibility(visibility) == false)
    {
        return HostedEndpointStatus::InvalidArgument;
    }
    mState.Visibility = visibility;
    Message message;
    message.Kind = MessageKind::Visibility;
    message.Visibility = visibility;
    return send(message);
}

HostedEndpointStatus EndpointSession::SendFocusEntered(const input::FocusBaseline& baseline) noexcept
{
    if (mIsCloseRequested == true)
    {
        return HostedEndpointStatus::InvalidState;
    }
    mIsFocused = true;
    mHeldKeys = {};
    mHeldKeys.Focused = true;
    for (usize i = 0; i < input::KEY_COUNT; ++i)
    {
        mHeldKeys.Keys[i] = baseline.Keys[i] == true && input::IsValidKey(static_cast<input::Key>(i)) == true;
    }
    return send(makeFocusMessage());
}

HostedEndpointStatus EndpointSession::SendFocusLost() noexcept
{
    if (mIsCloseRequested == true)
    {
        return HostedEndpointStatus::InvalidState;
    }
    if (mIsFocused == false)
    {
        return HostedEndpointStatus::Ok;
    }
    mIsFocused = false;
    mHeldKeys = {};
    Message message;
    message.Kind = MessageKind::Reset;
    message.Reason = input::ResetReason::FocusLost;
    return send(message);
}

HostedEndpointStatus EndpointSession::SendKey(input::Key key, input::KeyTransition transition, bool isRepeat) noexcept
{
    if (input::IsValidKey(key) == false ||
        static_cast<uint8>(transition) > static_cast<uint8>(input::KeyTransition::Up))
    {
        return HostedEndpointStatus::InvalidArgument;
    }
    if (mIsCloseRequested == true || mIsFocused == false)
    {
        return HostedEndpointStatus::InvalidState;
    }
    mHeldKeys.Keys[input::KeyIndex(key)] = transition == input::KeyTransition::Down;
    Message message;
    message.Kind = MessageKind::Key;
    message.PhysicalKey = key;
    message.Transition = transition;
    message.IsRepeat = isRepeat;
    return send(message);
}

HostedEndpointStatus EndpointSession::SendClose() noexcept
{
    if (mIsCloseRequested == true)
    {
        return HostedEndpointStatus::InvalidState;
    }
    mIsCloseRequested = true;
    Message message;
    message.Kind = MessageKind::Close;
    return send(message);
}
} // namespace ludus::platform::hosted

namespace ludus::platform::hosted
{
HostedEndpointStatus EndpointSession::SendRelease(uint32 generation, uint32 image) noexcept
{
    if (generation == 0 || image >= HOSTED_SURFACE_IMAGE_COUNT)
    {
        return HostedEndpointStatus::InvalidArgument;
    }
    if (mPhase != SessionPhase::Connected)
    {
        return HostedEndpointStatus::NotConnected;
    }
    // A release is never dropped: the producer could not reuse the image. Releases
    // that do not fit wait, in order, until the queue drains.
    if (mIsResynchronizationPending == true || mDeferredReleaseCount > 0)
    {
        if (mDeferredReleaseCount == MAX_DEFERRED_RELEASES)
        {
            return HostedEndpointStatus::Overflow;
        }
        mDeferredReleases[mDeferredReleaseCount] = { .Generation = generation, .Image = image };
        ++mDeferredReleaseCount;
        flush();
        return mPhase == SessionPhase::Connected ? HostedEndpointStatus::Ok : HostedEndpointStatus::NotConnected;
    }
    Message message;
    message.Kind = MessageKind::Release;
    message.Generation = generation;
    message.Image = image;
    if (enqueue(message) == false)
    {
        mDeferredReleases[0] = { .Generation = generation, .Image = image };
        mDeferredReleaseCount = 1;
    }
    flush();
    return mPhase == SessionPhase::Connected ? HostedEndpointStatus::Ok : HostedEndpointStatus::NotConnected;
}
} // namespace ludus::platform::hosted
