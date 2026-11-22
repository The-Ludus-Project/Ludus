#pragma once

/// @file
/// @brief Contacts of a RigidWorld: capacities, the plain-value contact export, cache retirement reasons and
/// bounded diagnostics.
///
/// See "Contacts" in docs/architecture/physics-rigid.md and "Collision pipeline and contact robustness" in
/// docs/architecture/physics-systems.md. Contact geometry comes from collision::CollisionReadView::GenerateContacts;
/// this header describes what the rigid world keeps of it from tick to tick.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/contacts.h>
#include <ludus/physics/rigid/body.h>
#include <ludus/physics/rigid/status.h>

namespace ludus::physics::rigid
{
/// Largest number of contact pairs a world admits.
inline constexpr uint32 MAX_RIGID_WORLD_CONTACT_PAIRS = 1U << 22U;
/// Largest number of contact points a world admits.
inline constexpr uint32 MAX_RIGID_WORLD_CONTACT_POINTS = 1U << 24U;

/// @brief A contact table whose reserved capacity one tick's contacts can exceed.
enum class ContactTable : uint8
{
    None,   ///< No table was exhausted.
    Pairs,  ///< Collider pairs that hold at least one contact point; reserved by MaxContactPairs.
    Points, ///< Contact points after reduction; reserved by MaxContactPoints.
};

/// @brief Why a cached contact point of the previous tick was not carried into the current one.
///
/// A cached point is carried over only while the pair, its geometry, its materials, the world's contact settings,
/// both bodies' teleport history, the point's features and the patch normal all still agree. Each cached point is
/// counted under exactly one reason, the first that applies in the order below.
enum class ContactRetirement : uint8
{
    /// The pair no longer has contacts: it moved apart, or a collider was destroyed, disabled or filtered out.
    Separated,
    GeometryChanged, ///< A collider of the pair now uses a different shape.
    MaterialChanged, ///< A collider of the pair now has a different material identifier.
    SettingsChanged, ///< The world's contact settings changed since the point was cached.
    Teleported,      ///< A body of the pair was teleported since the point was cached.
    /// The pair still touches, but no current point has this point's features: the contact moved to other
    /// vertices, edges, faces or triangles.
    FeatureChanged,
    /// A current point has the same features, but its patch normal differs from the cached one by more than
    /// collision::CONTACT_PATCH_NORMAL_COSINE allows.
    NormalChanged,
};

/// Number of ContactRetirement values.
inline constexpr usize CONTACT_RETIREMENT_COUNT = 7;

/// @brief Bounded diagnostics of the last contact detection, successful or not.
///
/// The detection counts describe what the last Step found, including a Step that failed on capacity. The cache
/// counts are zero for such a failed Step, which matches nothing: the cache it would have matched against is still
/// in place for the next attempt.
struct ContactDiagnostics final
{
    bool HasDetected = false; ///< Whether any Step has run contact detection.
    uint64 Tick = 0;          ///< The tick the last detection ran for.
    /// Collider pairs whose widened bounds overlap and whose filters, flags and solidity admit them, counted once
    /// per dynamic collider that found them, before bodies are considered.
    usize CandidatePairCount = 0;
    /// Pairs sent to contact generation: candidates on different bodies with at least one dynamic body, each once.
    usize TestedPairCount = 0;
    usize UnsupportedPairCount = 0;  ///< Tested pairs of two triangle meshes, which generate no contacts.
    usize PairCount = 0;             ///< Pairs with at least one contact point.
    usize PatchCount = 0;            ///< Contact patches over all pairs.
    usize GeneratedPointCount = 0;   ///< Contact points found before reduction.
    usize PointCount = 0;            ///< Contact points after reduction.
    usize TriangleTestCount = 0;     ///< Mesh triangles tested.
    usize ApproximatePatchCount = 0; ///< Patches whose penetration search stopped at a bound.
    usize DiscardedPatchCount = 0;   ///< Patches dropped beyond collision::MAX_CONTACT_PAIR_PATCHES.
    usize PersistedPointCount = 0;   ///< Points matched to a cached point of the previous tick: cache hits.
    usize NewPointCount = 0;         ///< Points with no usable cached point.
    /// Cached points of the previous tick that were not carried over, by ContactRetirement value.
    foundation::StaticArray<usize, CONTACT_RETIREMENT_COUNT> RetiredPointCounts{};
    usize PairHighWater = 0;  ///< Largest PairCount of any detection since initialization.
    usize PointHighWater = 0; ///< Largest PointCount of any detection since initialization.
    /// The table the last detection exhausted, or None. Pairs are checked before points.
    ContactTable ExhaustedTable = ContactTable::None;
    usize RequiredCount = 0; ///< Entries the exhausted table would have needed; zero when none was exhausted.

    /// Cached points retired for one reason by the last detection.
    [[nodiscard]] constexpr usize GetRetiredPointCount(ContactRetirement reason) const noexcept
    {
        return RetiredPointCounts[static_cast<usize>(reason)];
    }
};

/// @brief Plain-value state of one contact point of the last completed tick.
///
/// Contacts are detected at the start of a tick, from the poses the previous tick published. The first collider
/// of a pair is the one with the lower collider slot.
struct ContactPointState final
{
    BodyHandle FirstBody{};                     ///< Body of the first collider.
    BodyHandle SecondBody{};                    ///< Body of the second collider.
    collision::ColliderHandle FirstCollider{};  ///< First collider of the pair.
    collision::ColliderHandle SecondCollider{}; ///< Second collider of the pair.
    /// Index of the point's patch among all patches of the export: points of one patch are consecutive and share
    /// NormalWorld.
    uint32 PatchIndex = 0;
    foundation::math::Vector3d NormalWorld{};    ///< Unit normal from the first collider toward the second.
    foundation::math::Vector3d PositionWorld{};  ///< Midpoint between the two surface points, meters.
    float64 SeparationMeters = 0.0;              ///< Signed gap along the normal; negative while interpenetrating.
    collision::ContactFeature FirstFeature = 0;  ///< Feature of the first collider's shape.
    collision::ContactFeature SecondFeature = 0; ///< Feature of the second collider's shape.
    uint32 TriangleIndex = collision::INVALID_TRIANGLE_INDEX; ///< Source triangle of a mesh contact.
    /// Consecutive earlier ticks in which this point was matched to its cached predecessor; zero for a new point.
    uint32 PersistedTickCount = 0;
    bool IsApproximate = false; ///< The point's patch has collision::ContactPatch::IsApproximate set.
};

/// @brief Completion of a contact export.
///
/// Points are written in pair order (first collider slot, then second), then patch, then point, until the buffer
/// is full. IsComplete is true exactly when every point was written.
struct ContactExportResult final
{
    usize PointCount = 0;   ///< Contact points of the last completed tick.
    usize WrittenCount = 0; ///< Records written, a prefix of the buffer.
    bool IsComplete = true; ///< Whether WrittenCount equals PointCount.
    usize PairCount = 0;    ///< Pairs with contacts.
    usize PatchCount = 0;   ///< Patches over all pairs.
    uint64 Tick = 0;        ///< The tick the contacts were detected for; meaningful once a tick has completed.
};
} // namespace ludus::physics::rigid
