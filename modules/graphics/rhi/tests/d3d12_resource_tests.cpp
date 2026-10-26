#include "internal/d3d12_device.h"
#include "internal/raster.h"
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>
#if defined(LUDUS_TEST_DXIL)
#    include "raster.h"
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace
{
struct ResourceDevice final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    explicit ResourceDevice(rhi::StorageLimits limits = {})
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Storage = limits;
        REQUIRE(rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, Device, Surface) ==
                rhi::DeviceStatus::Ready);
    }
    ~ResourceDevice()
    {
        static_cast<void>(rhi::DestroyDevice(Device));
    }
};
void RequireReady(rhi::DeviceHandle device, rhi::TextureHandle texture)
{
    REQUIRE(rhi::backend::d3d12::WaitForQueue() == true);
    REQUIRE(rhi::GetStatus(device, texture) == rhi::RasterStatus::Ready);
}
} // namespace

TEST_CASE("D3a exposes resources while required raster and compute remain unavailable", "[rhi][windows][d3a]")
{
    ResourceDevice owner;
    rhi::DeviceInfo information;
    REQUIRE(rhi::GetDeviceInfo(owner.Device, information) == rhi::DeviceStatus::Ready);
    CHECK(information.Enabled.PortableRaster == false);
    CHECK(information.Enabled.Compute == false);
    CHECK(information.Enabled.OrderedRasterGraph == false);
    CHECK(information.Profile.Raster.MaxBufferSize > 0);
    CHECK(information.Profile.Raster.DrawsPerFrame == 0);
    rhi::FormatCapabilities format;
    REQUIRE(rhi::GetFormatCapabilities(owner.Device, rhi::RasterFormat::Rgba8Unorm, format) ==
            rhi::RasterStatus::Ready);
    CHECK(format.Uses.Sampled == true);
    CHECK(format.Uses.ColorAttachment == false);
    CHECK(format.Uses.CopySource == false);
    CHECK(format.Uses.CopyDestination == false);
    REQUIRE(rhi::DestroyDevice(owner.Device) == rhi::DeviceStatus::Ready);
    owner.Surface = {};
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    CHECK(rhi::CreateDevice({}, {}, description, owner.Device, owner.Surface) == rhi::DeviceStatus::Failed);
    CHECK(rhi::GetStartup().Error == rhi::StartupError::RequirementsUnsatisfied);
}

TEST_CASE("D3a binding snapshots retain native buffers and descriptors within configured limits", "[rhi][windows][d3a]")
{
    rhi::StorageLimits limits;
    limits.BindingDescriptors = 1;
    limits.DescriptorsPerSet = 1;
    ResourceDevice owner(limits);
    rhi::DeviceInfo information;
    REQUIRE(rhi::GetDeviceInfo(owner.Device, information) == rhi::DeviceStatus::Ready);
    CHECK(information.Profile.Binding.MaxDescriptorsPerSet == 1);
    constexpr usize UNIFORM_BYTES = 16;
    StaticArray<uint8, UNIFORM_BYTES> bytes{};
    rhi::BufferHandle buffer;
    REQUIRE(rhi::CreateBuffer(owner.Device,
                              { .Role = rhi::BufferRole::Uniform, .Size = bytes.GetSize() },
                              bytes.AsSpan(),
                              buffer) == rhi::RasterStatus::Ready);
    const rhi::RasterBinding binding
    {
        .Binding = 0,
        .Kind = rhi::RasterBindingKind::UniformBuffer,
        .MinSize = UNIFORM_BYTES,
    };
    rhi::BindingLayoutHandle layout;
    REQUIRE(rhi::CreateBindingLayout(owner.Device, {&binding, 1}, layout) == rhi::RasterStatus::Ready);
    const rhi::RasterBindingResource resource{ .Binding = 0, .Buffer = buffer, .Size = UNIFORM_BYTES };
    rhi::BindingSetHandle set;
    REQUIRE(rhi::CreateBindingSet(owner.Device, layout, {&resource, 1}, set) == rhi::RasterStatus::Ready);
    rhi::BindingSetHandle rejected;
    CHECK(rhi::CreateBindingSet(owner.Device, layout, {&resource, 1}, rejected) == rhi::RasterStatus::CapacityExceeded);
    REQUIRE(rhi::Destroy(owner.Device, buffer) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, layout) == rhi::RasterStatus::Ready);
    rhi::internal::DescriptorArenaUsage usage;
    REQUIRE(rhi::backend::GetDescriptorArenaUsage(usage) == true);
    CHECK(usage.ReservedDescriptors == 1);
    CHECK(usage.Pools == 1);
    REQUIRE(rhi::Destroy(owner.Device, set) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::backend::GetDescriptorArenaUsage(usage) == true);
    CHECK(usage.ReservedDescriptors == 0);
    CHECK(usage.Pools == 0);
}

