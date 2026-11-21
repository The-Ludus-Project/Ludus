#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/pointer.hpp>

#include "internal/session.hpp"

#include <steam/steamnetworkingsockets.h>

namespace ludus::network
{
// Owns the native process-wide trust domain. Explicit caller-thread startup
// and shutdown. Destroy all transport instances before destroying this runtime.
class NativeRuntime final
{
public:
    NativeRuntime() noexcept = default;
    ~NativeRuntime();
    NativeRuntime(const NativeRuntime&) = delete;
    NativeRuntime& operator=(const NativeRuntime&) = delete;
    bool Open(const char* rootCertificate) noexcept;

private:
    friend class NativeTransport;
    bool mIsOpen = false;
    usize mEndpointCount = 0;
};

inline constexpr usize NATIVE_SEND_BUFFER_BYTES = 4096;

class NativeTransport final : public SessionTransport
{
public:
    explicit NativeTransport(NativeRuntime& inoutRuntime) noexcept;
    ~NativeTransport() override;
    NativeTransport(const NativeTransport&) = delete;
    NativeTransport& operator=(const NativeTransport&) = delete;

    bool Open(const char* localIdentity, const char* expectedPeer) noexcept;
    // GNS generates and retains the endpoint key. The caller's external issuer
    // returns a signed cert for this request and the configured local identity.
    bool RequestCertificate(BorrowedView<uint8> bytes, usize& outWritten) noexcept;
    bool SetCertificate(BorrowedView<const uint8> bytes) noexcept;

    bool Listen(const SteamNetworkingIPAddr& address) noexcept;
    bool Connect(const SteamNetworkingIPAddr& address) noexcept;
    TransportState Poll() noexcept override;
    [[nodiscard]] BorrowedView<const char> GetPeerIdentity() const noexcept override;
    TransportResult Receive(ReceivedFrame& outFrame) noexcept override;
    TransportResult Send(Channel lane, BorrowedView<const uint8> bytes) noexcept override;
    // Close the current connection. A server may continue accepting on its listener.
    void Close() noexcept override;

    // Diagnostics from the congestion-controlled transport for the active connection.
    bool ReadStats(SteamNetConnectionRealTimeStatus_t& outStats) const noexcept;
    [[nodiscard]] uint64 GetRejectedPeerCount() const noexcept;

private:
    struct SocketDeleter final
    {
        void operator()(ISteamNetworkingSockets* sockets) const noexcept;
    };

    static void handleConnectionStatus(SteamNetConnectionStatusChangedCallback_t* status) noexcept;
    void accept(const SteamNetConnectionStatusChangedCallback_t& status) noexcept;
    [[nodiscard]] bool isExpectedPeer(const SteamNetConnectionInfo_t& info) const noexcept;
    [[nodiscard]] bool configureLanes(HSteamNetConnection connection) noexcept;

    NativeRuntime& mRuntime;
    foundation::UniquePtr<ISteamNetworkingSockets, SocketDeleter> mSockets;
    HSteamListenSocket mListener = k_HSteamListenSocket_Invalid;
    HSteamNetConnection mConnection = k_HSteamNetConnection_Invalid;
    SteamNetworkingIdentity mExpectedPeer{};
    bool mHasCertificate = false;
    uint64 mRejectedPeerCount = 0;
    bool mHasConfiguredLanes = false;
    bool mIsAuthenticated = false;
};
} // namespace ludus::network
