#include "internal/hosted_pipe.hpp"
#include "internal/window_hosted.hpp"

#include <ludus/platform/hosted_window.h>
#include <ludus/platform/native_window.h>

#include <catch2/catch_test_macros.hpp>

using ludus::foundation::Move;
using ludus::foundation::uint8;
using ludus::foundation::UniquePtr;
using ludus::platform::HostedConnectionState;
using ludus::platform::HostedEndpointStatus;
using ludus::platform::HostedRefusal;
using ludus::platform::HostedWindowEndpoint;
using ludus::platform::Window;
using ludus::platform::WindowSystem;

namespace hosted = ludus::platform::hosted;

TEST_CASE("A Win32 hosted pipe refuses a forged token and then admits the real window", "[platform][hosted][win32]")
{
    HostedWindowEndpoint endpoint;
    REQUIRE(endpoint.Start({ .Width = 640, .Height = 480, .Scale = 2.0F }) == HostedEndpointStatus::Ok);
    REQUIRE(endpoint.GetDirectory().empty() == true);
    hosted::Token token{};
    hosted::SocketPath path;
    REQUIRE(hosted::TryParseChannelAddress(endpoint.GetChannelAddress(), token, path) == true);
    REQUIRE(endpoint.Pump() == HostedEndpointStatus::Ok);

    hosted::Token forged = token;
    forged[0] = static_cast<uint8>(forged[0] ^ 1U);
    hosted::SocketDescriptor intruder = hosted::ConnectToPipe(path);
    REQUIRE(intruder.IsOpen() == true);
    REQUIRE(hosted::TryConfigureSocket(intruder.Get()) == true);
    REQUIRE(hosted::TrySendHello(intruder.Get(), forged) == true);
    REQUIRE(endpoint.Pump() == HostedEndpointStatus::Ok);
    CHECK(endpoint.GetLastRefusal() == HostedRefusal::BadToken);
    CHECK(endpoint.GetRefusalCount() == 1);
    CHECK(endpoint.GetState() == HostedConnectionState::Listening);
    intruder.Reset();

    REQUIRE(endpoint.Pump() == HostedEndpointStatus::Ok);
    hosted::SocketDescriptor connection = hosted::ConnectToPipe(path);
    REQUIRE(connection.IsOpen() == true);
    REQUIRE(hosted::TryConfigureSocket(connection.Get()) == true);
    REQUIRE(hosted::TrySendHello(connection.Get(), token) == true);
    REQUIRE(endpoint.Pump() == HostedEndpointStatus::Ok);
    HostedRefusal refusal = HostedRefusal::None;
    const UniquePtr<Window> window = hosted::OpenWindow(Move(connection),
                                                        token,
                                                        { .Name = "hosted test" },
                                                        hosted::WELCOME_TIMEOUT_MILLISECONDS,
                                                        refusal);
    REQUIRE(window.Get() != nullptr);
    CHECK(endpoint.GetState() == HostedConnectionState::Connected);
    CHECK(window->GetNativeWindowInfo().System == WindowSystem::Hosted);
    CHECK(window->GetNativeWindowInfo().Width == 640);
    CHECK(window->GetNativeWindowInfo().Scale == 2.0F);
    REQUIRE(endpoint.SendResize(320, 240, 1.0F) == HostedEndpointStatus::Ok);
    REQUIRE(window->HandleEvent({}) == true);
    CHECK(window->GetNativeWindowInfo().Width == 320);
    CHECK(window->GetNativeWindowInfo().Scale == 1.0F);
    endpoint.Stop();
    CHECK(window->HandleEvent({}) == false);
}
