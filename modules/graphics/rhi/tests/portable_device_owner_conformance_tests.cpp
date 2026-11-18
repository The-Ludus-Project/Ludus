#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/adapter.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>

// The diagnostic fullscreen shader exists where its target compiled it for the backend.
#if __has_include("diagnostic.h")
#    include "diagnostic.h"
#    define LUDUS_TEST_OWNER_PIPELINES 1
#endif
#include "internal/readback.h"

#include <span>

#include <catch2/catch_test_macros.hpp>

// R14c: concurrent device owners on every native backend (Direct3D 12, Vulkan and Metal).
// Each owner keeps its own device, queue, resources, frames and native identity; one
// owner's handles never resolve in another, and retiring one owner leaves the rest working.
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace
{
constexpr uint32 FIRST_WIDTH = 17;
constexpr uint32 FIRST_HEIGHT = 9;
constexpr uint32 SECOND_WIDTH = 11;
constexpr uint32 SECOND_HEIGHT = 5;
constexpr usize PIXEL_BYTES = 4;
constexpr usize FIRST_PIXEL_BYTES = static_cast<usize>(FIRST_WIDTH) * FIRST_HEIGHT * PIXEL_BYTES;
constexpr usize SECOND_PIXEL_BYTES = static_cast<usize>(SECOND_WIDTH) * SECOND_HEIGHT * PIXEL_BYTES;
/// Extent of the small headless targets the adapter and capacity case starts.
constexpr uint32 SMALL_EXTENT = 4;
/// Bytes of the storage buffer each owner writes and reads back.
constexpr usize OWNER_BUFFER_BYTES = 16;
/// Second owner's first buffer byte; disjoint from the first owner's bytes.
constexpr uint8 SECOND_OWNER_SEED = 101;
/// PollLifetime rounds before a readback that never completes fails the test.
constexpr uint32 MAX_READBACK_POLLS = 100000;
constexpr uint8 FULL_CHANNEL = 255;

/// Destroy every device a test left live, so a failed REQUIRE does not leak an owner.
struct OwnerCleanup final
{
    StaticArray<rhi::DeviceHandle, rhi::MAX_DEVICE_OWNERS> Devices{};
    OwnerCleanup() noexcept
    {
        rhi::Shutdown();
    }
    OwnerCleanup(const OwnerCleanup&) = delete;
    OwnerCleanup& operator=(const OwnerCleanup&) = delete;
    ~OwnerCleanup()
    {
        for (rhi::DeviceHandle& device : Devices)
        {
            static_cast<void>(rhi::DestroyDevice(device));
        }
        rhi::Shutdown();
    }
};
/// Create a headless owner; false when a Metal host has no adapter (the case then skips).
bool CreateOwner(uint32 width,
                 uint32 height,
                 const rhi::DeviceDescription& description,
                 rhi::DeviceHandle& outDevice,
                 rhi::SurfaceHandle& outSurface)
{
    rhi::StartupInfo failure;
    const rhi::DeviceStatus status =
        rhi::CreateDevice({}, { .Width = width, .Height = height }, description, outDevice, outSurface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (status != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        return false;
    }
#endif
    REQUIRE(status == rhi::DeviceStatus::Ready);
    return true;
}
StaticArray<uint8, OWNER_BUFFER_BYTES> MakeOwnerBytes(uint8 seed) noexcept
{
    StaticArray<uint8, OWNER_BUFFER_BYTES> bytes{};
    for (usize i = 0; i < bytes.GetSize(); ++i)
    {
        bytes[i] = static_cast<uint8>(seed + i);
    }
    return bytes;
}
/// Read a ready buffer back through the owner's lifetime ring, polling without a host wait.
void ReadBuffer(rhi::DeviceHandle device, rhi::BufferHandle buffer, StaticArray<uint8, OWNER_BUFFER_BYTES>& outBytes)
{
    rhi::ReadbackTicket ticket;
    REQUIRE(rhi::RequestBufferReadback(device, buffer, 0, OWNER_BUFFER_BYTES, ticket) == rhi::RasterStatus::Pending);
    rhi::RasterStatus status = rhi::RasterStatus::Pending;
    for (uint32 poll = 0; poll < MAX_READBACK_POLLS && status == rhi::RasterStatus::Pending; ++poll)
    {
        static_cast<void>(rhi::PollLifetime(device));
        status = rhi::GetStatus(device, ticket);
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CopyReadback(device, ticket, outBytes.GetData(), outBytes.GetSize()) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Release(device, ticket) == rhi::RasterStatus::Ready);
}
/// Clear the owner's headless surface and read the frame back from that owner.
void ClearAndRead(rhi::DeviceHandle device,
                  rhi::SurfaceHandle surface,
                  const rhi::FrameTarget& target,
                  std::span<uint8> pixels)
{
    REQUIRE(rhi::SetFrameTarget(device, surface, target) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels));
}
bool IsFilled(std::span<const uint8> pixels, uint8 red, uint8 green, uint8 blue) noexcept
{
    for (usize i = 0; i < pixels.size(); i += PIXEL_BYTES)
    {
        if (pixels[i] != red || pixels[i + 1] != green || pixels[i + 2] != blue || pixels[i + 3] != FULL_CHANNEL)
        {
            return false;
        }
    }
    return true;
}
#if defined(LUDUS_TEST_OWNER_PIPELINES)
bool AreEqual(std::span<const uint8> left, std::span<const uint8> right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (usize i = 0; i < left.size(); ++i)
    {
        if (left[i] != right[i])
        {
            return false;
        }
    }
    return true;
}
/// The diagnostic shader's uniform: extent, mode and two colors.
constexpr usize DIAGNOSTIC_UNIFORM_FLOATS = 12;
constexpr float32 DIAGNOSTIC_GRADIENT_MODE = 2;
/// One owner's fullscreen pipeline over its own shaders and uniform.
struct OwnerPipeline final
{
    rhi::ShaderHandle Vertex;
    rhi::ShaderHandle Fragment;
    rhi::UniformHandle Uniform;
    rhi::PipelineHandle Pipeline;
};
OwnerPipeline CreateOwnerPipeline(rhi::DeviceHandle device, uint32 width, uint32 height)
{
    OwnerPipeline owned;
    REQUIRE(rhi::CreateShader(device, ludus::shaders::diagnostic::Vertex(), owned.Vertex) ==
            rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreateShader(device, ludus::shaders::diagnostic::Fragment(), owned.Fragment) ==
            rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreateUniform(device, DIAGNOSTIC_UNIFORM_FLOATS * sizeof(float32), owned.Uniform) ==
            rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreatePipeline(device, {owned.Vertex, owned.Fragment, owned.Uniform}, owned.Pipeline) ==
            rhi::ResourceStatus::Ready);
    // ludus-style: magic-number diagnostic gradient colors, mirrored from render_tests.cpp
    const StaticArray<float32, DIAGNOSTIC_UNIFORM_FLOATS> values{static_cast<float32>(width),
                                                                 static_cast<float32>(height),
                                                                 DIAGNOSTIC_GRADIENT_MODE,
                                                                 0,
                                                                 0.2F,
                                                                 0.4F,
                                                                 0.6F,
                                                                 0,
                                                                 0.6F,
                                                                 0.4F,
                                                                 0.2F,
                                                                 1};
    REQUIRE(rhi::UpdateUniform(device,
                               owned.Uniform,
                               {reinterpret_cast<const uint8*>(values.GetData()),
                                sizeof(float32) * values.GetSize()}) == rhi::ResourceStatus::Ready);
    return owned;
}
void DrawAndRead(rhi::DeviceHandle device,
                 rhi::SurfaceHandle surface,
                 rhi::PipelineHandle pipeline,
                 uint32 width,
                 uint32 height,
                 std::span<uint8> pixels)
{
    REQUIRE(rhi::SetFrameTarget(device, surface, { .Width = width, .Height = height }) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::DrawFullscreen(device, pipeline) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels));
}
#endif
} // namespace

