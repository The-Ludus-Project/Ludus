#include "gns_credentials.hpp"

#include <new>

#include "steamnetworkingsockets/clientlib/csteamnetworkingsockets.h"
#include "steamnetworkingsockets/steamnetworkingsockets_certstore.h"

namespace SteamNetworkingSocketsLib
{
extern int s_csecsCertExpiryGrace;
}

namespace ludus::network::gns
{
using SteamNetworkingSocketsLib::CertAuthScope;
using SteamNetworkingSocketsLib::CertStore_AddCertFromBase64;
using SteamNetworkingSocketsLib::CertStore_CheckCert;
using SteamNetworkingSocketsLib::CertStore_Reset;
using SteamNetworkingSocketsLib::CheckCertAppID;
using SteamNetworkingSocketsLib::CSteamNetworkingSockets;
using SteamNetworkingSocketsLib::CSteamNetworkingUtils;
using SteamNetworkingSocketsLib::s_csecsCertExpiryGrace;
using SteamNetworkingSocketsLib::SteamNetworkingGlobalLock;

bool InstallTrustRoot(const char* rootCertificate) noexcept
{
    SteamNetworkingGlobalLock lock("Ludus trust root");
    SteamNetworkingErrMsg error{};
    s_csecsCertExpiryGrace = 0;
    return CertStore_AddCertFromBase64(rootCertificate, error);
}

void ClearTrustRoot() noexcept
{
    SteamNetworkingGlobalLock lock("Ludus trust reset");
    CertStore_Reset();
}

ISteamNetworkingSockets* CreateInterface(const SteamNetworkingIdentity& identity) noexcept
{
    SteamNetworkingGlobalLock lock("Ludus sockets interface");
    SteamNetworkingErrMsg error{};
    CSteamNetworkingSockets* socketsOrNull =
        new (std::nothrow) CSteamNetworkingSockets(static_cast<CSteamNetworkingUtils*>(SteamNetworkingUtils()));
    if (socketsOrNull != nullptr && socketsOrNull->BInitGameNetworkingSockets(&identity, error) == false)
    {
        socketsOrNull->Destroy();
        return nullptr;
    }
    return socketsOrNull;
}

void DestroyInterface(ISteamNetworkingSockets* sockets) noexcept
{
    SteamNetworkingGlobalLock lock("Ludus sockets destroy");
    static_cast<CSteamNetworkingSockets*>(sockets)->Destroy();
}

bool CheckLocalCertificate(ISteamNetworkingSockets& inoutSockets, const void* certificate, int size) noexcept
{
    SteamNetworkingGlobalLock lock("Ludus signed endpoint");
    CSteamNetworkingSockets& sockets = static_cast<CSteamNetworkingSockets&>(inoutSockets);
    CMsgSteamDatagramCertificateSigned signedCertificate;
    CMsgSteamDatagramCertificate leaf;
    SteamNetworkingErrMsg error{};
    if (signedCertificate.ParseFromArray(certificate, size) == false ||
        signedCertificate.has_private_key_data() == true)
    {
        return false;
    }
    const CertAuthScope* scopeOrNull =
        CertStore_CheckCert(signedCertificate, leaf, sockets.m_pSteamNetworkingUtils->GetTimeSecure(), error);
    return scopeOrNull != nullptr &&
           CheckCertAppID(leaf, scopeOrNull, sockets.m_pSteamNetworkingUtils->GetAppID(), error) == true &&
           sockets.SetCertificate(certificate, size, error) == true;
}
} // namespace ludus::network::gns
