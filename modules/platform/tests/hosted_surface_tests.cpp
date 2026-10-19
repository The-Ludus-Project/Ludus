// Hosted surface channel tests (protocol version 2). An EndpointSession and a
// hosted window are connected over a socketpair, as in hosted_window_tests.cpp,
// and exchange Images, Present and Release through the production codec. The
// file-descriptor transport runs on every host (Linux uses it in production); on
// macOS the Mach-port transport also runs, through a real per-session rendezvous
// whose sender is this process.

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/input/key.h>
#include <ludus/input/keyboard_event.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/entropy.h>
#include <ludus/platform/hosted_surface.h>
#include <ludus/platform/hosted_window.h>
#include <ludus/platform/native_window.h>

#include "internal/hosted_endpoint_session.hpp"
#include "internal/hosted_protocol.hpp"
#include "internal/hosted_socket.hpp"
#include "internal/window_hosted.hpp"
#if defined(__APPLE__)
#    include "internal/hosted_mach.hpp"

#    include <mach/mach.h>
#endif

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

using ludus::foundation::int32;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::UniquePtr;
using ludus::foundation::usize;
using ludus::foundation::time::Timestamp;
using ludus::platform::HOSTED_SURFACE_IMAGE_COUNT;
using ludus::platform::HostedEndpointStatus;
using ludus::platform::HostedHandleKind;
using ludus::platform::HostedImageFormat;
using ludus::platform::HostedImageHandle;
using ludus::platform::HostedImageRelease;
using ludus::platform::HostedRefusal;
using ludus::platform::HostedSurfaceChannel;
using ludus::platform::HostedSurfaceEvent;
using ludus::platform::HostedSurfaceEventKind;
using ludus::platform::HostedSurfaceImages;
using ludus::platform::HostedSurfaceStatus;
using ludus::platform::Window;

namespace hosted = ludus::platform::hosted;

namespace
{
constexpr uint32 OPEN_TIMEOUT_MILLISECONDS = 2000;
constexpr usize PUMP_ROUNDS = 1000;
constexpr usize FLOOD_LIMIT = 1'000'000;
constexpr uint32 SURFACE_WIDTH = 640;
constexpr uint32 SURFACE_HEIGHT = 360;

hosted::Token MakeToken(uint8 seed)
{
    hosted::Token token{};
    for (usize i = 0; i < hosted::TOKEN_BYTES; ++i)
    {
        token[i] = static_cast<uint8>(seed + i);
    }
    return token;
}

hosted::MirroredWindowState MakeInitialState()
{
    hosted::MirroredWindowState state;
    state.Width = SURFACE_WIDTH;
    state.Height = SURFACE_HEIGHT;
    return state;
}

Window::CreateInfo MakeCreateInfo()
{
    Window::CreateInfo info;
    info.Name = "hosted surface test";
    return info;
}

Timestamp At(uint64 nanoseconds)
{
    return {nanoseconds};
}

/// A session connected to a hosted window over a socketpair.
struct SurfacePair final
{
    hosted::Token SessionToken = MakeToken(9);
    hosted::EndpointSession Session;
    UniquePtr<Window> HostedWindow;

    [[nodiscard]] HostedSurfaceChannel& GetChannel() const
    {
        HostedSurfaceChannel* channelOrNull = HostedWindow->GetNativeWindowInfo().HostedSurfaceOrNull;
        REQUIRE(channelOrNull != nullptr);
        return *channelOrNull;
    }

