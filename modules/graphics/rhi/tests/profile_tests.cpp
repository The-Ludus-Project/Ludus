#include "internal/lifecycle.h"
#include "internal/profile.h"
#include "reference_raster.h"

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/raster.h>

#include <catch2/catch_test_macros.hpp>

namespace ludus::graphics::rhi::backend
{
extern foundation::uint32 PendingToken;
extern StartupError ImmediateError;
extern Backend ActiveKind;
extern bool SelectionSupported;
} // namespace ludus::graphics::rhi::backend
using namespace ludus::graphics::rhi;
namespace
{
struct ProfileFixture final
{
    DeviceHandle Device;
    SurfaceHandle Surface;
    ProfileFixture()
    {
        Shutdown();
        reference::Reset();
        backend::ImmediateError = StartupError::None;
        backend::ActiveKind = Backend::WebGPU;
        backend::SelectionSupported = true;
    }
    ~ProfileFixture()
    {
        Shutdown();
        reference::Reset();
    }
    DeviceStatus Start(const DeviceDescription& request)
    {
        return CreateDevice({}, {}, request, Device, Surface);
    }
    void Complete()
    {
        internal::Complete(backend::PendingToken,
                           StartupError::None,
                           { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
    }
};
} // namespace
TEST_CASE("Profile requests are copied and enabled snapshots are invalidated by loss", "[rhi][device][profile]")
{
    ProfileFixture fixture;
    DeviceDescription request;
    request.RequiredProfile.Raster.MinBufferSize = ludus::foundation::usize{64} * 1024U * 1024U;
    request.RequiredProfile.Raster.MaxUniformOffsetAlignment = 256;
    request.RequiredProfile.Compute.MinWorkgroupSize[2] = 64;
    request.RequiredProfile.Compute.MinWorkgroupInvocations = 256;
    request.RequiredProfile.Compute.MinDispatch[1] = 65535;
    request.RequiredProfile.FormatCount = 1;
    request.RequiredProfile.Formats[0] =
    {
        .Format = RasterFormat::Rg8Unorm,
        .Uses = { .Sampled = true, .Filterable = true },
        .MinDimension2D = 4096,
        .MinMipLevels = 13,
    };
    REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
    request.RequiredProfile.Raster.MinBufferSize = ~ludus::foundation::usize{0};
    request.RequiredProfile.Formats[0].Uses.Storage = true;
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Pending);
    CHECK(info.Description.RequiredProfile.Raster.MinBufferSize == ludus::foundation::usize{64} * 1024U * 1024U);
    CHECK_FALSE(info.Description.RequiredProfile.Formats[0].Uses.Storage);
    CHECK(info.Profile.Raster.MaxBufferSize == 0);
    CHECK(info.Profile.Storage.ResourcesPerKind == 0);
    CHECK(info.Profile.Storage.DrawsPerFrame == 0);
    CHECK(info.Profile.Storage.Graphs == 0);
    CHECK(info.Profile.Storage.RasterPassStates == 0);
    CHECK(info.Profile.Storage.Submissions == 0);
    CHECK(info.Profile.Storage.HostBytes == 0);
    CHECK(info.Profile.Storage.ResourceBytes == 0);
    CHECK(info.Profile.Storage.RayResourcesPerKind == 0);
    CHECK(info.Profile.Storage.RayInstances == 0);
    CHECK(info.Profile.Storage.TransferSlices == 0);
    CHECK(info.Profile.Storage.TransferBytes == 0);
    CHECK(internal::CurrentDeviceProfile().Storage.ResourcesPerKind == 0);
    FormatCapabilities format;
    format.MaxMipLevels = 777;
    CHECK(GetFormatCapabilities(fixture.Device, RasterFormat::Rg8Unorm, format) == RasterStatus::NotReady);
    CHECK(format.MaxMipLevels == 0);
    fixture.Complete();
    REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Ready);
    CHECK(info.Enabled.Compute);
    CHECK(info.Profile.Storage.ResourcesPerKind == 64);
    CHECK(info.Profile.Storage.HostBytes == StorageLimits{}.HostBytes);
    CHECK(info.Profile.Interface.MaxBindings == RASTER_BINDING_COUNT);
    REQUIRE(GetFormatCapabilities(fixture.Device, RasterFormat::Rg8Unorm, format) == RasterStatus::Ready);
    CHECK(format.Uses.Filterable);
    CHECK_FALSE(format.Uses.ColorAttachment);
    CHECK_FALSE(format.Uses.Storage);
    CHECK_FALSE(format.Uses.CopySource);
    CHECK_FALSE(format.Uses.CopyDestination);
    CHECK(format.MaxMipLevels == 13);
    CHECK(GetFormatCapabilities({}, RasterFormat::Rgba8Unorm, format) == RasterStatus::InvalidHandle);
    CHECK(format.MaxMipLevels == 13);
    CHECK(GetFormatCapabilities(fixture.Device, static_cast<RasterFormat>(255), format) ==
          RasterStatus::InvalidDescription);
    CHECK(format.MaxMipLevels == 13);
    internal::Fail(backend::PendingToken, StartupError::DeviceLost);
    REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::DeviceLost);
    CHECK(info.Profile.Raster.MaxBufferSize == 0);
    CHECK(info.Profile.Storage.ResourcesPerKind == 0);
    CHECK(info.Profile.Storage.DrawsPerFrame == 0);
    CHECK(info.Profile.Storage.Graphs == 0);
    CHECK(info.Profile.Storage.RasterPassStates == 0);
    CHECK(info.Profile.Storage.Submissions == 0);
    CHECK(info.Profile.Storage.HostBytes == 0);
    CHECK(info.Profile.Storage.ResourceBytes == 0);
    CHECK(info.Profile.Storage.RayResourcesPerKind == 0);
    CHECK(info.Profile.Storage.RayInstances == 0);
    CHECK(info.Profile.Storage.TransferSlices == 0);
    CHECK(info.Profile.Storage.TransferBytes == 0);
    CHECK(internal::CurrentDeviceProfile().Storage.ResourcesPerKind == 0);
    CHECK(info.Profile.Formats[0].SampleCounts == 0);
    CHECK(GetFormatCapabilities(fixture.Device, RasterFormat::Rg8Unorm, format) == RasterStatus::DeviceLost);
    CHECK(format.MaxMipLevels == 0);
}

