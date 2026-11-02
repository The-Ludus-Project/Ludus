#pragma once
#include "../../shaders/wave_scene_layout.h"
#include "reference_data.h"
#include <ludus/foundation/math/elliptical_cone.hpp>

namespace ludus::graphics::renderer::internal
{
/// Geometry-only region; materials, coverage and scattering are evaluated by the transport operator.
struct WaveInteractionRegion final
{
    math::EllipticalCone Frustum;
    StaticArray<uint32, LUDUS_WAVE_REGION_TRIANGLES> Triangles;
    uint32 TriangleCount = 0;
};
/// Two BVH walks: locate nearest axial interaction, then gather all intersections in its 2-major-radius slab.
/// Ready includes an empty scene/miss. Invalid input, malformed hierarchy or capacity exhaustion fail without
/// changing outRegion. All triangle IDs are sorted; shared edges and overlapping geometry are retained.
[[nodiscard]] rhi::RasterStatus FindWaveInteractionRegion(const ReferenceSceneData& scene,
                                                          const math::EllipticalCone& cone,
                                                          WaveInteractionRegion& outRegion) noexcept;
} // namespace ludus::graphics::renderer::internal
