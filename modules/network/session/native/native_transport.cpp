#include "native_transport.hpp"

#include "gns_credentials.hpp"

#include <ludus/foundation/base/checked_integer.hpp>

#include <cstring>

#include <steam/isteamnetworkingutils.h>

namespace ludus::network
{
namespace
{
// GNS invokes callbacks synchronously inside RunCallbacks on the calling thread.
// Only dispatch uses this pointer. Connection, keys and queues have explicit owners.
thread_local NativeTransport* gPollingTransportOrNull = nullptr;

constexpr usize CONNECTION_OPTION_COUNT = 8;
constexpr int32 AUTHENTICATION_REQUIRED = 0;
constexpr int32 WARN_ON_UNAUTHENTICATED = 1;
constexpr int32 NATIVE_RECEIVE_BUFFER_MESSAGES = 64;
constexpr int32 NATIVE_RECEIVE_BUFFER_BYTES = NATIVE_RECEIVE_BUFFER_MESSAGES * static_cast<int32>(MAX_MESSAGE_BYTES);

using StatusCallback = void (*)(SteamNetConnectionStatusChangedCallback_t*);
using ConnectionOptions = StaticArray<SteamNetworkingConfigValue_t, CONNECTION_OPTION_COUNT>;

ConnectionOptions MakeOptions(StatusCallback callback, int32 authentication) noexcept
{
    ConnectionOptions options{};
    options[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(callback));
    options[1].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, authentication);
    options[2].SetInt32(k_ESteamNetworkingConfig_IPLocalHost_AllowWithoutAuth, AUTHENTICATION_REQUIRED);
    constexpr usize ENCRYPTION_OPTION = 3;
    constexpr usize SEND_BUFFER_OPTION = 4;
    constexpr usize RECEIVE_BUFFER_OPTION = 5;
    constexpr usize MAXIMUM_MESSAGE_OPTION = 6;
    constexpr usize RECEIVE_MESSAGE_COUNT_OPTION = 7;
    constexpr int32 REQUIRE_ENCRYPTION = 0;
    options[ENCRYPTION_OPTION].SetInt32(k_ESteamNetworkingConfig_Unencrypted, REQUIRE_ENCRYPTION);
    options[SEND_BUFFER_OPTION].SetInt32(k_ESteamNetworkingConfig_SendBufferSize, NATIVE_SEND_BUFFER_BYTES);
    options[RECEIVE_BUFFER_OPTION].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize, NATIVE_RECEIVE_BUFFER_BYTES);
    options[MAXIMUM_MESSAGE_OPTION].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, MAX_MESSAGE_BYTES);
    options[RECEIVE_MESSAGE_COUNT_OPTION].SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages,
                                                   NATIVE_RECEIVE_BUFFER_MESSAGES);
    return options;
}

constexpr int REJECTED_PEER_REASON = k_ESteamNetConnectionEnd_AppException_Min + 1;
constexpr int CONNECTION_BUSY_REASON = k_ESteamNetConnectionEnd_AppException_Min + 2;
constexpr int LOCAL_CLOSE_REASON = k_ESteamNetConnectionEnd_App_Generic;
} // namespace

bool NativeRuntime::Open(const char* rootCertificate) noexcept
{
    LUDUS_ASSERT(rootCertificate != nullptr);
    if (mIsOpen == true || SteamNetworkingSockets() != nullptr)
    {
        return false;
    }
    SteamNetworkingErrMsg error{};
    if (GameNetworkingSockets_Init(nullptr, error) == false)
    {
        return false;
    }
    if (gns::InstallTrustRoot(rootCertificate) == false)
    {
        GameNetworkingSockets_Kill();
        gns::ClearTrustRoot();
        return false;
    }
    mIsOpen = true;
    return true;
}

NativeRuntime::~NativeRuntime()
{
    LUDUS_ASSERT(mEndpointCount == 0);
    if (mIsOpen == true)
    {
        GameNetworkingSockets_Kill();
        gns::ClearTrustRoot();
    }
}

NativeTransport::NativeTransport(NativeRuntime& inoutRuntime) noexcept : mRuntime(inoutRuntime) {}

