#pragma once
#include "wave_scene_queries.h"
#include <ludus/graphics/renderer/wave_tracing.hpp>

namespace ludus::graphics::renderer::internal
{
/// Coherent scalar Gaussian-mode connection through the current scene. Source emits a Gaussian waist
/// with Sigma = distance / wavenumber; detector samples the field on the mean axis. Geometry in each
/// interaction frustum is projected along propagation onto its midpoint screen. Sequential angular-spectrum
/// propagation accounts for interference across all open cells and successive regions; surfaces are opaque
/// according to their fixed-mip coverage. Returns field divided by the unobstructed analytic Gaussian field.
/// No reflection or polarization is performed here. Caller supplies a finite unit direction, positive metres,
/// receiverWorld retains the independently offset receiver for ballistic visibility; direction/distance define
/// the propagation axis. wavelength/distance <= 1/32 and valid scene data. Failure preserves outField. No
/// allocation/shared state.
[[nodiscard]] rhi::RasterStatus EvaluateWaveConnectionToReceiver(const ReferenceSceneData& scene,
                                                                 math::Vector3d sourceWorld,
                                                                 math::Vector3d receiverWorld,
                                                                 math::Vector3d directionWorld,
                                                                 float64 distanceWorld,
                                                                 float64 wavelengthWorld,
                                                                 WaveField& outField) noexcept;

[[nodiscard]] rhi::RasterStatus EvaluateWaveConnection(const ReferenceSceneData& scene,
                                                       math::Vector3d sourceWorld,
                                                       math::Vector3d directionWorld,
                                                       float64 distanceWorld,
                                                       float64 wavelengthWorld,
                                                       WaveField& outField) noexcept;
} // namespace ludus::graphics::renderer::internal
