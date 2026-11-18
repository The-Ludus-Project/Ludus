#pragma once
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>
#include <span>
namespace ludus::graphics::rhi::backend
{
void SetNativePipelinePreparationPaused(bool isPaused) noexcept;
ludus::foundation::uint32 GetNativePreparationStarted() noexcept;
// Test-only offscreen readback. Not installed; no backend handle is exported.
bool ReadHeadlessPixels(std::span<ludus::foundation::uint8> pixels) noexcept;
#if defined(LUDUS_PLATFORM_MACOS)
// Inspect the active surface's native depth caches. Returns whether it has a memoryless texture.
bool GetMetalSurfaceDepthAllocations(ludus::foundation::usize& outMemorylessBytes,
                                     ludus::foundation::usize& outPrivateBytes) noexcept;
#endif
} // namespace ludus::graphics::rhi::backend
