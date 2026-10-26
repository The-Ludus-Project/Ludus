#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

// Identical public transfer contracts and exact bytes on D3D12, Vulkan and Metal.
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace
{
constexpr uint32 WIDTH = 8;
constexpr uint32 HEIGHT = 4;
constexpr usize CHANNELS = 4;
constexpr usize ROW_BYTES = WIDTH * CHANNELS;
constexpr usize TEXTURE_BYTES = ROW_BYTES * HEIGHT;
constexpr usize BUFFER_BYTES = 16;
constexpr usize POLL_SECONDS = 10;
struct TransferDevice final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    explicit TransferDevice(rhi::StorageLimits limits = {})
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Storage = limits;
        rhi::StartupInfo failure;
        const rhi::DeviceStatus status =
            rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Device, Surface, &failure);
#if defined(LUDUS_TEST_METAL)
        if (status != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
        {
            SKIP("No Metal adapter on this host");
        }
#endif
        REQUIRE(status == rhi::DeviceStatus::Ready);
    }
    void SetOffscreenTarget()
    {
        rhi::FrameTarget target{ .Width = WIDTH, .Height = HEIGHT, .ShouldPresentToSurface = false };
        const rhi::DeviceStatus status = rhi::SetFrameTarget(Device, Surface, target);
#if defined(_WIN32)
        REQUIRE(status == rhi::DeviceStatus::Ready);
#else
        if (status == rhi::DeviceStatus::Unsupported)
        {
            // These backends retain their existing headless-frame contract.
            target.ShouldPresentToSurface = true;
            REQUIRE(rhi::SetFrameTarget(Device, Surface, target) == rhi::DeviceStatus::Ready);
        }
        else
        {
            REQUIRE(status == rhi::DeviceStatus::Ready);
        }
#endif
    }
    ~TransferDevice()
    {
        static_cast<void>(rhi::DestroyDevice(Device));
    }
};
template <typename Ticket>
void RequireComplete(rhi::DeviceHandle device, Ticket ticket)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(POLL_SECONDS);
    rhi::RasterStatus status = rhi::GetStatus(device, ticket);
    while (status == rhi::RasterStatus::Pending && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
        status = rhi::GetStatus(device, ticket);
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
}
} // namespace

TEST_CASE("Portable transfers snapshot uploads and preserve failed readback outputs", "[rhi][gpu][transfer]")
{
    TransferDevice owner;
    StaticArray<uint8, BUFFER_BYTES> bytes{};
    bytes[0] = 31;
    bytes[BUFFER_BYTES - 1] = 93;
    rhi::UploadTicket upload;
    REQUIRE(rhi::RequestBufferUpload(owner.Device,
                                     {rhi::BufferRole::Vertex, BUFFER_BYTES},
                                     bytes.GetData(),
                                     bytes.GetSize(),
                                     upload) == rhi::RasterStatus::Pending);
    bytes[0] = 0; // The call consumed the source before returning.
    RequireComplete(owner.Device, upload);
    rhi::BufferHandle buffer;
    REQUIRE(rhi::TakeUploadedBuffer(owner.Device, upload, buffer) == rhi::RasterStatus::Ready);
    rhi::TransferTicket update;
    const StaticArray<uint8, CHANNELS> patch{7, 8, 9, 10};
    REQUIRE(rhi::RequestBufferUpdate(owner.Device, buffer, CHANNELS, patch.GetData(), patch.GetSize(), update) ==
            rhi::RasterStatus::Pending);
    RequireComplete(owner.Device, update);
    REQUIRE(rhi::Release(owner.Device, update) == rhi::RasterStatus::Ready);
    rhi::ReadbackTicket readback;
    CHECK(rhi::RequestBufferReadback(owner.Device, buffer, 1, BUFFER_BYTES, readback) ==
          rhi::RasterStatus::InvalidDescription);
    REQUIRE(rhi::RequestBufferReadback(owner.Device, buffer, 0, BUFFER_BYTES, readback) == rhi::RasterStatus::Pending);
    // A readback retains its source after public destruction.
    REQUIRE(rhi::Destroy(owner.Device, buffer) == rhi::RasterStatus::Ready);
    StaticArray<uint8, BUFFER_BYTES> result{};
    result[0] = 55;
    CHECK(rhi::CopyReadback(owner.Device, readback, result.GetData(), 1) == rhi::RasterStatus::InvalidDescription);
    CHECK(result[0] == 55);
    RequireComplete(owner.Device, readback);
    REQUIRE(rhi::CopyReadback(owner.Device, readback, result.GetData(), result.GetSize()) == rhi::RasterStatus::Ready);
    CHECK(result[0] == 31);
    CHECK(result[BUFFER_BYTES - 1] == 93);
    for (usize index = 0; index < CHANNELS; ++index)
    {
        CHECK(result[CHANNELS + index] == patch[index]);
    }
    const rhi::ReadbackTicket stale = readback;
    REQUIRE(rhi::Release(owner.Device, readback) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetStatus(owner.Device, stale) == rhi::RasterStatus::InvalidHandle);
}

