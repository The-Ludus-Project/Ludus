// Addressed gradient noise; see terrain_noise.h for the contract and attribution.

#include <ludus/content/terrain/terrain_noise.h>

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::terrain
{
namespace
{
namespace math = foundation::math;

/// Bits of one packed lattice coordinate inside the 64-bit address scope.
constexpr uint32 COORDINATE_BITS = 32;
/// Gradient bit that negates the X offset.
constexpr uint32 GRADIENT_NEGATE_X = 1U;
/// Gradient bit that negates the Z offset.
constexpr uint32 GRADIENT_NEGATE_Z = 2U;

/// A lattice point, addressed by its signed global coordinates.
struct LatticePoint final
{
    int32 X = 0;
    int32 Z = 0;
};

/// A sample position relative to one lattice point, in lattice units.
struct LatticeOffset final
{
    float64 X = 0;
    float64 Z = 0;
};

// Pack the two signed lattice coordinates as their unsigned bit patterns.
[[nodiscard]] float64
ComputeGradient(math::RandomKey key, uint32 octave, LatticePoint point, LatticeOffset offset) noexcept
{
    const uint64 scope =
        (static_cast<uint64>(static_cast<uint32>(point.X)) << COORDINATE_BITS) | static_cast<uint32>(point.Z);
    const uint32 gradient = math::SampleUInt32(key, { .Scope = scope, .Event = octave });
    return ((gradient & GRADIENT_NEGATE_X) == 0 ? offset.X : -offset.X) +
           ((gradient & GRADIENT_NEGATE_Z) == 0 ? offset.Z : -offset.Z);
}
} // namespace

bool CanSampleTerrainNoise(const TerrainNoiseShape& shape, float64 halfExtentMetres) noexcept
{
    constexpr float64 HIGHEST_OCTAVE_MULTIPLIER = static_cast<float64>(1U << (TERRAIN_NOISE_OCTAVES - 1));
    if (math::IsFinite(shape.MinimumElevationMetres) == false ||
        math::IsFinite(shape.MaximumElevationMetres) == false || math::IsFinite(shape.FeatureSizeMetres) == false ||
        math::IsFinite(shape.Roughness) == false || math::IsFinite(halfExtentMetres) == false)
    {
        return false;
    }
    return halfExtentMetres >= 0 && shape.FeatureSizeMetres > 0 &&
           shape.MinimumElevationMetres <= shape.MaximumElevationMetres && shape.Roughness >= 0 &&
           shape.Roughness <= 1 &&
           halfExtentMetres / shape.FeatureSizeMetres * HIGHEST_OCTAVE_MULTIPLIER <= MAX_TERRAIN_LATTICE_POSITION;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the octave ordinal and the X then Z position are distinct.
float64 SampleTerrainNoise(math::RandomKey key, uint32 octave, float64 positionX, float64 positionZ) noexcept
{
    const int32 x = static_cast<int32>(math::Floor(positionX));
    const int32 z = static_cast<int32>(math::Floor(positionZ));
    const float64 offsetX = positionX - x;
    const float64 offsetZ = positionZ - z;
    const float64 fadeX = math::SmoothStep01(offsetX);
    const float64 fadeZ = math::SmoothStep01(offsetZ);
    const float64 near = math::Lerp(ComputeGradient(key, octave, {x, z}, {offsetX, offsetZ}),
                                    ComputeGradient(key, octave, {x + 1, z}, {offsetX - 1, offsetZ}),
                                    fadeX);
    const float64 far = math::Lerp(ComputeGradient(key, octave, {x, z + 1}, {offsetX, offsetZ - 1}),
                                   ComputeGradient(key, octave, {x + 1, z + 1}, {offsetX - 1, offsetZ - 1}),
                                   fadeX);
    return math::Clamp(math::Lerp(near, far, fadeZ), -1.0, 1.0);
}

float64 ComputeTerrainElevation(const TerrainNoiseShape& shape,
                                math::RandomKey key,
                                float64 positionX,
                                float64 positionZ) noexcept
{
    if (shape.Profile == TerrainNoiseProfile::Flat || shape.MinimumElevationMetres == shape.MaximumElevationMetres)
    {
        return shape.MinimumElevationMetres;
    }
    float64 frequency = 1 / shape.FeatureSizeMetres;
    float64 amplitude = 1;
    float64 totalWeight = 0;
    float64 elevation = 0;
    for (uint32 octave = 0; octave < TERRAIN_NOISE_OCTAVES; ++octave)
    {
        const float64 noise = SampleTerrainNoise(key, octave, positionX * frequency, positionZ * frequency);
        elevation +=
            amplitude * (shape.Profile == TerrainNoiseProfile::Ridges ? 1 - math::Abs(noise) : (noise + 1) * 0.5);
        totalWeight += amplitude;
        amplitude *= shape.Roughness;
        frequency *= 2;
    }
    return math::Lerp(shape.MinimumElevationMetres, shape.MaximumElevationMetres, elevation / totalWeight);
}
} // namespace ludus::content::terrain