TEST_CASE("Required profile limits and formats fail before resource creation with exact diagnostics", "[rhi][profile]")
{
    ProfileFixture fixture;
    DeviceDescription request;
    ProfileRequirement expected = ProfileRequirement::None;
    SECTION("engine capacity")
    {
        request.RequiredProfile.Raster.MinResourcesPerKind =
            static_cast<ludus::foundation::uint32>(internal::RASTER_CAPACITY) + 1;
        expected = ProfileRequirement::ResourceCount;
    }
    SECTION("binding capacity")
    {
        request.RequiredProfile.Raster.MinBindings = static_cast<ludus::foundation::uint32>(RASTER_BINDING_COUNT + 1);
        expected = ProfileRequirement::BindingCount;
    }
    SECTION("attachment capacity")
    {
        request.RequiredProfile.Raster.MinColorAttachments = 5;
        expected = ProfileRequirement::ColorAttachmentCount;
    }
    SECTION("uniform alignment is an upper bound")
    {
        request.RequiredProfile.Raster.MaxUniformOffsetAlignment = 128;
        expected = ProfileRequirement::UniformAlignment;
    }
    SECTION("storage alignment is an upper bound")
    {
        request.RequiredProfile.Compute.MaxStorageOffsetAlignment = 128;
        expected = ProfileRequirement::StorageAlignment;
    }
    SECTION("workgroup invocation capacity")
    {
        request.RequiredProfile.Compute.MinWorkgroupInvocations = 257;
        expected = ProfileRequirement::WorkgroupInvocations;
    }
    SECTION("dispatch axis")
    {
        request.RequiredProfile.Compute.MinDispatch[2] = 65536;
        expected = ProfileRequirement::DispatchCount;
    }
    SECTION("sampled-only format cannot be an attachment")
    {
        request.RequiredProfile.FormatCount = 1;
        request.RequiredProfile.Formats[0] = { .Format = RasterFormat::R8Unorm, .Uses = { .ColorAttachment = true } };
        expected = ProfileRequirement::ColorAttachment;
    }
    SECTION("driver capabilities do not enable storage textures")
    {
        request.RequiredProfile.FormatCount = 1;
        request.RequiredProfile.Formats[0].Uses.Storage = true;
        expected = ProfileRequirement::Storage;
    }
    SECTION("R8 copy profile excludes other formats")
    {
        request.RequiredProfile.FormatCount = 1;
        request.RequiredProfile.Formats[0].Format = RasterFormat::Rg8Unorm;
        request.RequiredProfile.Formats[0].Uses.CopyDestination = true;
        expected = ProfileRequirement::CopyDestination;
    }
    SECTION("unsupported sample count")
    {
        request.RequiredProfile.FormatCount = 1;
        request.RequiredProfile.Formats[0] = { .Uses = { .ColorAttachment = true }, .SampleCount = 8 };
        expected = ProfileRequirement::SampleCount;
    }
    SECTION("multisampled attachments cannot have a mip chain")
    {
        request.RequiredProfile.FormatCount = 1;
        request.RequiredProfile.Formats[0] =
        {
            .Uses =
            {
                .Sampled = true,
                .ColorAttachment = true,
            },
            .MinMipLevels = 2,
            .SampleCount = 4,
        };
        expected = ProfileRequirement::MipCount;
    }
    REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
    fixture.Complete();
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Failed);
    CHECK(info.Startup.Error == StartupError::RequirementsUnsatisfied);
    CHECK(info.Startup.UnmetRequirement == RequirementFailure::Profile);
    CHECK(info.Startup.ProfileFailure.Requirement == expected);
    if (expected == ProfileRequirement::DispatchCount)
    {
        CHECK(info.Startup.ProfileFailure.Axis == 2);
        CHECK(info.Startup.ProfileFailure.Requested == 65536);
        CHECK(info.Startup.ProfileFailure.Available == 65535);
    }
    CHECK_FALSE(info.Enabled.PortableRaster);
    for (const auto created : reference::Creates)
    {
        CHECK(created == 0);
    }
    BufferHandle buffer;
    const ludus::foundation::uint8 bytes[4]{};
    CHECK(CreateBuffer(fixture.Device, { .Size = 4 }, bytes, buffer) == RasterStatus::Failed);
    REQUIRE(DestroyDevice(fixture.Device) == DeviceStatus::Ready);
}

