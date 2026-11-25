#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/network/core/send_queue.h>
#include <ludus/network/core/sequence.h>

namespace ludus::network
{
using foundation::BorrowedView;
using foundation::StaticArray;
using foundation::time::Duration;
using foundation::time::Timestamp;

enum class TransportState : uint8
{
    Connecting,
    Authenticated,
    Closed,
    Rejected,
};

enum class TransportResult : uint8
{
    Ok,
    Empty,
    WouldBlock,
    Failed,
};

struct ReceivedFrame final
{
    OwnedMessage Message;
    Channel Lane = Channel::Control;
};

// Driven by one owner. Receive copies a whole frame and preserves output on failure.
// Send copies on Ok only. WouldBlock leaves ownership with the caller.
class SessionTransport
{
public:
    virtual ~SessionTransport() = default;
    virtual TransportState Poll() noexcept = 0;
    [[nodiscard]] virtual BorrowedView<const char> GetPeerIdentity() const noexcept = 0;
    virtual TransportResult Receive(ReceivedFrame& outFrame) noexcept = 0;
    virtual TransportResult Send(Channel lane, BorrowedView<const uint8> bytes) noexcept = 0;
    virtual void Close() noexcept = 0;
};

inline constexpr usize MAX_SESSION_TICKET_BYTES = 256;
inline constexpr Duration DEFAULT_ADMISSION_TIMEOUT = foundation::time::Seconds(5);
inline constexpr Duration DEFAULT_SEND_STALL_TIMEOUT = foundation::time::Seconds(5);

struct SessionSettings final
{
    uint32 ProtocolVersion = 0;
    uint64 SchemaFingerprint = 0;
    uint64 ContentFingerprint = 0;
    uint16 MessageBytes = MAX_MESSAGE_BYTES;
    Duration AdmissionTimeout = DEFAULT_ADMISSION_TIMEOUT;
    Duration SendStallTimeout = DEFAULT_SEND_STALL_TIMEOUT;
};

struct SessionBudget final
{
    // A backend message is atomic. Remaining byte budget below
    // MAX_MESSAGE_BYTES defers the next backend receive to a later pump.
    usize ReceiveMessages = SEND_QUEUE_CAPACITY_PER_CHANNEL;
    usize ReceiveBytes = ReceiveMessages * MAX_MESSAGE_BYTES;
    usize SendMessages = SEND_QUEUE_CAPACITY_PER_CHANNEL;
    usize SendBytes = SendMessages * MAX_MESSAGE_BYTES;
};

struct SessionCallbacks final
{
    void* ContextOrNull = nullptr;
    // The server policy must verify expiry, audience, replay and identity against the authenticated peer.
    // Called once for a well-formed compatible Hello. The ticket is borrowed for the call.
    bool (*AdmitOrNull)(void* contextOrNull,
                        BorrowedView<const char> peer,
                        BorrowedView<const uint8> ticket) noexcept = nullptr;
    // Validate must leave application state untouched on rejection. Deliver runs after sequence commit.
    // The payload is borrowed for the callback. Callbacks must not re-enter this Session.
    bool (*ValidateOrNull)(void* contextOrNull,
                           Channel lane,
                           uint16 kind,
                           BorrowedView<const uint8> payload) noexcept = nullptr;
    void (*DeliverOrNull)(void* contextOrNull,
                          Channel lane,
                          uint16 kind,
                          BorrowedView<const uint8> payload) noexcept = nullptr;
};

enum class SessionState : uint8
{
    Idle,
    Connecting,
    Negotiating,
    Active,
    Closed,
};

enum class SessionEnd : uint8
{
    None,
    LocalClose,
    TransportClosed,
    TransportRejected,
    InvalidHandshake,
    Incompatible,
    AdmissionRejected,
    AdmissionTimeout,
    SendStall,
    IngressFull,
    InvalidClock,
};

enum class SessionSendResult : uint8
{
    Ok,
    ReplacedSnapshot,
    Full,
    Inactive,
    InvalidMessage,
};

struct SessionCounters final
{
    uint64 Delivered = 0;
    uint64 Rejected = 0;
    uint64 BlockedSends = 0;
    uint64 DroppedUnreliableSends = 0;
};

// One connection at a time, driven by a single caller. The transport and callback
// context outlive the attempt. Start copies the ticket and preserves state on failure.
class Session final
{
public:
    explicit Session(SessionTransport& inoutTransport) noexcept;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool StartClient(const SessionSettings& settings,
                     Timestamp now,
                     BorrowedView<const uint8> ticket,
                     const SessionCallbacks& callbacks) noexcept;
    // The server supplies a nonzero epoch greater than the handshake marker and
    // all epochs previously offered by this owner, including failed attempts.
    bool StartServer(const SessionSettings& settings,
                     Timestamp now,
                     uint64 epoch,
                     const SessionCallbacks& callbacks) noexcept;
    void Pump(Timestamp now, const SessionBudget& budget = {}) noexcept;
    void Close() noexcept;
    SessionSendResult Queue(Channel lane, uint16 kind, BorrowedView<const uint8> payload) noexcept;
    [[nodiscard]] SessionState GetState() const noexcept;
    [[nodiscard]] SessionEnd GetEnd() const noexcept;
    [[nodiscard]] uint64 GetGeneration() const noexcept;
    [[nodiscard]] uint64 GetEpoch() const noexcept;
    [[nodiscard]] const SessionCounters& GetCounters() const noexcept;

private:
    enum class Handshake : uint8
    {
        SendHello,
        WaitHello,
        SendWelcome,
        WaitWelcome,
        SendReady,
        WaitReady,
    };

    bool begin(const SessionSettings& settings, Timestamp now, const SessionCallbacks& callbacks) noexcept;
    void finish(SessionEnd reason) noexcept;
    void receiveFrames(const SessionBudget& budget) noexcept;
    void receiveFrame(const ReceivedFrame& frame) noexcept;
    void receiveHandshake(const ReceivedFrame& frame, const MessageView& view) noexcept;
    void deliver(const MessageView& view) noexcept;
    void sendFrames(Timestamp now, const SessionBudget& budget) noexcept;
    bool prepareHandshake() noexcept;
    void advanceHandshakeAfterSend() noexcept;

    SessionTransport& mTransport;
    SessionState mState = SessionState::Idle;
    SessionEnd mEnd = SessionEnd::None;
    Handshake mHandshake = Handshake::WaitHello;
    SessionSettings mSettings;
    SessionCallbacks mCallbacks;
    SessionCounters mCounters;
    Timestamp mNow;
    Timestamp mAdmissionDeadline;
    Timestamp mBlockedSince;
    uint64 mGeneration = 0;
    uint64 mEpoch = 0;
    uint64 mLastOfferedEpoch = 0;
    uint64 mLastAcceptedEpoch = 0;
    uint16 mMessageBytes = 0;
    StaticArray<uint8, MAX_SESSION_TICKET_BYTES> mTicket{};
    usize mTicketBytes = 0;
    StaticArray<SequenceWindow, CHANNEL_COUNT> mWindows{};
    StaticArray<uint32, CHANNEL_COUNT> mOutgoingSequences{};
    SendQueue mQueue;
    OwnedMessage mPending;
    Channel mPendingLane = Channel::Control;
    bool mIsSendBlocked = false;
    StaticArray<OwnedMessage, SEND_QUEUE_CAPACITY_PER_CHANNEL> mEarlyBulk{};
    usize mEarlyBulkHead = 0;
    usize mEarlyBulkCount = 0;
};
} // namespace ludus::network