TEST_CASE("Portable texture transfers initialize mips and preserve region orientation and padding",
          "[rhi][gpu][transfer]")
{
    TransferDevice owner;
    rhi::TextureDescription description;
    description.Width = WIDTH;
    description.Height = HEIGHT;
    description.MipLevels = 2;
    description.DeferredInitialization = true;
    rhi::TextureHandle source;
    REQUIRE(rhi::CreateTexture(owner.Device, description, {}, source) == rhi::RasterStatus::Ready);
    StaticArray<uint8, TEXTURE_BYTES> bytes{};
    for (usize index = 0; index < TEXTURE_BYTES; ++index)
    {
        bytes[index] = static_cast<uint8>(index);
    }
    const rhi::TextureRegion full{0, 0, 0, WIDTH, HEIGHT};
    const rhi::TextureRegion partial{0, 1, 1, 2, 2};
    rhi::TransferTicket update;
    CHECK(
        rhi::RequestTextureUpdate(owner.Device, source, partial, bytes.GetData(), CHANNELS * 4, CHANNELS * 2, update) ==
        rhi::RasterStatus::InvalidState);
    REQUIRE(
        rhi::RequestTextureUpdate(owner.Device, source, full, bytes.GetData(), bytes.GetSize(), ROW_BYTES, update) ==
        rhi::RasterStatus::Pending);
    RequireComplete(owner.Device, update);
    REQUIRE(rhi::Release(owner.Device, update) == rhi::RasterStatus::Ready);
    rhi::TextureHandle destination;
    REQUIRE(rhi::CreateTexture(owner.Device, description, {}, destination) == rhi::RasterStatus::Ready);
    // Copy a rectangle into a complete smaller mip; it becomes independently defined.
    const rhi::TextureRegion sourceRegion{0, 2, 1, WIDTH / 2, HEIGHT / 2};
    const rhi::TextureRegion destinationRegion{1, 0, 0, WIDTH / 2, HEIGHT / 2};
    REQUIRE(rhi::RequestTextureCopy(owner.Device, source, sourceRegion, destination, destinationRegion, update) ==
            rhi::RasterStatus::Pending);
    REQUIRE(rhi::Destroy(owner.Device, source) == rhi::RasterStatus::Ready);
    RequireComplete(owner.Device, update);
    REQUIRE(rhi::Release(owner.Device, update) == rhi::RasterStatus::Ready);
    constexpr usize CALLER_PITCH = ROW_BYTES;
    constexpr usize RESULT_BYTES = CALLER_PITCH * HEIGHT / 2;
    StaticArray<uint8, RESULT_BYTES> result{};
    rhi::ReadbackTicket readback;
    REQUIRE(rhi::RequestTextureReadback(owner.Device, destination, destinationRegion, CALLER_PITCH, readback) ==
            rhi::RasterStatus::Pending);
    REQUIRE(rhi::Destroy(owner.Device, destination) == rhi::RasterStatus::Ready);
    RequireComplete(owner.Device, readback);
    REQUIRE(rhi::CopyReadback(owner.Device, readback, result.GetData(), result.GetSize()) == rhi::RasterStatus::Ready);
    for (usize row = 0; row < HEIGHT / 2; ++row)
    {
        for (usize column = 0; column < CALLER_PITCH; ++column)
        {
            const uint8 expected = column < ROW_BYTES / 2 ? bytes[(row + 1) * ROW_BYTES + CHANNELS * 2 + column] : 0;
            CHECK(result[row * CALLER_PITCH + column] == expected);
        }
    }
    REQUIRE(rhi::Release(owner.Device, readback) == rhi::RasterStatus::Ready);
}