TEST_CASE("R14c device owners keep isolated resources, frames and native identities", "[rhi][gpu][owner]")
{
    OwnerCleanup owners;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    rhi::SurfaceHandle firstSurface;
    if (CreateOwner(FIRST_WIDTH, FIRST_HEIGHT, description, owners.Devices[0], firstSurface) == false)
    {
        SKIP("No Metal adapter on this host");
    }
    rhi::SurfaceHandle secondSurface;
    REQUIRE(CreateOwner(SECOND_WIDTH, SECOND_HEIGHT, description, owners.Devices[1], secondSurface) == true);
    const rhi::DeviceHandle first = owners.Devices[0];
    const rhi::DeviceHandle second = owners.Devices[1];
    CHECK(first != second);

    // Each owner keeps its own buffers; neither resolves the other's handles.
    const StaticArray<uint8, OWNER_BUFFER_BYTES> firstBytes = MakeOwnerBytes(1);
    const StaticArray<uint8, OWNER_BUFFER_BYTES> secondBytes = MakeOwnerBytes(SECOND_OWNER_SEED);
    rhi::BufferHandle firstBuffer;
    REQUIRE(rhi::CreateBuffer(first,
                              { .Role = rhi::BufferRole::Storage, .Size = OWNER_BUFFER_BYTES },
                              {firstBytes.GetData(), firstBytes.GetSize()},
                              firstBuffer) == rhi::RasterStatus::Ready);
    rhi::BufferHandle secondBuffer;
    REQUIRE(rhi::CreateBuffer(second,
                              { .Role = rhi::BufferRole::Storage, .Size = OWNER_BUFFER_BYTES },
                              {secondBytes.GetData(), secondBytes.GetSize()},
                              secondBuffer) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetStatus(second, firstBuffer) == rhi::RasterStatus::InvalidHandle);
    rhi::ReadbackTicket foreign;
    CHECK(rhi::RequestBufferReadback(second, firstBuffer, 0, OWNER_BUFFER_BYTES, foreign) ==
          rhi::RasterStatus::InvalidHandle);
    CHECK(rhi::BeginFrame(first, secondSurface) == rhi::DeviceStatus::InvalidHandle);
    StaticArray<uint8, OWNER_BUFFER_BYTES> read{};
    ReadBuffer(second, secondBuffer, read);
    CHECK(read == secondBytes);
    ReadBuffer(first, firstBuffer, read);
    CHECK(read == firstBytes);

    // Both owners render and complete their own frames.
    StaticArray<uint8, FIRST_PIXEL_BYTES> firstPixels{};
    ClearAndRead(first,
                 firstSurface,
                 { .Width = FIRST_WIDTH, .Height = FIRST_HEIGHT, .Red = 1, .Alpha = 1 },
                 {firstPixels.GetData(), firstPixels.GetSize()});
    CHECK(IsFilled({firstPixels.GetData(), firstPixels.GetSize()}, FULL_CHANNEL, 0, 0));
    StaticArray<uint8, SECOND_PIXEL_BYTES> secondPixels{};
    ClearAndRead(second,
                 secondSurface,
                 { .Width = SECOND_WIDTH, .Height = SECOND_HEIGHT, .Green = 1, .Alpha = 1 },
                 {secondPixels.GetData(), secondPixels.GetSize()});
    CHECK(IsFilled({secondPixels.GetData(), secondPixels.GetSize()}, 0, FULL_CHANNEL, 0));
    rhi::SurfaceInfo info;
    REQUIRE(rhi::GetSurfaceInfo(first, firstSurface, info) == rhi::DeviceStatus::Ready);
    CHECK(info.SubmittedFrames == 1);

#if defined(LUDUS_TEST_OWNER_PIPELINES)
    // Each owner draws through its own pipeline; the images match and the handles stay owned.
    const OwnerPipeline firstPipeline = CreateOwnerPipeline(first, FIRST_WIDTH, FIRST_HEIGHT);
    const OwnerPipeline secondPipeline = CreateOwnerPipeline(second, FIRST_WIDTH, FIRST_HEIGHT);
    CHECK(rhi::DrawFullscreen(second, firstPipeline.Pipeline) == rhi::ResourceStatus::InvalidHandle);
    CHECK(rhi::GetStatus(first, secondPipeline.Pipeline) == rhi::ResourceStatus::InvalidHandle);
    StaticArray<uint8, FIRST_PIXEL_BYTES> firstImage{};
    DrawAndRead(first,
                firstSurface,
                firstPipeline.Pipeline,
                FIRST_WIDTH,
                FIRST_HEIGHT,
                {firstImage.GetData(), firstImage.GetSize()});
    StaticArray<uint8, FIRST_PIXEL_BYTES> secondImage{};
    DrawAndRead(second,
                secondSurface,
                secondPipeline.Pipeline,
                FIRST_WIDTH,
                FIRST_HEIGHT,
                {secondImage.GetData(), secondImage.GetSize()});
    CHECK(AreEqual({firstImage.GetData(), firstImage.GetSize()}, {secondImage.GetData(), secondImage.GetSize()}));
#endif

    // Native leases name their owner without a device handle and resolve after another
    // owner was addressed; each owner exposes its own native device and queue.
    rhi::NativeDeviceLease firstLease;
    REQUIRE(rhi::AcquireNativeDevice(first, firstLease) == rhi::RasterStatus::Ready);
    rhi::NativeDeviceLease secondLease;
    REQUIRE(rhi::AcquireNativeDevice(second, secondLease) == rhi::RasterStatus::Ready);
    rhi::NativeDeviceInfo firstNative;
    REQUIRE(rhi::GetNativeDeviceInfo(firstLease, firstNative) == rhi::RasterStatus::Ready);
    rhi::NativeDeviceInfo secondNative;
    REQUIRE(rhi::GetNativeDeviceInfo(secondLease, secondNative) == rhi::RasterStatus::Ready);
    CHECK(firstNative.Identity.Api == secondNative.Identity.Api);
    CHECK(firstNative.Identity.CommandQueue != secondNative.Identity.CommandQueue);
    if (firstNative.Identity.Api != rhi::Backend::Metal)
    {
        // Metal owners may share one MTLDevice; Vulkan and Direct3D 12 create a device per owner.
        CHECK(firstNative.Identity.Device != secondNative.Identity.Device);
    }
    REQUIRE(rhi::ReleaseNativeDevice(firstLease) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetNativeDeviceInfo(firstLease, firstNative) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(rhi::GetNativeDeviceInfo(secondLease, secondNative) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ReleaseNativeDevice(secondLease) == rhi::RasterStatus::Ready);

    // Retiring the first owner leaves the second owner's resources and frames working.
    const rhi::BufferHandle retiredBuffer = firstBuffer;
    REQUIRE(rhi::DestroyDevice(owners.Devices[0]) == rhi::DeviceStatus::Ready);
    CHECK(rhi::GetStatus(first, retiredBuffer) == rhi::RasterStatus::InvalidHandle);
    CHECK(rhi::GetStatus(second, secondBuffer) == rhi::RasterStatus::Ready);
    ReadBuffer(second, secondBuffer, read);
    CHECK(read == secondBytes);
    ClearAndRead(second,
                 secondSurface,
                 { .Width = SECOND_WIDTH, .Height = SECOND_HEIGHT, .Blue = 1, .Alpha = 1 },
                 {secondPixels.GetData(), secondPixels.GetSize()});
    CHECK(IsFilled({secondPixels.GetData(), secondPixels.GetSize()}, 0, 0, FULL_CHANNEL));

    // A replacement owner starts beside the survivor and rejects the retired owner's handles.
    rhi::SurfaceHandle replacementSurface;
    REQUIRE(CreateOwner(FIRST_WIDTH, FIRST_HEIGHT, description, owners.Devices[0], replacementSurface) == true);
    CHECK(owners.Devices[0] != first);
    CHECK(rhi::GetStatus(owners.Devices[0], retiredBuffer) == rhi::RasterStatus::InvalidHandle);
    CHECK(rhi::GetStatus(owners.Devices[0], secondBuffer) == rhi::RasterStatus::InvalidHandle);
    ClearAndRead(owners.Devices[0],
                 replacementSurface,
                 { .Width = FIRST_WIDTH, .Height = FIRST_HEIGHT, .Green = 1, .Alpha = 1 },
                 {firstPixels.GetData(), firstPixels.GetSize()});
    CHECK(IsFilled({firstPixels.GetData(), firstPixels.GetSize()}, 0, FULL_CHANNEL, 0));
    REQUIRE(rhi::Destroy(second, secondBuffer) == rhi::RasterStatus::Ready);
}

TEST_CASE("R14c device owners select catalog adapters independently up to capacity", "[rhi][gpu][owner][adapter]")
{
    OwnerCleanup owners;
    rhi::AdapterCatalog catalog;
    REQUIRE(rhi::GetAdapterCatalog(catalog) == rhi::AdapterStatus::Ready);
    usize selected = catalog.Count;
    for (usize i = 0; i < catalog.Count && selected == catalog.Count; ++i)
    {
        if (catalog.Adapters[i].HasBackendBaseline == true)
        {
            selected = i;
        }
    }
    if (selected == catalog.Count)
    {
        SKIP("No adapter meets the backend baseline");
    }
    rhi::DeviceDescription chosen;
    chosen.Adapter = catalog.Adapters[selected].Identity;
    rhi::SurfaceHandle chosenSurface;
    REQUIRE(CreateOwner(SMALL_EXTENT, SMALL_EXTENT, chosen, owners.Devices[0], chosenSurface) == true);
    rhi::SurfaceHandle automaticSurface;
    REQUIRE(CreateOwner(SMALL_EXTENT, SMALL_EXTENT, {}, owners.Devices[1], automaticSurface) == true);
    rhi::DeviceInfo info;
    REQUIRE(rhi::GetDeviceInfo(owners.Devices[0], info) == rhi::DeviceStatus::Ready);
    CHECK(info.Adapter.Identity == catalog.Adapters[selected].Identity);
    CHECK(info.Description.Adapter == catalog.Adapters[selected].Identity);
    REQUIRE(rhi::GetDeviceInfo(owners.Devices[1], info) == rhi::DeviceStatus::Ready);
    CHECK(info.Description.Adapter.IsAutomatic());
    CHECK(info.Adapter.Identity.Api == catalog.Api);

    // Listing adapters while owners live leaves their devices working.
    rhi::AdapterCatalog again;
    REQUIRE(rhi::GetAdapterCatalog(again) == rhi::AdapterStatus::Ready);
    CHECK(again.Count == catalog.Count);
    StaticArray<uint8, static_cast<usize>(SMALL_EXTENT) * SMALL_EXTENT * PIXEL_BYTES> pixels{};
    ClearAndRead(owners.Devices[0],
                 chosenSurface,
                 { .Width = SMALL_EXTENT, .Height = SMALL_EXTENT, .Red = 1, .Alpha = 1 },
                 {pixels.GetData(), pixels.GetSize()});
    CHECK(IsFilled({pixels.GetData(), pixels.GetSize()}, FULL_CHANNEL, 0, 0));

    // Capacity is explicit once every owner slot is live, and the last owner still renders.
    rhi::SurfaceHandle lastSurface;
    for (usize slot = 2; slot < rhi::MAX_DEVICE_OWNERS; ++slot)
    {
        lastSurface = {};
        REQUIRE(CreateOwner(SMALL_EXTENT, SMALL_EXTENT, {}, owners.Devices[slot], lastSurface) == true);
    }
    rhi::DeviceHandle extra;
    rhi::SurfaceHandle extraSurface;
    CHECK(rhi::CreateDevice({}, { .Width = SMALL_EXTENT, .Height = SMALL_EXTENT }, {}, extra, extraSurface) ==
          rhi::DeviceStatus::CapacityExceeded);
    CHECK(extra == rhi::DeviceHandle{});
    CHECK(extraSurface == rhi::SurfaceHandle{});
    ClearAndRead(owners.Devices[rhi::MAX_DEVICE_OWNERS - 1],
                 lastSurface,
                 { .Width = SMALL_EXTENT, .Height = SMALL_EXTENT, .Blue = 1, .Alpha = 1 },
                 {pixels.GetData(), pixels.GetSize()});
    CHECK(IsFilled({pixels.GetData(), pixels.GetSize()}, 0, 0, FULL_CHANNEL));
}