TEST_CASE("Malformed profile requests never launch a backend and preserve outputs", "[rhi][profile]")
{
    ProfileFixture fixture;
    DeviceDescription request;
    request.RequiredProfile.FormatCount = 1;
    request.RequiredProfile.Formats[0].Uses.Sampled = true;
    SECTION("too many formats")
    {
        request.RequiredProfile.FormatCount = static_cast<ludus::foundation::uint32>(RASTER_FORMAT_COUNT + 1);
    }
    SECTION("invalid format")
    {
        request.RequiredProfile.Formats[0].Format = static_cast<RasterFormat>(255);
    }
    SECTION("unaligned compressed requirement on uncompressed format")
    {
        request.RequiredProfile.Formats[0].RequiresUnalignedBlockExtents = true;
    }
    SECTION("no use")
    {
        request.RequiredProfile.Formats[0].Uses = {};
    }
    SECTION("duplicate format")
    {
        request.RequiredProfile.FormatCount = 2;
        request.RequiredProfile.Formats[1] = request.RequiredProfile.Formats[0];
    }
    SECTION("non-power-of-two samples")
    {
        request.RequiredProfile.Formats[0].SampleCount = 3;
    }
    SECTION("samples without attachment")
    {
        request.RequiredProfile.Formats[0].SampleCount = 4;
    }
    SECTION("malformed preference")
    {
        request.PreferredProfile.FormatCount = static_cast<ludus::foundation::uint32>(RASTER_FORMAT_COUNT + 1);
    }
    const auto token = backend::PendingToken;
    StartupInfo failure;
    failure.MaxTextureDimension2D = 777;
    CHECK(ValidateDeviceDescription(request) == DeviceStatus::InvalidDescription);
    CHECK(CreateDevice({}, {}, request, fixture.Device, fixture.Surface, &failure) == DeviceStatus::InvalidDescription);
    CHECK(backend::PendingToken == token);
    CHECK(failure.MaxTextureDimension2D == 777);
    CHECK(GetStartup().State == StartupState::Idle);
}

