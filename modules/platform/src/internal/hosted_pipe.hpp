#pragma once

#include "hosted_protocol.hpp"
#include "hosted_socket.hpp"

#include <ludus/foundation/base/pointer.hpp>

namespace ludus::platform::hosted
{
struct ClosePeerProcess final
{
    void operator()(void* processOrNull) const noexcept;
};
struct PeerProcessHandle;
using PeerProcess = foundation::UniquePtr<PeerProcessHandle, ClosePeerProcess>;

[[nodiscard]] bool MakePipePath(SocketPath& outPath) noexcept;
[[nodiscard]] SocketDescriptor ListenOnPipe(const SocketPath& path) noexcept;
[[nodiscard]] bool TryAcceptPipe(const SocketDescriptor& listener) noexcept;
[[nodiscard]] SocketDescriptor ConnectToPipe(const SocketPath& path) noexcept;
[[nodiscard]] PeerProcess OpenPipePeer(NativeStream descriptor, bool isServer) noexcept;
[[nodiscard]] HostedImageHandle DuplicateImageHandle(const PeerProcess& peer, uint64 value) noexcept;
} // namespace ludus::platform::hosted
