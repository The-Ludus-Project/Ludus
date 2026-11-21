#include "native_transport.hpp"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <ctime>
#include <string>

#include <openssl/evp.h>
#include <steam/isteamnetworkingutils.h>

#include "steamnetworkingsockets_messages_certs.pb.h"

namespace
{
using namespace ludus::network;
using ludus::foundation::uint16;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;
using ludus::foundation::threading::SleepFor;
using ludus::foundation::time::Milliseconds;
using ludus::foundation::time::Now;
using ludus::foundation::time::Seconds;
using ludus::foundation::time::TimeStatus;
using ludus::foundation::time::TryAdd;

constexpr char CLIENT_IDENTITY[] = "n2-client";
constexpr char SERVER_IDENTITY[] = "n2-server";
constexpr char OTHER_IDENTITY[] = "n2-other";
constexpr uint64 FIRST_EPOCH = 2;
constexpr uint16 APP_KIND = 7;
constexpr uint32 CERTIFICATE_APP = 0; // Pinned GNS standalone application's default AppID.
constexpr usize ED25519_PUBLIC_KEY_BYTES = 32;
constexpr usize ED25519_SIGNATURE_BYTES = 64;
constexpr usize SHA256_DIGEST_BYTES = 32;

// Fixture-only CA. Use Valve's generated certificate schema and OpenSSL's
// Ed25519 implementation. A fresh CA and keys are generated on every run.
struct KeyDeleter final
{
    void operator()(EVP_PKEY* key) const noexcept
    {
        EVP_PKEY_free(key);
    }
};

struct DigestDeleter final
{
    void operator()(EVP_MD_CTX* digest) const noexcept
    {
        EVP_MD_CTX_free(digest);
    }
};

class TestIssuer final
{
public:
    TestIssuer()
    {
        mKey.Reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"));
        REQUIRE(mKey.Get() != nullptr);
        StaticArray<uint8, ED25519_PUBLIC_KEY_BYTES> publicKey{};
        usize written = publicKey.GetSize();
        REQUIRE(EVP_PKEY_get_raw_public_key(mKey.Get(), publicKey.GetData(), &written) == 1);
        REQUIRE(written == publicKey.GetSize());
        StaticArray<uint8, SHA256_DIGEST_BYTES> digest{};
        unsigned digestBytes = 0;
        REQUIRE(EVP_Digest(publicKey.GetData(), written, digest.GetData(), &digestBytes, EVP_sha256(), nullptr) == 1);
        REQUIRE(digestBytes == digest.GetSize());
        // Valve's certificate key ID is the first eight SHA-256 bytes in little-endian order.
        REQUIRE(ludus::foundation::TryReadLittleEndian({digest.GetData(), digest.GetSize()}, mKeyId));
        CMsgSteamDatagramCertificate root;
        root.set_key_type(CMsgSteamDatagramCertificate::ED25519);
        root.set_key_data(publicKey.GetData(), publicKey.GetSize());
        const std::string encoded = Sign(root);
        mRoot.resize(4 * ((encoded.size() + 2) / 3) + 1);
        const int base64Bytes = EVP_EncodeBlock(reinterpret_cast<uint8*>(mRoot.data()),
                                                reinterpret_cast<const uint8*>(encoded.data()),
                                                static_cast<int>(encoded.size()));
        REQUIRE(base64Bytes > 0);
        mRoot.resize(static_cast<usize>(base64Bytes));
    }

    const char* GetRoot() const noexcept
    {
        return mRoot.c_str();
    }

