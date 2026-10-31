#pragma once

// Inline pbrt shapes to IR meshes: trianglemesh, bilinearmesh, loopsubdiv
// (base cage), and tessellated disk and sphere.

#include "../scene_ir.h"

#include <ludus/content/content.h>

#include <string_view>

namespace ludus::content::scene_cook::importers
{
/// Segments around a tessellated disk or sphere.
inline constexpr uint32 TESSELLATION_SEGMENTS = 48;
/// Rings from pole to pole of a tessellated sphere.
inline constexpr uint32 TESSELLATION_RINGS = 24;

/// Raw numeric parameters of an inline shape.
struct ShapeArguments final
{
    Array<float64> Positions;
    Array<float64> Normals;
    Array<float64> Uvs;
    Array<float64> Indices;
    float64 Radius = 1.0;
    float64 InnerRadius = 0.0;
    float64 Height = 0.0;
    /// Sphere clipping heights, clamped to [-radius, radius]; the defaults keep the full sphere.
    float64 ZMin = -1.0e300;
    float64 ZMax = 1.0e300;
    /// Sweep of disks and spheres, in degrees.
    float64 PhiMaxDegrees = 360.0;
};

/// Outcome of TessellateShape.
enum class ShapeResult : uint8
{
    Ok,
    /// Converted with loss (loopsubdiv base cage).
    Approximated,
    /// The shape type is not converted.
    Unsupported,
    /// Malformed arguments; outMessage explains.
    Invalid,
    OutOfMemory,
};

/// Supply PBRT's implicit per-triangle UVs when none were authored, splitting
/// corners so shared vertices can have different UVs. Preserves triangle order,
/// positions, normals and authored vertex statistics; failure preserves the mesh.
/// @return Ok, Invalid for malformed/non-triangle input, or OutOfMemory.
[[nodiscard]] Status ApplyDefaultTriangleUvs(IrMesh& inoutMesh) noexcept;

/// Build outMesh for shape type with arguments; uv v is flipped to top-down.
[[nodiscard]] ShapeResult TessellateShape(std::string_view type,
                                          const ShapeArguments& arguments,
                                          IrMesh& outMesh,
                                          std::string_view& outMessage) noexcept;
} // namespace ludus::content::scene_cook::importers