TEST_CASE("Preferred constraints report misses without failing or changing enabled limits", "[rhi][profile]")
{
    ProfileFixture fixture;
    backend::ActiveKind = Backend::WebGL2;
    DeviceDescription request;
    request.Selection = BackendSelection::WebGL2;
    request.Preferred.Compute = true;
    request.Preferred.IndirectRendering = true;
    request.PreferredProfile.Compute.MinStorageRange = 4;
    REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
    fixture.Complete();
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Ready);
    CHECK(info.UnmetPreferences.Compute);
    CHECK(info.UnmetPreferences.IndirectRendering);
    CHECK_FALSE(info.Enabled.Compute);
    CHECK(info.UnmetProfilePreference.Requirement == ProfileRequirement::StorageRange);
    CHECK(info.UnmetProfilePreference.Available == 0);
    CHECK(info.Profile.Raster.MaxBufferSize == ludus::foundation::usize{512} * 1024U * 1024U);
    REQUIRE(DestroyDevice(fixture.Device) == DeviceStatus::Ready);
    fixture.Surface = {};
    request.RequiredProfile.Compute.MinStorageRange = 4;
    const auto token = backend::PendingToken;
    CHECK(fixture.Start(request) == DeviceStatus::Unsupported);
    CHECK(backend::PendingToken == token);
}

