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
/// Native depth cache allocations of the active Metal surface, for tests.
struct MetalSurfaceDepthAllocations final
{
    /// Bytes allocated by the memoryless depth texture; 0 when it is absent.
    ludus::foundation::usize MemorylessBytes = 0;
    /// Bytes allocated by the private depth texture; 0 when it is absent.
    ludus::foundation::usize PrivateBytes = 0;
    /// Whether the surface has a memoryless depth texture.
    bool HasMemorylessTexture = false;
};
/// Inspect the active surface's native depth caches.
[[nodiscard]] MetalSurfaceDepthAllocations GetMetalSurfaceDepthAllocations() noexcept;
#endif
} // namespace ludus::graphics::rhi::backend
