#include "internal/session.hpp"

#include <ludus/network/core/link_simulator.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
using namespace ludus::network;
using ludus::foundation::time::Seconds;

constexpr uint16 APPLICATION_KIND = 7;
constexpr uint8 VALID_PAYLOAD = 9;
constexpr uint64 FIRST_EPOCH = 2;
constexpr uint64 NEXT_EPOCH = 3;
constexpr uint64 TICKET = 42;

struct SimulatedLinks final
{
    LinkSimulator& Ingress;
    LinkSimulator& Egress;
};

class SimulatedTransport final : public SessionTransport
{
public:
    explicit SimulatedTransport(SimulatedLinks links) noexcept : mLinks(links) {}

    TransportState Poll() noexcept override
    {
        return State;
    }

    [[nodiscard]] BorrowedView<const char> GetPeerIdentity() const noexcept override
    {
        return {"client", 6};
    }

    TransportResult Send(Channel, BorrowedView<const uint8> bytes) noexcept override
    {
        if (isSendBlocked == true)
        {
            return TransportResult::WouldBlock;
        }
        const LinkStatus status = mLinks.Egress.Send(NowUs, {bytes.GetDataOrNull(), bytes.GetSize()});
        if (status == LinkStatus::Ok || status == LinkStatus::Dropped)
        {
            return TransportResult::Ok;
        }
        return status == LinkStatus::Full ? TransportResult::WouldBlock : TransportResult::Failed;
    }

    TransportResult Receive(ReceivedFrame& outFrame) noexcept override
    {
        const LinkStatus status = mLinks.Ingress.Receive(NowUs, outFrame.Message);
        if (status == LinkStatus::Empty)
        {
            return TransportResult::Empty;
        }
        MessageView view;
        if (status != LinkStatus::Ok || DecodeMessage(outFrame.Message.GetBytes(), view) != WireStatus::Ok)
        {
            return TransportResult::Failed;
        }
        outFrame.Lane = view.Header.Lane;
        return TransportResult::Ok;
    }

    void Close() noexcept override
    {
        State = TransportState::Closed;
    }

    TransportState State = TransportState::Authenticated;
    bool isSendBlocked = false;
    uint64 NowUs = 0;

private:
    SimulatedLinks mLinks;
};

struct Application final
{
    uint64 Admissions = 0;
    uint64 Received = 0;
    bool hasTicketBeenConsumed = false;
};

bool AdmitTicket(void* contextOrNull, BorrowedView<const char> peer, BorrowedView<const uint8> ticket) noexcept
{
    Application& application = *static_cast<Application*>(contextOrNull);
    ++application.Admissions;
    if (application.hasTicketBeenConsumed == true || peer.GetSize() != 6 ||
        std::memcmp(peer.GetDataOrNull(), "client", peer.GetSize()) != 0 || ticket.GetSize() != 1 ||
        ticket[0] != TICKET)
    {
        return false;
    }
    application.hasTicketBeenConsumed = true;
    return true;
}

bool ValidatePayload(void*, Channel, uint16 kind, BorrowedView<const uint8> payload) noexcept
{
    return kind == APPLICATION_KIND && payload.GetSize() == 1 && payload[0] == VALID_PAYLOAD;
}

void DeliverPayload(void* contextOrNull, Channel, uint16, BorrowedView<const uint8>) noexcept
{
    ++static_cast<Application*>(contextOrNull)->Received;
}

struct Peers final
{
    LinkSimulator ToClient;
    LinkSimulator ToServer;
    SimulatedTransport ClientTransport{{ .Ingress = ToClient, .Egress = ToServer }};
    SimulatedTransport ServerTransport{{ .Ingress = ToServer, .Egress = ToClient }};
    Session Client{ClientTransport};
    Session Server{ServerTransport};
    Application ClientApplication;
    Application ServerApplication;
    SessionSettings Settings{ .ProtocolVersion = 1, .SchemaFingerprint = 2, .ContentFingerprint = 3 };
    SessionCallbacks ClientCallbacks{&ClientApplication, nullptr, ValidatePayload, DeliverPayload};
    SessionCallbacks ServerCallbacks{&ServerApplication, AdmitTicket, ValidatePayload, DeliverPayload};
    StaticArray<uint8, 1> Ticket{{TICKET}};

