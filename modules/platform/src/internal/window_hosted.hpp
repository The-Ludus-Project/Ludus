#pragma once

// Hosted window backend (macOS and Linux). WindowManager selects it when
// HOSTED_WINDOW_ENVIRONMENT_VARIABLE is present. OpenWindow() is the
// connection-independent half, so tests can drive it over a socketpair.

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>

#include <ludus/platform/base/window.h>
#include <ludus/platform/hosted_window.h>

#include "hosted_protocol.hpp"
#include "hosted_socket.hpp"

namespace ludus::platform::hosted
{
/// Time CreateWindow() waits for the endpoint's Welcome or refusal.
inline constexpr uint32 WELCOME_TIMEOUT_MILLISECONDS = 5000;
/// Bytes buffered from the channel between HandleEvent() calls.
inline constexpr usize RECEIVE_BUFFER_BYTES = 4096;
static_assert(RECEIVE_BUFFER_BYTES >= MAX_FRAME_BYTES);

/// True when HOSTED_WINDOW_ENVIRONMENT_VARIABLE is present in the environment.
[[nodiscard]] bool IsRequested() noexcept;

/// Validate the channel address; the hosted backend needs no other initialization.
[[nodiscard]] bool Initialize() noexcept;

/// @brief Connect to the address in the environment and open a hosted window.
///
/// Fails (null) on a malformed address, a connection failure, a peer under another
/// user id, a refusal or a missing Welcome within WELCOME_TIMEOUT_MILLISECONDS.
[[nodiscard]] foundation::UniquePtr<Window> CreateWindow(const Window::CreateInfo& info) noexcept;

/// Send the version-1 Hello for `token` on a freshly connected socket.
[[nodiscard]] bool TrySendHello(int32 descriptor, const Token& token) noexcept;

/// @brief Wait for the Welcome on a connection whose Hello was sent, then build the window.
///
/// Takes ownership of `connection` whatever the outcome. The caller configured it
/// with TryConfigureSocket() before sending the Hello, so HandleEvent() never blocks;
/// it is not reconfigured here because macOS rejects socket options once a refusing
/// endpoint has closed its side. The window publishes the
/// Welcome's extent, scale and visibility; bytes that followed the Welcome are
/// processed by its first HandleEvent().
/// @param outRefusal The endpoint's reason when it refused; unchanged otherwise.
/// @return The window, or null on refusal, channel loss, a protocol violation or timeout.
[[nodiscard]] foundation::UniquePtr<Window> OpenWindow(SocketDescriptor&& connection,
                                                       const Window::CreateInfo& info,
                                                       uint32 timeoutMilliseconds,
                                                       HostedRefusal& outRefusal) noexcept;
} // namespace ludus::platform::hosted