TEST_CASE("Profile admission rechecks fallback and isolates old callbacks", "[rhi][profile]")
{
    ProfileFixture fixture;
    DeviceDescription request;
    request.RequiredProfile.FormatCount = 1;
    request.RequiredProfile.Formats[0] = { .Uses = { .Sampled = true, .Blendable = true }, .MinDimension2D = 1024 };
    REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
    const auto oldToken = backend::PendingToken;
    const auto owner = internal::DeviceOwner();
    reference::FormatBlendUnavailable = true;
    internal::SetFallback([](ludus::foundation::uint32 token, StartupError error) noexcept {
        internal::RecordAttempt(token, Backend::WebGPU, error);
        const auto next = internal::Reissue(token);
        backend::ActiveKind = Backend::WebGL2;
        internal::SelectBackend(next, Backend::WebGL2);
        reference::FormatBlendUnavailable = false;
        internal::Complete(next, StartupError::None, { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
        return true;
    });
    fixture.Complete();
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Ready);
    CHECK(internal::DeviceOwner() == owner);
    CHECK(info.Startup.WebGpu.ProfileFailure.Requirement == ProfileRequirement::Blendable);
    CHECK(info.Startup.WebGL2.ProfileFailure.Requirement == ProfileRequirement::None);
    CHECK(info.Startup.ProfileFailure.Requirement == ProfileRequirement::None);
    CHECK(info.Profile.Formats[0].Uses.Blendable);
    CHECK_FALSE(info.Enabled.Compute);
    internal::Fail(oldToken, StartupError::DeviceLost);
    internal::Complete(oldToken, StartupError::None, { .MaxFrameDimension2D = 1, .MaxUniformBufferSize = 16 });
    CHECK(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Ready);
}

TEST_CASE("Format creation uses the admitted snapshot before publishing a resource", "[rhi][profile]")
{
    ProfileFixture fixture;
    reference::FormatUnavailable[static_cast<ludus::foundation::usize>(RasterFormat::Rg8Unorm)] = true;
    REQUIRE(fixture.Start({}) == DeviceStatus::Pending);
    fixture.Complete();
    FormatCapabilities format;
    CHECK(GetFormatCapabilities(fixture.Device, RasterFormat::Rg8Unorm, format) == RasterStatus::Unsupported);
    TextureHandle texture;
    const ludus::foundation::uint8 bytes[4]{};
    CHECK(CreateTexture(fixture.Device,
                        { .Width = 1, .Height = 1, .Format = RasterFormat::Rg8Unorm },
                        { .Bytes = bytes, .RowPitch = 4 },
                        texture) == RasterStatus::Unsupported);
    CHECK(reference::Creates[static_cast<ludus::foundation::usize>(internal::RasterKind::Texture)] == 0);
}

TEST_CASE("Fallback profile failure preserves both diagnostics and clears enabled capabilities", "[rhi][profile]")
{
    ProfileFixture fixture;
    DeviceDescription request;
    request.RequiredProfile.Raster.MinTextureDimension2D = 2048;
    REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
    reference::OverrideRasterLimits = true;
    reference::RasterLimitOverride = {ludus::foundation::usize{64} * 1024U * 1024U, 1024, 256, 16384, 32, 128};
    internal::SetFallback([](ludus::foundation::uint32 token, StartupError error) noexcept {
        if (backend::ActiveKind != Backend::WebGPU)
        {
            return false;
        }
        internal::RecordAttempt(token, Backend::WebGPU, error);
        const auto next = internal::Reissue(token);
        backend::ActiveKind = Backend::WebGL2;
        internal::SelectBackend(next, Backend::WebGL2);
        reference::RasterLimitOverride.MaxTextureDimension2D = 512;
        internal::Complete(next, StartupError::None, { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
        return true;
    });
    fixture.Complete();
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Failed);
    CHECK(info.Startup.Error == StartupError::RequirementsUnsatisfied);
    CHECK(info.Startup.WebGpu.ProfileFailure.Requirement == ProfileRequirement::TextureDimension);
    CHECK(info.Startup.WebGpu.ProfileFailure.Available == 1024);
    CHECK(info.Startup.WebGL2.ProfileFailure.Requirement == ProfileRequirement::TextureDimension);
    CHECK(info.Startup.WebGL2.ProfileFailure.Available == 512);
    CHECK(info.Startup.ProfileFailure.Requested == 2048);
    CHECK(info.Startup.ProfileFailure.Available == 512);
    CHECK(info.Profile.Raster.MaxTextureDimension2D == 0);
    CHECK_FALSE(info.Enabled.PortableRaster);
}

TEST_CASE("Unavailable preferred format uses remain optional", "[rhi][profile]")
{
    ProfileFixture fixture;
    DeviceDescription request;
    request.PreferredProfile.FormatCount = 1;
    request.PreferredProfile.Formats[0] = { .Uses = { .Sampled = true, .Storage = true } };
    REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
    fixture.Complete();
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Ready);
    CHECK(info.UnmetProfilePreference.Requirement == ProfileRequirement::Storage);
    CHECK(info.UnmetProfilePreference.Requested == 1);
    CHECK(info.UnmetProfilePreference.Available == 0);
    CHECK(info.Profile.Formats[0].Uses.Sampled);
    CHECK_FALSE(info.Profile.Formats[0].Uses.Storage);
}

TEST_CASE("Stride admission honors the enabled backend's aligned boundary", "[rhi][profile]")
{
    ProfileFixture fixture;
    DeviceDescription request;
    bool rejected = false;
    SECTION("WebGPU supports 256 bytes")
    {
        request.RequiredProfile.Raster.MinVertexStride = 256;
    }
    SECTION("WebGL supports 252 bytes")
    {
        backend::ActiveKind = Backend::WebGL2;
        request.RequiredProfile.Raster.MinVertexStride = 252;
    }
    SECTION("WebGL rejects 256 bytes before Ready")
    {
        backend::ActiveKind = Backend::WebGL2;
        request.RequiredProfile.Raster.MinVertexStride = 256;
        rejected = true;
    }
    REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
    fixture.Complete();
    DeviceInfo info;
    if (rejected)
    {
        REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Failed);
        CHECK(info.Startup.ProfileFailure.Requirement == ProfileRequirement::VertexStride);
        CHECK(info.Startup.ProfileFailure.Requested == 256);
        CHECK(info.Startup.ProfileFailure.Available == 252);
    }
    else
    {
        REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Ready);
        CHECK(info.Profile.Interface.MaxVertexStride == request.RequiredProfile.Raster.MinVertexStride);
    }
}

