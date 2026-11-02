#pragma once

// Exact direct irradiance from a convex polygonal emitter that disjoint coplanar convex occluders partially hide, for
// the analytic shadow-ray tests (render_graph_ray_tests.cpp, "partially occluded quad light"). From a receiver point,
// central projection maps each occluder onto the emitter's plane as a convex polygon, and disjoint coplanar occluders
// project to disjoint polygons. The visible projected solid angle is therefore the emitter's minus that of each
// projected occluder clipped to the emitter, with no union to form. Each convex polygon's projected solid angle (the
// cosine-weighted solid angle, irradiance per unit radiance) is Lambert's edge formula, evaluated in float64.
// Thanks to Johann Heinrich Lambert, "Photometria", 1760, for the polygon formula, as presented by James Arvo,
// "Applications of Irradiance Tensors to the Simulation of Non-Lambertian Phenomena", SIGGRAPH 1995
// (https://doi.org/10.1145/218380.218467); and to Ivan E. Sutherland and Gary W. Hodgman, "Reentrant Polygon
// Clipping", Communications of the ACM 17(1), 1974 (https://doi.org/10.1145/360767.360802), for the clipping.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer::testing
{
/// Largest convex polygon the oracle stores, including the vertices clipping adds.
inline constexpr foundation::usize OCCLUSION_POLYGON_CAPACITY = 12;
/// No occluder covers the whole emitter.
inline constexpr foundation::usize NO_COVERING_OCCLUDER = ~foundation::usize{0};

/// A convex planar polygon with no three collinear vertices, in either winding.
struct OcclusionPolygon final
{
    foundation::StaticArray<foundation::math::Vector3d, OCCLUSION_POLYGON_CAPACITY> VerticesWorld{};
    foundation::usize VertexCount = 0; ///< Valid prefix of VerticesWorld; zero for an empty polygon.
};

/// A one-sided emitter of constant radiance and the occluders between it and the receivers.
struct OccludedPolygonLight final
{
    OcclusionPolygon Emitter;
    /// Unit normal of the emitter's plane on its emitting side.
    foundation::math::Vector3d EmittingNormalWorld{0, 0, 1};
    /// Pairwise disjoint convex polygons in one plane, which must not cross the emitter's plane. Borrowed.
    foundation::BorrowedView<const OcclusionPolygon> Occluders;
};

/// Projected solid angles of one receiver, in steradians: irradiance per unit emitted radiance.
struct VisibleProjectedSolidAngle final
{
    foundation::float64 Emitter = 0; ///< The whole emitter, unoccluded.
    foundation::float64 Visible = 0; ///< The part no occluder hides; within rounding of [0, Emitter].
    /// The occluder whose projection contains the whole emitter, which leaves the receiver in umbra, or
    /// NO_COVERING_OCCLUDER.
    foundation::usize CoveringOccluder = NO_COVERING_OCCLUDER;
};

/// Outcome of TryComputeVisibleProjectedSolidAngle.
enum class OcclusionOracleStatus : foundation::uint8
{
    Success = 0,
    InvalidInput = 1,       ///< A degenerate polygon or normal, or a nonfinite position.
    BelowHorizon = 2,       ///< Part of the emitter lies below the receiver's horizon, which the oracle does not clip.
    StraddlingOccluder = 3, ///< An occluder has vertices on both sides of the receiver or of the emitter's plane.
};

/// Lambert's formula: the projected solid angle of a convex polygon entirely above the horizon of a receiver.
/// @return The cosine-weighted solid angle in steradians, or zero for an empty polygon.
[[nodiscard]] foundation::float64 ComputeLambertProjectedSolidAngle(const OcclusionPolygon& polygon,
                                                                    foundation::math::Vector3d receiverWorld,
                                                                    foundation::math::Vector3d normalWorld) noexcept;

/// Clip a convex polygon to another convex polygon in the same plane (Sutherland and Hodgman).
/// @param outPolygon The intersection, with VertexCount zero when it is empty or has no area.
/// @return False, leaving outPolygon unchanged, for a degenerate clip polygon or an intersection beyond
/// OCCLUSION_POLYGON_CAPACITY.
[[nodiscard]] bool TryClipConvexPolygon(const OcclusionPolygon& subject,
                                        const OcclusionPolygon& clip,
                                        OcclusionPolygon& outPolygon) noexcept;

/// The exact projected solid angle of the emitter's visible part at one receiver.
/// @param normalWorld Unit receiver normal.
/// @param outSolidAngle Written only on Success. A receiver behind the emitting side sees nothing.
[[nodiscard]] OcclusionOracleStatus TryComputeVisibleProjectedSolidAngle(const OccludedPolygonLight& light,
                                                                         foundation::math::Vector3d receiverWorld,
                                                                         foundation::math::Vector3d normalWorld,
                                                                         VisibleProjectedSolidAngle& outSolidAngle);

/// Whether the open segment from start to end crosses the polygon's interior or boundary.
[[nodiscard]] bool DoesSegmentCrossPolygon(foundation::math::Vector3d startWorld,
                                           foundation::math::Vector3d endWorld,
                                           const OcclusionPolygon& polygon) noexcept;

// The partially occluded room: a wall at z = WALL_Z_WORLD facing +z, lit by a one-sided rectangular emitter in the
// ceiling plane y = EMITTER_HEIGHT_WORLD that faces down, through a louvre of parallel horizontal slats.
inline constexpr foundation::float64 WALL_Z_WORLD = -1;
inline constexpr foundation::float64 EMITTER_HEIGHT_WORLD = 1.1;

/// The emitter, counterclockwise seen from below, so its geometric normal faces -y.
[[nodiscard]] OcclusionPolygon MakeCeilingEmitter() noexcept;

/// Parallel slats of one width and pitch in the plane y = HeightWorld, clipped to an axis-aligned box of that plane.
struct LouvreDescription final
{
    foundation::float64 HeightWorld = 0;
    foundation::float64 SlatWidthWorld = 0;
    foundation::float64 PitchWorld = 0; ///< Distance between the leading edges of neighboring slats; > SlatWidthWorld.
    foundation::float64 AngleDegrees = 0; ///< Angle of the slats' long axis from +x towards +z.
    foundation::float64 MinimumXWorld = 0;
    foundation::float64 MaximumXWorld = 0;
    foundation::float64 MinimumZWorld = 0; ///< WALL_Z_WORLD puts the slats against the wall.
    foundation::float64 MaximumZWorld = 0;
};

/// Slats a little below the emitter: most of the wall sees part of the emitter through several gaps.
inline constexpr LouvreDescription NEAR_EMITTER_LOUVRE
{
    .HeightWorld = 0.95,
    .SlatWidthWorld = 0.1,
    .PitchWorld = 0.2,
    .AngleDegrees = 30,
    .MinimumXWorld = -1.6,
    .MaximumXWorld = 1.6,
    .MinimumZWorld = -0.95,
    .MaximumZWorld = 0.5,
};
/// Low slats that touch the wall: contact shadows with umbrae, and penumbrae that narrow towards the wall.
inline constexpr LouvreDescription WALL_LOUVRE
{
    .HeightWorld = 0.3,
    .SlatWidthWorld = 0.35,
    .PitchWorld = 0.5,
    .AngleDegrees = 60,
    .MinimumXWorld = -1.6,
    .MaximumXWorld = 1.6,
    .MinimumZWorld = WALL_Z_WORLD,
    .MaximumZWorld = -0.2,
};

inline constexpr foundation::usize LOUVRE_SLAT_CAPACITY = 32;

/// The slats of one louvre; each is a convex polygon with at most six vertices.
struct Louvre final
{
    foundation::StaticArray<OcclusionPolygon, LOUVRE_SLAT_CAPACITY> Slats{};
    foundation::usize SlatCount = 0;
};

/// @return False, leaving outLouvre unchanged, for an invalid description or more than LOUVRE_SLAT_CAPACITY slats.
[[nodiscard]] bool TryBuildLouvre(const LouvreDescription& description, Louvre& outLouvre) noexcept;
} // namespace ludus::graphics::renderer::testing
