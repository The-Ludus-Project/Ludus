#pragma once

#include <ludus/foundation/base/types.h>

#import <Metal/MTLCommandBuffer.h>
#import <Metal/MTLDevice.h>
#import <Metal/MTLTexture.h>

namespace ludus::graphics::rhi::backend
{
// Borrowed from the existing Metal session/registry, on its owner thread only.
// Common RHI admission and frame retention validate/pin each texture slot first.
id<MTLDevice> GetSpatialUpscaleMetalDevice() noexcept;
id<MTLTexture> GetSpatialUpscaleMetalTexture(foundation::usize slot) noexcept;
id<MTLCommandBuffer> GetSpatialUpscaleMetalCommandBuffer() noexcept;
} // namespace ludus::graphics::rhi::backend
