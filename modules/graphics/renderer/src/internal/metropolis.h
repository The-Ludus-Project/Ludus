#pragma once

#include "../../shaders/metropolis_layout.h"
#include "reference_data.h"

#include <ludus/foundation/math/metropolis.hpp>

namespace ludus::graphics::renderer::internal
{
inline constexpr uint32 METROPOLIS_MAXIMUM_DEPTH = LUDUS_MH_MAXIMUM_SCATTERING_DEPTH;
inline constexpr usize METROPOLIS_CAMERA_DIMENSIONS = LUDUS_MH_CAMERA_DIMENSIONS;
inline constexpr usize METROPOLIS_BSDF_DIMENSIONS = LUDUS_MH_BSDF_DIMENSIONS;
inline constexpr usize METROPOLIS_PRIMARY_DIMENSIONS =
    METROPOLIS_CAMERA_DIMENSIONS + METROPOLIS_MAXIMUM_DEPTH * METROPOLIS_BSDF_DIMENSIONS;
using MetropolisPrimary = StaticArray<float32, METROPOLIS_PRIMARY_DIMENSIONS>;
inline constexpr uint32 METROPOLIS_DEFAULT_BURN_IN = 128;
inline constexpr float32 METROPOLIS_DEFAULT_LARGE_STEP_PROBABILITY = 0.3F;
inline constexpr float32 METROPOLIS_DEFAULT_SMALL_STEP_RADIUS = 0.05F;
enum class MetropolisEstimator : uint32
{
    Standard,
    WasteRecycling,
    RaoBlackwell,
};
struct MetropolisOptions final
{
    uint32 BurnIn = METROPOLIS_DEFAULT_BURN_IN;
    float32 LargeStepProbability = METROPOLIS_DEFAULT_LARGE_STEP_PROBABILITY;
    float32 SmallStepRadius = METROPOLIS_DEFAULT_SMALL_STEP_RADIUS;
    float32 TargetFloor = 1;
    float32 RestorationScale = 1;
};
struct MetropolisChain final
{
    MetropolisPrimary Primary{};
    math::Vector3 Radiance{};
    float32 Target = 0;
    math::MetropolisTour Tour{};
    math::WeightedRadiance Accumulation{};
    uint32 Iteration = 0;
    bool IsInitialized = false;
};
struct MetropolisPathAovs final
{
    math::Vector3 Emission{};
    math::Vector3 Direct{};
    math::Vector3 Indirect{};
};
using MetropolisEvaluator = rhi::RasterStatus (*)(const void*, const MetropolisPrimary&, math::Vector3&) noexcept;

/// Independently executable CPU chain. One proposal per call, the same addressed
/// random scopes as the template; no allocation. Failure preserves chain/output.
/// Keep evaluator, context, key, pixel and options fixed until resetting the chain.
[[nodiscard]] rhi::RasterStatus AdvanceMetropolisChain(const MetropolisOptions& options,
                                                       MetropolisEstimator estimator,
                                                       math::RandomKey key,
                                                       uint32 pixel,
                                                       MetropolisEvaluator evaluate,
                                                       const void* context,
                                                       MetropolisChain& inoutChain,
                                                       math::Vector3& outRadiance) noexcept;

/// Independent Jump Restore CPU oracle with unit-rate small-step MH local dynamics
/// and uniform restorations at rate RestorationScale/target. One competing event
/// per call; the standard, waste-recycling and RB modes estimate the same weighted
/// ratio as MH. RB flushes at accepted local moves and restorations. Event clocks
/// and acceptance use independent addressed dimensions. Failure preserves chain/output.
/// Keep evaluator/context/key/pixel/options fixed until resetting; no allocation.
[[nodiscard]] rhi::RasterStatus AdvanceJumpRestoreChain(const MetropolisOptions& options,
                                                        MetropolisEstimator estimator,
                                                        math::RandomKey key,
                                                        uint32 pixel,
                                                        MetropolisEvaluator evaluate,
                                                        const void* context,
                                                        MetropolisChain& inoutChain,
                                                        math::Vector3& outRadiance) noexcept;

/// CPU transport oracle over the renderer's retained scene. BSDF-only finite-depth
/// reflection, mesh emission and environment; no NEE, roulette or energy clamps.
/// Coated diffuse is unsupported because its extra random dimensions are not in
/// this primary cube. Failure preserves outRadiance. ray is the jittered camera ray.
[[nodiscard]] rhi::RasterStatus EvaluateMetropolisPath(const ReferenceSceneData& scene,
                                                       rhi::RayInput ray,
                                                       const MetropolisPrimary& primary,
                                                       uint32 scatteringDepth,
                                                       math::Vector3 environment,
                                                       math::Vector3& outRadiance,
                                                       const EnvironmentData* environmentOrNull = nullptr,
                                                       MetropolisPathAovs* outAovsOrNull = nullptr) noexcept;
} // namespace ludus::graphics::renderer::internal
