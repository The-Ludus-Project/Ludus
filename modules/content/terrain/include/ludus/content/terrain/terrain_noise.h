#pragma once

// Addressed gradient-noise elevation recipe of terrain slice T1: the bounded
// macro-shape profile the World editor generates default terrain with, moved out
// of the editor so cookers, tests and tools sample the same elevation. The lattice
// is global and independent of any mesh grid; every octave gets its own address,
// so sample resolution and traversal order never change a height. Contract:
// docs/architecture/terrain-generation.md#noise-and-reproducibility.
//
// Thanks to Ken Perlin, "Implementing Improved Perlin Noise", GPU Gems 1,
// chapter 5, sections 5.3-5.4, for the smooth fade and balanced gradient
// argument. Ludus's two-dimensional gradient set, addressed sampling and
// ridged transform are separate versioned choices; no chapter shader or
// periodic lookup table is copied.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/addressed_random.hpp>

namespace ludus::content::terrain
{
using foundation::core::float64;
using foundation::core::int32;
using foundation::core::uint32;
using foundation::core::uint64;
using foundation::core::uint8;

/// Version of the lattice addressing, gradient set, fade and octave layout below; store it with generated content.
inline constexpr uint32 TERRAIN_NOISE_LAYOUT_VERSION = 1;
/// Detail octaves summed by ComputeTerrainElevation; each doubles the frequency of the one before.
inline constexpr uint32 TERRAIN_NOISE_OCTAVES = 3;
/// Largest lattice coordinate the v1 addresses admit; sampling also reads the next lattice point.
inline constexpr float64 MAX_TERRAIN_LATTICE_POSITION = 2147483646;

/// @brief Which elevation profile the summed octaves follow.
enum class TerrainNoiseProfile : uint8
{
    /// Octaves map to [0, 1] and sum to rolling hills.
    Hills,
    /// Octaves fold around zero so crests become sharp ridges.
    Ridges,
    /// Every sample is the minimum elevation.
    Flat,
};

/// @brief Bounded macro-shape controls shared by the editor recipe and headless cookers.
struct TerrainNoiseShape final
{
    /// Elevation of the lowest possible sample, in metres.
    float64 MinimumElevationMetres = 0;
    /// Elevation of the highest possible sample, in metres; at least the minimum.
    float64 MaximumElevationMetres = 0;
    /// Wavelength of the first octave, in metres; positive.
    float64 FeatureSizeMetres = 1;
    /// Amplitude ratio between successive octaves, 0 through 1.
    float64 Roughness = 0;
    /// Elevation profile.
    TerrainNoiseProfile Profile = TerrainNoiseProfile::Hills;
};

/// @brief Whether a shape can sample every position within halfExtentMetres of the origin.
///
/// Rejects nonpositive feature sizes, reversed elevation bounds, non-finite values and extents whose highest
/// octave would address a lattice point beyond MAX_TERRAIN_LATTICE_POSITION.
/// @param shape The controls to check.
/// @param halfExtentMetres Largest absolute X or Z coordinate that will be sampled; nonnegative.
[[nodiscard]] bool CanSampleTerrainNoise(const TerrainNoiseShape& shape, float64 halfExtentMetres) noexcept;

/// @brief One octave of smooth gradient noise at a lattice-space position, in [-1, 1].
///
/// @param key Key of the terrain's random domain.
/// @param octave Octave ordinal; it selects the address event.
/// @param positionX Lattice-space X; its floor must lie within the v1 address range.
/// @param positionZ Lattice-space Z; its floor must lie within the v1 address range.
[[nodiscard]] float64
SampleTerrainNoise(foundation::math::RandomKey key, uint32 octave, float64 positionX, float64 positionZ) noexcept;

/// @brief Elevation in metres at a terrain-space position, within the shape's elevation bounds.
///
/// @pre CanSampleTerrainNoise(shape, max(|positionX|, |positionZ|)) is true.
[[nodiscard]] float64 ComputeTerrainElevation(const TerrainNoiseShape& shape,
                                              foundation::math::RandomKey key,
                                              float64 positionX,
                                              float64 positionZ) noexcept;
} // namespace ludus::content::terrain
