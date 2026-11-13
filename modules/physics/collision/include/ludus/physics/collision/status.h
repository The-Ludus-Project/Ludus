#pragma once

/// @file
/// @brief Result vocabulary and numeric envelope shared by PhysicsCollision cooking and queries.
///
/// See docs/architecture/physics-collision.md for the contracts these values belong to.

#include <ludus/foundation/base/types.h>

namespace ludus::physics::collision
{
using foundation::float32;
using foundation::float64;
using foundation::uint32;
using foundation::uint8;
using foundation::usize;

/// @brief Outcome of a PhysicsCollision cooking or query operation.
///
/// Every failing value leaves the caller's outputs unchanged. A query that completes and finds nothing returns
/// `Success` with an explicit miss in its result; a miss is never reported as a failure.
enum class Status : uint8
{
    Success,            ///< The operation completed; its outputs were written.
    InvalidArgument,    ///< A view, count, enumerator, direction or extent violates the documented contract.
    NonFiniteInput,     ///< A coordinate, direction, distance or extent is NaN or infinite.
    OutOfRange,         ///< A finite value lies outside the documented coordinate or query envelope.
    IndexOutOfRange,    ///< A triangle index refers past the end of the borrowed positions.
    DegenerateTriangle, ///< A triangle has no usable area and the cook settings reject degenerate triangles.
    NoGeometry,         ///< The source holds no admissible triangle, or the queried mesh was never cooked.
    CapacityExceeded,   ///< The source exceeds a named admission limit.
    AllocationFailure,  ///< Storage for a new mesh could not be allocated.
    InitialOverlap,     ///< A cast with the reject initial-overlap policy started overlapping a triangle.
    DegenerateShape,    ///< A convex hull source has fewer than four points or encloses no usable volume.
    NotConverged,       ///< A convex cast exhausted MAX_CONVEX_CAST_ITERATIONS on one triangle without converging.
};

/// Largest magnitude, in meters, of any mesh vertex coordinate or query origin/center component.
///
/// float32 vertices keep about 7.8 mm spacing at this magnitude; larger scenes need local mesh frames.
inline constexpr float32 MAX_COORDINATE_METERS = 1.0e5f;

/// Largest ray distance, shape dimension or cast translation component, in meters, that a query admits.
///
/// The bound keeps every separating-axis product finite in float64 for admitted mesh coordinates.
inline constexpr float64 MAX_QUERY_DISTANCE_METERS = 1.0e6;

/// Sentinel for "no triangle": reported when a cook failure is not attributable to one triangle, or by a miss.
inline constexpr uint32 INVALID_TRIANGLE_INDEX = 0xFFFFFFFFU;
} // namespace ludus::physics::collision