TEST_CASE("Negotiation retains L4 HDR formats and sixteen vertex attributes", "[rhi][profile][hdr]")
{
    ProfileFixture fixture;
    DeviceDescription request;
    request.RequiredProfile.Raster.MinVertexAttributes = 16;
    request.RequiredProfile.FormatCount = 5;
    for (ludus::foundation::uint32 i = 0; i < 5; ++i)
    {
        request.RequiredProfile.Formats[i].Format = static_cast<RasterFormat>(i);
        request.RequiredProfile.Formats[i].Uses.Sampled = true;
    }
    request.RequiredProfile.Formats[4].Uses.ColorAttachment = true;
    request.RequiredProfile.Formats[4].Uses.Blendable = true;
    REQUIRE(ValidateDeviceDescription(request) == DeviceStatus::Ready);
    REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
    fixture.Complete();
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Ready);
    CHECK(info.Profile.Interface.MaxVertexAttributes == 16);
    CHECK(info.Profile.Raster.HdrColor);
    FormatCapabilities format;
    REQUIRE(GetFormatCapabilities(fixture.Device, RasterFormat::Rgba16Float, format) == RasterStatus::Ready);
    CHECK(format.Uses.Sampled);
    CHECK(format.Uses.Filterable);
    CHECK(format.Uses.ColorAttachment);
    CHECK(format.Uses.Blendable);
    CHECK(format.MaxMipLevels == 13);
    TextureHandle texture;
    CHECK(CreateTexture(fixture.Device,
                        { .Width = 16, .Height = 16, .Format = RasterFormat::Rgba16Float, .Attachment = true },
                        {},
                        texture) == RasterStatus::Ready);
    TextureHandle uploaded;
    CHECK(CreateTexture(fixture.Device,
                        {
                            .Width = 16,
                            .Height = 16,
                            .Format = RasterFormat::Rgba16Float,
                        },
                        {},
                        uploaded) == RasterStatus::InvalidDescription);
}

TEST_CASE("Unavailable HDR requirements reject while preferences preserve the LDR fallback", "[rhi][profile][hdr]")
{
    ProfileFixture fixture;
    reference::HdrAvailable = false;
    DeviceDescription request;
    bool required = false;
    SECTION("required HDR")
    {
        required = true;
        request.RequiredProfile.FormatCount = 1;
        request.RequiredProfile.Formats[0] = { .Format = RasterFormat::Rgba16Float, .Uses = { .Blendable = true } };
    }
    SECTION("preferred HDR")
    {
        request.PreferredProfile.FormatCount = 1;
        request.PreferredProfile.Formats[0] = { .Format = RasterFormat::Rgba16Float, .Uses = { .Blendable = true } };
    }
    REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
    fixture.Complete();
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(fixture.Device, info) == (required ? DeviceStatus::Failed : DeviceStatus::Ready));
    const auto diagnostic = required ? info.Startup.ProfileFailure : info.UnmetProfilePreference;
    CHECK(diagnostic.Requirement == ProfileRequirement::Blendable);
    CHECK(diagnostic.Format == RasterFormat::Rgba16Float);
    CHECK_FALSE(info.Profile.Raster.HdrColor);
    if (required == false)
    {
        FormatCapabilities format;
        CHECK(GetFormatCapabilities(fixture.Device, RasterFormat::Rgba16Float, format) == RasterStatus::Ready);
        CHECK_FALSE(format.Uses.ColorAttachment);
        CHECK(format.MaxMipLevels == 13);
        CHECK(GetFormatCapabilities(fixture.Device, RasterFormat::Rgba8Unorm, format) == RasterStatus::Ready);
        CHECK(format.Uses.Blendable);
    }
}

TEST_CASE("R9 format dimensions and partial view requirements negotiate before Ready", "[rhi][profile][r9]")
{
    ProfileFixture fixture;
    DeviceDescription request;
    request.RequiredProfile.FormatCount = 1;
    request.RequiredProfile.Formats[0] =
    {
        .Format = RasterFormat::Rgba8Unorm,
        .Uses = { .Sampled = true },
        .Dimension = TextureDimension::D2Array,
        .MinDepthOrLayers = 2049,
        .PartialViews = true,
    };
    ProfileRequirement expected = ProfileRequirement::TextureLayerCount;
    ludus::foundation::uint64 available = 2048;
    SECTION("array layers") {}
    SECTION("volume mip count")
    {
        request.RequiredProfile.Formats[0].Dimension = TextureDimension::D3;
        request.RequiredProfile.Formats[0].MinDepthOrLayers = 0;
        request.RequiredProfile.Formats[0].MinMipLevels = 13;
        expected = ProfileRequirement::MipCount;
        available = 12;
    }
    REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
    fixture.Complete();
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Failed);
    CHECK(info.Startup.ProfileFailure.Requirement == expected);
    CHECK(info.Startup.ProfileFailure.Available == available);
}

