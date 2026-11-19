#pragma once

#include "internal/world_surface_bake.h"

namespace ludus::editor::testing
{
// CPU evaluation of the native atlas' red irradiance for a receiver facing +Z.
[[nodiscard]] foundation::float32 ReadPositiveZProbeIrradiance(const WorldProbeAtlas& atlas,
                                                               const graphics::renderer::GiProbeGrid& grid,
                                                               foundation::math::Vector3 positionWorld);
} // namespace ludus::editor::testing
