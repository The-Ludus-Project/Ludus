#include "internal/session.hpp"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>

#include <cstring>
#include <span>

namespace ludus::network
{
namespace
{
using foundation::TryReadLittleEndian;
using foundation::TryWriteLittleEndian;
using foundation::time::TimeStatus;
using foundation::time::TryAdd;
using foundation::time::TryElapsed;

constexpr uint64 HANDSHAKE_EPOCH = 1;
constexpr uint16 HELLO_KIND = 1;
constexpr uint16 WELCOME_KIND = 2;
constexpr uint16 READY_KIND = 3;
constexpr usize SCHEMA_OFFSET = sizeof(SessionSettings::ProtocolVersion);
constexpr usize CONTENT_OFFSET = SCHEMA_OFFSET + sizeof(SessionSettings::SchemaFingerprint);
constexpr usize MESSAGE_BYTES_OFFSET = CONTENT_OFFSET + sizeof(SessionSettings::ContentFingerprint);
constexpr usize COMPATIBILITY_BYTES = MESSAGE_BYTES_OFFSET + sizeof(SessionSettings::MessageBytes);
constexpr usize HELLO_PAYLOAD_BYTES = COMPATIBILITY_BYTES + MAX_SESSION_TICKET_BYTES;

bool IsReliable(Channel lane) noexcept
{
    return lane == Channel::Control || lane == Channel::Bulk;
}

bool AreSettingsValid(const SessionSettings& settings) noexcept
{
    return settings.ProtocolVersion != 0 && settings.SchemaFingerprint != 0 && settings.ContentFingerprint != 0 &&
           settings.MessageBytes >= MESSAGE_HEADER_BYTES && settings.MessageBytes <= MAX_MESSAGE_BYTES &&
           settings.AdmissionTimeout.Nanoseconds != 0 && settings.SendStallTimeout.Nanoseconds != 0;
}

bool WriteCompatibility(const SessionSettings& settings, uint16 messageBytes, std::span<uint8> destination) noexcept
{
    return TryWriteLittleEndian(settings.ProtocolVersion, ludus::foundation::MakeBorrowedView(destination)) == true &&
           TryWriteLittleEndian(settings.SchemaFingerprint,
                                ludus::foundation::MakeBorrowedView(destination.subspan(SCHEMA_OFFSET))) == true &&
           TryWriteLittleEndian(settings.ContentFingerprint,
                                ludus::foundation::MakeBorrowedView(destination.subspan(CONTENT_OFFSET))) == true &&
           TryWriteLittleEndian(messageBytes,
                                ludus::foundation::MakeBorrowedView(destination.subspan(MESSAGE_BYTES_OFFSET))) == true;
}

bool ReadCompatibility(const SessionSettings& settings, std::span<const uint8> source, uint16& outMessageBytes) noexcept
{
    if (source.size() < COMPATIBILITY_BYTES)
    {
        return false;
    }
    uint32 protocol = 0;
    uint64 schema = 0;
    uint64 content = 0;
    uint16 messageBytes = 0;
    if (TryReadLittleEndian(ludus::foundation::MakeBorrowedView(source), protocol) == false ||
        TryReadLittleEndian(ludus::foundation::MakeBorrowedView(source.subspan(SCHEMA_OFFSET)), schema) == false ||
        TryReadLittleEndian(ludus::foundation::MakeBorrowedView(source.subspan(CONTENT_OFFSET)), content) == false ||
        TryReadLittleEndian(ludus::foundation::MakeBorrowedView(source.subspan(MESSAGE_BYTES_OFFSET)), messageBytes) ==
            false ||
        protocol != settings.ProtocolVersion || schema != settings.SchemaFingerprint ||
        content != settings.ContentFingerprint || messageBytes < MESSAGE_HEADER_BYTES ||
        messageBytes > MAX_MESSAGE_BYTES)
    {
        return false;
    }
    outMessageBytes = messageBytes;
    return true;
}

BorrowedView<const uint8> BorrowBytes(std::span<const uint8> bytes) noexcept
{
    return {bytes.data(), bytes.size()};
}
} // namespace

Session::Session(SessionTransport& inoutTransport) noexcept : mTransport(inoutTransport) {}

Session::~Session()
{
    Close();
}

bool Session::begin(const SessionSettings& settings, Timestamp now, const SessionCallbacks& callbacks) noexcept
{
    Timestamp deadline;
    if ((mState != SessionState::Idle && mState != SessionState::Closed) || mGeneration == ~uint64{0} ||
        now.Nanoseconds < mNow.Nanoseconds || AreSettingsValid(settings) == false ||
        callbacks.ValidateOrNull == nullptr || callbacks.DeliverOrNull == nullptr ||
        TryAdd(now, settings.AdmissionTimeout, deadline) != TimeStatus::Ok)
    {
        return false;
    }
    mSettings = settings;
    mCallbacks = callbacks;
    mCounters = {};
    mNow = now;
    mAdmissionDeadline = deadline;
    mMessageBytes = settings.MessageBytes;
    mEpoch = 0;
    mTicketBytes = 0;
    mWindows = {};
    mOutgoingSequences = {};
    mQueue.Reset();
    mPending.Size = 0;
    mEarlyBulkHead = 0;
    mEarlyBulkCount = 0;
    mIsSendBlocked = false;
    mEnd = SessionEnd::None;
    mState = SessionState::Connecting;
    ++mGeneration;
    return true;
}

bool Session::StartClient(const SessionSettings& settings,
                          Timestamp now,
                          BorrowedView<const uint8> ticket,
                          const SessionCallbacks& callbacks) noexcept
{
    if (ticket.IsValid() == false || ticket.IsEmpty() == true || ticket.GetSize() > mTicket.GetSize() ||
        begin(settings, now, callbacks) == false)
    {
        return false;
    }
    mTicketBytes = ticket.GetSize();
    std::memcpy(mTicket.GetData(), ticket.GetDataOrNull(), mTicketBytes);
    mHandshake = Handshake::SendHello;
    return true;
}

bool Session::StartServer(const SessionSettings& settings,
                          Timestamp now,
                          uint64 epoch,
                          const SessionCallbacks& callbacks) noexcept
{
    if (epoch <= HANDSHAKE_EPOCH || epoch <= mLastOfferedEpoch || callbacks.AdmitOrNull == nullptr ||
        begin(settings, now, callbacks) == false)
    {
        return false;
    }
    mEpoch = epoch;
    mLastOfferedEpoch = epoch;
    mHandshake = Handshake::WaitHello;
    return true;
}

void Session::Pump(Timestamp now, const SessionBudget& budget) noexcept
{
    if (mState == SessionState::Idle || mState == SessionState::Closed)
    {
        return;
    }
    if (now.Nanoseconds < mNow.Nanoseconds)
    {
        finish(SessionEnd::InvalidClock);
        return;
    }
    mNow = now;
    if (mState != SessionState::Active && now.Nanoseconds >= mAdmissionDeadline.Nanoseconds)
    {
        finish(SessionEnd::AdmissionTimeout);
        return;
    }
    const TransportState transportState = mTransport.Poll();
    if (transportState == TransportState::Rejected)
    {
        finish(SessionEnd::TransportRejected);
        return;
    }
    if (transportState == TransportState::Closed ||
        (mState != SessionState::Connecting && transportState != TransportState::Authenticated))
    {
        finish(SessionEnd::TransportClosed);
        return;
    }
    if (transportState != TransportState::Authenticated)
    {
        return;
    }
    if (mState == SessionState::Connecting)
    {
        mState = SessionState::Negotiating;
    }
    if (mIsSendBlocked == true && mState == SessionState::Active)
    {
        Duration blocked;
        if (TryElapsed(mBlockedSince, now, blocked) != TimeStatus::Ok ||
            blocked.Nanoseconds >= mSettings.SendStallTimeout.Nanoseconds)
        {
            finish(SessionEnd::SendStall);
            return;
        }
    }
    receiveFrames(budget);
    if (mState != SessionState::Closed)
    {
        sendFrames(now, budget);
    }
}

void Session::Close() noexcept
{
    if (mState != SessionState::Idle && mState != SessionState::Closed)
    {
        finish(SessionEnd::LocalClose);
    }
}

void Session::finish(SessionEnd reason) noexcept
{
    mTransport.Close();
    mTicket = {};
    mTicketBytes = 0;
    mPending = {};
    mQueue.Reset();
    mEarlyBulkCount = 0;
    mEnd = reason;
    mState = SessionState::Closed;
}

SessionSendResult Session::Queue(Channel lane, uint16 kind, BorrowedView<const uint8> payload) noexcept
{
    if (mState != SessionState::Active)
    {
        return SessionSendResult::Inactive;
    }
    const usize index = static_cast<usize>(lane);
    if (index >= CHANNEL_COUNT || kind == 0 || payload.IsValid() == false ||
        payload.GetSize() > mMessageBytes - MESSAGE_HEADER_BYTES)
    {
        return SessionSendResult::InvalidMessage;
    }
    OwnedMessage message;
    const MessageHeader header =
    {
        .Lane = lane,
        .Kind = kind,
        .Session = mEpoch,
        .Sequence = mOutgoingSequences[index],
    };
    if (EncodeMessage(header, {payload.GetDataOrNull(), payload.GetSize()}, message.Bytes, message.Size) !=
        WireStatus::Ok)
    {
        return SessionSendResult::InvalidMessage;
    }
    const QueueStatus result = mQueue.Enqueue(message.GetBytes());
    if (result == QueueStatus::Full)
    {
        return SessionSendResult::Full;
    }
    if (result != QueueStatus::Ok && result != QueueStatus::ReplacedSnapshot)
    {
        return SessionSendResult::InvalidMessage;
    }
    ++mOutgoingSequences[index];
    return result == QueueStatus::ReplacedSnapshot ? SessionSendResult::ReplacedSnapshot : SessionSendResult::Ok;
}

void Session::receiveFrames(const SessionBudget& budget) noexcept
{
    usize bytes = budget.ReceiveBytes;
    for (usize messageIndex = 0; messageIndex < budget.ReceiveMessages && mState != SessionState::Closed;
         ++messageIndex)
    {
        if (mState == SessionState::Active && mEarlyBulkCount != 0)
        {
            const OwnedMessage& saved = mEarlyBulk[mEarlyBulkHead];
            if (saved.Size > bytes)
            {
                return;
            }
            MessageView view;
            if (DecodeMessage(saved.GetBytes(), view) == WireStatus::Ok)
            {
                deliver(view);
            }
            bytes -= saved.Size;
            ++mEarlyBulkHead;
            --mEarlyBulkCount;
            continue;
        }
        // Receive is atomic. Reserve one maximum frame before taking backend ownership.
        if (bytes < MAX_MESSAGE_BYTES)
        {
            return;
        }
        ReceivedFrame frame;
        const TransportResult result = mTransport.Receive(frame);
        if (result == TransportResult::Empty)
        {
            return;
        }
        if (result != TransportResult::Ok || frame.Message.Size > MAX_MESSAGE_BYTES)
        {
            finish(SessionEnd::TransportClosed);
            return;
        }
        bytes -= frame.Message.Size;
        receiveFrame(frame);
    }
}

void Session::receiveFrame(const ReceivedFrame& frame) noexcept
{
    MessageView view;
    if (DecodeMessage(frame.Message.GetBytes(), view) != WireStatus::Ok || view.Header.Lane != frame.Lane)
    {
        ++mCounters.Rejected;
        return;
    }
    if (mState == SessionState::Negotiating)
    {
        receiveHandshake(frame, view);
        return;
    }
    if (view.Header.Session != mEpoch || frame.Message.Size > mMessageBytes)
    {
        ++mCounters.Rejected;
        return;
    }
    deliver(view);
}

void Session::receiveHandshake(const ReceivedFrame& frame, const MessageView& view) noexcept
{
    if (frame.Lane != Channel::Control)
    {
        // Bulk has independent reliable ordering and can overtake Ready.
        if (mHandshake == Handshake::WaitReady && frame.Lane == Channel::Bulk && view.Header.Session == mEpoch &&
            frame.Message.Size <= mMessageBytes)
        {
            if (mEarlyBulkCount == mEarlyBulk.GetSize())
            {
                finish(SessionEnd::IngressFull);
                return;
            }
            mEarlyBulk[mEarlyBulkCount] = frame.Message;
            ++mEarlyBulkCount;
        }
        else
        {
            ++mCounters.Rejected;
        }
        return;
    }
    if (view.Header.Sequence != 0)
    {
        finish(SessionEnd::InvalidHandshake);
        return;
    }
    if (mHandshake == Handshake::WaitReady && view.Header.Kind == READY_KIND && view.Header.Session == mEpoch &&
        view.Payload.empty() == true)
    {
        mState = SessionState::Active;
        return;
    }
    if (mHandshake == Handshake::WaitHello && view.Header.Kind == HELLO_KIND &&
        view.Header.Session == HANDSHAKE_EPOCH && view.Payload.size() > COMPATIBILITY_BYTES &&
        view.Payload.size() <= HELLO_PAYLOAD_BYTES)
    {
        uint16 peerBytes = 0;
        if (ReadCompatibility(mSettings, view.Payload, peerBytes) == false)
        {
            finish(SessionEnd::Incompatible);
            return;
        }
        if (mCallbacks.AdmitOrNull(mCallbacks.ContextOrNull,
                                   mTransport.GetPeerIdentity(),
                                   BorrowBytes(view.Payload.subspan(COMPATIBILITY_BYTES))) == false)
        {
            finish(SessionEnd::AdmissionRejected);
            return;
        }
        mMessageBytes = peerBytes < mMessageBytes ? peerBytes : mMessageBytes;
        mHandshake = Handshake::SendWelcome;
        return;
    }
    if (mHandshake == Handshake::WaitWelcome && view.Header.Kind == WELCOME_KIND &&
        view.Header.Session > HANDSHAKE_EPOCH && view.Header.Session > mLastAcceptedEpoch &&
        view.Payload.size() == COMPATIBILITY_BYTES)
    {
        uint16 peerBytes = 0;
        if (ReadCompatibility(mSettings, view.Payload, peerBytes) == false || peerBytes > mMessageBytes)
        {
            finish(SessionEnd::Incompatible);
            return;
        }
        mMessageBytes = peerBytes;
        mEpoch = view.Header.Session;
        mLastAcceptedEpoch = mEpoch;
        mHandshake = Handshake::SendReady;
        return;
    }
    finish(SessionEnd::InvalidHandshake);
}

void Session::deliver(const MessageView& view) noexcept
{
    const usize index = static_cast<usize>(view.Header.Lane);
    SequenceWindow window = mWindows[index];
    const SequenceStatus sequence = window.Observe(view.Header.Sequence);
    const bool isReorderable = view.Header.Lane == Channel::Input;
    if ((sequence != SequenceStatus::Newest && (sequence != SequenceStatus::OutOfOrder || isReorderable == false)) ||
        mCallbacks.ValidateOrNull(mCallbacks.ContextOrNull,
                                  view.Header.Lane,
                                  view.Header.Kind,
                                  BorrowBytes(view.Payload)) == false)
    {
        ++mCounters.Rejected;
        return;
    }
    mWindows[index] = window;
    ++mCounters.Delivered;
    mCallbacks.DeliverOrNull(mCallbacks.ContextOrNull, view.Header.Lane, view.Header.Kind, BorrowBytes(view.Payload));
}

void Session::sendFrames(Timestamp now, const SessionBudget& budget) noexcept
{
    usize bytes = budget.SendBytes;
    for (usize messageIndex = 0; messageIndex < budget.SendMessages; ++messageIndex)
    {
        const bool isHandshake = mState == SessionState::Negotiating;
        if (mPending.Size == 0)
        {
            if (isHandshake == true)
            {
                if (prepareHandshake() == false)
                {
                    return;
                }
            }
            else
            {
                if (mQueue.Dequeue(bytes, mPending) != QueueStatus::Ok)
                {
                    return;
                }
                MessageView queued;
                if (DecodeMessage(mPending.GetBytes(), queued) != WireStatus::Ok)
                {
                    finish(SessionEnd::TransportClosed);
                    return;
                }
                mPendingLane = queued.Header.Lane;
            }
        }
        if (mPending.Size > bytes)
        {
            return;
        }
        const TransportResult result = mTransport.Send(mPendingLane, BorrowBytes(mPending.GetBytes()));
        if (result == TransportResult::WouldBlock)
        {
            ++mCounters.BlockedSends;
            if (IsReliable(mPendingLane) == true)
            {
                if (mIsSendBlocked == false)
                {
                    mBlockedSince = now;
                    mIsSendBlocked = true;
                }
                return;
            }
            // Obsolete unreliable messages must not hold reliable traffic behind them.
            ++mCounters.DroppedUnreliableSends;
        }
        else if (result != TransportResult::Ok)
        {
            finish(SessionEnd::TransportClosed);
            return;
        }
        bytes -= mPending.Size;
        mPending.Size = 0;
        mIsSendBlocked = false;
        if (isHandshake == true)
        {
            advanceHandshakeAfterSend();
            return;
        }
    }
}

bool Session::prepareHandshake() noexcept
{
    StaticArray<uint8, HELLO_PAYLOAD_BYTES> payload{};
    std::span<uint8> payloadBytes{payload.GetData(), payload.GetSize()};
    MessageHeader header;
    header.Session = mEpoch;
    usize payloadSize = 0;
    switch (mHandshake)
    {
        case Handshake::SendHello:
            header.Kind = HELLO_KIND;
            header.Session = HANDSHAKE_EPOCH;
            std::memcpy(payload.GetData() + COMPATIBILITY_BYTES, mTicket.GetData(), mTicketBytes);
            payloadSize = COMPATIBILITY_BYTES + mTicketBytes;
            break;
        case Handshake::SendWelcome:
            header.Kind = WELCOME_KIND;
            payloadSize = COMPATIBILITY_BYTES;
            break;
        case Handshake::SendReady:
            header.Kind = READY_KIND;
            break;
        default:
            return false;
    }
    if ((payloadSize != 0 && WriteCompatibility(mSettings, mMessageBytes, payloadBytes) == false) ||
        EncodeMessage(header, payloadBytes.first(payloadSize), mPending.Bytes, mPending.Size) != WireStatus::Ok)
    {
        finish(SessionEnd::InvalidHandshake);
        return false;
    }
    mPendingLane = Channel::Control;
    return true;
}

void Session::advanceHandshakeAfterSend() noexcept
{
    switch (mHandshake)
    {
        case Handshake::SendHello:
            for (uint8& byte : mTicket)
            {
                byte = 0;
            }
            mTicketBytes = 0;
            // The transport has taken the encoded copy.
            for (uint8& byte : mPending.Bytes)
            {
                byte = 0;
            }
            mHandshake = Handshake::WaitWelcome;
            break;
        case Handshake::SendWelcome:
            mHandshake = Handshake::WaitReady;
            break;
        case Handshake::SendReady:
            mState = SessionState::Active;
            break;
        default:
            finish(SessionEnd::InvalidHandshake);
            break;
    }
}

SessionState Session::GetState() const noexcept
{
    return mState;
}

SessionEnd Session::GetEnd() const noexcept
{
    return mEnd;
}

uint64 Session::GetGeneration() const noexcept
{
    return mGeneration;
}

uint64 Session::GetEpoch() const noexcept
{
    return mEpoch;
}

const SessionCounters& Session::GetCounters() const noexcept
{
    return mCounters;
}
} // namespace ludus::network
