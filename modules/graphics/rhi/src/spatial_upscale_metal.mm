#include "internal/spatial_upscale_metal.h"
#include "internal/spatial_upscale_backend.h"
#include "internal/storage.h"

#import <MetalFX/MTLFXSpatialScaler.h>

// Thanks to Kelvin Chiu / Apple, "Boost performance with MetalFX Upscaling",
// WWDC22 session 10103, spatial initialization/per-frame encoding (3:39, 9:16):
// https://developer.apple.com/videos/play/wwdc2022/10103/
// Reuse dimension-specific effects, declare color interpretation, and honor the
// effect's texture usage requirements. RHI retains submission ownership; MetalFX
// owns its reconstruction algorithm. No sample algorithm or shader is copied.
namespace ludus::graphics::rhi::backend
{
namespace
{
internal::StorageTable<id<MTLFXSpatialScaler>> gSpatialUpscalers;
}
bool IsMetalFxSpatialSupported() noexcept
{
    id<MTLDevice> deviceOrNull = GetSpatialUpscaleMetalDevice();
    return deviceOrNull != nil && [MTLFXSpatialScalerDescriptor supportsDevice:deviceOrNull] == true;
}
RasterStatus SpatialUpscaleGrow(foundation::usize count) noexcept
{
    return gSpatialUpscalers.EnsureSlots(count) == true ? RasterStatus::Ready : internal::StorageFailure();
}
RasterStatus SpatialUpscaleCreate(foundation::usize slot, const SpatialUpscalerDescription& description) noexcept
{
    @autoreleasepool
    {
        if (IsMetalFxSpatialSupported() == false)
        {
            return RasterStatus::Unsupported;
        }
        MTLFXSpatialScalerDescriptor* descriptorOrNull = [MTLFXSpatialScalerDescriptor new];
        if (descriptorOrNull == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        descriptorOrNull.inputWidth = description.InputWidth;
        descriptorOrNull.inputHeight = description.InputHeight;
        descriptorOrNull.outputWidth = description.OutputWidth;
        descriptorOrNull.outputHeight = description.OutputHeight;
        descriptorOrNull.colorTextureFormat = MTLPixelFormatRGBA8Unorm;
        descriptorOrNull.outputTextureFormat = MTLPixelFormatRGBA8Unorm;
        descriptorOrNull.colorProcessingMode = description.ColorProcessing == SpatialUpscaleColorProcessing::Linear
                                                   ? MTLFXSpatialScalerColorProcessingModeLinear
                                                   : MTLFXSpatialScalerColorProcessingModePerceptual;
        id<MTLFXSpatialScaler> scalerOrNull =
            [descriptorOrNull newSpatialScalerWithDevice:GetSpatialUpscaleMetalDevice()];
        if (scalerOrNull == nil)
        {
            return RasterStatus::Failed;
        }
        gSpatialUpscalers[slot] = scalerOrNull;
        return RasterStatus::Ready;
    }
}
RasterStatus
SpatialUpscaleEncode(foundation::usize slot, foundation::usize source, foundation::usize destination) noexcept
{
    id<MTLFXSpatialScaler> scalerOrNull = gSpatialUpscalers[slot];
    id<MTLTexture> inputOrNull = GetSpatialUpscaleMetalTexture(source);
    id<MTLTexture> outputOrNull = GetSpatialUpscaleMetalTexture(destination);
    id<MTLCommandBuffer> commandOrNull = GetSpatialUpscaleMetalCommandBuffer();
    if (commandOrNull == nil || inputOrNull == nil || outputOrNull == nil || scalerOrNull == nil)
    {
        return RasterStatus::InvalidState;
    }
    if ((inputOrNull.usage & scalerOrNull.colorTextureUsage) != scalerOrNull.colorTextureUsage ||
        (outputOrNull.usage & scalerOrNull.outputTextureUsage) != scalerOrNull.outputTextureUsage ||
        outputOrNull.storageMode != MTLStorageModePrivate)
    {
        return RasterStatus::InvalidDescription;
    }
    scalerOrNull.colorTexture = inputOrNull;
    scalerOrNull.inputContentWidth = scalerOrNull.inputWidth;
    scalerOrNull.inputContentHeight = scalerOrNull.inputHeight;
    scalerOrNull.outputTexture = outputOrNull;
    [scalerOrNull encodeToCommandBuffer:commandOrNull];
    // Command buffers and the common RHI frame ledger retain the actual uses.
    // Do not leave an extra texture-generation pin in mutable framework properties.
    scalerOrNull.colorTexture = nil;
    scalerOrNull.outputTexture = nil;
    return RasterStatus::Ready;
}
void SpatialUpscaleDestroy(foundation::usize slot) noexcept
{
    gSpatialUpscalers[slot] = nil;
}
} // namespace ludus::graphics::rhi::backend
