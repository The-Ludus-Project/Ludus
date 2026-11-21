// Hosted surface and recording channel tests (protocol version 4). An EndpointSession and a
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
#include <ludus/platform/hosted_recording.h>
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
#include <catch2/generators/catch_generators.hpp>

using ludus::foundation::int32;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::UniquePtr;
using ludus::foundation::usize;
using ludus::foundation::time::Timestamp;
using ludus::platform::HOSTED_ADAPTER_UUID_BYTES;
using ludus::platform::HOSTED_SURFACE_IMAGE_COUNT;
using ludus::platform::HostedAdapterIdentity;
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

/// A consumer adapter whose UUID bytes are all distinct.
HostedAdapterIdentity MakeConsumerAdapter()
{
    HostedAdapterIdentity adapter;
    for (usize i = 0; i < HOSTED_ADAPTER_UUID_BYTES; ++i)
    {
        adapter.DeviceUuid[i] = static_cast<uint8>(0x40 + i);
        adapter.DriverUuid[i] = static_cast<uint8>(0x80 + i);
    }
    return adapter;
}

void Connect(SurfacePair& outPair,
             bool shouldStartRendezvous,
             const HostedAdapterIdentity* consumerAdapterOrNull = nullptr)
{
    outPair.Session.Configure(outPair.SessionToken, hosted::GetCurrentUserId(), MakeInitialState());
    if (consumerAdapterOrNull != nullptr)
    {
        outPair.Session.SetConsumerAdapter(*consumerAdapterOrNull);
    }
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

TEST_CASE("Hosted surface messages round-trip through the version-3 codec", "[platform][hosted][surface]")
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

TEST_CASE("The version-3 Welcome carries the consumer's adapter", "[platform][hosted][surface]")
{
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    hosted::Message decoded;
    usize consumed = 0;
    const HostedAdapterIdentity adapter = MakeConsumerAdapter();

    hosted::Message welcome;
    welcome.Kind = hosted::MessageKind::Welcome;
    welcome.Width = 8;
    welcome.Height = 8;
    welcome.HasConsumerAdapter = true;
    welcome.DeviceUuid = adapter.DeviceUuid;
    welcome.DriverUuid = adapter.DriverUuid;
    REQUIRE(hosted::TryEncodeMessage(welcome, {bytes.GetData(), bytes.GetSize()}, size));
    CHECK(size == hosted::HEADER_BYTES + hosted::WELCOME_PAYLOAD_BYTES);
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    CHECK(decoded.HasConsumerAdapter == true);
    CHECK(std::memcmp(decoded.DeviceUuid.GetData(), adapter.DeviceUuid.GetData(), HOSTED_ADAPTER_UUID_BYTES) == 0);
    CHECK(std::memcmp(decoded.DriverUuid.GetData(), adapter.DriverUuid.GetData(), HOSTED_ADAPTER_UUID_BYTES) == 0);

    // The flags byte follows the version and visibility; the UUIDs end the payload.
    constexpr usize FLAGS_OFFSET = hosted::HEADER_BYTES + 3;
    constexpr usize LAST_UUID_OFFSET = hosted::HEADER_BYTES + hosted::WELCOME_PAYLOAD_BYTES - 1;
    CHECK(bytes[FLAGS_OFFSET] == hosted::WELCOME_FLAG_CONSUMER_ADAPTER);
    CHECK(bytes[LAST_UUID_OFFSET] == adapter.DriverUuid[HOSTED_ADAPTER_UUID_BYTES - 1]);
    bytes[FLAGS_OFFSET] = static_cast<uint8>(hosted::WELCOME_FLAG_CONSUMER_ADAPTER | 0x02U);
    CHECK(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Malformed);

    // Without an adapter the UUID bytes are reserved and must be zero, whatever the message holds.
    welcome.HasConsumerAdapter = false;
    REQUIRE(hosted::TryEncodeMessage(welcome, {bytes.GetData(), bytes.GetSize()}, size));
    CHECK(size == hosted::HEADER_BYTES + hosted::WELCOME_PAYLOAD_BYTES);
    CHECK(bytes[FLAGS_OFFSET] == 0);
    CHECK(bytes[LAST_UUID_OFFSET] == 0);
    hosted::Message plain;
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, plain, consumed) == hosted::DecodeStatus::Ok);
    CHECK(plain.HasConsumerAdapter == false);
    CHECK(plain.DeviceUuid[0] == 0);
    bytes[LAST_UUID_OFFSET] = 1;
    CHECK(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Malformed);

    // A version-2 Welcome is shorter; a version-3 window cannot read it as its own.
    hosted::Message shorter;
    REQUIRE(hosted::TryEncodeMessage(welcome, {bytes.GetData(), bytes.GetSize()}, size));
    const ludus::foundation::uint16 version2PayloadBytes =
        static_cast<ludus::foundation::uint16>(hosted::WELCOME_PAYLOAD_BYTES - (2 * HOSTED_ADAPTER_UUID_BYTES));
    bytes[2] = static_cast<uint8>(version2PayloadBytes & 0xFFU);
    bytes[3] = static_cast<uint8>(version2PayloadBytes >> 8U);
    CHECK(hosted::TryDecodeMessage({bytes.GetData(), hosted::HEADER_BYTES + version2PayloadBytes}, shorter, consumed) ==
          hosted::DecodeStatus::Malformed);
}