TEST_CASE("R10 startup rejects unimplemented multisample dimension and sampled-use combinations", "[rhi][profile][r10]")
{
    ProfileFixture fixture;
    DeviceDescription request;
    request.RequiredProfile.FormatCount = 1;
    auto& format = request.RequiredProfile.Formats[0];
    format.Uses.ColorAttachment = true;
    format.SampleCount = 4;
    SECTION("array MSAA")
    {
        format.Dimension = TextureDimension::D2Array;
        format.MinDepthOrLayers = 2;
    }
    SECTION("cube MSAA")
    {
        format.Dimension = TextureDimension::Cube;
        format.MinDepthOrLayers = 6;
    }
    SECTION("sampled MSAA")
    {
        format.Uses.Sampled = true;
    }
    SECTION("private sampled depth MSAA")
    {
        format.DepthSampled = true;
    }
    REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
    fixture.Complete();
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Failed);
    CHECK(info.Startup.ProfileFailure.Requirement == ProfileRequirement::SampleCount);
    CHECK(info.Startup.ProfileFailure.Requested == 4);
    CHECK(info.Startup.ProfileFailure.Available == 1);
}

TEST_CASE("Texture dimensions and BC5 are negotiated against enabled profiles", "[rhi][r18][profile]")
{
    ProfileFixture fixture;
    reference::RasterLimitOverride = backend::RasterLimits();
    reference::RasterLimitOverride.MaxTextureDimension2D = 16384;
    reference::OverrideRasterLimits = true;
    DeviceDescription request;
    request.RequiredProfile.Raster.MinTextureDimension2D = 8240;
    request.RequiredProfile.FormatCount = 1;
    request.RequiredProfile.Formats[0] =
    {
        .Format = RasterFormat::Bc5RgUnorm,
        .Uses = { .Sampled = true, .Filterable = true },
        .MinDimension2D = 8240,
        .MinMipLevels = 14,
    };
    SECTION("enabled native-sized profile")
    {
        REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
        fixture.Complete();
        DeviceInfo info;
        REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Ready);
        CHECK(info.Profile.Raster.MaxTextureDimension2D == 16384);
        FormatCapabilities format;
        REQUIRE(GetFormatCapabilities(fixture.Device, RasterFormat::Bc5RgUnorm, format) == RasterStatus::Ready);
        CHECK(format.MaxSampledDimension2D == 16384);
        CHECK(format.MaxMipLevels == 15);
        CHECK(format.Uses.ColorAttachment == false);
        CHECK(format.MaxDimension3D == 0);
    }
    SECTION("lower device limit")
    {
        reference::RasterLimitOverride.MaxTextureDimension2D = 8192;
        REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
        fixture.Complete();
        DeviceInfo info;
        CHECK(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Failed);
        CHECK(info.Startup.ProfileFailure.Requirement == ProfileRequirement::TextureDimension);
        CHECK(info.Startup.ProfileFailure.Requested == 8240);
        CHECK(info.Startup.ProfileFailure.Available == 8192);
    }
    SECTION("unsupported BC5")
    {
        reference::FormatUnavailable[static_cast<ludus::foundation::usize>(RasterFormat::Bc5RgUnorm)] = true;
        REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
        fixture.Complete();
        DeviceInfo info;
        CHECK(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Failed);
        CHECK(info.Startup.ProfileFailure.Requirement == ProfileRequirement::Sampled);
    }
    SECTION("unaligned compressed base is independently required")
    {
        request.RequiredProfile.Formats[0].RequiresUnalignedBlockExtents = true;
        REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
        fixture.Complete();
        DeviceInfo info;
        CHECK(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Failed);
        CHECK(info.Startup.ProfileFailure.Requirement == ProfileRequirement::UnalignedBlockExtents);
    }
    SECTION("native unaligned compressed base is admitted")
    {
        backend::ActiveKind = Backend::Metal;
        request.RequiredProfile.Formats[0].RequiresUnalignedBlockExtents = true;
        REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
        fixture.Complete();
        DeviceInfo info;
        REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Ready);
        CHECK(info.Profile.Formats[static_cast<ludus::foundation::usize>(RasterFormat::Bc5RgUnorm)]
                  .HasUnalignedBlockExtents == true);
    }
    SECTION("preference is diagnostic only")
    {
        request.PreferredProfile = request.RequiredProfile;
        request.RequiredProfile = {};
        reference::RasterLimitOverride.MaxTextureDimension2D = 8192;
        REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
        fixture.Complete();
        DeviceInfo info;
        REQUIRE(GetDeviceInfo(fixture.Device, info) == DeviceStatus::Ready);
        CHECK(info.UnmetProfilePreference.Requirement == ProfileRequirement::TextureDimension);
        CHECK(info.Profile.Raster.MaxTextureDimension2D == 8192);
    }
}

