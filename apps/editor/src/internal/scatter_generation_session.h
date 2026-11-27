#pragma once

// The Editor's staged scatter generation session over World-demo rooms (Generation P1).
// It owns the author's overrides (locks), one pending candidate and the accepted
// manifest, and runs ProceduralEnvironment's ScatterGenerationCoordinator over an
// in-process product cache. A publication is refused while the authored input differs
// from the one the candidate was staged from; the refusal reports each override
// conflict by placement and property, and keeping the authored values restages and
// publishes. The widget is WorldScatterPanel. Contract:
// docs/architecture/procedural-content-generation.md#p1-staged-scatter-generation.

#include <ludus/foundation/base/core.h>

#include <ludus/content/procedural/core/generation_cache.h>
#include <ludus/content/procedural/core/generation_trace.h>
#include <ludus/content/procedural/environment/scatter_generation.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

#include "internal/level.h"
#include "internal/room_catalog.h"
#include "internal/room_scatter.h"

namespace ludus::editor
{
/// Most rooms one session generates: every room of the World demo's room catalog.
inline constexpr foundation::usize MAX_SCATTER_SESSION_ROOMS = world_demo::ROOM_CATALOG_COUNT;

/// @brief Staged generation of World-demo room scatter with author overrides, for one Editor World workspace.
///
/// The accepted manifest changes only through Publish or RetryWithAuthoredValues; staging, editing an override and
/// rejecting never change it. Overrides and the accepted manifest are session state: they are not saved with the
/// World. Single-owner; GUI thread only.
class ScatterGenerationSession final
{
public:
    /// An empty session: no rooms, no overrides, an empty accepted manifest at revision zero.
    ScatterGenerationSession() noexcept;
    ScatterGenerationSession(const ScatterGenerationSession&) = delete;
    ScatterGenerationSession& operator=(const ScatterGenerationSession&) = delete;

    /// @brief Replace the generated rooms with the levels' scatter recipes, in ascending room order.
    ///
    /// Levels without scatter are skipped. A pending candidate is kept; it is stale whenever a recipe changed.
    /// @param levels The rooms' levels, each in the room catalog at most once.
    /// @param outFailedLevel On failure, the index of the first level whose recipe could not be built or that
    ///        repeats a room; unchanged on success.
    /// @return Whether every level built; the rooms are unchanged on failure.
    [[nodiscard]] bool TrySetRooms(foundation::BorrowedView<const world_demo::Level> levels,
                                   foundation::usize& outFailedLevel) noexcept;
    /// Number of generated rooms.
    [[nodiscard]] LUDUS_INLINE foundation::usize GetRoomCount() const noexcept
    {
        return mRoomCount;
    }
    /// The `index`th room's recipe, in ascending room order; index below GetRoomCount.
    [[nodiscard]] const content::procedural::ScatterRecipe& GetRecipe(foundation::usize index) const noexcept;
    /// The `index`th room's level; index below GetRoomCount.
    [[nodiscard]] const world_demo::Level& GetLevel(foundation::usize index) const noexcept;
    /// The layer of a generated room, or null when the session has no such room or layer.
    [[nodiscard]] const content::procedural::ScatterLayer* FindLayerOrNull(foundation::uint16 room,
                                                                           foundation::uint8 layer) const noexcept;

    /// @brief Lock a placement at the given values, replacing an existing override of the same identity.
    /// @return False, with the overrides unchanged, when the identity's room is not generated, the room already has
    ///         content::procedural::MAX_SCATTER_SCOPE_LOCKS overrides, or allocation fails.
    [[nodiscard]] bool TrySetOverride(const content::procedural::ScatterPlacement& placement) noexcept;
    /// Remove the override of an identity; returns whether there was one.
    bool RemoveOverride(content::procedural::ScatterIdentity identity) noexcept;
    /// The override of an identity, or null.
    [[nodiscard]] const content::procedural::ScatterPlacement*
    FindOverrideOrNull(content::procedural::ScatterIdentity identity) const noexcept;
    /// Every override, in the order they were first set.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const content::procedural::ScatterPlacement>
    GetOverrides() const noexcept
    {
        return mOverrides.AsView();
    }