NativeTransport::~NativeTransport()
{
    Close();
    if (mListener != k_HSteamListenSocket_Invalid)
    {
        static_cast<void>(mSockets->CloseListenSocket(mListener));
    }
    if (mSockets.Get() != nullptr)
    {
        mSockets.Reset();
        --mRuntime.mEndpointCount;
    }
}

void NativeTransport::SocketDeleter::operator()(ISteamNetworkingSockets* sockets) const noexcept
{
    gns::DestroyInterface(sockets);
}

bool NativeTransport::Open(const char* localIdentity, const char* expectedPeer) noexcept
{
    LUDUS_ASSERT(localIdentity != nullptr && expectedPeer != nullptr);
    SteamNetworkingIdentity local{};
    SteamNetworkingIdentity expected{};
    if (mRuntime.mIsOpen == false || mSockets.Get() != nullptr || localIdentity[0] == '\0' || expectedPeer[0] == '\0' ||
        local.SetGenericString(localIdentity) == false || expected.SetGenericString(expectedPeer) == false)
    {
        return false;
    }
    ISteamNetworkingSockets* socketsOrNull = gns::CreateInterface(local);
    if (socketsOrNull == nullptr)
    {
        return false;
    }
    mSockets.Reset(socketsOrNull);
    mExpectedPeer = expected;
    ++mRuntime.mEndpointCount;
    return true;
}

bool NativeTransport::RequestCertificate(BorrowedView<uint8> bytes, usize& outWritten) noexcept
{
    int capacity = 0;
    SteamNetworkingErrMsg error{};
    if (mSockets.Get() == nullptr || mHasCertificate == true || bytes.IsValid() == false || bytes.IsEmpty() == true ||
        foundation::TryIntegerCast(bytes.GetSize(), capacity) == false ||
        mSockets->GetCertificateRequest(&capacity, bytes.GetDataOrNull(), error) == false)
    {
        return false;
    }
    outWritten = static_cast<usize>(capacity);
    return true;
}

bool NativeTransport::SetCertificate(BorrowedView<const uint8> bytes) noexcept
{
    int size = 0;
    if (mSockets.Get() == nullptr || mHasCertificate == true || mConnection != k_HSteamNetConnection_Invalid ||
        mListener != k_HSteamListenSocket_Invalid || bytes.IsValid() == false || bytes.IsEmpty() == true ||
        foundation::TryIntegerCast(bytes.GetSize(), size) == false)
    {
        return false;
    }
    mHasCertificate = gns::CheckLocalCertificate(*mSockets, bytes.GetDataOrNull(), size);
    return mHasCertificate;
}

bool NativeTransport::Listen(const SteamNetworkingIPAddr& address) noexcept
{
    if (mHasCertificate == false || mListener != k_HSteamListenSocket_Invalid ||
        mConnection != k_HSteamNetConnection_Invalid)
    {
        return false;
    }
    // The pinned open-source implementation gates CreateListenSocketIP on an
    // upstream account-service compile flag even with a locally issued cert.
    // Tighten the listener immediately; accept() also checks the identity and
    // authentication flags before exposing any inbound connection to the session.
    const ConnectionOptions options = MakeOptions(handleConnectionStatus, WARN_ON_UNAUTHENTICATED);
    const HSteamListenSocket listener =
        mSockets->CreateListenSocketIP(address, static_cast<int>(options.GetSize()), options.GetData());
    if (listener == k_HSteamListenSocket_Invalid)
    {
        return false;
    }
    if (SteamNetworkingUtils()->SetConfigValue(k_ESteamNetworkingConfig_IP_AllowWithoutAuth,
                                               k_ESteamNetworkingConfig_ListenSocket,
                                               listener,
                                               k_ESteamNetworkingConfig_Int32,
                                               &AUTHENTICATION_REQUIRED) == false)
    {
        static_cast<void>(mSockets->CloseListenSocket(listener));
        return false;
    }
    mListener = listener;
    return true;
}

bool NativeTransport::Connect(const SteamNetworkingIPAddr& address) noexcept
{
    if (mHasCertificate == false || mConnection != k_HSteamNetConnection_Invalid ||
        mListener != k_HSteamListenSocket_Invalid)
    {
        return false;
    }
    const ConnectionOptions options = MakeOptions(handleConnectionStatus, AUTHENTICATION_REQUIRED);
    mConnection = mSockets->ConnectByIPAddress(address, static_cast<int>(options.GetSize()), options.GetData());
    return mConnection != k_HSteamNetConnection_Invalid;
}