TEST_CASE("R18c byte and descriptor requirements use enabled limits independently", "[rhi][profile][r18c]")
{
    ProfileFixture fixture;
    DeviceDescription request;
    request.RequiredProfile.Raster.MinBufferSize = ludus::foundation::usize{64} * 1024 * 1024 + 256;
    request.RequiredProfile.Raster.MinTextureUploadBytes = ludus::foundation::usize{64} * 1024 * 1024 + 4;
    request.RequiredProfile.Raster.MinBindings = 9;
    request.RequiredProfile.Raster.MinUniformBindings = 12;
    request.RequiredProfile.Raster.MinTextureBindings = 16;
    request.RequiredProfile.Raster.MinSamplerBindings = 16;
    request.RequiredProfile.Compute.MinStorageRange = ludus::foundation::usize{16} * 1024 * 1024 + 4;
    request.RequiredProfile.Compute.MinStorageBindings = 5;
    ProfileRequirement expected = ProfileRequirement::None;
    SECTION("expanded supported profile") {}
    SECTION("required upload limit")
    {
        request.RequiredProfile.Raster.MinTextureUploadBytes = ludus::foundation::usize{512} * 1024 * 1024 + 1;
        expected = ProfileRequirement::TextureUploadBytes;
    }
    SECTION("required uniform count")
    {
        request.RequiredProfile.Raster.MinUniformBindings = 13;
        expected = ProfileRequirement::UniformBindings;
    }
    SECTION("required sampled texture count")
    {
        request.RequiredProfile.Raster.MinTextureBindings = 17;
        expected = ProfileRequirement::TextureBindings;
    }
    SECTION("required sampler count")
    {
        request.RequiredProfile.Raster.MinSamplerBindings = 17;
        expected = ProfileRequirement::SamplerBindings;
    }
    SECTION("required set-wide uniform count")
    {
        request.RequiredProfile.Raster.MinUniformBindingsPerSet = 17;
        expected = ProfileRequirement::UniformBindingsPerSet;
    }
    SECTION("required set-wide sampler count")
    {
        request.RequiredProfile.Raster.MinSamplerBindingsPerSet = 17;
        expected = ProfileRequirement::SamplerBindingsPerSet;
    }
    SECTION("required storage count")
    {
        request.RequiredProfile.Compute.MinStorageBindings = 9;
        expected = ProfileRequirement::StorageBindings;
    }
    SECTION("preferred storage count preserves Ready")
    {
        request.PreferredProfile.Compute.MinStorageBindings = 9;
    }
    REQUIRE(fixture.Start(request) == DeviceStatus::Pending);
    fixture.Complete();
    DeviceInfo info;
    const DeviceStatus status = GetDeviceInfo(fixture.Device, info);
    CHECK(status == (expected == ProfileRequirement::None ? DeviceStatus::Ready : DeviceStatus::Failed));
    CHECK(info.Startup.ProfileFailure.Requirement == expected);
    if (request.PreferredProfile.Compute.MinStorageBindings != 0)
    {
        CHECK(info.UnmetProfilePreference.Requirement == ProfileRequirement::StorageBindings);
        CHECK(info.UnmetProfilePreference.Requested == 9);
        CHECK(info.UnmetProfilePreference.Available == 8);
    }
}