    void Certify(NativeTransport& inoutTransport) const
    {
        StaticArray<uint8, MAX_MESSAGE_BYTES> requestBytes{};
        usize size = 0;
        REQUIRE(inoutTransport.RequestCertificate({requestBytes.GetData(), requestBytes.GetSize()}, size));
        CMsgSteamDatagramCertificateRequest request;
        REQUIRE(request.ParseFromArray(requestBytes.GetData(), static_cast<int>(size)));
        REQUIRE(request.has_cert());
        const std::string certificate = Sign(request.cert());
        REQUIRE(
            inoutTransport.SetCertificate({reinterpret_cast<const uint8*>(certificate.data()), certificate.size()}));
    }

private:
    std::string Sign(CMsgSteamDatagramCertificate certificate) const
    {
        const std::time_t now = std::time(nullptr);
        REQUIRE(now > 0);
        certificate.set_time_created(static_cast<uint32>(now - 1));
        certificate.set_time_expiry(static_cast<uint32>(now + 600));
        certificate.add_app_ids(CERTIFICATE_APP);

        CMsgSteamDatagramCertificateSigned issued;
        REQUIRE(certificate.SerializeToString(issued.mutable_cert()));
        issued.set_ca_key_id(mKeyId);
        ludus::foundation::UniquePtr<EVP_MD_CTX, DigestDeleter> digest;
        digest.Reset(EVP_MD_CTX_new());
        REQUIRE(digest.Get() != nullptr);
        REQUIRE(EVP_DigestSignInit(digest.Get(), nullptr, nullptr, nullptr, mKey.Get()) == 1);
        StaticArray<uint8, ED25519_SIGNATURE_BYTES> signature{};
        usize size = signature.GetSize();
        REQUIRE(EVP_DigestSign(digest.Get(),
                               signature.GetData(),
                               &size,
                               reinterpret_cast<const uint8*>(issued.cert().data()),
                               issued.cert().size()) == 1);
        REQUIRE(size == signature.GetSize());
        issued.set_ca_signature(signature.GetData(), size);
        std::string bytes;
        REQUIRE(issued.SerializeToString(&bytes));
        return bytes;
    }

    ludus::foundation::UniquePtr<EVP_PKEY, KeyDeleter> mKey;
    uint64 mKeyId = 0;
    std::string mRoot;
};

struct TestTicket final
{
    uint8 Identifier = 1;
    uint8 Audience = 2;
    Timestamp Expires{};
    bool IsConsumed = false;
};

inline constexpr usize TRACKED_MESSAGES = 32;
struct TestApplication final
{
    TestTicket Ticket;
    uint32 AdmissionAttempts = 0;
    usize Received = 0;
    StaticArray<uint8, TRACKED_MESSAGES> MessageIds{};
    StaticArray<Channel, TRACKED_MESSAGES> Lanes{};
};

bool AdmitTicket(void* contextOrNull, BorrowedView<const char> peer, BorrowedView<const uint8> ticket) noexcept
{
    TestApplication& application = *static_cast<TestApplication*>(contextOrNull);
    ++application.AdmissionAttempts;
    constexpr usize CLIENT_IDENTITY_LENGTH = sizeof(CLIENT_IDENTITY) - 1;
    if (peer.GetSize() != CLIENT_IDENTITY_LENGTH ||
        std::memcmp(peer.GetDataOrNull(), CLIENT_IDENTITY, peer.GetSize()) != 0 || ticket.GetSize() != 2 ||
        ticket[0] != application.Ticket.Identifier || ticket[1] != application.Ticket.Audience ||
        application.Ticket.IsConsumed == true || Now().Nanoseconds >= application.Ticket.Expires.Nanoseconds)
    {
        return false;
    }
    application.Ticket.IsConsumed = true;
    return true;
}

bool ValidateMessage(void*, Channel, uint16 kind, BorrowedView<const uint8> payload) noexcept
{
    return kind == APP_KIND && payload.IsEmpty() == false;
}

void DeliverMessage(void* contextOrNull, Channel lane, uint16, BorrowedView<const uint8> payload) noexcept
{
    TestApplication& application = *static_cast<TestApplication*>(contextOrNull);
    if (application.Received < TRACKED_MESSAGES)
    {
        application.MessageIds[application.Received] = payload[0];
        application.Lanes[application.Received] = lane;
    }
    ++application.Received;
}

class Fixture final
{
public:
    Fixture()
    {
        REQUIRE(Runtime.Open(Issuer.GetRoot()));
        REQUIRE(ServerTransport.Open(SERVER_IDENTITY, CLIENT_IDENTITY));
        REQUIRE(ClientTransport.Open(CLIENT_IDENTITY, SERVER_IDENTITY));
        Issuer.Certify(ServerTransport);
        Issuer.Certify(ClientTransport);

        // GNS 1.6.1 requires an explicit listener port. Retry a bounded range
        // in case another process owns the first candidate.
        constexpr uint16 FIRST_PORT = 49'152;
        constexpr uint16 PORT_RANGE = 16'000;
        constexpr uint32 IPV4_LOOPBACK = 0x7f000001;
        const uint64 seed = Now().Nanoseconds;
        for (uint16 offset = 0; offset < 32; ++offset)
        {
            const uint16 port = static_cast<uint16>(FIRST_PORT + (seed + offset) % PORT_RANGE);
            Address.SetIPv4(IPV4_LOOPBACK, port);
            if (ServerTransport.Listen(Address) == true)
            {
                return;
            }
        }
        FAIL("Could not bind a real GameNetworkingSockets UDP listener on loopback");
    }

