#pragma once

#include <steam/steamnetworkingsockets.h>

// Private bridge for the pinned GNS implementation. All calls use the upstream
// global lock. The native runtime owns one trust domain for the process.
namespace ludus::network::gns
{
bool InstallTrustRoot(const char* rootCertificate) noexcept;
void ClearTrustRoot() noexcept;
ISteamNetworkingSockets* CreateInterface(const SteamNetworkingIdentity& identity) noexcept;
void DestroyInterface(ISteamNetworkingSockets* sockets) noexcept;
bool CheckLocalCertificate(ISteamNetworkingSockets& inoutSockets, const void* certificate, int size) noexcept;
} // namespace ludus::network::gns
