#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/graphics/renderer/scene_shader.hpp>

namespace ludus::graphics::renderer::internal
{
/// Record a measured candidate rejection without touching the drawable scene.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): required and remaining bytes have distinct reporting roles.
[[nodiscard]] LUDUS_INLINE rhi::RasterStatus RejectSceneMemory(SceneShaderDiagnostics* diagnosticsOrNull,
                                                               SceneMemoryBudget budget,
                                                               foundation::uint64 requiredBytes,
                                                               foundation::uint64 availableBytes) noexcept
{
    if (diagnosticsOrNull != nullptr)
    {
        diagnosticsOrNull->ExceededBudget = budget;
        diagnosticsOrNull->RequiredBytes = requiredBytes;
        diagnosticsOrNull->AvailableBytes = availableBytes;
    }
    return rhi::RasterStatus::CapacityExceeded;
}
// NOLINTEND(bugprone-easily-swappable-parameters)
} // namespace ludus::graphics::renderer::internal