    ~Fixture()
    {
        static_cast<void>(
            SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 0.0F));
    }

    void Start(uint64 epoch, uint8 identifier = 1, uint8 audience = 2)
    {
        const Timestamp now = Now();
        const StaticArray<uint8, 2> ticket{{identifier, audience}};
        REQUIRE(Server.StartServer(Settings, now, epoch, ServerCallbacks));
        REQUIRE(ClientTransport.Connect(Address));
        REQUIRE(Client.StartClient(Settings, now, {ticket.GetData(), ticket.GetSize()}, ClientCallbacks));
    }

    void IssueTicket(uint8 identifier)
    {
        ServerApplication.Ticket.Identifier = identifier;
        ServerApplication.Ticket.IsConsumed = false;
        REQUIRE(TryAdd(Now(), Seconds(10), ServerApplication.Ticket.Expires) == TimeStatus::Ok);
    }

    template <typename Predicate>
    bool WaitUntil(Predicate condition, Duration limit = Seconds(4))
    {
        Timestamp deadline;
        REQUIRE(TryAdd(Now(), limit, deadline) == TimeStatus::Ok);
        for (Timestamp now = Now(); now.Nanoseconds < deadline.Nanoseconds; now = Now())
        {
            Server.Pump(now);
            Client.Pump(now);
            if (condition() == true)
            {
                return true;
            }
            SleepFor(Milliseconds(2));
        }
        return false;
    }

    bool IsActive() const
    {
        return Client.GetState() == SessionState::Active && Server.GetState() == SessionState::Active;
    }

    void CompleteAdmission()
    {
        const bool isSettled = WaitUntil([this] {
            return IsActive() || Client.GetState() == SessionState::Closed || Server.GetState() == SessionState::Closed;
        });
        INFO("client end " << static_cast<usize>(Client.GetEnd()) << ", server end "
                           << static_cast<usize>(Server.GetEnd()));
        REQUIRE(isSettled);
        REQUIRE(IsActive());
    }

    void CloseBoth()
    {
        Client.Close();
        Server.Close();
    }

    TestIssuer Issuer;
    NativeRuntime Runtime;
    NativeTransport ServerTransport{Runtime};
    NativeTransport ClientTransport{Runtime};
    Session Server{ServerTransport};
    Session Client{ClientTransport};
    SessionSettings Settings{ .ProtocolVersion = 1, .SchemaFingerprint = 2, .ContentFingerprint = 3 };
    TestApplication ServerApplication;
    TestApplication ClientApplication;
    SessionCallbacks ServerCallbacks{&ServerApplication, AdmitTicket, ValidateMessage, DeliverMessage};
    SessionCallbacks ClientCallbacks{&ClientApplication, nullptr, ValidateMessage, DeliverMessage};
    SteamNetworkingIPAddr Address{};
};
} // namespace