TransportState NativeTransport::Poll() noexcept
{
    mIsAuthenticated = false;
    if (mSockets.Get() == nullptr)
    {
        return TransportState::Closed;
    }
    LUDUS_ASSERT(gPollingTransportOrNull == nullptr);
    gPollingTransportOrNull = this;
    mSockets->RunCallbacks();
    gPollingTransportOrNull = nullptr;
    if (mConnection == k_HSteamNetConnection_Invalid)
    {
        return mListener == k_HSteamListenSocket_Invalid ? TransportState::Closed : TransportState::Connecting;
    }
    SteamNetConnectionInfo_t info{};
    if (mSockets->GetConnectionInfo(mConnection, &info) == false)
    {
        return TransportState::Closed;
    }
    if (info.m_eState == k_ESteamNetworkingConnectionState_Connecting)
    {
        return TransportState::Connecting;
    }
    if (info.m_eState != k_ESteamNetworkingConnectionState_Connected)
    {
        return info.m_eEndReason == REJECTED_PEER_REASON ? TransportState::Rejected : TransportState::Closed;
    }
    if (isExpectedPeer(info) == false || (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Unencrypted) != 0)
    {
        return TransportState::Rejected;
    }
    if (mHasConfiguredLanes == false && configureLanes(mConnection) == false)
    {
        return TransportState::Closed;
    }
    mIsAuthenticated = true;
    return TransportState::Authenticated;
}

BorrowedView<const char> NativeTransport::GetPeerIdentity() const noexcept
{
    const char* peerOrNull = mExpectedPeer.GetGenericString();
    if (mIsAuthenticated == false || peerOrNull == nullptr)
    {
        return {};
    }
    return {peerOrNull, std::strlen(peerOrNull)};
}

TransportResult NativeTransport::Receive(ReceivedFrame& outFrame) noexcept
{
    if (mIsAuthenticated == false)
    {
        return TransportResult::Failed;
    }
    SteamNetworkingMessage_t* receivedOrNull = nullptr;
    const int result = mSockets->ReceiveMessagesOnConnection(mConnection, &receivedOrNull, 1);
    if (result == 0)
    {
        return TransportResult::Empty;
    }
    if (result != 1 || receivedOrNull == nullptr)
    {
        return TransportResult::Failed;
    }
    const int size = receivedOrNull->m_cbSize;
    const uint16 lane = receivedOrNull->m_idxLane;
    const bool isValid = size > 0 && static_cast<usize>(size) <= MAX_MESSAGE_BYTES && lane < CHANNEL_COUNT &&
                         receivedOrNull->m_pData != nullptr;
    if (isValid == true)
    {
        outFrame.Lane = static_cast<Channel>(lane);
        outFrame.Message.Size = static_cast<usize>(size);
        std::memcpy(outFrame.Message.Bytes, receivedOrNull->m_pData, outFrame.Message.Size);
    }
    receivedOrNull->Release();
    return isValid ? TransportResult::Ok : TransportResult::Failed;
}

TransportResult NativeTransport::Send(Channel lane, BorrowedView<const uint8> bytes) noexcept
{
    if (mIsAuthenticated == false || bytes.IsValid() == false || bytes.IsEmpty() == true ||
        bytes.GetSize() > MAX_MESSAGE_BYTES || static_cast<usize>(lane) >= CHANNEL_COUNT)
    {
        return TransportResult::Failed;
    }
    SteamNetworkingMessage_t* messageOrNull =
        SteamNetworkingUtils()->AllocateMessage(static_cast<int>(bytes.GetSize()));
    if (messageOrNull == nullptr)
    {
        return TransportResult::Failed;
    }
    messageOrNull->m_conn = mConnection;
    messageOrNull->m_idxLane = static_cast<uint16>(lane);
    const bool isReliable = lane == Channel::Control || lane == Channel::Bulk;
    messageOrNull->m_nFlags =
        isReliable ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoNagle;
    std::memcpy(messageOrNull->m_pData, bytes.GetDataOrNull(), bytes.GetSize());
    ::int64 result = 0;
    mSockets->SendMessages(1, &messageOrNull, &result, true);
    if (result > 0)
    {
        return TransportResult::Ok;
    }
    return result == -k_EResultLimitExceeded ? TransportResult::WouldBlock : TransportResult::Failed;
}