TEST_CASE("D3a resource exhaustion preserves outputs and permits reuse", "[rhi][windows][d3a]")
{
    rhi::StorageLimits limits;
    limits.ResourcesPerKind = 1;
    ResourceDevice owner(limits);
    rhi::SamplerHandle sampler;
    REQUIRE(rhi::CreateSampler(owner.Device, {}, sampler) == rhi::RasterStatus::Ready);
    rhi::SamplerHandle rejected;
    CHECK(rhi::CreateSampler(owner.Device, {}, rejected) == rhi::RasterStatus::CapacityExceeded);
    CHECK(rhi::GetStatus(owner.Device, rejected) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(rhi::Destroy(owner.Device, sampler) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateSampler(owner.Device, {}, rejected) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, rejected) == rhi::RasterStatus::Ready);
    rhi::RasterBinding binding
    {
        .Kind = rhi::RasterBindingKind::UniformBuffer,
        .MinSize = 16,
        .HasDynamicOffset = true,
    };
    rhi::BindingLayoutHandle layout;
    CHECK(rhi::CreateBindingLayout(owner.Device, {&binding, 1}, layout) == rhi::RasterStatus::Unsupported);
    binding.HasDynamicOffset = false;
    binding.Count = 2;
    CHECK(rhi::CreateBindingLayout(owner.Device, {&binding, 1}, layout) == rhi::RasterStatus::Unsupported);
}

TEST_CASE("D3a uploads complete padded mip chains and retains sampled dependencies", "[rhi][windows][d3a]")
{
    ResourceDevice owner;
    const rhi::TextureDimension dimension = GENERATE(rhi::TextureDimension::D2,
                                                     rhi::TextureDimension::D2Array,
                                                     rhi::TextureDimension::Cube,
                                                     rhi::TextureDimension::D3);
    const rhi::RasterFormat format = GENERATE(rhi::RasterFormat::Rgba8Unorm,
                                              rhi::RasterFormat::Rgba8Srgb,
                                              rhi::RasterFormat::R8Unorm,
                                              rhi::RasterFormat::Rg8Unorm,
                                              rhi::RasterFormat::Rgba16Float,
                                              rhi::RasterFormat::Bc1RgbaUnorm,
                                              rhi::RasterFormat::Bc3RgbaUnorm,
                                              rhi::RasterFormat::Bc5RgUnorm);
    rhi::FormatCapabilities capabilities;
    if (rhi::GetFormatCapabilities(owner.Device, format, capabilities) == rhi::RasterStatus::Unsupported)
    {
        return;
    }
    if (dimension == rhi::TextureDimension::D3 && capabilities.MaxDimension3D == 0)
    {
        return;
    }
    constexpr usize MIP_COUNT = 2;
    rhi::TextureDescription description;
    description.Width = 8;
    description.Height = 8;
    description.Format = format;
    description.Dimension = dimension;
    description.DepthOrLayers = dimension == rhi::TextureDimension::Cube ? 6
                                : dimension == rhi::TextureDimension::D2 ? 1
                                                                         : 2;
    description.MipLevels = MIP_COUNT;
    StaticArray<rhi::TextureMipUpload, MIP_COUNT> mips{};
    usize total = 0;
    for (uint32 mip = 0; mip < MIP_COUNT; ++mip)
    {
        const usize rows = rhi::internal::TextureBlockRows(format, description.Height >> mip);
        const usize rowBytes = rhi::internal::TextureBlockRows(format, description.Width >> mip) *
                               rhi::internal::TextureBlockBytes(format);
        mips[mip] = { .Offset = total, .RowPitch = rowBytes + 16 };
        total += mips[mip].RowPitch * rows * rhi::internal::TextureSlices(description, mip);
    }
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(total) == true);
    for (usize index = 0; index < total; ++index)
    {
        bytes[index] = static_cast<uint8>(index);
    }
    rhi::TextureHandle texture;
    REQUIRE(rhi::CreateTexture(owner.Device,
                               description,
                               {
                                   .Bytes = bytes.AsSpan(),
                                   .Mips = mips.AsSpan(),
                               },
                               texture) == rhi::RasterStatus::Pending);
    RequireReady(owner.Device, texture);
    rhi::TextureViewHandle view;
    REQUIRE(rhi::CreateTextureView(owner.Device, texture, view) == rhi::RasterStatus::Ready);
    rhi::SamplerHandle sampler;
    REQUIRE(rhi::CreateSampler(owner.Device,
                               { .Filter = rhi::RasterFilter::Linear, .MipFilter = rhi::RasterMipFilter::Linear },
                               sampler) == rhi::RasterStatus::Ready);
    constexpr usize BINDING_COUNT = 2;
    const StaticArray<rhi::RasterBinding, BINDING_COUNT> bindings{{
        { .Binding = 1, .Kind = rhi::internal::TextureBindingType(dimension) },
        { .Binding = 2, .Kind = rhi::RasterBindingKind::Sampler },
    }};
    const StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources{{
        { .Binding = 1, .Texture = view },
        { .Binding = 2, .Sampler = sampler },
    }};
    rhi::BindingLayoutHandle layout;
    REQUIRE(rhi::CreateBindingLayout(owner.Device, bindings.AsSpan(), layout) == rhi::RasterStatus::Ready);
    rhi::BindingSetHandle set;
    REQUIRE(rhi::CreateBindingSet(owner.Device, layout, resources.AsSpan(), set) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, texture) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, view) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, sampler) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, layout) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::GetStatus(owner.Device, set) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, set) == rhi::RasterStatus::Ready);
}