TEST_CASE("Native admission requires the signed peer and an unexpired audience-bound one-use ticket", "[network]")
{
    Fixture peers;
    peers.Start(FIRST_EPOCH);
    REQUIRE(peers.WaitUntil([&peers] { return peers.Server.GetEnd() != SessionEnd::None; }));
    CHECK(peers.Server.GetEnd() == SessionEnd::AdmissionRejected);
    CHECK(peers.ServerApplication.AdmissionAttempts == 1);
    CHECK(peers.ServerApplication.Received == 0);
    peers.CloseBoth();

    peers.IssueTicket(1);
    peers.Start(FIRST_EPOCH + 1, 1, 0);
    REQUIRE(peers.WaitUntil([&peers] { return peers.Server.GetEnd() != SessionEnd::None; }));
    CHECK(peers.Server.GetEnd() == SessionEnd::AdmissionRejected);
    CHECK_FALSE(peers.ServerApplication.Ticket.IsConsumed);
    peers.CloseBoth();

    peers.Start(FIRST_EPOCH + 2);
    peers.CompleteAdmission();
    CHECK(peers.ServerApplication.Ticket.IsConsumed);
    CHECK(peers.Client.GetEpoch() == FIRST_EPOCH + 2);
    peers.CloseBoth();

    peers.Start(FIRST_EPOCH + 3);
    REQUIRE(peers.WaitUntil([&peers] { return peers.Server.GetEnd() != SessionEnd::None; }));
    CHECK(peers.Server.GetEnd() == SessionEnd::AdmissionRejected);
    CHECK(peers.ServerApplication.AdmissionAttempts == 4);
    CHECK(peers.ServerApplication.Received == 0);
}

TEST_CASE("Native listener rejects an authenticated certificate for a different peer", "[network]")
{
    Fixture peers;
    NativeTransport other(peers.Runtime);
    REQUIRE(other.Open(OTHER_IDENTITY, SERVER_IDENTITY));
    peers.Issuer.Certify(other);
    REQUIRE(peers.Server.StartServer(peers.Settings, Now(), FIRST_EPOCH, peers.ServerCallbacks));
    REQUIRE(other.Connect(peers.Address));
    REQUIRE(peers.WaitUntil([&] {
        static_cast<void>(other.Poll());
        return peers.ServerTransport.GetRejectedPeerCount() == 1;
    }));
    CHECK(peers.Server.GetState() == SessionState::Connecting);
    CHECK(peers.ServerApplication.AdmissionAttempts == 0);
    CHECK(peers.ServerApplication.Received == 0);
}

TEST_CASE("Native listener never admits an unsigned UDP peer", "[network]")
{
    Fixture peers;
    peers.Settings.AdmissionTimeout = Milliseconds(500);
    REQUIRE(peers.Server.StartServer(peers.Settings, Now(), FIRST_EPOCH, peers.ServerCallbacks));
    // The unused default GNS interface has no signed certificate. Only this
    // negative fixture dials using Valve's anonymous handshake behavior.
    const HSteamNetConnection unsignedPeer = SteamNetworkingSockets()->ConnectByIPAddress(peers.Address, 0, nullptr);
    REQUIRE(unsignedPeer != k_HSteamNetConnection_Invalid);
    REQUIRE(peers.WaitUntil([&peers] { return peers.Server.GetEnd() != SessionEnd::None; }));
    CHECK(peers.Server.GetEnd() == SessionEnd::AdmissionTimeout);
    CHECK(peers.ServerApplication.AdmissionAttempts == 0);
    CHECK(peers.ServerApplication.Received == 0);
    static_cast<void>(
        SteamNetworkingSockets()->CloseConnection(unsignedPeer, k_ESteamNetConnectionEnd_App_Generic, nullptr, false));
}

