#pragma once

// G2 Metal capture adapter: MTLCaptureManager writes the captured frame to a .gputrace
// document. Contract: docs/architecture/graphics-debugging.md, "Metal on macOS".

#if !defined(__OBJC__)
#    error "graphics_capture_metal.h is Objective-C++ only"
#endif

#import <Metal/MTLDevice.h>

namespace ludus::graphics::rhi::backend
{
/// Register the bound owner slot's Metal capture adapter for `device`, which it retains
/// until ReleaseMetalCaptureAdapter. Owner thread only, after the device is created.
void RegisterMetalCaptureAdapter(id<MTLDevice> device) noexcept;
/// Release the device the bound owner slot's adapter retains. Call during backend
/// shutdown, after the facade ended the capture session; owner thread only.
void ReleaseMetalCaptureAdapter() noexcept;
} // namespace ludus::graphics::rhi::backend