    /// Pump the endpoint until it has queued `count` events or the rounds run out.
    void PumpEndpoint()
    {
        for (usize i = 0; i < PUMP_ROUNDS; ++i)
        {
            Session.Pump(At(2));
        }
    }
};

void Connect(SurfacePair& outPair, bool shouldStartRendezvous)
{
    outPair.Session.Configure(outPair.SessionToken, hosted::GetCurrentUserId(), MakeInitialState());
#if defined(__APPLE__)
    if (shouldStartRendezvous == true)
    {
        hosted::RendezvousNonce nonce{};
        REQUIRE(ludus::platform::TryFillSystemEntropy(nonce.GetData(), nonce.GetSize()));
        nonce[0] = static_cast<uint8>(nonce[0] | 1U);
        REQUIRE(outPair.Session.TryStartRendezvous(nonce));
    }
#else
    static_cast<void>(shouldStartRendezvous);
#endif
    int32 descriptors[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    hosted::SocketDescriptor endpointSide(descriptors[0]);
    hosted::SocketDescriptor windowSide(descriptors[1]);
    REQUIRE(hosted::TryConfigureSocket(windowSide.Get()));
    REQUIRE(hosted::TrySendHello(windowSide.Get(), outPair.SessionToken));
    outPair.Session.Admit(ludus::foundation::Move(endpointSide), At(0));
    outPair.Session.Pump(At(1));
    REQUIRE(outPair.Session.GetPhase() == hosted::SessionPhase::Connected);
    HostedRefusal refusal = HostedRefusal::None;
    outPair.HostedWindow = hosted::OpenWindow(ludus::foundation::Move(windowSide),
                                              outPair.SessionToken,
                                              MakeCreateInfo(),
                                              OPEN_TIMEOUT_MILLISECONDS,
                                              refusal);
    REQUIRE(outPair.HostedWindow.Get() != nullptr);
    REQUIRE(outPair.HostedWindow->GetNativeWindowInfo().HostedSurfaceOrNull != nullptr);
}

/// Three temporary files stand in for exported GPU memory; returns their inodes.
HostedSurfaceImages MakeDescriptorImages(uint32 generation,
                                         ludus::foundation::StaticArray<uint64, HOSTED_SURFACE_IMAGE_COUNT>& outInodes)
{
    HostedSurfaceImages images;
    images.Generation = generation;
    images.Width = SURFACE_WIDTH;
    images.Height = SURFACE_HEIGHT;
    images.Format = HostedImageFormat::Rgba8Unorm;
    images.MemoryTypeIndex = 3;
    for (usize i = 0; i < HOSTED_SURFACE_IMAGE_COUNT; ++i)
    {
        char path[] = "/tmp/ludus-hosted-surface-XXXXXX";
        const int32 descriptor = ::mkstemp(path);
        REQUIRE(descriptor >= 0);
        static_cast<void>(::unlink(path));
        struct stat status{};
        REQUIRE(::fstat(descriptor, &status) == 0);
        outInodes[i] = static_cast<uint64>(status.st_ino);
        images.Handles[i] = HostedImageHandle::FromFileDescriptor(descriptor);
        images.AllocationSizes[i] = 4096 * (i + 1);
        images.DeviceUuid[i] = static_cast<uint8>(0xD0 + i);
        images.DriverUuid[i] = static_cast<uint8>(0xE0 + i);
    }
    return images;
}

bool TryTakeEvent(SurfacePair& inoutPair, HostedSurfaceEvent& outEvent)
{
    for (usize i = 0; i < PUMP_ROUNDS; ++i)
    {
        if (inoutPair.Session.TryTakeSurfaceEvent(outEvent) == true)
        {
            return true;
        }
        inoutPair.Session.Pump(At(2));
    }
    return false;
}

HostedSurfaceStatus TakeReleaseWithin(HostedSurfaceChannel& inoutChannel, HostedImageRelease& outRelease)
{
    HostedSurfaceStatus status = HostedSurfaceStatus::Busy;
    for (usize i = 0; i < PUMP_ROUNDS && status == HostedSurfaceStatus::Busy; ++i)
    {
        status = inoutChannel.TakeRelease(outRelease);
    }
    return status;
}

void PublishDescriptorGeneration(SurfacePair& inoutPair, uint32 generation)
{
    ludus::foundation::StaticArray<uint64, HOSTED_SURFACE_IMAGE_COUNT> inodes{};
    HostedSurfaceImages images = MakeDescriptorImages(generation, inodes);
    REQUIRE(inoutPair.GetChannel().PublishImages(images) == HostedSurfaceStatus::Ok);
    HostedSurfaceEvent event;
    REQUIRE(TryTakeEvent(inoutPair, event));
    REQUIRE(event.Kind == HostedSurfaceEventKind::Images);
}
} // namespace

TEST_CASE("Hosted surface messages round-trip through the version-2 codec", "[platform][hosted][surface]")
{
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    hosted::Message decoded;
    usize consumed = 0;

    hosted::Message images;
    images.Kind = hosted::MessageKind::Images;
    images.Generation = 5;
    images.Width = 1280;
    images.Height = 720;
    images.HandleKind = HostedHandleKind::OpaqueFileDescriptor;
    images.AllocationSizes = {11, 22, 33};
    images.MemoryTypeIndex = 2;
    images.DeviceUuid[15] = 0xAB;
    images.DriverUuid[0] = 0xCD;
    REQUIRE(hosted::TryEncodeMessage(images, {bytes.GetData(), bytes.GetSize()}, size));
    CHECK(size == hosted::HEADER_BYTES + hosted::IMAGES_PAYLOAD_BYTES);
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    CHECK(decoded.Generation == 5);
    CHECK(decoded.Width == 1280);
    CHECK(decoded.Height == 720);
    CHECK(decoded.HandleKind == HostedHandleKind::OpaqueFileDescriptor);
    CHECK(decoded.AllocationSizes[2] == 33);
    CHECK(decoded.MemoryTypeIndex == 2);
    CHECK(decoded.DeviceUuid[15] == 0xAB);
    CHECK(decoded.DriverUuid[0] == 0xCD);

    hosted::Message present;
    present.Kind = hosted::MessageKind::Present;
    present.Generation = 5;
    present.Image = 2;
    present.Sequence = 0x0102030405060708ULL;
    REQUIRE(hosted::TryEncodeMessage(present, {bytes.GetData(), bytes.GetSize()}, size));
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    CHECK(decoded.Kind == hosted::MessageKind::Present);
    CHECK(decoded.Image == 2);
    CHECK(decoded.Sequence == 0x0102030405060708ULL);

    hosted::Message release;
    release.Kind = hosted::MessageKind::Release;
    release.Generation = 6;
    release.Image = 1;
    REQUIRE(hosted::TryEncodeMessage(release, {bytes.GetData(), bytes.GetSize()}, size));
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    CHECK(decoded.Kind == hosted::MessageKind::Release);
    CHECK(decoded.Generation == 6);
    CHECK(decoded.Image == 1);
    // A nonzero reserved byte is a violation.
    bytes[hosted::HEADER_BYTES + 5] = 1;
    CHECK(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Malformed);

    hosted::Message welcome;
    welcome.Kind = hosted::MessageKind::Welcome;
    welcome.Width = 8;
    welcome.Height = 8;
    welcome.Nonce[0] = 0x5A;
    welcome.Nonce[15] = 0xA5;
    REQUIRE(hosted::TryEncodeMessage(welcome, {bytes.GetData(), bytes.GetSize()}, size));
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    CHECK(decoded.Nonce[0] == 0x5A);
    CHECK(decoded.Nonce[15] == 0xA5);
}

TEST_CASE("The hosted codec rejects invalid surface references and generations", "[platform][hosted][surface]")
{
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    hosted::Message present;
    present.Kind = hosted::MessageKind::Present;
    present.Generation = 1;
    present.Image = static_cast<uint32>(HOSTED_SURFACE_IMAGE_COUNT);
    CHECK_FALSE(hosted::TryEncodeMessage(present, {bytes.GetData(), bytes.GetSize()}, size));
    present.Image = 0;
    present.Generation = 0;
    CHECK_FALSE(hosted::TryEncodeMessage(present, {bytes.GetData(), bytes.GetSize()}, size));

    hosted::Message images;
    images.Kind = hosted::MessageKind::Images;
    images.Generation = 1;
    images.Width = 4;
    images.Height = 4;
    images.HandleKind = HostedHandleKind::None;
    CHECK_FALSE(hosted::TryEncodeMessage(images, {bytes.GetData(), bytes.GetSize()}, size));
    images.HandleKind = HostedHandleKind::MachPort;
    images.Width = 0;
    CHECK_FALSE(hosted::TryEncodeMessage(images, {bytes.GetData(), bytes.GetSize()}, size));
    images.Width = hosted::MAX_EXTENT_PIXELS + 1;
    CHECK_FALSE(hosted::TryEncodeMessage(images, {bytes.GetData(), bytes.GetSize()}, size));
}

TEST_CASE("A version-1 window is refused as incompatible", "[platform][hosted][surface]")
{
    const hosted::Token token = MakeToken(2);
    hosted::EndpointSession session;
    session.Configure(token, hosted::GetCurrentUserId(), MakeInitialState());
    int32 descriptors[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    hosted::SocketDescriptor endpointSide(descriptors[0]);
    hosted::SocketDescriptor windowSide(descriptors[1]);
    hosted::Message hello;
    hello.Kind = hosted::MessageKind::Hello;
    hello.Version = 1;
    hello.KeyCount = static_cast<ludus::foundation::uint16>(ludus::input::KEY_COUNT);
    hello.SessionToken = token;
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    REQUIRE(hosted::TryEncodeMessage(hello, {bytes.GetData(), bytes.GetSize()}, size));
    REQUIRE(::write(windowSide.Get(), bytes.GetData(), size) == static_cast<ssize_t>(size));
    session.Admit(ludus::foundation::Move(endpointSide), At(0));
    session.Pump(At(1));
    CHECK(session.GetLastRefusal() == HostedRefusal::IncompatibleVersion);
}

TEST_CASE("Hosted images travel with their file descriptors", "[platform][hosted][surface]")
{
    SurfacePair pair;
    Connect(pair, false);
    ludus::foundation::StaticArray<uint64, HOSTED_SURFACE_IMAGE_COUNT> inodes{};
    HostedSurfaceImages images = MakeDescriptorImages(1, inodes);
    REQUIRE(pair.GetChannel().PublishImages(images) == HostedSurfaceStatus::Ok);
    // The window's own descriptors are closed once the kernel has duplicated them.
    for (const HostedImageHandle& handle : images.Handles)
    {
        CHECK_FALSE(handle.IsValid());
    }

    HostedSurfaceEvent event;
    REQUIRE(TryTakeEvent(pair, event));
    REQUIRE(event.Kind == HostedSurfaceEventKind::Images);
    CHECK(event.Images.Generation == 1);
    CHECK(event.Images.Width == SURFACE_WIDTH);
    CHECK(event.Images.Height == SURFACE_HEIGHT);
    CHECK(event.Images.MemoryTypeIndex == 3);
    CHECK(event.Images.AllocationSizes[1] == 8192);
    CHECK(event.Images.DeviceUuid[2] == 0xD2);
    CHECK(event.Images.DriverUuid[1] == 0xE1);
    for (usize i = 0; i < HOSTED_SURFACE_IMAGE_COUNT; ++i)
    {
        REQUIRE(event.Images.Handles[i].GetKind() == HostedHandleKind::OpaqueFileDescriptor);
        struct stat status{};
        REQUIRE(::fstat(event.Images.Handles[i].GetFileDescriptor(), &status) == 0);
        // The same open file, in send order, received close-on-exec.
        CHECK(static_cast<uint64>(status.st_ino) == inodes[i]);
        CHECK((::fcntl(event.Images.Handles[i].GetFileDescriptor(), F_GETFD) & FD_CLOEXEC) != 0);
    }
    CHECK_FALSE(pair.Session.TryTakeSurfaceEvent(event));

    // A generation is published once and generations increase.
    HostedSurfaceImages again = MakeDescriptorImages(1, inodes);
    CHECK(pair.GetChannel().PublishImages(again) == HostedSurfaceStatus::InvalidArgument);
    CHECK(again.Handles[0].IsValid());
}

#if defined(__APPLE__)
TEST_CASE("Hosted images travel as Mach send rights through the session rendezvous", "[platform][hosted][surface]")
{
    SurfacePair pair;
    Connect(pair, true);
    HostedSurfaceImages images;
    images.Generation = 1;
    images.Width = SURFACE_WIDTH;
    images.Height = SURFACE_HEIGHT;
    ludus::foundation::StaticArray<uint32, HOSTED_SURFACE_IMAGE_COUNT> ports{};
    for (usize i = 0; i < HOSTED_SURFACE_IMAGE_COUNT; ++i)
    {
        mach_port_t port = MACH_PORT_NULL;
        REQUIRE(::mach_port_allocate(::mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS);
        REQUIRE(::mach_port_insert_right(::mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
        ports[i] = port;
        images.Handles[i] = HostedImageHandle::FromMachPort(port);
    }
    REQUIRE(pair.GetChannel().PublishImages(images) == HostedSurfaceStatus::Ok);
    CHECK_FALSE(images.Handles[0].IsValid());

    HostedSurfaceEvent event;
    REQUIRE(TryTakeEvent(pair, event));
    REQUIRE(event.Kind == HostedSurfaceEventKind::Images);
    for (usize i = 0; i < HOSTED_SURFACE_IMAGE_COUNT; ++i)
    {
        REQUIRE(event.Images.Handles[i].GetKind() == HostedHandleKind::MachPort);
        // Send rights to a port this task holds coalesce into the port's own name.
        CHECK(event.Images.Handles[i].GetMachPort() == ports[i]);
    }
    event = {};
    for (const uint32 port : ports)
    {
        static_cast<void>(::mach_port_mod_refs(::mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1));
    }
}

TEST_CASE("Mach images need the endpoint's rendezvous", "[platform][hosted][surface]")
{
    SurfacePair pair;
    Connect(pair, false);
    HostedSurfaceImages images;
    images.Generation = 1;
    images.Width = SURFACE_WIDTH;
    images.Height = SURFACE_HEIGHT;
    mach_port_t port = MACH_PORT_NULL;
    REQUIRE(::mach_port_allocate(::mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS);
    REQUIRE(::mach_port_insert_right(::mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    for (HostedImageHandle& handle : images.Handles)
    {
        REQUIRE(::mach_port_mod_refs(::mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS);
        handle = HostedImageHandle::FromMachPort(port);
    }
    CHECK(pair.GetChannel().PublishImages(images) == HostedSurfaceStatus::Unavailable);
    CHECK(images.Handles[0].IsValid());
    images = {};
    static_cast<void>(::mach_port_deallocate(::mach_task_self(), port));
    static_cast<void>(::mach_port_mod_refs(::mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1));
}
#endif

TEST_CASE("Presents reach the endpoint and releases reach the window in order", "[platform][hosted][surface]")
{
    SurfacePair pair;
    Connect(pair, false);
    PublishDescriptorGeneration(pair, 1);
    HostedSurfaceChannel& channel = pair.GetChannel();
    CHECK(channel.Present(1, 0, 10) == HostedSurfaceStatus::Ok);
    CHECK(channel.Present(1, 1, 11) == HostedSurfaceStatus::Ok);
    // Sequences increase, images belong to a published generation.
    CHECK(channel.Present(1, 2, 11) == HostedSurfaceStatus::InvalidArgument);
    CHECK(channel.Present(2, 2, 12) == HostedSurfaceStatus::InvalidArgument);
    CHECK(channel.Present(1, 3, 12) == HostedSurfaceStatus::InvalidArgument);

    HostedSurfaceEvent first;
    HostedSurfaceEvent second;
    REQUIRE(TryTakeEvent(pair, first));
    REQUIRE(TryTakeEvent(pair, second));
    CHECK(first.Kind == HostedSurfaceEventKind::Present);
    CHECK(first.Generation == 1);
    CHECK(first.Image == 0);
    CHECK(first.Sequence == 10);
    CHECK(second.Image == 1);
    CHECK(second.Sequence == 11);

    HostedImageRelease release;
    CHECK(channel.TakeRelease(release) == HostedSurfaceStatus::Busy);
    // Releases interleaved with window events: the producer takes the releases, and
    // HandleEvent() still applies the resize.
    CHECK(pair.Session.SendRelease(1, 1) == HostedEndpointStatus::Ok);
    CHECK(pair.Session.SendResize(800, 600, 1.0F) == HostedEndpointStatus::Ok);
    CHECK(pair.Session.SendRelease(1, 0) == HostedEndpointStatus::Ok);
    REQUIRE(TakeReleaseWithin(channel, release) == HostedSurfaceStatus::Ok);
    CHECK(release.Generation == 1);
    CHECK(release.Image == 1);
    REQUIRE(channel.TakeRelease(release) == HostedSurfaceStatus::Ok);
    CHECK(release.Image == 0);
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Width == SURFACE_WIDTH);
    REQUIRE(pair.HostedWindow->HandleEvent({}));
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Width == 800);

    CHECK(pair.Session.SendRelease(0, 0) == HostedEndpointStatus::InvalidArgument);
    CHECK(pair.Session.SendRelease(1, static_cast<uint32>(HOSTED_SURFACE_IMAGE_COUNT)) ==
          HostedEndpointStatus::InvalidArgument);
}

TEST_CASE("A consumer that stops taking events holds the producer back", "[platform][hosted][surface]")
{
    SurfacePair pair;
    Connect(pair, false);
    PublishDescriptorGeneration(pair, 1);
    HostedSurfaceChannel& channel = pair.GetChannel();
    uint64 sequence = 1;
    HostedSurfaceStatus status = HostedSurfaceStatus::Ok;
    for (usize i = 0; i < FLOOD_LIMIT && status == HostedSurfaceStatus::Ok; ++i)
    {
        status = channel.Present(1, static_cast<uint32>(i % HOSTED_SURFACE_IMAGE_COUNT), sequence);
        sequence += status == HostedSurfaceStatus::Ok ? 1 : 0;
        pair.Session.Pump(At(2));
    }
    CHECK(status == HostedSurfaceStatus::Busy);
    // Nothing is lost: every accepted frame is delivered once the consumer drains.
    uint64 expected = 1;
    HostedSurfaceEvent event;
    while (TryTakeEvent(pair, event) == true)
    {
        REQUIRE(event.Kind == HostedSurfaceEventKind::Present);
        CHECK(event.Sequence == expected);
        ++expected;
        static_cast<void>(pair.HostedWindow->HandleEvent({}));
    }
    CHECK(expected == sequence);
    CHECK(channel.Present(1, 0, sequence) == HostedSurfaceStatus::Ok);
}

TEST_CASE("A release waits for a full send queue instead of being dropped", "[platform][hosted][surface]")
{
    SurfacePair pair;
    Connect(pair, false);
    PublishDescriptorGeneration(pair, 1);
    HostedSurfaceChannel& channel = pair.GetChannel();
    REQUIRE(channel.Present(1, 2, 1) == HostedSurfaceStatus::Ok);
    HostedSurfaceEvent event;
    REQUIRE(TryTakeEvent(pair, event));
    bool hasOverflowed = false;
    for (usize i = 0; i < FLOOD_LIMIT && hasOverflowed == false; ++i)
    {
        hasOverflowed =
            pair.Session.SendResize(static_cast<uint32>(i % 64) + 1, 1, 1.0F) == HostedEndpointStatus::Overflow;
    }
    REQUIRE(hasOverflowed);
    CHECK(pair.Session.SendRelease(1, 2) == HostedEndpointStatus::Ok);

    HostedImageRelease release;
    HostedSurfaceStatus status = HostedSurfaceStatus::Busy;
    for (usize i = 0; i < PUMP_ROUNDS && status != HostedSurfaceStatus::Ok; ++i)
    {
        REQUIRE(pair.HostedWindow->HandleEvent({}));
        pair.Session.Pump(At(2));
        status = channel.TakeRelease(release);
    }
    REQUIRE(status == HostedSurfaceStatus::Ok);
    CHECK(release.Generation == 1);
    CHECK(release.Image == 2);
}

TEST_CASE("The endpoint disconnects a window that breaks the surface protocol", "[platform][hosted][surface]")
{
    // A raw socket plays the window, so it can send what WindowHosted never would.
    const hosted::Token token = MakeToken(6);
    hosted::EndpointSession session;
    session.Configure(token, hosted::GetCurrentUserId(), MakeInitialState());
    int32 descriptors[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    hosted::SocketDescriptor endpointSide(descriptors[0]);
    hosted::SocketDescriptor windowSide(descriptors[1]);
    REQUIRE(hosted::TrySendHello(windowSide.Get(), token));
    session.Admit(ludus::foundation::Move(endpointSide), At(0));
    session.Pump(At(1));
    REQUIRE(session.GetPhase() == hosted::SessionPhase::Connected);

    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    hosted::Message message;
    SECTION("A Present before its generation was published")
    {
        message.Kind = hosted::MessageKind::Present;
        message.Generation = 4;
        message.Sequence = 1;
    }
    SECTION("Images that claim descriptors which never arrived")
    {
        message.Kind = hosted::MessageKind::Images;
        message.Generation = 1;
        message.Width = 4;
        message.Height = 4;
        message.HandleKind = HostedHandleKind::OpaqueFileDescriptor;
    }
    SECTION("Mach images without a rendezvous")
    {
        message.Kind = hosted::MessageKind::Images;
        message.Generation = 1;
        message.Width = 4;
        message.Height = 4;
        message.HandleKind = HostedHandleKind::MachPort;
    }
    SECTION("An endpoint-to-window message")
    {
        message.Kind = hosted::MessageKind::Release;
        message.Generation = 1;
    }
    REQUIRE(hosted::TryEncodeMessage(message, {bytes.GetData(), bytes.GetSize()}, size));
    REQUIRE(::write(windowSide.Get(), bytes.GetData(), size) == static_cast<ssize_t>(size));
    session.Pump(At(2));
    CHECK(session.GetPhase() == hosted::SessionPhase::Disconnected);
    HostedSurfaceEvent event;
    CHECK_FALSE(session.TryTakeSurfaceEvent(event));
    CHECK(session.SendRelease(1, 0) == HostedEndpointStatus::NotConnected);
}

TEST_CASE("A window refuses endpoint-bound surface messages from the endpoint", "[platform][hosted][surface]")
{
    int32 descriptors[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    hosted::SocketDescriptor endpointSide(descriptors[0]);
    hosted::SocketDescriptor windowSide(descriptors[1]);
    REQUIRE(hosted::TryConfigureSocket(windowSide.Get()));
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    hosted::Message welcome;
    welcome.Kind = hosted::MessageKind::Welcome;
    REQUIRE(hosted::TryEncodeMessage(welcome, {bytes.GetData(), bytes.GetSize()}, size));
    REQUIRE(::write(endpointSide.Get(), bytes.GetData(), size) == static_cast<ssize_t>(size));
    HostedRefusal refusal = HostedRefusal::None;
    UniquePtr<Window> window = hosted::OpenWindow(ludus::foundation::Move(windowSide),
                                                  MakeToken(0),
                                                  MakeCreateInfo(),
                                                  OPEN_TIMEOUT_MILLISECONDS,
                                                  refusal);
    REQUIRE(window.Get() != nullptr);
    hosted::Message present;
    present.Kind = hosted::MessageKind::Present;
    present.Generation = 1;
    REQUIRE(hosted::TryEncodeMessage(present, {bytes.GetData(), bytes.GetSize()}, size));
    REQUIRE(::write(endpointSide.Get(), bytes.GetData(), size) == static_cast<ssize_t>(size));
    CHECK_FALSE(window->HandleEvent({}));
    HostedImageRelease release;
    CHECK(window->GetNativeWindowInfo().HostedSurfaceOrNull->TakeRelease(release) == HostedSurfaceStatus::Disconnected);
}
