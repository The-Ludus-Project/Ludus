#pragma once

/// @file
/// @brief Contact generation between two colliders: candidate pairs, contact points, features and reduced patches.
///
/// See "Contact generation" in docs/architecture/physics-collision.md and "Collision pipeline and contact
/// robustness" in docs/architecture/physics-systems.md. A contact query is pure geometry: it keeps no state between
/// calls, and persistence across ticks belongs to the rigid world. World-space values carry the suffix `World`.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>

namespace ludus::physics::collision
{
/// Most contact points one patch keeps after reduction: enough to hold a resting face in place.
inline constexpr uint32 MAX_CONTACT_PATCH_POINTS = 4;
/// @brief Most patches one collider pair keeps.
///
/// Two convex shapes always make one patch. A convex shape against a triangle mesh makes one patch per group of
/// compatible normals; when a pair finds more groups than this, the patches with the deepest points are kept and
/// ContactPairContacts::DiscardedPatchCount counts the rest.
inline constexpr uint32 MAX_CONTACT_PAIR_PATCHES = 8;
/// Most contact points one collider pair reports.
inline constexpr uint32 MAX_CONTACT_PAIR_POINTS = MAX_CONTACT_PAIR_PATCHES * MAX_CONTACT_PATCH_POINTS;
/// Default of ContactSettings::ContactOffsetMeters: surfaces within two centimeters make a contact.
inline constexpr float64 DEFAULT_CONTACT_OFFSET_METERS = 0.02;
/// Largest admitted contact offset, meters.
inline constexpr float64 MAX_CONTACT_OFFSET_METERS = 1.0;
/// @brief Cosine of the largest angle between two normals that still belong to one patch: five degrees.
///
/// Mesh triangles whose contact normals agree this closely share a patch, so a flat floor of many triangles
/// supports a box with one four-point patch. Normals further apart are never merged or averaged.
inline constexpr float64 CONTACT_PATCH_NORMAL_COSINE = 0.99619469809174555;

/// @brief Kind of the shape feature a contact point lies on.
enum class ContactFeatureKind : uint8
{
    Vertex, ///< A corner, a hull point, a capsule end or a sphere.
    Edge,   ///< An edge of a box, hull or triangle, or a capsule's segment.
    Face,   ///< A face of a box or hull, or a triangle's interior.
};

/// @brief A feature of one shape: a ContactFeatureKind and an index, packed into one comparable value.
///
/// Feature values name the same geometry on every tick, so equal values on both sides of a pair identify the same
/// contact point. Build and read them with MakeContactFeature, GetContactFeatureKind and GetContactFeatureIndex.
///
/// | Shape | Vertex index | Edge index | Face index |
/// | --- | --- | --- | --- |
/// | Sphere | 0 | none | none |
/// | Capsule | 0 for the -Y end, 1 for the +Y end | 0, the segment | none |
/// | Box | bit 0, 1, 2 set for the +X, +Y, +Z side | `axis * 4 + s`: the edge along `axis`, where bit 0 and bit 1 of
/// `s` are set for the positive side of the next and the second next axis | `axis * 2`, plus 1 for the positive side |
/// | Hull | the point's index in the hull | the two point indices, the lower in bits 0 to 7 | the three lowest point
/// indices of the face, the lowest in bits 0 to 7 | | Triangle | the corner, 0 to 2 | the edge leaving that corner, 0
/// to 2 | 0 |
using ContactFeature = uint32;

/// Bit position of the kind in a ContactFeature; the index occupies the bits below it.
inline constexpr uint32 CONTACT_FEATURE_KIND_SHIFT = 24;
/// Mask of the index bits of a ContactFeature.
inline constexpr uint32 CONTACT_FEATURE_INDEX_MASK = (1U << CONTACT_FEATURE_KIND_SHIFT) - 1U;

/// @brief Pack a feature kind and an index below 2^24.
[[nodiscard]] constexpr ContactFeature MakeContactFeature(ContactFeatureKind kind, uint32 index) noexcept
{
    return (static_cast<uint32>(kind) << CONTACT_FEATURE_KIND_SHIFT) | (index & CONTACT_FEATURE_INDEX_MASK);
}

/// @brief The kind of a packed feature.
[[nodiscard]] constexpr ContactFeatureKind GetContactFeatureKind(ContactFeature feature) noexcept
{
    return static_cast<ContactFeatureKind>(feature >> CONTACT_FEATURE_KIND_SHIFT);
}

/// @brief The index of a packed feature; see ContactFeature for its meaning per shape.
[[nodiscard]] constexpr uint32 GetContactFeatureIndex(ContactFeature feature) noexcept
{
    return feature & CONTACT_FEATURE_INDEX_MASK;
}

/// @brief Tuning of one contact query.
struct ContactSettings final
{
    /// Largest gap between two surfaces that still makes a contact point, in [0, MAX_CONTACT_OFFSET_METERS].
    /// A positive offset reports contacts slightly before the surfaces touch, so a resting pair keeps its points
    /// while it jitters.
    float64 ContactOffsetMeters = DEFAULT_CONTACT_OFFSET_METERS;
};

/// @brief One contact point between two colliders.
struct ContactPoint final
{
    /// Midpoint between the two surface points, meters.
    foundation::math::Vector3d PositionWorld{};
    /// Signed gap between the surfaces along the patch normal, meters: negative while the shapes interpenetrate,
    /// positive up to the contact offset while they are apart.
    float64 SeparationMeters = 0.0;
    ContactFeature FirstFeature = 0;  ///< Feature of the first collider's shape the point lies on.
    ContactFeature SecondFeature = 0; ///< Feature of the second collider's shape the point lies on.
    /// Source triangle of the pair's mesh collider the point belongs to; INVALID_TRIANGLE_INDEX for two convex
    /// colliders. Together with the two features it identifies the point across ticks.
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX;
};

/// @brief A group of contact points that share one normal.
struct ContactPatch final
{
    /// Unit normal from the first collider toward the second: moving the second along it separates the pair.
    foundation::math::Vector3d NormalWorld{};
    uint32 FirstPoint = 0; ///< Index of the patch's first point in ContactPairContacts::Points.
    uint32 PointCount = 0; ///< Points of the patch, in [1, MAX_CONTACT_PATCH_POINTS].
    /// The penetration search stopped at an iteration or storage bound, so the normal and depths are a lower bound
    /// of the true penetration rather than its exact value.
    bool IsApproximate = false;
};

/// @brief The contacts of one collider pair.
///
/// Patches come first found; a patch's points are consecutive in Points and keep the order they were generated in.
struct ContactPairContacts final
{
    foundation::StaticArray<ContactPatch, MAX_CONTACT_PAIR_PATCHES> Patches{}; ///< The first PatchCount are valid.
    foundation::StaticArray<ContactPoint, MAX_CONTACT_PAIR_POINTS> Points{};   ///< The first PointCount are valid.
    uint32 PatchCount = 0;                                                     ///< Patches kept.
    uint32 PointCount = 0;                                                     ///< Points kept, after reduction.
    uint32 GeneratedPointCount = 0;                                            ///< Points found before reduction.
    uint32 TriangleTestCount = 0;     ///< Mesh triangles tested against the convex shape.
    uint32 ApproximatePatchCount = 0; ///< Kept patches with IsApproximate set.
    /// Patches found beyond MAX_CONTACT_PAIR_PATCHES and dropped, shallowest first.
    uint32 DiscardedPatchCount = 0;
};
} // namespace ludus::physics::collision