TEST_CASE("D3a shutdown safely drains destroyed pending creation uploads", "[rhi][windows][d3a]")
{
    constexpr usize TEXEL_BYTES = 4;
    StaticArray<uint8, TEXEL_BYTES> bytes{};
    for (usize iteration = 0; iteration < 8; ++iteration)
    {
        ResourceDevice owner;
        rhi::TextureHandle texture;
        REQUIRE(rhi::CreateTexture(owner.Device,
                                   { .Width = 1, .Height = 1 },
                                   { .Bytes = bytes.AsSpan(), .RowPitch = TEXEL_BYTES },
                                   texture) == rhi::RasterStatus::Pending);
        REQUIRE(rhi::Destroy(owner.Device, texture) == rhi::RasterStatus::Ready);
    }
}
#if defined(LUDUS_TEST_DXIL)
TEST_CASE("D3a creates raster DXIL stages and rejects incompatible reflected bindings", "[rhi][windows][d3a][dxil]")
{
    ResourceDevice owner;
    rhi::RasterShaderHandle vertex;
    rhi::RasterShaderHandle fragment;
    REQUIRE(rhi::CreateRasterShader(owner.Device, ludus::shaders::raster::Vertex(), vertex) ==
            rhi::RasterStatus::Ready);
    rhi::RasterShaderDescription description = ludus::shaders::raster::Fragment();
    REQUIRE(rhi::CreateRasterShader(owner.Device, description, fragment) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, vertex) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, fragment) == rhi::RasterStatus::Ready);
    description.Bindings = {};
    description.UniformMembers = {};
    CHECK(rhi::CreateRasterShader(owner.Device, description, fragment) == rhi::RasterStatus::InvalidDescription);
    CHECK(rhi::GetStatus(owner.Device, fragment) == rhi::RasterStatus::InvalidHandle);
}
#endif

TEST_CASE("D3a failed staging admission releases resource budget and preserves output", "[rhi][windows][d3a]")
{
    constexpr usize LOGICAL_BYTES = 8;
    rhi::StorageLimits limits;
    limits.ResourceBytes = LOGICAL_BYTES;
    ResourceDevice owner(limits);
    StaticArray<uint8, LOGICAL_BYTES> bytes{};
    rhi::TextureHandle texture;
    CHECK(rhi::CreateTexture(owner.Device,
                             { .Width = 1, .Height = 2 },
                             { .Bytes = bytes.AsSpan(), .RowPitch = 4 },
                             texture) == rhi::RasterStatus::CapacityExceeded);
    CHECK(rhi::GetStatus(owner.Device, texture) == rhi::RasterStatus::InvalidHandle);
    rhi::BufferHandle buffer;
    REQUIRE(rhi::CreateBuffer(owner.Device,
                              { .Role = rhi::BufferRole::Vertex, .Size = LOGICAL_BYTES },
                              bytes.AsSpan(),
                              buffer) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, buffer) == rhi::RasterStatus::Ready);
}
