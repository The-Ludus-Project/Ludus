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
// argument, and to Ken Perlin, "Improving Noise", SIGGRAPH '02 (ACM
// Transactions on Graphics 21(3)), pp. 681-682,
// https://www.cs.rpi.edu/~cutler/classes/advancedgraphics/S23/papers/perlin_improving_noise_2002.pdf,
// which shows that the cubic interpolant 3t^2 - 2t^3 has nonzero second
// derivatives at t = 0 and t = 1 and replaces it with 6t^5 - 15t^4 + 10t^3.
// Layout 1 keeps the cubic for saved recipes; layout 2 adopts the quintic.
// Ludus's two-dimensional gradient set, addressed sampling and ridged
// transform are separate versioned choices; no chapter shader, paper code or
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

/// @brief Stored version of the lattice addressing, gradient set, fade and octave layout below.
///
/// Every layout shares the lattice addresses, gradient set, octave order and normalization; they differ only in the
/// fade that blends the four gradients of a lattice cell. A stored value never changes meaning: a different sample
/// value needs a new layout, never an edit of an existing one.
enum class TerrainNoiseLayout : uint8
{
    /// Version 1: the cubic fade 3t^2 - 2t^3 (FoundationMath SmoothStep01). Every terrain recipe saved before the
    /// layout was recorded, and every cook replay of one, uses it. Values and first derivatives are continuous across
    /// lattice edges; second derivatives jump there.
    CubicFade = 1,
    /// Version 2: Perlin's quintic fade 6t^5 - 15t^4 + 10t^3, evaluated as t * t * t * (t * (6t - 15) + 10). Its first
    /// and second derivatives vanish at t = 0 and t = 1, so second derivatives are also continuous across lattice
    /// edges.
    QuinticFade = 2,
};

/// Layout that newly authored or cooked content records; readers use the version their data records instead.
inline constexpr TerrainNoiseLayout LATEST_TERRAIN_NOISE_LAYOUT = TerrainNoiseLayout::QuinticFade;
/// Stored integer of LATEST_TERRAIN_NOISE_LAYOUT, for recipes and cooked manifests (terrain_cook.h RecipeVersion).
inline constexpr uint32 TERRAIN_NOISE_LAYOUT_VERSION = static_cast<uint32>(LATEST_TERRAIN_NOISE_LAYOUT);
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
    /// Noise layout. New content keeps the latest layout and records it; a reader of stored content sets the layout
    /// the content records, and a terrain recipe saved before the field existed is TerrainNoiseLayout::CubicFade.
    TerrainNoiseLayout Layout = LATEST_TERRAIN_NOISE_LAYOUT;
};

/// @brief Read a stored noise layout version.
///
/// @param version The stored integer.
/// @param outLayout Receives the layout; unchanged on failure.
/// @return Whether version names a layout this build implements.
[[nodiscard]] bool TryGetTerrainNoiseLayout(uint32 version, TerrainNoiseLayout& outLayout) noexcept;

/// @brief Whether a shape can sample every position within halfExtentMetres of the origin.
///
/// Rejects unknown layouts, nonpositive feature sizes, reversed elevation bounds, non-finite values and extents whose
/// highest octave would address a lattice point beyond MAX_TERRAIN_LATTICE_POSITION.
/// @param shape The controls to check.
/// @param halfExtentMetres Largest absolute X or Z coordinate that will be sampled; nonnegative.
[[nodiscard]] bool CanSampleTerrainNoise(const TerrainNoiseShape& shape, float64 halfExtentMetres) noexcept;

/// @brief One octave of smooth gradient noise at a lattice-space position, in [-1, 1].
///
/// The value is zero at every lattice point. Each layout reads the same gradients at the same lattice addresses,
/// so layouts agree wherever both fade weights are 0, 1/2 or 1.
/// @param key Key of the terrain's random domain.
/// @param layout Noise layout; selects the fade. A value outside TerrainNoiseLayout uses the cubic fade.
/// @param octave Octave ordinal; it selects the address event.
/// @param positionX Lattice-space X; its floor must lie within the v1 address range.
/// @param positionZ Lattice-space Z; its floor must lie within the v1 address range.
[[nodiscard]] float64 SampleTerrainNoise(foundation::math::RandomKey key,
                                         TerrainNoiseLayout layout,
                                         uint32 octave,
                                         float64 positionX,
                                         float64 positionZ) noexcept;

/// @brief Elevation in metres at a terrain-space position, within the shape's elevation bounds.
///
/// @pre CanSampleTerrainNoise(shape, max(|positionX|, |positionZ|)) is true.
[[nodiscard]] float64 ComputeTerrainElevation(const TerrainNoiseShape& shape,
                                              foundation::math::RandomKey key,
                                              float64 positionX,
                                              float64 positionZ) noexcept;
} // namespace ludus::content::terrain