void NativeTransport::Close() noexcept
{
    if (mConnection != k_HSteamNetConnection_Invalid)
    {
        static_cast<void>(mSockets->CloseConnection(mConnection, LOCAL_CLOSE_REASON, nullptr, false));
        mConnection = k_HSteamNetConnection_Invalid;
    }
    mHasConfiguredLanes = false;
    mIsAuthenticated = false;
}

bool NativeTransport::ReadStats(SteamNetConnectionRealTimeStatus_t& outStats) const noexcept
{
    if (mIsAuthenticated == false)
    {
        return false;
    }
    SteamNetConnectionRealTimeStatus_t stats{};
    if (mSockets->GetConnectionRealTimeStatus(mConnection, &stats, 0, nullptr) != k_EResultOK)
    {
        return false;
    }
    outStats = stats;
    return true;
}

uint64 NativeTransport::GetRejectedPeerCount() const noexcept
{
    return mRejectedPeerCount;
}

void NativeTransport::handleConnectionStatus(SteamNetConnectionStatusChangedCallback_t* status) noexcept
{
    LUDUS_ASSERT(status != nullptr && gPollingTransportOrNull != nullptr);
    gPollingTransportOrNull->accept(*status);
}

void NativeTransport::accept(const SteamNetConnectionStatusChangedCallback_t& status) noexcept
{
    if (status.m_info.m_hListenSocket != mListener || mListener == k_HSteamListenSocket_Invalid ||
        status.m_info.m_eState != k_ESteamNetworkingConnectionState_Connecting || status.m_hConn == mConnection)
    {
        return;
    }
    // Recheck the live handle; a callback queued before Close may be stale.
    SteamNetConnectionInfo_t info{};
    if (mSockets->GetConnectionInfo(status.m_hConn, &info) == false ||
        info.m_eState != k_ESteamNetworkingConnectionState_Connecting)
    {
        return;
    }
    if (mConnection != k_HSteamNetConnection_Invalid)
    {
        static_cast<void>(mSockets->CloseConnection(status.m_hConn, CONNECTION_BUSY_REASON, nullptr, false));
        return;
    }
    if (isExpectedPeer(info) == false)
    {
        ++mRejectedPeerCount;
        static_cast<void>(mSockets->CloseConnection(status.m_hConn, REJECTED_PEER_REASON, nullptr, false));
        return;
    }
    if (configureLanes(status.m_hConn) == false || mSockets->AcceptConnection(status.m_hConn) != k_EResultOK)
    {
        mHasConfiguredLanes = false;
        static_cast<void>(mSockets->CloseConnection(status.m_hConn, LOCAL_CLOSE_REASON, nullptr, false));
        return;
    }
    mConnection = status.m_hConn;
}

bool NativeTransport::isExpectedPeer(const SteamNetConnectionInfo_t& info) const noexcept
{
    return (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Unauthenticated) == 0 &&
           info.m_identityRemote == mExpectedPeer;
}

bool NativeTransport::configureLanes(HSteamNetConnection connection) noexcept
{
    // Lower numerical priority wins. Control cannot wait behind bulk assembly;
    // Input and Snapshot share scheduling priority but use different weights.
    constexpr uint16 CONTROL_WEIGHT = 1;
    constexpr uint16 INPUT_WEIGHT = 2;
    constexpr uint16 SNAPSHOT_WEIGHT = 1;
    constexpr uint16 BULK_WEIGHT = 1;
    constexpr StaticArray<int, CHANNEL_COUNT> PRIORITIES{{0, 1, 1, 2}};
    constexpr StaticArray<uint16, CHANNEL_COUNT> WEIGHTS{{CONTROL_WEIGHT, INPUT_WEIGHT, SNAPSHOT_WEIGHT, BULK_WEIGHT}};
    mHasConfiguredLanes =
        mSockets->ConfigureConnectionLanes(connection, CHANNEL_COUNT, PRIORITIES.GetData(), WEIGHTS.GetData()) ==
        k_EResultOK;
    return mHasConfiguredLanes;
}
} // namespace ludus::network
