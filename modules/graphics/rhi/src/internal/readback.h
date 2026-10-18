#pragma once
#include <ludus/foundation/base/types.h>
#include <span>
namespace ludus::graphics::rhi::backend
{
void SetNativePipelinePreparationPaused(bool isPaused) noexcept;
ludus::foundation::uint32 GetNativePreparationStarted() noexcept;
// Test-only offscreen readback. Not installed; no backend handle is exported.
bool ReadHeadlessPixels(std::span<ludus::foundation::uint8> pixels) noexcept;
} // namespace ludus::graphics::rhi::backend