TEST_CASE("Real UDP recovers reliable loss and drains backpressure, then reconnects with a fresh epoch", "[network]")
{
    Fixture peers;
    peers.IssueTicket(1);
    peers.Start(FIRST_EPOCH);
    peers.CompleteAdmission();
    const uint8 input = 99;
    REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 100.0F));
    REQUIRE(peers.Client.Queue(Channel::Input, APP_KIND, {&input, 1}) == SessionSendResult::Ok);

    constexpr usize PAYLOAD_BYTES = MAX_MESSAGE_BYTES - MESSAGE_HEADER_BYTES;
    StaticArray<uint8, PAYLOAD_BYTES> payload{};
    for (usize message = 0; message < SEND_QUEUE_CAPACITY_PER_CHANNEL; ++message)
    {
        payload[0] = static_cast<uint8>(message);
        REQUIRE(peers.Client.Queue(Channel::Bulk, APP_KIND, {payload.GetData(), payload.GetSize()}) ==
                SessionSendResult::Ok);
    }
    CHECK(peers.Client.Queue(Channel::Bulk, APP_KIND, {payload.GetData(), payload.GetSize()}) ==
          SessionSendResult::Full);

    REQUIRE(peers.WaitUntil([&peers] {
        SteamNetConnectionRealTimeStatus_t stats{};
        return peers.ClientTransport.ReadStats(stats) == true && stats.m_cbPendingUnreliable == 0 &&
               stats.m_cbSentUnackedReliable > 0 && peers.Client.GetCounters().BlockedSends > 0;
    }));
    CHECK(peers.ServerApplication.Received == 0);
    REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 0.0F));
    REQUIRE(peers.WaitUntil([&peers] { return peers.ServerApplication.Received >= SEND_QUEUE_CAPACITY_PER_CHANNEL; }));
    REQUIRE(peers.ServerApplication.Received == SEND_QUEUE_CAPACITY_PER_CHANNEL);
    for (usize message = 0; message < SEND_QUEUE_CAPACITY_PER_CHANNEL; ++message)
    {
        CHECK(peers.ServerApplication.MessageIds[message] == message);
        CHECK(peers.ServerApplication.Lanes[message] == Channel::Bulk);
    }

    peers.Client.Close();
    REQUIRE(peers.WaitUntil([&peers] { return peers.Server.GetState() == SessionState::Closed; }));
    CHECK(peers.Server.GetEnd() == SessionEnd::TransportClosed);
    peers.IssueTicket(2);
    constexpr uint64 RECONNECTED_EPOCH = FIRST_EPOCH + 1;
    peers.Start(RECONNECTED_EPOCH, 2);
    peers.CompleteAdmission();
    CHECK(peers.Client.GetGeneration() == 2);
    CHECK(peers.Server.GetGeneration() == 2);
    CHECK(peers.Client.GetEpoch() == RECONNECTED_EPOCH);
    CHECK(peers.Server.GetEpoch() == RECONNECTED_EPOCH);

    OwnedMessage previous;
    REQUIRE(EncodeMessage({Channel::Bulk, APP_KIND, FIRST_EPOCH, 100}, {&input, 1}, previous.Bytes, previous.Size) ==
            WireStatus::Ok);
    REQUIRE(peers.ClientTransport.Send(Channel::Bulk, {previous.Bytes, previous.Size}) == TransportResult::Ok);
    REQUIRE(peers.Client.Queue(Channel::Bulk, APP_KIND, {&input, 1}) == SessionSendResult::Ok);
    REQUIRE(peers.WaitUntil([&peers] { return peers.ServerApplication.Received > SEND_QUEUE_CAPACITY_PER_CHANNEL; }));
    CHECK(peers.Server.GetCounters().Rejected == 1);
    CHECK(peers.Server.GetCounters().Delivered == 1);
    CHECK(peers.ServerApplication.MessageIds[SEND_QUEUE_CAPACITY_PER_CHANNEL] == input);

    REQUIRE(peers.Server.Queue(Channel::Control, APP_KIND, {&input, 1}) == SessionSendResult::Ok);
    REQUIRE(peers.WaitUntil([&peers] { return peers.ClientApplication.Received == 1; }));
    CHECK(peers.ClientApplication.MessageIds[0] == input);
}
