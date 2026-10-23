#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::rhi
{
/// Color interpretation supplied to the native spatial reconstruction algorithm.
/// Neither mode performs application tone mapping or changes the texture format.
enum class SpatialUpscaleColorProcessing : foundation::uint8
{
    Linear,     ///< Input and output contain linear-light values in [0,1].
    Perceptual, ///< Input and output contain tone-mapped perceptual (sRGB) values in [0,1].
};
/// Runtime support on the selected device, independent of shader artifact availability.
struct SpatialUpscaleCapabilities final
{
    /// True only when the selected Metal device supports MetalFX Spatial.
    bool IsMetalFxSpatialSupported = false;
};
/// Immutable full-image, single-sample RGBA8 reconstruction configuration.
/// Creation can compile vendor pipelines; prepare between frames and reuse until
/// dimensions or color interpretation change. Framework-private GPU allocations
/// are opaque; the RHI's per-kind resource bound still limits effect instances.
struct SpatialUpscalerDescription final
{
    foundation::uint32 InputWidth = 0;   ///< Complete source width in pixels, nonzero.
    foundation::uint32 InputHeight = 0;  ///< Complete source height in pixels, nonzero.
    foundation::uint32 OutputWidth = 0;  ///< Destination width in pixels, at least InputWidth.
    foundation::uint32 OutputHeight = 0; ///< Destination height in pixels, at least InputHeight.
    /// Interpret stored source/output channels explicitly; defaults to linear light.
    SpatialUpscaleColorProcessing ColorProcessing = SpatialUpscaleColorProcessing::Linear;
};
/// Device-scoped prepared MetalFX Spatial instance. Copies borrow its identity.
/// Destroy invalidates the owner immediately; accepted frame submissions retain
/// the physical effect until completion. All operations use the RHI owner thread.
class SpatialUpscalerHandle final
{
public:
    /// Compare identities only; this does not query readiness or device health.
    friend LUDUS_INLINE constexpr bool operator==(SpatialUpscalerHandle, SpatialUpscalerHandle) noexcept = default;

private:
    foundation::uint64 mOwner = 0;
    foundation::uint64 mGeneration = 0;
    foundation::uint32 mSlot = 0;
    /// Internal registry access publishes and validates device/generation identity.
    friend struct internal::RasterAccess;
};
/// Query support without allocating an effect. Ready with a false capability is
/// a valid unsupported backend/device. Invalid or non-ready devices preserve output.
[[nodiscard]] RasterStatus GetSpatialUpscaleCapabilities(DeviceHandle device,
                                                         SpatialUpscaleCapabilities& outCapabilities) noexcept;
/// Prepare an immutable effect outside frames. Only Ready/Pending publish the
/// previously empty output; failures preserve it. Unsupported is explicit on
/// non-Metal backends and Metal devices without MetalFX Spatial. No fallback.
[[nodiscard]] RasterStatus CreateSpatialUpscaler(DeviceHandle device,
                                                 const SpatialUpscalerDescription& description,
                                                 SpatialUpscalerHandle& outUpscaler) noexcept;
/// Query readiness of a live effect identity; stale or foreign owners return InvalidHandle.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle device, SpatialUpscalerHandle upscaler) noexcept;
/// Release ownership between frames, preserving the handle on failure. Submitted
/// uses delay physical retirement; Ready clears the handle. No GPU wait occurs.
[[nodiscard]] RasterStatus Destroy(DeviceHandle device, SpatialUpscalerHandle& inoutUpscaler) noexcept;
/// Encode native reconstruction in the acquired RHI frame, following preceding
/// producers on the same queue. Both textures must be distinct RGBA8 D2 images,
/// one mip/layer/sample, with the exact configured dimensions. Source color must
/// be initialized; destination must be a private offscreen attachment with no
/// external readers. Ready fully defines color and invalidates private depth/stencil.
/// Validation failures preserve the current pass. An accepted call ends it;
/// subsequent raster draws require BeginRasterPass. The frame retains effect and
/// textures through submission completion. No pipeline creation or CPU copies.
[[nodiscard]] RasterStatus EncodeSpatialUpscale(DeviceHandle device,
                                                SpatialUpscalerHandle upscaler,
                                                TextureHandle source,
                                                TextureHandle destination) noexcept;
} // namespace ludus::graphics::rhi
