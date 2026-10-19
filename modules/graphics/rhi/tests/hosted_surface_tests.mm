// Two-process hosted surface tests on Metal (P5 of the play-in-viewport design,
// docs/architecture/editor-gui-systems.md, "Delivery stages"). The test process
// plays the editor: it owns the hosted-window endpoint and imports the game's
// IOSurfaces onto its own MTLDevice. The game is ludus_rhi_hosted_producer, an
// ordinary RHI application rendering into its hosted window.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/hosted_surface.h>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/graphics/rhi/rhi.h>

#include "hosted_test_harness.h"
#include "hosted_test_protocol.h"

#import <Metal/MTLBlitCommandEncoder.h>
#import <Metal/MTLBuffer.h>
#import <Metal/MTLCommandBuffer.h>
#import <Metal/MTLCommandQueue.h>
#import <Metal/MTLDevice.h>
#import <Metal/MTLTexture.h>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace hosted_test = ludus::graphics::rhi::hosted_test;

namespace
{
constexpr usize BYTES_PER_PIXEL = 4;
constexpr usize ROW_ALIGNMENT = 256;
constexpr usize RESIZE_ROUNDS = 5;

/// The editor's own Metal device, as Qt would own it.
struct ConsumerDevice final
{
    id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> Queue = [Device newCommandQueue];