TEST_CASE("Portable transfers reuse completed slices across offscreen frames and restart", "[rhi][gpu][transfer]")
{
    rhi::StorageLimits limits;
    limits.TransferSlices = 1;
    TransferDevice owner(limits);
    owner.SetOffscreenTarget();
    constexpr usize ITERATIONS = 12;
    StaticArray<uint8, BUFFER_BYTES> bytes{};
    for (usize iteration = 0; iteration < ITERATIONS; ++iteration)
    {
        rhi::UploadTicket upload;
        bytes[0] = static_cast<uint8>(iteration);
        REQUIRE(rhi::RequestBufferUpload(owner.Device,
                                         {rhi::BufferRole::Vertex, BUFFER_BYTES},
                                         bytes.GetData(),
                                         bytes.GetSize(),
                                         upload) == rhi::RasterStatus::Pending);
        RequireComplete(owner.Device, upload);
        rhi::BufferHandle buffer;
        REQUIRE(rhi::TakeUploadedBuffer(owner.Device, upload, buffer) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::BeginFrame(owner.Device, owner.Surface) == rhi::DeviceStatus::Ready);
        rhi::SubmissionToken submission;
        REQUIRE(rhi::EndFrame(owner.Device, submission) == rhi::RasterStatus::Ready);
        rhi::ReadbackTicket readback;
        REQUIRE(rhi::RequestBufferReadback(owner.Device, buffer, 0, BUFFER_BYTES, readback) ==
                rhi::RasterStatus::Pending);
        RequireComplete(owner.Device, readback);
        RequireComplete(owner.Device, submission);
        StaticArray<uint8, BUFFER_BYTES> result{};
        REQUIRE(rhi::CopyReadback(owner.Device, readback, result.GetData(), result.GetSize()) ==
                rhi::RasterStatus::Ready);
        CHECK(result[0] == iteration);
        REQUIRE(rhi::Release(owner.Device, readback) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(owner.Device, buffer) == rhi::RasterStatus::Ready);
    }
    rhi::UploadTicket pending;
    REQUIRE(rhi::RequestBufferUpload(owner.Device,
                                     {rhi::BufferRole::Vertex, BUFFER_BYTES},
                                     bytes.GetData(),
                                     bytes.GetSize(),
                                     pending) == rhi::RasterStatus::Pending);
    REQUIRE(rhi::DestroyDevice(owner.Device) == rhi::DeviceStatus::Ready);
    owner.Surface = {};
    REQUIRE(rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, {}, owner.Device, owner.Surface) ==
            rhi::DeviceStatus::Ready);
    CHECK(rhi::GetStatus(owner.Device, pending) == rhi::RasterStatus::InvalidHandle);
    rhi::UploadTicket fresh;
    REQUIRE(rhi::RequestBufferUpload(owner.Device,
                                     {rhi::BufferRole::Vertex, BUFFER_BYTES},
                                     bytes.GetData(),
                                     bytes.GetSize(),
                                     fresh) == rhi::RasterStatus::Pending);
    RequireComplete(owner.Device, fresh);
    REQUIRE(rhi::Release(owner.Device, fresh) == rhi::RasterStatus::Ready);
}

TEST_CASE("Portable raster attachments read back through asynchronous transfers", "[rhi][gpu][transfer]")
{
    TransferDevice owner;
    owner.SetOffscreenTarget();
    rhi::TextureHandle texture;
    REQUIRE(rhi::CreateTexture(owner.Device, { .Width = WIDTH, .Height = HEIGHT, .Attachment = true }, {}, texture) ==
            rhi::RasterStatus::Ready);
    constexpr usize FRAMES = 6;
    for (usize frame = 0; frame < FRAMES; ++frame)
    {
        REQUIRE(rhi::BeginFrame(owner.Device, owner.Surface) == rhi::DeviceStatus::Ready);
        rhi::RasterPassDescription pass;
        pass.Color = texture;
        pass.Clear[0] = 1;
        REQUIRE(rhi::BeginRasterPass(owner.Device, pass) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken submission;
        REQUIRE(rhi::EndFrame(owner.Device, submission) == rhi::RasterStatus::Ready);
    }
    rhi::ReadbackTicket readback;
    REQUIRE(rhi::RequestTextureReadback(owner.Device, texture, {0, 0, 0, WIDTH, HEIGHT}, ROW_BYTES, readback) ==
            rhi::RasterStatus::Pending);
    RequireComplete(owner.Device, readback);
    StaticArray<uint8, TEXTURE_BYTES> result{};
    REQUIRE(rhi::CopyReadback(owner.Device, readback, result.GetData(), result.GetSize()) == rhi::RasterStatus::Ready);
    constexpr uint8 FULL_CHANNEL = 255;
    for (usize pixel = 0; pixel < WIDTH * HEIGHT; ++pixel)
    {
        CHECK(result[pixel * CHANNELS] == FULL_CHANNEL);
        CHECK(result[pixel * CHANNELS + 1] == 0);
        CHECK(result[pixel * CHANNELS + 2] == 0);
        CHECK(result[pixel * CHANNELS + CHANNELS - 1] == FULL_CHANNEL);
    }
    REQUIRE(rhi::Release(owner.Device, readback) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, texture) == rhi::RasterStatus::Ready);
}