TEST_CASE("A hosted window reports the consumer adapter announced in its Welcome", "[platform][hosted][surface]")
{
    const HostedAdapterIdentity adapter = MakeConsumerAdapter();
    SurfacePair announced;
    Connect(announced, false, &adapter);
    HostedAdapterIdentity reported;
    REQUIRE(announced.GetChannel().TryGetConsumerAdapter(reported));
    CHECK(std::memcmp(reported.DeviceUuid.GetData(), adapter.DeviceUuid.GetData(), HOSTED_ADAPTER_UUID_BYTES) == 0);
    CHECK(std::memcmp(reported.DriverUuid.GetData(), adapter.DriverUuid.GetData(), HOSTED_ADAPTER_UUID_BYTES) == 0);

    // Without an announcement there is no preference, and the output is untouched.
    SurfacePair silent;
    Connect(silent, false);
    HostedAdapterIdentity untouched = adapter;
    CHECK_FALSE(silent.GetChannel().TryGetConsumerAdapter(untouched));
    CHECK(untouched.DeviceUuid[0] == adapter.DeviceUuid[0]);

    // Configure() forgets an adapter from an earlier session.
    SurfacePair reused;
    reused.Session.SetConsumerAdapter(adapter);
    Connect(reused, false);
    CHECK_FALSE(reused.GetChannel().TryGetConsumerAdapter(untouched));
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

TEST_CASE("A window of an earlier protocol version is refused as incompatible", "[platform][hosted][surface]")
{
    const ludus::foundation::uint16 version =
        GENERATE(ludus::foundation::uint16{1},
                 ludus::foundation::uint16{2},
                 ludus::foundation::uint16{3},
                 static_cast<ludus::foundation::uint16>(hosted::PROTOCOL_VERSION - 1));
    const hosted::Token token = MakeToken(2);
    hosted::EndpointSession session;
    session.Configure(token, hosted::GetCurrentUserId(), MakeInitialState());
    int32 descriptors[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    hosted::SocketDescriptor endpointSide(descriptors[0]);
    hosted::SocketDescriptor windowSide(descriptors[1]);
    hosted::Message hello;
    hello.Kind = hosted::MessageKind::Hello;
    hello.Version = version;
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

namespace
{
using ludus::platform::HostedPassKind;
using ludus::platform::HostedRecordingAcknowledgement;
using ludus::platform::HostedRecordingChannel;
using ludus::platform::HostedRecordingEvent;
using ludus::platform::HostedRecordingEventKind;
using ludus::platform::HostedRecordingRequest;
using ludus::platform::HostedRecordingRequestKind;
using ludus::platform::HostedRecordingResult;
using ludus::platform::HostedRecordingSample;

HostedRecordingChannel& GetRecordingChannel(const SurfacePair& pair)
{
    HostedRecordingChannel* channelOrNull = pair.HostedWindow->GetNativeWindowInfo().HostedRecordingOrNull;
    REQUIRE(channelOrNull != nullptr);
    return *channelOrNull;
}

HostedRecordingRequest MakeRequest(HostedRecordingRequestKind kind, uint64 recording, uint64 request)
{
    return { .Kind = kind, .Recording = recording, .Request = request };
}

template <ludus::foundation::usize Size>
void CopyName(const char* text, ludus::foundation::StaticArray<char, Size>& outName)
{
    outName = {};
    std::memcpy(outName.GetData(), text, std::strlen(text));
}

// Recording then frame, as in HostedRecordingSample.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
HostedRecordingSample MakeSample(uint64 recording, uint64 frame)
{
    HostedRecordingSample sample;
    sample.Recording = recording;
    sample.Frame = frame;
    sample.SubmissionTicks = 100;
    sample.CollectionTicks = 250;
    sample.DurationNanoseconds = 42;
    sample.ScopeIdentity = 7;
    sample.ScopePassIndex = 1;
    sample.LogicalPassCount = 2;
    sample.Kind = HostedPassKind::Compute;
    sample.HasCpuInterval = true;
    sample.StartTicks = 120;
    sample.EndTicks = 162;
    sample.ClockUncertaintyNanoseconds = 3;
    CopyName("Scene/Opaque", sample.Name);
    return sample;
}

/// Pump the window until it has received `request`, then take it as an attached recorder.
bool TakeRequest(SurfacePair& inoutPair, HostedRecordingRequest& outRequest)
{
    for (usize i = 0; i < PUMP_ROUNDS; ++i)
    {
        inoutPair.Session.Pump(At(2));
        static_cast<void>(inoutPair.HostedWindow->HandleEvent({}));
        if (GetRecordingChannel(inoutPair).TryTakeRecordingRequest(outRequest) == true)
        {
            return true;
        }
    }
    return false;
}

bool TakeRecordingEvent(SurfacePair& inoutPair, HostedRecordingEvent& outEvent)
{
    for (usize i = 0; i < PUMP_ROUNDS; ++i)
    {
        if (inoutPair.Session.TryTakeRecordingEvent(outEvent) == true)
        {
            return true;
        }
        static_cast<void>(inoutPair.HostedWindow->HandleEvent({}));
        inoutPair.Session.Pump(At(2));
    }
    return false;
}

/// Start `recording` and answer it Ok from frame `frame` as an attached recorder.
// Recording, request, then frame, as in HostedRecordingAcknowledgement.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void StartRecording(SurfacePair& inoutPair, uint64 recording, uint64 request, uint64 frame)
{
    REQUIRE(inoutPair.Session.SendRecordingRequest(
                MakeRequest(HostedRecordingRequestKind::Start, recording, request)) == HostedEndpointStatus::Ok);
    HostedRecordingRequest taken;
    REQUIRE(TakeRequest(inoutPair, taken));
    HostedRecordingAcknowledgement acknowledgement;
    acknowledgement.Kind = taken.Kind;
    acknowledgement.Recording = taken.Recording;
    acknowledgement.Request = taken.Request;
    acknowledgement.Frame = frame;
    acknowledgement.Ticks = 1;
    REQUIRE(GetRecordingChannel(inoutPair).Acknowledge(acknowledgement) == HostedSurfaceStatus::Ok);
    HostedRecordingEvent event;
    REQUIRE(TakeRecordingEvent(inoutPair, event));
    REQUIRE(event.Kind == HostedRecordingEventKind::Acknowledgement);
}
} // namespace

TEST_CASE("Hosted recording messages round-trip through the recording codec", "[platform][hosted][recording]")
{
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    hosted::Message request;
    request.Kind = hosted::MessageKind::RecordingRequest;
    request.RecordingRequest = MakeRequest(HostedRecordingRequestKind::Stop, 3, 9);
    REQUIRE(hosted::TryEncodeMessage(request, {bytes.GetData(), bytes.GetSize()}, size));
    CHECK(size == hosted::HEADER_BYTES + hosted::RECORDING_REQUEST_PAYLOAD_BYTES);
    hosted::Message decoded;
    usize consumed = 0;
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    CHECK(decoded.RecordingRequest.Kind == HostedRecordingRequestKind::Stop);
    CHECK(decoded.RecordingRequest.Recording == 3);
    CHECK(decoded.RecordingRequest.Request == 9);

    hosted::Message acknowledgement;
    acknowledgement.Kind = hosted::MessageKind::RecordingAcknowledgement;
    HostedRecordingAcknowledgement& sent = acknowledgement.RecordingAcknowledgement;
    sent.Kind = HostedRecordingRequestKind::Stop;
    sent.Result = HostedRecordingResult::Ok;
    sent.Recording = 3;
    sent.Request = 9;
    sent.Frame = 120;
    sent.Ticks = 555;
    sent.Coverage =
    {
        .SubmittedFrames = 10,
        .MeasuredFrames = 6,
        .SkippedFrames = 4,
        .DeliveredSamples = 5,
        .DroppedSamples = 1,
        .InvalidSamples = 2,
        .QueryDroppedPasses = 3,
        .UnresolvedBatches = 1,
    };
    CopyName("game \xc3\xa9", sent.ProducerName);
    REQUIRE(hosted::TryEncodeMessage(acknowledgement, {bytes.GetData(), bytes.GetSize()}, size));
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    CHECK(decoded.RecordingAcknowledgement.Frame == 120);
    CHECK(decoded.RecordingAcknowledgement.Ticks == 555);
    CHECK(decoded.RecordingAcknowledgement.Coverage.SkippedFrames == 4);
    CHECK(decoded.RecordingAcknowledgement.Coverage.UnresolvedBatches == 1);
    CHECK(std::strcmp(decoded.RecordingAcknowledgement.ProducerName.GetData(), "game \xc3\xa9") == 0);
    CHECK_FALSE(decoded.RecordingAcknowledgement.IsPassTimingSupported);

    hosted::Message sample;
    sample.Kind = hosted::MessageKind::RecordingSample;
    sample.RecordingSample = MakeSample(3, 17);
    REQUIRE(hosted::TryEncodeMessage(sample, {bytes.GetData(), bytes.GetSize()}, size));
    CHECK(size == hosted::MAX_MESSAGE_BYTES);
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    const HostedRecordingSample& received = decoded.RecordingSample;
    CHECK(received.Frame == 17);
    CHECK(received.DurationNanoseconds == 42);
    CHECK(received.ScopePassIndex == 1);
    CHECK(received.LogicalPassCount == 2);
    CHECK(received.Kind == HostedPassKind::Compute);
    CHECK(received.HasCpuInterval);
    CHECK(received.EndTicks == 162);
    CHECK(std::strcmp(received.Name.GetData(), "Scene/Opaque") == 0);

    // Name padding past the terminator is reserved.
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> padded = bytes;
    padded[size - 1] = 'x';
    CHECK(hosted::TryDecodeMessage({padded.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Malformed);
    // Invalid UTF-8, an empty name, zero identities and inverted intervals are refused.
    HostedRecordingSample invalid = MakeSample(3, 17);
    invalid.Name[0] = static_cast<char>(0xFF);
    sample.RecordingSample = invalid;
    CHECK_FALSE(hosted::TryEncodeMessage(sample, {bytes.GetData(), bytes.GetSize()}, size));
    invalid = MakeSample(3, 17);
    invalid.Name = {};
    CHECK_FALSE(hosted::IsValidRecordingSample(invalid));
    invalid = MakeSample(0, 17);
    CHECK_FALSE(hosted::IsValidRecordingSample(invalid));
    invalid = MakeSample(3, 0);
    CHECK_FALSE(hosted::IsValidRecordingSample(invalid));
    invalid = MakeSample(3, 17);
    invalid.EndTicks = invalid.StartTicks - 1;
    CHECK_FALSE(hosted::IsValidRecordingSample(invalid));
    CHECK_FALSE(hosted::IsValidRecordingRequest(MakeRequest(HostedRecordingRequestKind::Start, 0, 1)));
    CHECK_FALSE(hosted::IsValidRecordingRequest(MakeRequest(HostedRecordingRequestKind::Start, 1, 0)));
}

TEST_CASE("A Start acknowledgement carries whether the game's device has per-pass counters",
          "[platform][hosted][recording]")
{
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    usize consumed = 0;
    hosted::Message message;
    message.Kind = hosted::MessageKind::RecordingAcknowledgement;
    HostedRecordingAcknowledgement& sent = message.RecordingAcknowledgement;
    sent.Kind = HostedRecordingRequestKind::Start;
    sent.Result = HostedRecordingResult::Ok;
    sent.Recording = 4;
    sent.Request = 11;
    sent.Frame = 30;
    sent.Ticks = 77;
    for (const bool isPassTimingSupported : {true, false})
    {
        sent.IsPassTimingSupported = isPassTimingSupported;
        REQUIRE(hosted::TryEncodeMessage(message, {bytes.GetData(), bytes.GetSize()}, size));
        CHECK(size == hosted::HEADER_BYTES + hosted::RECORDING_ACKNOWLEDGEMENT_PAYLOAD_BYTES);
        hosted::Message decoded;
        REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
        CHECK(decoded.RecordingAcknowledgement.IsPassTimingSupported == isPassTimingSupported);
        CHECK(decoded.RecordingAcknowledgement.Frame == 30);
    }
    // The flags byte follows kind, result and name length; unknown flags are a violation.
    sent.IsPassTimingSupported = true;
    REQUIRE(hosted::TryEncodeMessage(message, {bytes.GetData(), bytes.GetSize()}, size));
    constexpr usize FLAGS_OFFSET = hosted::HEADER_BYTES + 3;
    CHECK(bytes[FLAGS_OFFSET] == hosted::ACKNOWLEDGEMENT_FLAG_PASS_TIMING);
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> unknown = bytes;
    unknown[FLAGS_OFFSET] = static_cast<uint8>(hosted::ACKNOWLEDGEMENT_FLAG_PASS_TIMING << 1);
    hosted::Message decoded;
    CHECK(hosted::TryDecodeMessage({unknown.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Malformed);
    // Only a successful Start may claim per-pass counters.
    HostedRecordingAcknowledgement invalid = sent;
    invalid.Result = HostedRecordingResult::Unsupported;
    CHECK_FALSE(hosted::IsValidRecordingAcknowledgement(invalid));
    invalid = sent;
    invalid.Kind = HostedRecordingRequestKind::Stop;
    CHECK_FALSE(hosted::IsValidRecordingAcknowledgement(invalid));
    message.RecordingAcknowledgement = invalid;
    CHECK_FALSE(hosted::TryEncodeMessage(message, {bytes.GetData(), bytes.GetSize()}, size));
}

TEST_CASE("A whole-frame recording sample names no pass", "[platform][hosted][recording]")
{
    HostedRecordingSample frame;
    frame.Recording = 4;
    frame.Frame = 31;
    frame.SubmissionTicks = 100;
    frame.CollectionTicks = 150;
    frame.DurationNanoseconds = 16'000'000;
    frame.IsFrame = true;
    REQUIRE(hosted::IsValidRecordingSample(frame));
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    usize consumed = 0;
    hosted::Message message;
    message.Kind = hosted::MessageKind::RecordingSample;
    message.RecordingSample = frame;
    REQUIRE(hosted::TryEncodeMessage(message, {bytes.GetData(), bytes.GetSize()}, size));
    hosted::Message decoded;
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    CHECK(decoded.RecordingSample.IsFrame);
    CHECK(decoded.RecordingSample.ScopeIdentity == 0);
    CHECK(decoded.RecordingSample.Name[0] == '\0');
    CHECK(decoded.RecordingSample.DurationNanoseconds == 16'000'000);
    // A pass sample is still not a frame.
    message.RecordingSample = MakeSample(4, 31);
    REQUIRE(hosted::TryEncodeMessage(message, {bytes.GetData(), bytes.GetSize()}, size));
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    CHECK_FALSE(decoded.RecordingSample.IsFrame);
    // A frame carries none of a pass's fields.
    HostedRecordingSample invalid = frame;
    invalid.ScopeIdentity = 9;
    CHECK_FALSE(hosted::IsValidRecordingSample(invalid));
    invalid = frame;
    CopyName("Scene/Opaque", invalid.Name);
    CHECK_FALSE(hosted::IsValidRecordingSample(invalid));
    invalid = frame;
    invalid.ScopePassIndex = 1;
    CHECK_FALSE(hosted::IsValidRecordingSample(invalid));
    invalid = frame;
    invalid.Kind = HostedPassKind::Compute;
    CHECK_FALSE(hosted::IsValidRecordingSample(invalid));
    invalid = frame;
    invalid.HasCpuInterval = true;
    invalid.StartTicks = 1;
    invalid.EndTicks = 2;
    CHECK_FALSE(hosted::IsValidRecordingSample(invalid));
    // A frame still needs its recording and frame.
    invalid = frame;
    invalid.Frame = 0;
    CHECK_FALSE(hosted::IsValidRecordingSample(invalid));
}

TEST_CASE("A window without a recorder answers recording requests itself", "[platform][hosted][recording]")
{
    SurfacePair pair;
    Connect(pair, false);
    REQUIRE(pair.Session.SendRecordingRequest(MakeRequest(HostedRecordingRequestKind::Start, 1, 1)) ==
            HostedEndpointStatus::Ok);
    HostedRecordingEvent event;
    REQUIRE(TakeRecordingEvent(pair, event));
    REQUIRE(event.Kind == HostedRecordingEventKind::Acknowledgement);
    CHECK(event.Acknowledgement.Kind == HostedRecordingRequestKind::Start);
    CHECK(event.Acknowledgement.Result == HostedRecordingResult::Unsupported);
    CHECK(event.Acknowledgement.Recording == 1);
    CHECK(event.Acknowledgement.Request == 1);
    REQUIRE(pair.Session.SendRecordingRequest(MakeRequest(HostedRecordingRequestKind::Stop, 1, 2)) ==
            HostedEndpointStatus::Ok);
    REQUIRE(TakeRecordingEvent(pair, event));
    CHECK(event.Acknowledgement.Result == HostedRecordingResult::UnknownRecording);
    // No recorder takes the requests the window answered.
    HostedRecordingRequest request;
    CHECK_FALSE(GetRecordingChannel(pair).TryTakeRecordingRequest(request));
}

TEST_CASE("Recording events reach the endpoint and outlive the window's disconnection", "[platform][hosted][recording]")
{
    SurfacePair pair;
    Connect(pair, false);
    CHECK(pair.Session.GetPeerProcessId() == static_cast<uint64>(::getpid()));
    HostedRecordingChannel& channel = GetRecordingChannel(pair);
    channel.AttachRecorder();
    StartRecording(pair, 4, 1, 1);
    for (uint64 frame = 1; frame <= 3; ++frame)
    {
        REQUIRE(channel.SendSample(MakeSample(4, frame)) == HostedSurfaceStatus::Ok);
    }
    CHECK(channel.SendSample(MakeSample(4, 0)) == HostedSurfaceStatus::InvalidArgument);
    // The game goes away before it acknowledges a Stop.
    pair.HostedWindow.Reset();
    for (usize i = 0; i < PUMP_ROUNDS && pair.Session.GetPhase() != hosted::SessionPhase::Disconnected; ++i)
    {
        pair.Session.Pump(At(3));
    }
    REQUIRE(pair.Session.GetPhase() == hosted::SessionPhase::Disconnected);
    for (uint64 frame = 1; frame <= 3; ++frame)
    {
        HostedRecordingEvent event;
        REQUIRE(pair.Session.TryTakeRecordingEvent(event));
        REQUIRE(event.Kind == HostedRecordingEventKind::Sample);
        CHECK(event.Sample.Recording == 4);
        CHECK(event.Sample.Frame == frame);
    }
    HostedRecordingEvent none;
    CHECK_FALSE(pair.Session.TryTakeRecordingEvent(none));
    // The admitted process stays known after the loss; requests no longer go out.
    CHECK(pair.Session.GetPeerProcessId() == static_cast<uint64>(::getpid()));
    CHECK(pair.Session.SendRecordingRequest(MakeRequest(HostedRecordingRequestKind::Stop, 4, 2)) ==
          HostedEndpointStatus::NotConnected);
}

TEST_CASE("An earlier recording's delayed samples arrive tagged with that recording", "[platform][hosted][recording]")
{
    SurfacePair pair;
    Connect(pair, false);
    HostedRecordingChannel& channel = GetRecordingChannel(pair);
    channel.AttachRecorder();
    StartRecording(pair, 1, 1, 1);
    StartRecording(pair, 2, 2, 1);
    // Frame 1 repeats; only the recording identity tells the two apart.
    REQUIRE(channel.SendSample(MakeSample(1, 1)) == HostedSurfaceStatus::Ok);
    REQUIRE(channel.SendSample(MakeSample(2, 1)) == HostedSurfaceStatus::Ok);
    HostedRecordingEvent event;
    REQUIRE(TakeRecordingEvent(pair, event));
    CHECK(event.Sample.Recording == 1);
    REQUIRE(TakeRecordingEvent(pair, event));
    CHECK(event.Sample.Recording == 2);
    // A sample of a recording that was never started breaks the protocol.
    REQUIRE(channel.SendSample(MakeSample(3, 1)) == HostedSurfaceStatus::Ok);
    for (usize i = 0; i < PUMP_ROUNDS && pair.Session.GetPhase() == hosted::SessionPhase::Connected; ++i)
    {
        pair.Session.Pump(At(3));
    }
    CHECK(pair.Session.GetPhase() == hosted::SessionPhase::Disconnected);
}

TEST_CASE("The endpoint admits only ordered requests and their own acknowledgements", "[platform][hosted][recording]")
{
    SurfacePair pair;
    Connect(pair, false);
    HostedRecordingChannel& channel = GetRecordingChannel(pair);
    channel.AttachRecorder();
    CHECK(pair.Session.SendRecordingRequest(MakeRequest(HostedRecordingRequestKind::Stop, 1, 1)) ==
          HostedEndpointStatus::InvalidArgument);
    REQUIRE(pair.Session.SendRecordingRequest(MakeRequest(HostedRecordingRequestKind::Start, 5, 3)) ==
            HostedEndpointStatus::Ok);
    // Request numbers increase and a recording identity starts once.
    CHECK(pair.Session.SendRecordingRequest(MakeRequest(HostedRecordingRequestKind::Stop, 5, 3)) ==
          HostedEndpointStatus::InvalidArgument);
    CHECK(pair.Session.SendRecordingRequest(MakeRequest(HostedRecordingRequestKind::Start, 5, 4)) ==
          HostedEndpointStatus::InvalidArgument);
    REQUIRE(pair.Session.SendRecordingRequest(MakeRequest(HostedRecordingRequestKind::Stop, 5, 4)) ==
            HostedEndpointStatus::Ok);
    REQUIRE(pair.Session.SendRecordingRequest(MakeRequest(HostedRecordingRequestKind::Start, 6, 5)) ==
            HostedEndpointStatus::Ok);
    REQUIRE(pair.Session.SendRecordingRequest(MakeRequest(HostedRecordingRequestKind::Stop, 6, 6)) ==
            HostedEndpointStatus::Ok);
    // At most MAX_HOSTED_RECORDING_REQUESTS stay unanswered.
    CHECK(pair.Session.SendRecordingRequest(MakeRequest(HostedRecordingRequestKind::Start, 7, 7)) ==
          HostedEndpointStatus::InvalidState);
    // An acknowledgement must answer an outstanding request of the same kind and recording.
    HostedRecordingAcknowledgement forged;
    forged.Kind = HostedRecordingRequestKind::Start;
    forged.Recording = 5;
    forged.Request = 4;
    REQUIRE(channel.Acknowledge(forged) == HostedSurfaceStatus::Ok);
    for (usize i = 0; i < PUMP_ROUNDS && pair.Session.GetPhase() == hosted::SessionPhase::Connected; ++i)
    {
        pair.Session.Pump(At(3));
    }
    CHECK(pair.Session.GetPhase() == hosted::SessionPhase::Disconnected);
}

TEST_CASE("Recording samples never take the send room of the window's frames", "[platform][hosted][recording]")
{
    SurfacePair pair;
    Connect(pair, false);
    PublishDescriptorGeneration(pair, 1);
    HostedRecordingChannel& channel = GetRecordingChannel(pair);
    channel.AttachRecorder();
    StartRecording(pair, 1, 1, 1);
    // The endpoint stops reading: its socket and the window's queue fill with samples.
    HostedSurfaceStatus status = HostedSurfaceStatus::Ok;
    usize sent = 0;
    for (usize i = 0; i < FLOOD_LIMIT && status == HostedSurfaceStatus::Ok; ++i)
    {
        status = channel.SendSample(MakeSample(1, i + 1));
        sent += status == HostedSurfaceStatus::Ok ? 1 : 0;
    }
    REQUIRE(status == HostedSurfaceStatus::Busy);
    // A frame still fits; the consumer receives it after the samples queued before it.
    CHECK(pair.GetChannel().Present(1, 0, 1) == HostedSurfaceStatus::Ok);
    HostedSurfaceEvent event;
    bool hasEvent = false;
    for (usize i = 0; i < PUMP_ROUNDS && hasEvent == false; ++i)
    {
        // The window flushes its queue as it is pumped; the endpoint drains it.
        static_cast<void>(pair.HostedWindow->HandleEvent({}));
        pair.Session.Pump(At(2));
        hasEvent = pair.Session.TryTakeSurfaceEvent(event);
    }
    REQUIRE(hasEvent);
    CHECK(event.Kind == HostedSurfaceEventKind::Present);
    CHECK(event.Sequence == 1);
    // The endpoint's bounded queue kept what fit and counted the rest.
    usize received = 0;
    HostedRecordingEvent recording;
    while (pair.Session.TryTakeRecordingEvent(recording) == true)
    {
        ++received;
    }
    CHECK(received + pair.Session.GetDroppedRecordingEvents() <= sent);
    CHECK(received > 0);
}