    /// @brief Stage a candidate from the current rooms and overrides, replacing any pending candidate.
    ///
    /// Records the stage trace and the candidate's changes against the accepted manifest, and clears the override
    /// conflicts of an earlier refusal.
    /// @return The candidate's status: Success; ConstraintFailed when an override cannot be honoured (see
    ///         GetCandidate().Conflicts); or the coordinator's InvalidInput, CapacityExceeded, OutOfMemory or
    ///         SearchExhausted, with no pending candidate.
    [[nodiscard]] content::procedural::GenerationStatus Stage() noexcept;
    /// @brief Publish the pending candidate as the accepted manifest.
    ///
    /// A candidate staged from another authored input, such as an override edited while it was pending, is refused
    /// as StaleCandidate. The pending candidate is then kept and GetOverrideConflicts names each edit by placement and
    /// property; the accepted manifest and revision are unchanged on every failure.
    /// @return Success; NotReady without a publishable candidate; StaleCandidate; InvalidInput; or OutOfMemory.
    [[nodiscard]] content::procedural::GenerationStatus Publish() noexcept;
    /// @brief Keep the authored values: stage the current input again and publish it when it stages successfully.
    ///
    /// Only the layers whose input changed run again; every other layer is a cache hit in GetTrace.
    /// @return Publish's status after a successful stage; otherwise Stage's status, with the new candidate pending.
    [[nodiscard]] content::procedural::GenerationStatus RetryWithAuthoredValues() noexcept;
    /// Discard the pending candidate and its conflicts; the accepted manifest is unchanged.
    void Reject() noexcept;

    /// Whether a candidate with a manifest is pending: its status is Success or ConstraintFailed.
    [[nodiscard]] bool HasPendingCandidate() const noexcept;
    /// The pending candidate, or an empty NotReady candidate.
    [[nodiscard]] LUDUS_INLINE const content::procedural::ScatterCandidate& GetCandidate() const noexcept
    {
        return mCandidate;
    }
    /// The accepted manifest.
    [[nodiscard]] LUDUS_INLINE const content::procedural::ScatterManifest& GetAccepted() const noexcept
    {
        return mCoordinator.GetAccepted();
    }
    /// Revision of the accepted manifest; zero before the first publication.
    [[nodiscard]] LUDUS_INLINE foundation::uint64 GetAcceptedRevision() const noexcept
    {
        return mCoordinator.GetAcceptedRevision();
    }
    /// The pending candidate's differences from the manifest accepted when it was staged.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const content::procedural::ScatterChange>
    GetChanges() const noexcept
    {
        return mChanges.AsView();
    }
    /// The override conflicts of the last refused publication; empty after a stage, rejection or publication.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const content::procedural::ScatterOverrideConflict>
    GetOverrideConflicts() const noexcept
    {
        return mOverrideConflicts.AsView();
    }
    /// The layer evaluations of the last stage, in scope then layer order.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const content::procedural::GenerationStageRecord>
    GetTrace() const noexcept
    {
        return mTrace.GetRecords();
    }

private:
    [[nodiscard]] content::procedural::GenerationStatus tryBuildInput() noexcept;
    [[nodiscard]] foundation::usize findRoomIndex(foundation::uint16 room) const noexcept;

    world_demo::Level mLevels[MAX_SCATTER_SESSION_ROOMS] = {};
    world_demo::RoomScatterRecipe mRooms[MAX_SCATTER_SESSION_ROOMS];
    foundation::usize mRoomCount = 0;
    foundation::Array<content::procedural::ScatterPlacement> mOverrides;
    foundation::Array<content::procedural::ScatterPlacement> mScopeLocks[MAX_SCATTER_SESSION_ROOMS];
    content::procedural::ScatterScope mScopes[MAX_SCATTER_SESSION_ROOMS] = {};
    content::procedural::ScatterGenerationInput mInput;
    content::procedural::MemoryGenerationCacheStore mStore;
    content::procedural::ScatterGenerationCoordinator mCoordinator;
    content::procedural::GenerationTrace mTrace;
    content::procedural::ScatterCandidate mCandidate;
    foundation::Array<content::procedural::ScatterChange> mChanges;
    foundation::Array<content::procedural::ScatterOverrideConflict> mOverrideConflicts;
};
} // namespace ludus::editor