    [[nodiscard]] rhi::NativeDeviceIdentity GetIdentity() const
    {
        rhi::NativeDeviceIdentity identity;
        identity.Api = rhi::Backend::Metal;
        identity.Device = rhi::NativeLogicalDevice((__bridge void*)Device);
        identity.CommandQueue = rhi::NativeCommandQueue((__bridge void*)Queue);
        return identity;
    }
};

/// Sample a frame on the consumer's device: copy its texture to a shared buffer.
StaticArray<uint8, BYTES_PER_PIXEL> ReadCenterPixel(const ConsumerDevice& consumer, const rhi::HostedFrame& frame)
{
    id<MTLTexture> texture = (__bridge id<MTLTexture>)frame.MetalTexture.GetObject();
    REQUIRE(texture != nil);
    REQUIRE(texture.width == frame.Width);
    REQUIRE(texture.height == frame.Height);
    REQUIRE(texture.pixelFormat == MTLPixelFormatRGBA8Unorm);
    const usize pitch = ((frame.Width * BYTES_PER_PIXEL) + ROW_ALIGNMENT - 1) / ROW_ALIGNMENT * ROW_ALIGNMENT;
    id<MTLBuffer> buffer = [consumer.Device newBufferWithLength:pitch * frame.Height
                                                        options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = [consumer.Queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    [encoder copyFromTexture:texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOrigin{0, 0, 0}
                      sourceSize:MTLSize{frame.Width, frame.Height, 1}
                        toBuffer:buffer
               destinationOffset:0
          destinationBytesPerRow:pitch
        destinationBytesPerImage:pitch * frame.Height];
    [encoder endEncoding];
    [command commit];
    // The frame may be released only after this sampling has completed.
    [command waitUntilCompleted];
    REQUIRE(command.status == MTLCommandBufferStatusCompleted);
    const uint8* bytes = static_cast<const uint8*>(buffer.contents);
    const usize offset = (frame.Height / 2 * pitch) + (frame.Width / 2 * BYTES_PER_PIXEL);
    StaticArray<uint8, BYTES_PER_PIXEL> pixel{};
    for (usize i = 0; i < BYTES_PER_PIXEL; ++i)
    {
        pixel[i] = bytes[offset + i];
    }
    return pixel;
}

void CheckTestColor(const StaticArray<uint8, BYTES_PER_PIXEL>& pixel)
{
    for (usize i = 0; i < BYTES_PER_PIXEL; ++i)
    {
        const int32 difference = static_cast<int32>(pixel[i]) - static_cast<int32>(hosted_test::HOSTED_TEST_BYTES[i]);
        CHECK(difference <= hosted_test::COLOR_TOLERANCE);
        CHECK(-difference <= hosted_test::COLOR_TOLERANCE);
    }
}

void RequireMetalDevice(const ConsumerDevice& consumer)
{
    if (consumer.Device == nil)
    {
        SKIP("No Metal adapter on this host");
    }
}
} // namespace

TEST_CASE("A hosted game's frame arrives in the editor with its known colour", "[rhi][hosted][macos][gpu]")
{
    const ConsumerDevice consumer;
    RequireMetalDevice(consumer);
    hosted_test::HostedSession session;
    session.Start(consumer.GetIdentity(), "render");

    rhi::HostedFrame frame;
    REQUIRE(session.TryAcquireNext(frame));
    CHECK(frame.Width == hosted_test::INITIAL_WIDTH);
    CHECK(frame.Height == hosted_test::INITIAL_HEIGHT);
    CHECK(frame.Generation == 1);
    CHECK(frame.MetalTexture.IsNull() == false);
    CheckTestColor(ReadCenterPixel(consumer, frame));
    CHECK(session.GetConsumer().ReleaseFrame(frame) == rhi::HostedConsumerStatus::Ok);
    CHECK(session.GetConsumer().ReleaseFrame(frame) == rhi::HostedConsumerStatus::InvalidState);

    // Later frames keep arriving once the image is returned.
    rhi::HostedFrame next;
    REQUIRE(session.TryAcquireNext(next));
    CHECK(next.Sequence > frame.Sequence);
    CHECK(session.GetConsumer().ReleaseFrame(next) == rhi::HostedConsumerStatus::Ok);

    hosted_test::ProducerStatistics statistics;
    session.Close(statistics);
    CHECK(statistics.ReadyFrames >= 2);
    CHECK(session.GetConsumer().GetRefusal() == rhi::HostedConsumerRefusal::None);
}

TEST_CASE("A hosted game skips frames while the editor holds every image, and resumes on release",
          "[rhi][hosted][macos][gpu]")
{
    const ConsumerDevice consumer;
    RequireMetalDevice(consumer);
    hosted_test::HostedSession session;
    session.Start(consumer.GetIdentity(), "render");

    StaticArray<rhi::HostedFrame, ludus::platform::HOSTED_SURFACE_IMAGE_COUNT> held{};
    StaticArray<bool, ludus::platform::HOSTED_SURFACE_IMAGE_COUNT> isImageHeld{};
    for (rhi::HostedFrame& frame : held)
    {
        REQUIRE(session.TryAcquireNext(frame));
        REQUIRE(frame.Image < isImageHeld.GetSize());
        // The game never reuses an image the editor still holds.
        CHECK_FALSE(isImageHeld[frame.Image]);
        isImageHeld[frame.Image] = true;
    }
    // Every image is held: the game cannot acquire one, so nothing new arrives.
    CHECK_FALSE(
        session.PumpUntil([&session]() { return session.GetConsumer().HasFrame(); }, hosted_test::QUIET_NANOSECONDS));

    // Returning one image lets the game render into exactly that image again.
    REQUIRE(session.GetConsumer().ReleaseFrame(held[1]) == rhi::HostedConsumerStatus::Ok);
    rhi::HostedFrame resumed;
    REQUIRE(session.TryAcquireNext(resumed));
    CHECK(resumed.Image == held[1].Image);
    CHECK(resumed.Sequence > held[2].Sequence);
    CheckTestColor(ReadCenterPixel(consumer, resumed));
    CHECK(session.GetConsumer().ReleaseFrame(resumed) == rhi::HostedConsumerStatus::Ok);
    CHECK(session.GetConsumer().ReleaseFrame(held[0]) == rhi::HostedConsumerStatus::Ok);
    CHECK(session.GetConsumer().ReleaseFrame(held[2]) == rhi::HostedConsumerStatus::Ok);

    hosted_test::ProducerStatistics statistics;
    session.Close(statistics);
    CHECK(statistics.SkippedFrames > 0);
    // Skipping never waits for the editor.
    CHECK(statistics.MaxSkippedNanoseconds < hosted_test::MAX_SKIPPED_NANOSECONDS);
}

TEST_CASE("Resizing a hosted game publishes new generations and retires the old ones", "[rhi][hosted][macos][gpu]")
{
    const ConsumerDevice consumer;
    RequireMetalDevice(consumer);
    hosted_test::HostedSession session;
    session.Start(consumer.GetIdentity(), "render");

    rhi::HostedFrame shown;
    REQUIRE(session.TryAcquireNext(shown));
    uint32 generation = shown.Generation;
    // More rounds than the game keeps generations alive: it must retire them to go on.
    for (usize round = 0; round < RESIZE_ROUNDS; ++round)
    {
        const uint32 width = 32 + static_cast<uint32>(round * 8);
        const uint32 height = 16 + static_cast<uint32>(round * 4);
        REQUIRE(session.GetEndpoint().SendResize(width, height, 1.0F) == ludus::platform::HostedEndpointStatus::Ok);
        // The editor keeps showing the old image until the new generation's first frame.
        rhi::HostedFrame next;
        REQUIRE(session.TryAcquireNext(next));
        while (next.Width != width)
        {
            REQUIRE(session.GetConsumer().ReleaseFrame(next) == rhi::HostedConsumerStatus::Ok);
            REQUIRE(session.TryAcquireNext(next));
        }
        CHECK(next.Height == height);
        CHECK(next.Generation > generation);
        CheckTestColor(ReadCenterPixel(consumer, next));
        CHECK(session.GetConsumer().GetImportedGenerationCount() == 2);
        REQUIRE(session.GetConsumer().ReleaseFrame(shown) == rhi::HostedConsumerStatus::Ok);
        // Once nothing of it is held, the old generation's imports are destroyed.
        CHECK(session.GetConsumer().GetImportedGenerationCount() == 1);
        shown = next;
        generation = next.Generation;
    }
    REQUIRE(session.GetConsumer().ReleaseFrame(shown) == rhi::HostedConsumerStatus::Ok);
    hosted_test::ProducerStatistics statistics;
    session.Close(statistics);
}

TEST_CASE("The Metal consumer refuses images that are not IOSurface send rights", "[rhi][hosted][macos]")
{
    const ConsumerDevice consumer;
    RequireMetalDevice(consumer);
    hosted_test::HostedSession session;
    session.Start(consumer.GetIdentity(), "forged");
    REQUIRE(session.PumpUntil(
        [&session]() { return session.GetConsumer().GetRefusal() != rhi::HostedConsumerRefusal::None; }));
    CHECK(session.GetConsumer().GetRefusal() == rhi::HostedConsumerRefusal::UnsupportedHandle);
    CHECK(session.GetLastStatus() == rhi::HostedConsumerStatus::Refused);
    rhi::HostedFrame frame;
    CHECK(session.GetConsumer().AcquireFrame(frame) == rhi::HostedConsumerStatus::Refused);
    hosted_test::ProducerStatistics statistics;
    session.Close(statistics);
}

TEST_CASE("The hosted consumer validates its device and frames", "[rhi][hosted][macos]")
{
    ludus::platform::HostedWindowEndpoint endpoint;
    rhi::HostedSurfaceConsumer consumer;
    rhi::NativeDeviceIdentity identity;
    identity.Api = rhi::Backend::Metal;
    CHECK(consumer.Start(endpoint, identity) == rhi::HostedConsumerStatus::InvalidArgument);
    const ConsumerDevice device;
    RequireMetalDevice(device);
    identity = device.GetIdentity();
    identity.Api = rhi::Backend::Vulkan;
    CHECK(consumer.Start(endpoint, identity) == rhi::HostedConsumerStatus::InvalidArgument);
    CHECK(consumer.Pump() == rhi::HostedConsumerStatus::InvalidState);
    REQUIRE(consumer.Start(endpoint, device.GetIdentity()) == rhi::HostedConsumerStatus::Ok);
    CHECK(consumer.Start(endpoint, device.GetIdentity()) == rhi::HostedConsumerStatus::InvalidState);
    rhi::HostedFrame frame;
    CHECK(consumer.AcquireFrame(frame) == rhi::HostedConsumerStatus::Empty);
    CHECK(consumer.ReleaseFrame(frame) == rhi::HostedConsumerStatus::InvalidState);
    CHECK(consumer.Pump() == rhi::HostedConsumerStatus::NotConnected);
}
