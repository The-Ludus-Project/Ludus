#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::editor
{
inline constexpr foundation::int32 TERRAIN_STROKE_MAX_POINTS = 64;
inline constexpr foundation::int32 TERRAIN_LAYER_MAX_POINTS = 256;
inline constexpr foundation::float64 TERRAIN_BRUSH_MIN_RADIUS_METRES = 0.25;
inline constexpr foundation::float64 TERRAIN_BRUSH_MAX_RADIUS_METRES = 64;
inline constexpr foundation::float64 TERRAIN_BRUSH_ELEVATION_LIMIT_METRES = 128;

enum class TerrainBrushKind : foundation::uint8
{
    Raise,
    Lower,
    Smooth,
    Flatten
};

struct TerrainBrush final
{
    static constexpr foundation::float64 DEFAULT_RADIUS_METRES = 4;
    TerrainBrushKind Kind = TerrainBrushKind::Raise;
    foundation::float64 RadiusMetres = DEFAULT_RADIUS_METRES;
    foundation::float64 Strength = 0.5;
    foundation::float64 TargetElevationMetres = 0;
    bool ShouldPickElevation = false;
    bool ShouldShowSlope = false;
    [[nodiscard]] bool operator==(const TerrainBrush& other) const noexcept = default;
};
} // namespace ludus::editor