    void Start(uint64 epoch)
    {
        REQUIRE(Client.StartClient(Settings, {}, {Ticket.GetData(), Ticket.GetSize()}, ClientCallbacks));
        REQUIRE(Server.StartServer(Settings, {}, epoch, ServerCallbacks));
    }

    void CompleteAdmission()
    {
        Client.Pump({});
        Server.Pump({});
        Client.Pump({});
        Server.Pump({});
        REQUIRE(Client.GetState() == SessionState::Active);
        REQUIRE(Server.GetState() == SessionState::Active);
    }

    void SendDirect(Channel lane, uint64 epoch, uint32 sequence, uint8 payload)
    {
        OwnedMessage message;
        REQUIRE(EncodeMessage({lane, APPLICATION_KIND, epoch, sequence}, {&payload, 1}, message.Bytes, message.Size) ==
                WireStatus::Ok);
        REQUIRE(ToServer.Send(0, message.GetBytes()) == LinkStatus::Ok);
    }
};
} // namespace

TEST_CASE("Session authenticates before exchanging a ticket and compatibility", "[network]")
{
    Peers peers;
    peers.Start(FIRST_EPOCH);
    peers.ClientTransport.State = TransportState::Connecting;
    peers.Client.Pump({});
    CHECK(peers.ToServer.GetPendingCount() == 0);
    CHECK(peers.Client.Queue(Channel::Control, APPLICATION_KIND, {}) == SessionSendResult::Inactive);
    peers.ClientTransport.State = TransportState::Authenticated;
    peers.CompleteAdmission();
    CHECK(peers.ServerApplication.Admissions == 1);
    CHECK(peers.Client.GetEpoch() == FIRST_EPOCH);
    CHECK(peers.Server.GetEpoch() == FIRST_EPOCH);

    Peers incompatible;
    REQUIRE(incompatible.Server.StartServer(incompatible.Settings, {}, FIRST_EPOCH, incompatible.ServerCallbacks));
    ++incompatible.Settings.ProtocolVersion;
    REQUIRE(incompatible.Client.StartClient(incompatible.Settings,
                                            {},
                                            {incompatible.Ticket.GetData(), 1},
                                            incompatible.ClientCallbacks));
    incompatible.Client.Pump({});
    incompatible.Server.Pump({});
    CHECK(incompatible.Server.GetEnd() == SessionEnd::Incompatible);
    CHECK(incompatible.ServerApplication.Admissions == 0);
}

