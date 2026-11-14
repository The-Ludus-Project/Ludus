#pragma once

#include <ludus/graphics/rhi/profile.h>

namespace ludus::graphics::rhi::internal
{
/// Minimum guaranteed uniform range of the complete portable raster profile.
inline constexpr foundation::usize PORTABLE_UNIFORM_BYTES = foundation::usize{16} * 1024;
bool ValidProfileRequirements(const DeviceProfileRequirements&) noexcept;
bool NeedsRaster(const DeviceProfileRequirements&) noexcept;
bool NeedsCompute(const DeviceProfileRequirements&) noexcept;
ProfileDiagnostic CheckProfile(const DeviceProfileRequirements&, const DeviceProfile&) noexcept;
DeviceProfile QueryDeviceProfile() noexcept;
// Clamp native byte limits to the addressable host size; logical budgets are independently reserved.
foundation::usize GetResourceSizeLimit(foundation::uint64 nativeBytes) noexcept;
DeviceProfile CurrentDeviceProfile() noexcept;
// Guaranteed normalized formats and the enabled L4 HDR attachment profile.
FormatCapabilities PortableFormatLimits(RasterFormat, const RasterCapabilities&) noexcept;
// R12b write-only storage textures admit only the portable RGBA8Unorm/RGBA16Float pair: core
// WebGPU, Vulkan's mandatory storage-image formats, Direct3D 12 typed UAV stores and Metal.
[[nodiscard]] constexpr bool IsStorageTextureFormat(RasterFormat format) noexcept
{
    return format == RasterFormat::Rgba8Unorm || format == RasterFormat::Rgba16Float;
}
} // namespace ludus::graphics::rhi::internal
