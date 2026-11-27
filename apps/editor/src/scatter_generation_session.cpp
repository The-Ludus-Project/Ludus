#include "internal/scatter_generation_session.h"

#include "internal/world.h"

namespace ludus::editor
{
namespace
{
namespace procedural = content::procedural;
using foundation::BorrowedView;
using foundation::usize;
using procedural::GenerationStatus;
/// Product cache budget of one session. A refused write only costs a later cache miss, never a result.
/// Most cached layer products, far above the products of many edits to every room's layers.
constexpr usize SESSION_CACHE_ENTRIES = 1024;
/// Most cached product bytes: 16 MiB of 24-byte placement records.
constexpr usize SESSION_CACHE_BYTES = usize{1} << 24U; // ludus-style: magic-number 2^24 bytes is the 16 MiB budget
constexpr procedural::GenerationCacheLimits SESSION_CACHE_LIMITS{SESSION_CACHE_ENTRIES, SESSION_CACHE_BYTES};
/// Trace records of one stage: every layer of every room.
constexpr usize SESSION_TRACE_CAPACITY = MAX_SCATTER_SESSION_ROOMS * world_demo::MAX_ROOM_SCATTER_LAYERS;
} // namespace

ScatterGenerationSession::ScatterGenerationSession() noexcept
    : mStore(SESSION_CACHE_LIMITS), mCoordinator(mStore), mTrace(SESSION_TRACE_CAPACITY)
{
}

bool ScatterGenerationSession::TrySetRooms(BorrowedView<const world_demo::Level> levels, usize& outFailedLevel) noexcept
{
    // Validate every level first, so a failure leaves the rooms unchanged.
    usize order[MAX_SCATTER_SESSION_ROOMS] = {};
    foundation::uint16 rooms[MAX_SCATTER_SESSION_ROOMS] = {};
    usize count = 0;
    for (usize index = 0; index < levels.GetSize(); ++index)
    {
        if (levels[index].HasScatter == false)
        {
            continue;
        }
        world_demo::RoomScatterRecipe check;
        if (world_demo::TryBuildRoomScatterRecipe(levels[index], check) != gameplay::world::Status::Success ||
            count == MAX_SCATTER_SESSION_ROOMS)
        {
            outFailedLevel = index;
            return false;
        }
        // Insert in ascending room order; a repeated room is a failure.
        usize position = count;
        for (usize prior = 0; prior < count; ++prior)
        {
            if (rooms[prior] == check.Recipe.Room)
            {
                outFailedLevel = index;
                return false;
            }
            if (rooms[prior] > check.Recipe.Room && position == count)
            {
                position = prior;
            }
        }
        for (usize later = count; later > position; --later)
        {
            rooms[later] = rooms[later - 1];
            order[later] = order[later - 1];
        }
        rooms[position] = check.Recipe.Room;
        order[position] = index;
        ++count;
    }
    for (usize index = 0; index < count; ++index)
    {
        mLevels[index] = levels[order[index]];
        // The level was built above, so this cannot fail.
        [[maybe_unused]] const gameplay::world::Status status =
            world_demo::TryBuildRoomScatterRecipe(mLevels[index], mRooms[index]);
        LUDUS_ASSERT(status == gameplay::world::Status::Success);
    }
    mRoomCount = count;
    return true;
}

const procedural::ScatterRecipe& ScatterGenerationSession::GetRecipe(usize index) const noexcept
{
    LUDUS_ASSERT(index < mRoomCount);
    return mRooms[index].Recipe;
}

const world_demo::Level& ScatterGenerationSession::GetLevel(usize index) const noexcept
{
    LUDUS_ASSERT(index < mRoomCount);
    return mLevels[index];
}

usize ScatterGenerationSession::findRoomIndex(foundation::uint16 room) const noexcept
{
    for (usize index = 0; index < mRoomCount; ++index)
    {
        if (mRooms[index].Recipe.Room == room)
        {
            return index;
        }
    }
    return mRoomCount;
}

const procedural::ScatterLayer* ScatterGenerationSession::FindLayerOrNull(foundation::uint16 room,
                                                                          foundation::uint8 layer) const noexcept
{
    const usize index = findRoomIndex(room);
    if (index == mRoomCount)
    {
        return nullptr;
    }
    for (const procedural::ScatterLayer& candidate : mRooms[index].Recipe.Layers)
    {
        if (candidate.Id == layer)
        {
            return &candidate;
        }
    }
    return nullptr;
}

bool ScatterGenerationSession::TrySetOverride(const procedural::ScatterPlacement& placement) noexcept
{
    if (findRoomIndex(placement.Identity.Room) == mRoomCount)
    {
        return false;
    }
    usize roomOverrides = 0;
    for (procedural::ScatterPlacement& existing : mOverrides)
    {
        if (existing.Identity == placement.Identity)
        {
            existing = placement;
            return true;
        }
        roomOverrides += existing.Identity.Room == placement.Identity.Room ? 1 : 0;
    }
    if (roomOverrides == procedural::MAX_SCATTER_SCOPE_LOCKS)
    {
        return false;
    }
    return mOverrides.TryAdd(placement);
}

bool ScatterGenerationSession::RemoveOverride(procedural::ScatterIdentity identity) noexcept
{
    for (usize index = 0; index < mOverrides.GetSize(); ++index)
    {
        if (mOverrides[index].Identity == identity)
        {
            mOverrides.RemoveAt(index);
            return true;
        }
    }
    return false;
}

const procedural::ScatterPlacement*
ScatterGenerationSession::FindOverrideOrNull(procedural::ScatterIdentity identity) const noexcept
{
    for (const procedural::ScatterPlacement& existing : mOverrides)
    {
        if (existing.Identity == identity)
        {
            return &existing;
        }
    }
    return nullptr;
}

GenerationStatus ScatterGenerationSession::tryBuildInput() noexcept
{
    if (mRoomCount == 0)
    {
        return GenerationStatus::InvalidInput;
    }
    for (usize index = 0; index < mRoomCount; ++index)
    {
        foundation::Array<procedural::ScatterPlacement>& locks = mScopeLocks[index];
        locks.Clear();
        for (const procedural::ScatterPlacement& placement : mOverrides)
        {
            if (placement.Identity.Room == mRooms[index].Recipe.Room && locks.TryAdd(placement) == false)
            {
                return GenerationStatus::OutOfMemory;
            }
        }
        mScopes[index] =
        {
            .Recipe = mRooms[index].Recipe,
            .Locks = BorrowedView<const procedural::ScatterPlacement>(locks.GetData(), locks.GetSize()),
        };
    }
    mInput =
    {
        .WorldSeed = world_demo::WorldSeed{}.Value,
        .Domains = world_demo::WORLD_SCATTER_DOMAINS,
        .Scopes = BorrowedView<const procedural::ScatterScope>(mScopes, mRoomCount),
    };
    return GenerationStatus::Success;
}

GenerationStatus ScatterGenerationSession::Stage() noexcept
{
    mOverrideConflicts.Clear();
    mChanges.Clear();
    mTrace.Clear();
    const GenerationStatus input = tryBuildInput();
    if (input != GenerationStatus::Success)
    {
        mCoordinator.Reject(mCandidate);
        return input;
    }
    const GenerationStatus status = mCoordinator.TryStage(mInput, mTrace, mCandidate);
    if (HasPendingCandidate() == true &&
        procedural::TryCompareScatterManifests(GetAccepted(), mCandidate.Manifest, mInput, mChanges) !=
            GenerationStatus::Success)
    {
        // The comparison only explains the candidate; without it the candidate is still pending.
        mChanges.Clear();
    }
    return status;
}

GenerationStatus ScatterGenerationSession::Publish() noexcept
{
    mOverrideConflicts.Clear();
    if (HasPendingCandidate() == false)
    {
        return GenerationStatus::NotReady;
    }
    const GenerationStatus input = tryBuildInput();
    if (input != GenerationStatus::Success)
    {
        return input;
    }
    const GenerationStatus status = mCoordinator.TryPublish(mInput, mCandidate);
    if (status == GenerationStatus::Success)
    {
        mChanges.Clear();
    }
    else if (status == GenerationStatus::StaleCandidate &&
             procedural::TryFindScatterOverrideConflicts(mCandidate, mInput, mOverrideConflicts) !=
                 GenerationStatus::Success)
    {
        mOverrideConflicts.Clear();
    }
    return status;
}

GenerationStatus ScatterGenerationSession::RetryWithAuthoredValues() noexcept
{
    const GenerationStatus status = Stage();
    if (status != GenerationStatus::Success)
    {
        return status;
    }
    return Publish();
}

void ScatterGenerationSession::Reject() noexcept
{
    mCoordinator.Reject(mCandidate);
    mChanges.Clear();
    mOverrideConflicts.Clear();
}

bool ScatterGenerationSession::HasPendingCandidate() const noexcept
{
    return mCandidate.Status == GenerationStatus::Success || mCandidate.Status == GenerationStatus::ConstraintFailed;
}
} // namespace ludus::editor