TEST_CASE("Session validates before sequence commit and rejects stale epochs and duplicates", "[network]")
{
    Peers peers;
    peers.Start(FIRST_EPOCH);
    peers.CompleteAdmission();
    REQUIRE(peers.ToServer.Reset({ .DuplicatePerTenThousand = 10'000 }) == LinkStatus::Ok);
    peers.SendDirect(Channel::Input, FIRST_EPOCH, 90, 0);
    peers.SendDirect(Channel::Input, FIRST_EPOCH, 1, VALID_PAYLOAD);
    peers.Server.Pump({});
    CHECK(peers.ServerApplication.Received == 1);
    CHECK(peers.Server.GetCounters().Rejected == 3);

    peers.Server.Close();
    peers.Client.Close();
    peers.ServerApplication.hasTicketBeenConsumed = false;
    peers.ServerTransport.State = TransportState::Authenticated;
    peers.ClientTransport.State = TransportState::Authenticated;
    REQUIRE(peers.ToServer.Reset({}) == LinkStatus::Ok);
    CHECK_FALSE(peers.Server.StartServer(peers.Settings, {}, FIRST_EPOCH, peers.ServerCallbacks));
    peers.Start(NEXT_EPOCH);
    peers.CompleteAdmission();
    peers.SendDirect(Channel::Input, FIRST_EPOCH, 0, VALID_PAYLOAD);
    peers.SendDirect(Channel::Input, NEXT_EPOCH, 0, VALID_PAYLOAD);
    peers.Server.Pump({});
    CHECK(peers.Server.GetCounters().Rejected == 1);
    CHECK(peers.ServerApplication.Received == 2);
    CHECK(peers.Server.GetGeneration() == 2);
    CHECK(peers.Client.GetGeneration() == 2);
}

TEST_CASE("Session holds reliable sends on backpressure and enforces the stall deadline", "[network]")
{
    Peers peers;
    peers.Start(FIRST_EPOCH);
    peers.CompleteAdmission();
    const uint8 payload = VALID_PAYLOAD;
    peers.ClientTransport.isSendBlocked = true;
    REQUIRE(peers.Client.Queue(Channel::Control, APPLICATION_KIND, {&payload, 1}) == SessionSendResult::Ok);
    peers.Client.Pump({});
    CHECK(peers.Client.GetCounters().BlockedSends == 1);
    CHECK(peers.ToServer.GetPendingCount() == 0);
    peers.ClientTransport.isSendBlocked = false;
    peers.Client.Pump({});
    peers.Server.Pump({});
    CHECK(peers.ServerApplication.Received == 1);
    CHECK(peers.Client.GetState() == SessionState::Active);

    peers.ClientTransport.isSendBlocked = true;
    REQUIRE(peers.Client.Queue(Channel::Bulk, APPLICATION_KIND, {&payload, 1}) == SessionSendResult::Ok);
    peers.Client.Pump({});
    peers.Client.Pump({peers.Settings.SendStallTimeout.Nanoseconds});
    CHECK(peers.Client.GetEnd() == SessionEnd::SendStall);
    CHECK(peers.ToServer.GetPendingCount() == 0);
}

TEST_CASE("Session applies receive budgets and bounds admission time", "[network]")
{
    Peers peers;
    peers.Start(FIRST_EPOCH);
    peers.CompleteAdmission();
    peers.SendDirect(Channel::Input, FIRST_EPOCH, 0, VALID_PAYLOAD);
    peers.SendDirect(Channel::Input, FIRST_EPOCH, 1, VALID_PAYLOAD);
    peers.Server.Pump({}, { .ReceiveMessages = 1, .ReceiveBytes = MAX_MESSAGE_BYTES });
    CHECK(peers.ServerApplication.Received == 1);
    CHECK(peers.ToServer.GetPendingCount() == 1);
    peers.Server.Pump({});
    CHECK(peers.ServerApplication.Received == 2);

    Peers waiting;
    waiting.ServerTransport.State = TransportState::Connecting;
    waiting.Start(FIRST_EPOCH);
    waiting.Server.Pump({Seconds(10).Nanoseconds});
    CHECK(waiting.Server.GetEnd() == SessionEnd::AdmissionTimeout);
    CHECK(waiting.ServerApplication.Admissions == 0);
}

TEST_CASE("Reliable bulk can overtake Ready without reaching the app before admission", "[network]")
{
    Peers peers;
    peers.Start(FIRST_EPOCH);
    peers.Client.Pump({});
    peers.Server.Pump({});

    // Simulate a short cross-lane reorder on a connection with an agreed epoch.
    peers.SendDirect(Channel::Bulk, FIRST_EPOCH, 0, VALID_PAYLOAD);
    peers.Server.Pump({});
    REQUIRE(peers.Server.GetState() == SessionState::Negotiating);
    CHECK(peers.ServerApplication.Received == 0);
    peers.Client.Pump({});
    peers.Server.Pump({});
    CHECK(peers.Server.GetState() == SessionState::Active);
    CHECK(peers.ServerApplication.Received == 1);
}
