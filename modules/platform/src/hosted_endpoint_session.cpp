#include "internal/hosted_endpoint_session.hpp"

#include <cstring>

namespace ludus::platform::hosted
{
namespace
{
/// Welcome or overflow resize, visibility, overflow reset, focus and close.
constexpr usize MAX_SYNCHRONIZATION_MESSAGES = 5;
/// Bytes read per check for unexpected traffic from a connected window.
constexpr usize DISCONNECTION_PROBE_BYTES = 64;

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
    if (TryReadPeerUserId(mConnection.Get(), peerUserId) == false || peerUserId != mExpectedUserId)
    {
        refuse(HostedRefusal::WrongUser);
        return;
    }
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
        detectDisconnection();
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

void EndpointSession::detectDisconnection() noexcept
{
    foundation::StaticArray<uint8, DISCONNECTION_PROBE_BYTES> probe{};
    usize received = 0;
    const TransferStatus status = ReceiveSome(mConnection.Get(), {probe.GetData(), probe.GetSize()}, received);
    // A version-1 window sends nothing after its Hello, so any byte is a violation.
    if (status != TransferStatus::WouldBlock)
    {
        disconnect();
    }
}

void EndpointSession::disconnect() noexcept
{
    mConnection.Reset();
    mPhase = SessionPhase::Disconnected;
    mPendingSize = 0;
    mIsResynchronizationPending = false;
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
        if (mIsResynchronizationPending == false || enqueueSynchronization(true) == false)
        {
            return;
        }
        mIsResynchronizationPending = false;
    }
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
