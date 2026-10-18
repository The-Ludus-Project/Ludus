#include "internal/world.h"
#include "internal/random_domains.h"
#include "internal/room_catalog.h"

#include <ludus/content/procedural/environment/scatter.h>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/profiling/profiling.hpp>

#include <bit>
#include <cmath>
#include <limits>

namespace ludus::world_demo
{
namespace
{
namespace procedural = content::procedural;
using foundation::BorrowedView;
using foundation::math::RandomAddress;
using foundation::math::RandomKey;
// The room's authored limits must stay within the kernel's and the render frame's.
static_assert(MAX_ROOM_SCATTER_CANDIDATES_PER_CELL == procedural::MAX_SCATTER_CANDIDATES_PER_CELL);
static_assert(MAX_ROOM_SCATTER_LAYERS <= procedural::MAX_SCATTER_LAYERS);
static_assert(MAX_ROOM_SCATTER_VARIANTS <= procedural::MAX_SCATTER_VARIANTS);
static_assert(sizeof(Level::Entities) / sizeof(Recipe) + MAX_WORLD_COLLISION <= procedural::MAX_SCATTER_KEEP_OUTS);
static_assert(sizeof(Level::Collision) / sizeof(Box) == MAX_WORLD_COLLISION);
/// Event of every presentation sample; cosmetic draws never advance an event.
constexpr uint32 COSMETIC_EVENT = 0;
/// Cosmetic-domain dimension of the brightness tint applied to a drawn placement.
constexpr uint16 COSMETIC_TINT_DIMENSION = 0;
/// Darkest cosmetic tint; the tint lies in [COSMETIC_TINT_FLOOR, 1].
constexpr float32 COSMETIC_TINT_FLOOR = 0.8F;
/// Rotation units per turn of a generated placement (1/65536 turns).
constexpr float64 ROTATION_UNITS_PER_TURN = 65536.0;
/// FNV-1a parameters shared with the replay hash and the scatter golden digest.
constexpr uint64 FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64 FNV_PRIME = 1099511628211ULL;
constexpr usize BITS_PER_BYTE = 8;
constexpr uint64 BYTE_MASK = 0xffu;
bool Finite(Vec2 value) noexcept
{
    return std::isfinite(value.X) && std::isfinite(value.Y);
}
/// Half-open lattice rectangle around a box, expanded by the keep-out margin; never empty.
bool TryMakeKeepOut(Vec2 center,
                    Vec2 half,
                    float32 margin,
                    uint32 units,
                    procedural::ScatterRectangle& outRectangle) noexcept
{
    procedural::ScatterRectangle rectangle;
    if (TryScaleToLattice(center.X - half.X - margin, units, rectangle.MinimumX) == false ||
        TryScaleToLattice(center.Y - half.Y - margin, units, rectangle.MinimumY) == false ||
        TryScaleToLattice(center.X + half.X + margin, units, rectangle.MaximumX) == false ||
        TryScaleToLattice(center.Y + half.Y + margin, units, rectangle.MaximumY) == false)
    {
        return false;
    }
    rectangle.MaximumX = foundation::math::Max(rectangle.MaximumX, rectangle.MinimumX + 1);
    rectangle.MaximumY = foundation::math::Max(rectangle.MaximumY, rectangle.MinimumY + 1);
    outRectangle = rectangle;
    return true;
}
/// Fold the low ByteCount little-endian bytes of `value` into an FNV-1a hash.
template <usize ByteCount>
void MixBytes(uint64& inoutHash, uint64 value) noexcept
{
    for (usize byte = 0; byte < ByteCount; ++byte)
    {
        inoutHash ^= (value >> (byte * BITS_PER_BYTE)) & BYTE_MASK;
        inoutHash *= FNV_PRIME;
    }
}
float32 Distance(Vec2 first, Vec2 second) noexcept
{
    return std::hypot(first.X - second.X, first.Y - second.Y);
}
bool Overlap(Vec2 first, Vec2 a, Vec2 second, Vec2 b) noexcept
{
    return std::abs(first.X - second.X) < a.X + b.X && std::abs(first.Y - second.Y) < a.Y + b.Y;
}
} // namespace
void InputBridge::Sample(TickInput input) noexcept
{
    if (input.Cancelled)
    {
        Cancel();
        return;
    }
    if (mGate)
    {
        if (input.Held == false && input.Axis.X == 0 && input.Axis.Y == 0)
        {
            mGate = false;
        }
        return;
    }
    mPending.Axis = input.Axis;
    mPending.Held = input.Held;
    mPending.Pressed |= input.Pressed;
    mPending.Released |= input.Released;
}
void InputBridge::Cancel() noexcept
{
    mPending = {};
    mPending.Cancelled = true;
    mGate = true;
}
TickInput InputBridge::Consume() noexcept
{
    const auto result = mPending;
    mPending.Pressed = false;
    mPending.Released = false;
    mPending.Cancelled = false;
    return result;
}
Status GameWorld::Prepare(const Level& level, uint32 capacity, WorldSeed worldSeed) noexcept
{
    if (ValidateLevel(level) != LevelError::None || capacity < level.EntityCount || capacity > MAX_WORLD_ENTITIES)
    {
        return Status::InvalidConfiguration;
    }
    auto status = mRegistry.TryInitialize(capacity);
    if (status != Status::Success)
    {
        return status;
    }
    status = mTransforms.TryInitialize(mRegistry);
    if (status != Status::Success)
    {
        return status;
    }
    status = mMotion.TryInitialize(mRegistry);
    if (status != Status::Success)
    {
        return status;
    }
    status = mColliders.TryInitialize(mRegistry);
    if (status != Status::Success)
    {
        return status;
    }
    status = mVisuals.TryInitialize(mRegistry);
    if (status != Status::Success)
    {
        return status;
    }
    status = mHealth.TryInitialize(mRegistry);
    if (status != Status::Success)
    {
        return status;
    }
    status = mPlayers.TryInitialize(mRegistry);
    if (status != Status::Success)
    {
        return status;
    }
    status = mEnemies.TryInitialize(mRegistry);
    if (status != Status::Success)
    {
        return status;
    }
    status = mExits.TryInitialize(mRegistry);
    if (status != Status::Success)
    {
        return status;
    }
    mLevel = level;
    foundation::Sort(mLevel.Entities, mLevel.EntityCount, [](const Recipe& first, const Recipe& second) {
        return first.Id.View() < second.Id.View();
    });
    return prepareScatter(worldSeed);
}
Status GameWorld::prepareScatter(WorldSeed worldSeed) noexcept
{
    // Headless room scatter (docs/architecture/procedural-content-generation.md#p0-room-scoped-scatter): the
    // recipe borrows this frame's arrays, the kernel writes every layer into one scratch allocation, and only
    // the accepted placements are kept. Nothing here runs again on the tick or extraction path.
    mWorldSeed = worldSeed;
    mPlacements.Clear();
    mPlacementDigest = 0;
    mRoom = 0;
    if (mLevel.HasScatter == false)
    {
        return Status::Success;
    }
    const RoomScatter& scatter = mLevel.Scatter;
    const uint16 room = FindCatalogRoom(mLevel.Document, mLevel.Id.View());
    if (room == 0)
    {
        return Status::InvalidConfiguration;
    }
    procedural::ScatterKeys keys;
    RandomKey cosmeticKey;
    if (procedural::TryMakeScatterKeys(worldSeed.Value,
                                       {
                                           .Placement = RANDOM_DOMAIN_SCATTER_PLACEMENT,
                                           .Variant = RANDOM_DOMAIN_SCATTER_VARIANT,
                                           .Transform = RANDOM_DOMAIN_SCATTER_TRANSFORM,
                                       },
                                       keys) != procedural::ScatterStatus::Success ||
        foundation::math::TryMakeRandomKey(worldSeed.Value, RANDOM_DOMAIN_SCATTER_COSMETIC, cosmeticKey) !=
            foundation::math::MathStatus::Success)
    {
        return Status::InvalidConfiguration;
    }
    procedural::ScatterRecipe recipe;
    recipe.Room = room;
    if (TryScaleToLattice(scatter.Minimum.X, scatter.UnitsPerMetre, recipe.Region.MinimumX) == false ||
        TryScaleToLattice(scatter.Minimum.Y, scatter.UnitsPerMetre, recipe.Region.MinimumY) == false ||
        TryScaleToLattice(scatter.Maximum.X, scatter.UnitsPerMetre, recipe.Region.MaximumX) == false ||
        TryScaleToLattice(scatter.Maximum.Y, scatter.UnitsPerMetre, recipe.Region.MaximumY) == false)
    {
        return Status::InvalidConfiguration;
    }
    // Keep-outs: every authored spawn point and exit, then every collision box, each with the margin.
    procedural::ScatterRectangle keepOuts[procedural::MAX_SCATTER_KEEP_OUTS] = {};
    usize keepOutCount = 0;
    for (usize index = 0; index < mLevel.EntityCount; ++index)
    {
        const Recipe& entity = mLevel.Entities[index];
        const Vec2 half{entity.HalfExtent.X * entity.Scale.X, entity.HalfExtent.Y * entity.Scale.Y};
        if (TryMakeKeepOut(entity.Position,
                           half,
                           scatter.KeepOutMargin,
                           scatter.UnitsPerMetre,
                           keepOuts[keepOutCount]) == false)
        {
            return Status::InvalidConfiguration;
        }
        ++keepOutCount;
    }
    for (usize index = 0; index < mLevel.CollisionCount; ++index)
    {
        const Box& box = mLevel.Collision[index];
        if (TryMakeKeepOut(box.Center,
                           box.HalfExtent,
                           scatter.KeepOutMargin,
                           scatter.UnitsPerMetre,
                           keepOuts[keepOutCount]) == false)
        {
            return Status::InvalidConfiguration;
        }
        ++keepOutCount;
    }
    recipe.KeepOuts = BorrowedView<const procedural::ScatterRectangle>(keepOuts, keepOutCount);
    procedural::ScatterVariant variants[MAX_ROOM_SCATTER_LAYERS][MAX_ROOM_SCATTER_VARIANTS] = {};
    procedural::ScatterLayer layers[MAX_ROOM_SCATTER_LAYERS] = {};
    for (usize index = 0; index < scatter.LayerCount; ++index)
    {
        const ScatterLayerRecipe& authored = scatter.Layers[index];
        for (usize variant = 0; variant < authored.VariantCount; ++variant)
        {
            variants[index][variant].ExclusionRadius = authored.Variants[variant].ExclusionRadius;
        }
        layers[index] =
        {
            .Id = authored.Layer,
            .CellSize = authored.CellSize,
            .CandidatesPerCell = authored.Candidates,
            .Variants = BorrowedView<const procedural::ScatterVariant>(variants[index], authored.VariantCount),
            .MinimumScale = authored.MinimumScale,
            .MaximumScale = authored.MaximumScale,
        };
    }
    recipe.Layers = BorrowedView<const procedural::ScatterLayer>(layers, scatter.LayerCount);
    if (procedural::ValidateScatterRecipe(recipe).Status != procedural::ScatterStatus::Success)
    {
        return Status::InvalidConfiguration;
    }
    procedural::ScatterCellRange cells[MAX_ROOM_SCATTER_LAYERS] = {};
    usize candidateOffsets[MAX_ROOM_SCATTER_LAYERS + 1] = {};
    for (usize index = 0; index < scatter.LayerCount; ++index)
    {
        usize count = 0;
        if (procedural::TryGetScatterCells(recipe, index, cells[index]) != procedural::ScatterStatus::Success ||
            procedural::TryCountScatterCandidates(recipe, index, cells[index], count) !=
                procedural::ScatterStatus::Success)
        {
            return Status::InvalidConfiguration;
        }
        candidateOffsets[index + 1] = candidateOffsets[index] + count;
        if (candidateOffsets[index + 1] > MAX_ROOM_SCATTER_CANDIDATES)
        {
            return Status::CapacityExceeded;
        }
    }
    foundation::Array<procedural::ScatterPlacement> scratch;
    if (scratch.TryResize(candidateOffsets[scatter.LayerCount]) == false)
    {
        return Status::AllocationFailure;
    }
    usize accepted[MAX_ROOM_SCATTER_LAYERS] = {};
    usize total = 0;
    for (usize index = 0; index < scatter.LayerCount; ++index)
    {
        const BorrowedView<procedural::ScatterPlacement> storage(scratch.GetData() + candidateOffsets[index],
                                                                 candidateOffsets[index + 1] - candidateOffsets[index]);
        if (procedural::TryGenerateScatter(recipe, keys, index, cells[index], storage, accepted[index]) !=
            procedural::ScatterStatus::Success)
        {
            return Status::InvalidConfiguration;
        }
        total += accepted[index];
    }
    if (total > MAX_ROOM_PLACEMENTS)
    {
        return Status::CapacityExceeded;
    }
    if (mPlacements.TryEnsureCapacity(total) == false)
    {
        return Status::AllocationFailure;
    }
    uint64 digest = FNV_OFFSET_BASIS;
    for (usize index = 0; index < scatter.LayerCount; ++index)
    {
        for (usize i = 0; i < accepted[index]; ++i)
        {
            const procedural::ScatterPlacement& generated = scratch[candidateOffsets[index] + i];
            const Placement placement
            {
                .Scope = procedural::PackScatterScope(generated.Identity),
                .LatticeX = generated.LatticeX,
                .LatticeY = generated.LatticeY,
                .Rotation = generated.Rotation,
                .Scale = generated.Scale,
                .Layer = static_cast<uint8>(index),
                .Variant = static_cast<uint8>(generated.Variant),
            };
            // Capacity was reserved above, so this append cannot allocate.
            if (mPlacements.TryAdd(placement) == false)
            {
                return Status::AllocationFailure;
            }
            MixBytes<sizeof(uint64)>(digest, placement.Scope);
            MixBytes<sizeof(int32)>(digest, static_cast<uint32>(placement.LatticeX));
            MixBytes<sizeof(int32)>(digest, static_cast<uint32>(placement.LatticeY));
            MixBytes<sizeof(uint32)>(digest, generated.Variant);
            MixBytes<sizeof(uint16)>(digest, placement.Rotation);
            MixBytes<sizeof(uint16)>(digest, placement.Scale);
        }
    }
    mPlacementDigest = digest;
    mCosmeticKey = cosmeticKey.Value;
    mRoom = room;
    return Status::Success;
}
Status GameWorld::Spawn(const Recipe& recipe, EntityId& output) noexcept
{
    auto status = mRegistry.TryCreate(output);
    if (status != Status::Success)
    {
        return status;
    }
    status =
        mTransforms.TryInsert(mRegistry, output, {recipe.Position, recipe.Position, recipe.Rotation, recipe.Scale});
    if (status != Status::Success)
    {
        return status;
    }
    const Vec2 extent{recipe.HalfExtent.X * recipe.Scale.X, recipe.HalfExtent.Y * recipe.Scale.Y};
    status = mColliders.TryInsert(mRegistry, output, {extent});
    if (status != Status::Success)
    {
        return status;
    }
    Visual visual{ .HalfExtent = extent };
    visual.Color[3] = 1;
    if (recipe.Type == Kind::Player)
    {
        visual.Color[0] = 1;
        visual.Color[1] = 0.65F;
        visual.Color[2] = 0.15F;
    }
    else if (recipe.Type == Kind::Enemy)
    {
        visual.Color[0] = 0.9F;
        visual.Color[1] = 0.15F;
        visual.Color[2] = 0.2F;
    }
    else
    {
        visual.Color[0] = 0.2F;
        visual.Color[1] = 0.9F;
        visual.Color[2] = 0.5F;
    }
    status = mVisuals.TryInsert(mRegistry, output, visual);
    if (status != Status::Success)
    {
        return status;
    }
    if (recipe.Type == Kind::Exit)
    {
        return mExits.TryInsert(mRegistry, output, {recipe.NextLevel});
    }
    status = mMotion.TryInsert(mRegistry, output, { .Speed = recipe.Speed });
    if (status != Status::Success)
    {
        return status;
    }
    status = mHealth.TryInsert(mRegistry, output, {recipe.Health});
    if (status != Status::Success)
    {
        return status;
    }
    if (recipe.Type == Kind::Player)
    {
        mPlayer = output;
        return mPlayers.TryInsert(mRegistry, output, {});
    }
    return mEnemies.TryInsert(mRegistry, output, {mPlayer});
}
Status GameWorld::BuildNext() noexcept
{
    if (mPhase != Phase::Construction || mRegistry.GetWorld() == 0 || IsBuilt())
    {
        return Status::InvalidPhase;
    }
    auto status = Spawn(mLevel.Entities[mBuilt], mAuthored[mBuilt]);
    if (status != Status::Success)
    {
        return status;
    }
    ++mBuilt;
    if (IsBuilt())
    {
        for (usize index = 0; index < mBuilt; ++index)
        {
            if (auto* enemy = mEnemies.Find(mAuthored[index]))
            {
                enemy->Target = FindAuthored(mLevel.Entities[index].Target.View());
            }
        }
        mRegistry.SetStructuralPhase(false);
        mPhase = Phase::Idle;
    }
    return Status::Success;
}
EntityId GameWorld::FindAuthored(std::string_view id) const noexcept
{
    for (usize index = 0; index < mBuilt; ++index)
    {
        if (mLevel.Entities[index].Id.View() == id)
        {
            return mAuthored[index];
        }
    }
    return {};
}
Status GameWorld::QueueDestroy(EntityId entity) noexcept
{
    if (mPhase != Phase::Gameplay && mPhase != Phase::BeginTick && mPhase != Phase::Idle)
    {
        return Status::InvalidPhase;
    }
    if (mRegistry.IsAlive(entity) == false)
    {
        return Status::InvalidEntity;
    }
    if (mRegistry.IsActive(entity) == false)
    {
        return Status::Success;
    }
    if (mCommandCount == 32)
    {
        return Status::CapacityExceeded;
    }
    mCommands[mCommandCount++] = { .Kind = CommandKind::Destroy, .Entity = entity };
    return mRegistry.MarkPendingDestroy(entity);
}
Status GameWorld::QueueSpawn(Recipe recipe, uint64& request) noexcept
{
    if (mPhase != Phase::Gameplay && mPhase != Phase::BeginTick && mPhase != Phase::Idle)
    {
        return Status::InvalidPhase;
    }
    // This game's runtime recipe is an additional guard targeting the existing
    // player. No cross-reference to another queued spawn is supported.
    if (recipe.Type != Kind::Enemy || mRegistry.IsActive(mPlayer) == false)
    {
        return Status::InvalidConfiguration;
    }
    Level validation = mLevel;
    if (validation.EntityCount == 32)
    {
        return Status::CapacityExceeded;
    }
    validation.Entities[validation.EntityCount++] = recipe;
    if (ValidateLevel(validation) != LevelError::None)
    {
        return Status::InvalidConfiguration;
    }
    if (mCommandCount == 32)
    {
        return Status::CapacityExceeded;
    }
    if (mNextRequest == std::numeric_limits<uint64>::max())
    {
        return Status::IdentityExhausted;
    }
    request = mNextRequest++;
    mCommands[mCommandCount++] = { .Kind = CommandKind::Spawn, .Spawn = recipe, .Request = request };
    return Status::Success;
}
Status GameWorld::BeginRequest(EntityId entity, Completion& ticket) noexcept
{
    if (mPhase != Phase::Idle)
    {
        return Status::InvalidPhase;
    }
    auto* motion = mMotion.Find(entity);
    if (mRegistry.IsActive(entity) == false || motion == nullptr)
    {
        return Status::InvalidEntity;
    }
    if (mNextRequest == std::numeric_limits<uint64>::max() || motion->Revision == std::numeric_limits<uint64>::max())
    {
        return Status::IdentityExhausted;
    }
    RequestLease* lease = nullptr;
    for (auto& entry : mRequests)
    {
        if (entry.Id == 0)
        {
            lease = &entry;
            break;
        }
    }
    if (lease == nullptr)
    {
        return Status::CapacityExceeded;
    }
    const auto request = mNextRequest++;
    *lease =
    {
        .Id = request,
        .Entity = entity,
        .Revision = motion->Revision + 1,
        .CreationSequence = mRegistry.GetCreationSequence(entity),
    };
    motion->PendingRequest = request;
    ++motion->Revision;
    ticket = { .Entity = entity, .Request = request, .Revision = motion->Revision };
    return Status::Success;
}
Status GameWorld::SubmitCompletion(Completion completion) noexcept
{
    // Main-thread mailbox entry. A worker must hand an owned result to its own
    // synchronized transport; it may never call this or retain pool borrows.
    if (mPhase != Phase::Idle)
    {
        return Status::InvalidPhase;
    }
    if (completion.Entity.World != mRegistry.GetWorld())
    {
        ++mStaleCompletions;
        return Status::InvalidEntity;
    }
    if (completion.State != CompletionState::Ready && completion.State != CompletionState::Cancelled &&
        completion.State != CompletionState::Failed)
    {
        return Status::InvalidConfiguration;
    }
    if (Finite(completion.Velocity) == false || std::abs(completion.Velocity.X) > 32 ||
        std::abs(completion.Velocity.Y) > 32)
    {
        return Status::InvalidConfiguration;
    }
    RequestLease* lease = nullptr;
    for (auto& entry : mRequests)
    {
        if (entry.Id == completion.Request && entry.Id != 0)
        {
            lease = &entry;
            break;
        }
    }
    if (lease == nullptr || lease->Completed)
    {
        return Status::InvalidConfiguration;
    }
    if (lease->Entity != completion.Entity || lease->Revision != completion.Revision)
    {
        ++mStaleCompletions;
        return Status::InvalidEntity;
    }
    if (mCompletionCount == 16)
    {
        return Status::CapacityExceeded;
    }
    lease->Completed = true;
    mCompletions[mCompletionCount++] = completion;
    return Status::Success;
}
void GameWorld::Intent(TickInput input) noexcept
{
    LUDUS_PROFILE_SCOPE(WorldIntent);
    // Reads active identities, targets and positions. Writes velocities and AI
    // phase/deadline only; structural effects are recorded by Gameplay.
    if (auto* motion = mMotion.Find(mPlayer); motion != nullptr && mRegistry.IsActive(mPlayer))
    {
        if (motion->Override == false)
        {
            motion->Velocity = {foundation::math::Clamp(input.Axis.X, -1.0F, 1.0F) * motion->Speed,
                                foundation::math::Clamp(input.Axis.Y, -1.0F, 1.0F) * motion->Speed};
        }
    }
    const auto owners = mEnemies.GetOwners();
    for (usize index = 0; index < owners.size(); ++index)
    {
        const auto entity = owners[index];
        if (mRegistry.IsActive(entity) == false)
        {
            continue;
        }
        auto& enemy = mEnemies.GetValues()[index];
        auto* motion = mMotion.Find(entity);
        if (mRegistry.IsActive(enemy.Target) == false)
        {
            motion->Velocity = {};
            enemy.Phase = ActionPhase::SelectTarget;
            continue;
        }
        if (motion->Override)
        {
            continue;
        }
        const auto* target = mTransforms.Find(enemy.Target);
        const auto* transform = mTransforms.Find(entity);
        const float32 distance = Distance(target->Current, transform->Current);
        if (mTick + 1 < enemy.WakeTick)
        {
            enemy.Phase = ActionPhase::Recover;
            motion->Velocity = {};
        }
        else if (distance < 0.9F)
        {
            enemy.Phase = ActionPhase::Attack;
            motion->Velocity = {};
        }
        else
        {
            enemy.Phase = ActionPhase::Approach;
            motion->Velocity = {(target->Current.X - transform->Current.X) / distance * motion->Speed,
                                (target->Current.Y - transform->Current.Y) / distance * motion->Speed};
        }
    }
}
void GameWorld::MoveAndCollide() noexcept
{
    LUDUS_PROFILE_SCOPE(WorldMotion);
    // Axis-separated swept box motion against static authored boxes; no gravity.
    // Authoritative positions are never interpolated or written by rendering.
    const auto owners = mMotion.GetOwners();
    for (usize index = 0; index < owners.size(); ++index)
    {
        const auto entity = owners[index];
        if (mRegistry.IsActive(entity) == false)
        {
            continue;
        }
        auto* transform = mTransforms.Find(entity);
        const auto half = mColliders.Find(entity)->HalfExtent;
        const auto velocity = mMotion.GetValues()[index].Velocity;
        for (usize axis = 0; axis < 2; ++axis)
        {
            Vec2 proposed = transform->Current;
            const float32 movement = (axis == 0 ? velocity.X : velocity.Y) / static_cast<float32>(TICKS_PER_SECOND);
            float32& position = axis == 0 ? proposed.X : proposed.Y;
            position += movement;
            for (usize boxIndex = 0; boxIndex < mLevel.CollisionCount; ++boxIndex)
            {
                const auto& box = mLevel.Collision[boxIndex];
                const float32 old = axis == 0 ? transform->Current.X : transform->Current.Y;
                const float32 size = axis == 0 ? half.X : half.Y;
                const float32 center = axis == 0 ? box.Center.X : box.Center.Y;
                const float32 extent = axis == 0 ? box.HalfExtent.X : box.HalfExtent.Y;
                const bool otherOverlap = axis == 0 ? std::abs(proposed.Y - box.Center.Y) < half.Y + box.HalfExtent.Y
                                                    : std::abs(proposed.X - box.Center.X) < half.X + box.HalfExtent.X;
                if (otherOverlap && movement > 0 && old + size <= center - extent && position + size > center - extent)
                {
                    position = center - extent - size;
                }
                if (otherOverlap && movement < 0 && old - size >= center + extent && position - size < center + extent)
                {
                    position = center + extent + size;
                }
            }
            position = foundation::math::Clamp(position,
                                               (axis == 0 ? mLevel.Minimum.X + half.X : mLevel.Minimum.Y + half.Y),
                                               (axis == 0 ? mLevel.Maximum.X - half.X : mLevel.Maximum.Y - half.Y));
            transform->Current = proposed;
        }
    }
    mContactCount = 0;
    for (auto entity : mExits.GetOwners())
    {
        if (mRegistry.IsActive(entity) == false)
        {
            continue;
        }
        auto* exit = mExits.Find(entity);
        const bool touching = mRegistry.IsActive(mPlayer) && Overlap(mTransforms.Find(entity)->Current,
                                                                     mColliders.Find(entity)->HalfExtent,
                                                                     mTransforms.Find(mPlayer)->Current,
                                                                     mColliders.Find(mPlayer)->HalfExtent);
        if (touching || exit->Touching)
        {
            auto first = entity;
            auto second = mPlayer;
            if (mRegistry.GetCreationSequence(first) > mRegistry.GetCreationSequence(second))
            {
                std::swap(first, second);
            }
            mContacts[mContactCount++] = {first,
                                          second,
                                          touching ? (exit->Touching ? ContactPhase::Stay : ContactPhase::Enter)
                                                   : ContactPhase::Exit};
        }
        exit->Touching = touching;
    }
    foundation::Sort(mContacts, mContactCount, [&](const ContactEvent& first, const ContactEvent& second) {
        const auto a = mRegistry.GetCreationSequence(first.First);
        const auto b = mRegistry.GetCreationSequence(second.First);
        return a != b ? a < b
                      : mRegistry.GetCreationSequence(first.Second) < mRegistry.GetCreationSequence(second.Second);
    });
}
void GameWorld::Stage(EventKind kind, Vec2 anchor) noexcept
{
    if (mStagedCount == 64)
    {
        ++mDroppedPresentation;
        return;
    }
    mStaged[mStagedCount] = {mRegistry.GetWorld(), mTick + 1, static_cast<uint64>(mStagedCount), kind, anchor};
    ++mStagedCount;
}
Status GameWorld::Damage(EntityId entity) noexcept
{
    auto* health = mHealth.Find(entity);
    if (mRegistry.IsActive(entity) == false || health == nullptr || health->Current == 0)
    {
        return Status::Success;
    }
    --health->Current;
    const Vec2 position = mTransforms.Find(entity)->Current;
    Stage(EventKind::Damage, position);
    if (health->Current == 0)
    {
        const auto status = QueueDestroy(entity);
        if (status != Status::Success)
        {
            return status;
        }
        Stage(EventKind::Death, position);
    }
    return Status::Success;
}
Status GameWorld::Gameplay(TickInput input, Name& nextLevel) noexcept
{
    LUDUS_PROFILE_SCOPE(WorldGameplay);
    // Damage order is player intent, then stable enemy creation sequence, then
    // exit triggers. Pending destruction immediately excludes later interactions.
    if (mRegistry.IsActive(mPlayer))
    {
        auto* player = mPlayers.Find(mPlayer);
        if (input.Pressed && input.Cancelled == false && mTick + 1 >= player->AttackReady)
        {
            if (mTick > std::numeric_limits<uint64>::max() - 31)
            {
                return Status::IdentityExhausted;
            }
            player->AttackReady = mTick + 31;
            EntityId enemies[64] = {};
            usize count = 0;
            for (auto entity : mEnemies.GetOwners())
            {
                enemies[count++] = entity;
            }
            foundation::Sort(enemies, count, [&](EntityId first, EntityId second) {
                return mRegistry.GetCreationSequence(first) < mRegistry.GetCreationSequence(second);
            });
            for (usize index = 0; index < count; ++index)
            {
                if (mRegistry.IsActive(enemies[index]) &&
                    Distance(mTransforms.Find(mPlayer)->Current, mTransforms.Find(enemies[index])->Current) < 2)
                {
                    const auto status = Damage(enemies[index]);
                    if (status != Status::Success)
                    {
                        return status;
                    }
                }
            }
        }
    }
    EntityId enemies[64] = {};
    usize count = 0;
    for (auto entity : mEnemies.GetOwners())
    {
        enemies[count++] = entity;
    }
    foundation::Sort(enemies, count, [&](EntityId first, EntityId second) {
        return mRegistry.GetCreationSequence(first) < mRegistry.GetCreationSequence(second);
    });
    for (usize index = 0; index < count; ++index)
    {
        if (mRegistry.IsActive(enemies[index]) == false)
        {
            continue;
        }
        auto* enemy = mEnemies.Find(enemies[index]);
        if (enemy->Phase == ActionPhase::Attack && mRegistry.IsActive(enemy->Target))
        {
            if (mTick > std::numeric_limits<uint64>::max() - 31)
            {
                return Status::IdentityExhausted;
            }
            enemy->WakeTick = mTick + 31;
            enemy->Phase = ActionPhase::Recover;
            const auto status = Damage(enemy->Target);
            if (status != Status::Success)
            {
                return status;
            }
        }
    }
    for (const auto& contact : GetContacts())
    {
        if (contact.Kind != ContactPhase::Enter || mRegistry.IsActive(contact.First) == false ||
            mRegistry.IsActive(contact.Second) == false)
        {
            continue;
        }
        const auto entity = mExits.Find(contact.First) != nullptr ? contact.First : contact.Second;
        auto* exit = mExits.Find(entity);
        if (exit != nullptr && exit->Entered == false)
        {
            exit->Entered = true;
            if (nextLevel.View().empty())
            {
                nextLevel = exit->NextLevel;
            }
            Stage(EventKind::Exit, mTransforms.Find(entity)->Current);
        }
    }
    return Status::Success;
}
Status GameWorld::Commit() noexcept
{
    LUDUS_PROFILE_SCOPE(WorldCommit);
    usize creates = 0;
    usize releases = 0;
    for (usize index = 0; index < mCommandCount; ++index)
    {
        if (mCommands[index].Kind == CommandKind::Spawn)
        {
            ++creates;
        }
        else
        {
            ++releases;
        }
    }
    auto status = mRegistry.CheckStructuralBudget(creates, releases);
    if (status != Status::Success)
    {
        return status;
    }
    mRegistry.SetStructuralPhase(true);
    for (usize index = 0; index < mCommandCount; ++index)
    {
        const auto& command = mCommands[index];
        if (command.Kind == CommandKind::Spawn)
        {
            EntityId entity;
            status = Spawn(command.Spawn, entity);
            if (status != Status::Success)
            {
                mRegistry.SetStructuralPhase(false);
                return status;
            }
            mOutcomes[mOutcomeCount++] = {command.Request, entity};
            Stage(EventKind::Spawn, command.Spawn.Position);
        }
        else
        {
            const auto entity = command.Entity;
            if (entity == mPlayer)
            {
                for (auto& exit : mExits.GetValues())
                {
                    exit.Touching = false;
                }
            }
            const auto remove = [&]<typename T>(gameplay::world::ComponentPool<T>& pool) {
                if (pool.Find(entity) != nullptr)
                {
                    return pool.TryRemove(mRegistry, entity);
                }
                return Status::Success;
            };
            status = remove(mTransforms);
            if (status != Status::Success)
            {
                return status;
            }
            status = remove(mMotion);
            if (status != Status::Success)
            {
                return status;
            }
            status = remove(mColliders);
            if (status != Status::Success)
            {
                return status;
            }
            status = remove(mVisuals);
            if (status != Status::Success)
            {
                return status;
            }
            status = remove(mHealth);
            if (status != Status::Success)
            {
                return status;
            }
            status = remove(mPlayers);
            if (status != Status::Success)
            {
                return status;
            }
            status = remove(mEnemies);
            if (status != Status::Success)
            {
                return status;
            }
            status = remove(mExits);
            if (status != Status::Success)
            {
                return status;
            }
            status = mRegistry.TryRelease(entity);
            if (status != Status::Success)
            {
                return status;
            }
        }
    }
    mRegistry.SetStructuralPhase(false);
    mCommandCount = 0;
    return Status::Success;
}
Status GameWorld::RunTick(TickInput input) noexcept
{
    LUDUS_PROFILE_SCOPE(WorldTick);
    if (IsBuilt() == false || mPhase != Phase::Idle)
    {
        return Status::InvalidPhase;
    }
    if (Finite(input.Axis) == false)
    {
        mPhase = Phase::Faulted;
        return Status::InvalidConfiguration;
    }
    if (mTick == std::numeric_limits<uint64>::max())
    {
        mPhase = Phase::Faulted;
        return Status::IdentityExhausted;
    }
    auto& trace = mTrace[mTraceNext];
    trace = { .Tick = mTick + 1, .Input = input, .Completions = mCompletionCount };
    mTraceNext = (mTraceNext + 1) % 64;
    mTraceCount = foundation::math::Min(mTraceCount + 1, static_cast<usize>(64));
    mPhase = Phase::BeginTick;
    mStagedCount = 0;
    mOutcomeCount = 0;
    for (auto& motion : mMotion.GetValues())
    {
        motion.Override = false;
    }
    const auto completionCount = mCompletionCount;
    mCompletionCount = 0;
    foundation::Sort(mCompletions, completionCount, [](const Completion& first, const Completion& second) {
        return first.Request < second.Request;
    });
    for (usize index = 0; index < completionCount; ++index)
    {
        const auto& result = mCompletions[index];
        for (auto& lease : mRequests)
        {
            if (lease.Id == result.Request)
            {
                lease = {};
                break;
            }
        }
        auto* motion = mMotion.Find(result.Entity);
        if (mRegistry.IsActive(result.Entity) == false || motion == nullptr || result.Request == 0 ||
            motion->PendingRequest != result.Request || motion->Revision != result.Revision)
        {
            ++mStaleCompletions;
            continue;
        }
        motion->PendingRequest = 0;
        if (result.State == CompletionState::Ready)
        {
            motion->Velocity = result.Velocity;
            motion->Override = true;
        }
    }
    for (auto& transform : mTransforms.GetValues())
    {
        transform.Previous = transform.Current;
    }
    mPhase = Phase::Intent;
    Intent(input);
    mPhase = Phase::Motion;
    MoveAndCollide();
    mPhase = Phase::Gameplay;
    // Required session outcomes are owned tick-local values until commit succeeds.
    Name nextLevel = mNextLevel;
    auto status = Gameplay(input, nextLevel);
    trace.Commands = mCommandCount;
    if (status == Status::Success)
    {
        mPhase = Phase::Commit;
        status = Commit();
    }
    trace.LastPhase = mPhase;
    trace.Result = status;
    trace.Contacts = mContactCount;
    trace.Presentation = mStagedCount;
    if (status != Status::Success)
    {
        mStagedCount = 0;
        mRegistry.SetStructuralPhase(false);
        mPhase = Phase::Faulted;
        return status;
    }
    mNextLevel = nextLevel;
    // Optional cosmetics may be dropped, required level outcome lives in state.
    for (usize index = 0; index < mStagedCount; ++index)
    {
        if (mOutboxCount == 64)
        {
            ++mDroppedPresentation;
        }
        else
        {
            mOutbox[mOutboxCount++] = mStaged[index];
        }
    }
    ++mTick;
    mPhase = Phase::Idle;
    return Status::Success;
}
bool GameWorld::Extract(float32 alpha,
                        RenderFrame& frame,
                        bool followPlayer,
                        uint64 presentationSequence) const noexcept
{
    LUDUS_PROFILE_SCOPE(WorldExtract);
    if (IsBuilt() == false || mPhase != Phase::Idle || std::isfinite(alpha) == false || alpha < 0 || alpha > 1)
    {
        return false;
    }
    namespace camera = gameplay::camera;
    RenderFrame candidate{ .Tick = mTick };
    const camera::CameraRigDefinition definition
    {
        .Recipe = followPlayer ? camera::CameraRecipe::OrthographicFollow : camera::CameraRecipe::Fixed,
        .Pose = { .Position = {static_cast<float64>(mLevel.Camera.X), static_cast<float64>(mLevel.Camera.Y), 1.0} },
        .Lens =
        {
            .VerticalSpan = mLevel.VerticalExtent,
            .MinimumVisibleWidth = mLevel.Maximum.X - mLevel.Minimum.X,
        },
    };
    const camera::CameraUpdateContext context
    {
        .PreviousTick = mTick == 0 ? 0 : mTick - 1,
        .CurrentTick = mTick,
        .RenderAlpha = alpha,
        .PresentationSequence = presentationSequence,
        .DiscontinuityRevision = mRegistry.GetWorld(),
    };
    // Use exactly the draw's previous/current pair and alpha, without retaining
    // a pool pointer or interpolating the camera a second time.
    camera::CameraTargetSample target;
    if (followPlayer)
    {
        if (mRegistry.IsActive(mPlayer) == false)
        {
            return false;
        }
        const auto* transform = mTransforms.Find(mPlayer);
        if (transform == nullptr)
        {
            return false;
        }
        target.Position = {
            static_cast<float64>(transform->Previous.X + (transform->Current.X - transform->Previous.X) * alpha),
            static_cast<float64>(transform->Previous.Y + (transform->Current.Y - transform->Previous.Y) * alpha),
            0.0};
        target.BindingId = mRegistry.GetCreationSequence(mPlayer);
    }
    if (camera::TryEvaluateCamera(definition,
                                  context,
                                  followPlayer ? &target : nullptr,
                                  candidate.Camera,
                                  &candidate.CameraTrace) != camera::CameraStatus::Success)
    {
        return false;
    }
    const auto add = [&](Draw draw) {
        if (candidate.Count == MAX_RENDER_DRAWS)
        {
            return false;
        }
        candidate.Draws[candidate.Count++] = draw;
        return true;
    };
    for (usize index = 0; index < mLevel.CollisionCount; ++index)
    {
        if (add(
        {
            .Position = mLevel.Collision[index].Center,
            .HalfExtent = mLevel.Collision[index].HalfExtent,
            .Color = {0.2F, 0.25F, 0.35F, 1},
        }) == false)
        {
            return false;
        }
    }
    // Placements draw behind entities. The cosmetic tint is the only draw-time sample: it reads the
    // presentation-only cosmetic domain at event zero and never enters simulation state or the replay hash.
    const float32 units = static_cast<float32>(mLevel.Scatter.UnitsPerMetre);
    const RandomKey cosmeticKey{mCosmeticKey};
    for (const Placement& placement : GetPlacements())
    {
        const ScatterVariantRecipe& variant = mLevel.Scatter.Layers[placement.Layer].Variants[placement.Variant];
        const float32 scale = static_cast<float32>(placement.Scale) / static_cast<float32>(SCATTER_SCALE_UNIT);
        const float32 tint =
            COSMETIC_TINT_FLOOR +
            (1.0F - COSMETIC_TINT_FLOOR) * foundation::math::SampleFloat01(
                                               cosmeticKey,
                                               RandomAddress{placement.Scope, COSMETIC_EVENT, COSMETIC_TINT_DIMENSION});
        Draw draw
        {
            .Position = {static_cast<float32>(placement.LatticeX) / units,
                         static_cast<float32>(placement.LatticeY) / units},
            .HalfExtent = {variant.HalfExtent.X * scale, variant.HalfExtent.Y * scale},
            .Rotation = static_cast<float32>(static_cast<float64>(placement.Rotation) *
                                             (2.0 * foundation::math::kPi / ROTATION_UNITS_PER_TURN)),
            .Color = {variant.Color[0] * tint, variant.Color[1] * tint, variant.Color[2] * tint, 1},
        };
        if (add(draw) == false)
        {
            return false;
        }
    }
    EntityId entities[MAX_WORLD_ENTITIES] = {};
    usize count = 0;
    for (auto entity : mVisuals.GetOwners())
    {
        if (mRegistry.IsActive(entity))
        {
            entities[count++] = entity;
        }
    }
    foundation::Sort(entities, count, [&](EntityId first, EntityId second) {
        return mRegistry.GetCreationSequence(first) < mRegistry.GetCreationSequence(second);
    });
    for (usize index = 0; index < count; ++index)
    {
        const auto* transform = mTransforms.Find(entities[index]);
        const auto* visual = mVisuals.Find(entities[index]);
        const Vec2 position{transform->Previous.X + (transform->Current.X - transform->Previous.X) * alpha,
                            transform->Previous.Y + (transform->Current.Y - transform->Previous.Y) * alpha};
        Draw draw{ .Position = position, .HalfExtent = visual->HalfExtent, .Rotation = transform->Rotation };
        for (usize channel = 0; channel < 4; ++channel)
        {
            draw.Color[channel] = visual->Color[channel];
        }
        if (add(draw) == false)
        {
            return false;
        }
    }
    frame = candidate;
    return true;
}
// Thanks to Glenn Fowler, Landon Curt Noll and Phong Vo, "FNV Hash",
// "FNV-1a alternate algorithm" and "Parameters of the FNV-1/FNV-1a hash":
// https://www.isthe.com/chongo/tech/comp/fnv/ . Adopt the XOR/multiply sequence
// and published constants for a non-cryptographic hash; collisions remain possible.
uint64 GameWorld::GetReplayHash() const noexcept
{
    // Hash only a completed boundary; queued work belongs to the next tick.
    if (mPhase != Phase::Idle || mCommandCount != 0 || mCompletionCount != 0)
    {
        return 0;
    }
    uint64 hash = FNV_OFFSET_BASIS;
    const auto mix = [&](uint64 value) { MixBytes<sizeof(uint64)>(hash, value); };
    const auto number = [&](float32 value) { mix(std::bit_cast<uint32>(value)); };
    mix(mTick);
    mix(mLevel.Seed);
    mix(mWorldSeed.Value);
    mix(mRoom);
    mix(mNextRequest);
    mix(mRegistry.GetCapacity());
    mix(mRegistry.GetAvailable());
    RequestLease requests[16] = {};
    usize requestCount = 0;
    for (const auto& request : mRequests)
    {
        if (request.Id != 0)
        {
            requests[requestCount++] = request;
        }
    }
    foundation::Sort(requests, requestCount, [](const RequestLease& first, const RequestLease& second) {
        return first.Id < second.Id;
    });
    mix(requestCount);
    for (usize index = 0; index < requestCount; ++index)
    {
        const auto& request = requests[index];
        mix(request.Id);
        mix(request.Revision);
        mix(request.CreationSequence);
    }
    const auto name = [&](const Name& value) {
        mix(value.View().size());
        for (const char ch : value.View())
        {
            mix(static_cast<uint8>(ch));
        }
    };
    const auto pair = [&](Vec2 value) {
        number(value.X);
        number(value.Y);
    };
    name(mLevel.Id);
    pair(mLevel.Minimum);
    pair(mLevel.Maximum);
    pair(mLevel.Camera);
    number(mLevel.VerticalExtent);
    mix(mLevel.CollisionCount);
    for (usize index = 0; index < mLevel.CollisionCount; ++index)
    {
        const auto& box = mLevel.Collision[index];
        name(box.Id);
        pair(box.Center);
        pair(box.HalfExtent);
    }
    mix(mLevel.EntityCount);
    for (usize index = 0; index < mLevel.EntityCount; ++index)
    {
        const auto& recipe = mLevel.Entities[index];
        name(recipe.Id);
        mix(static_cast<uint64>(recipe.Type));
        pair(recipe.Position);
        number(recipe.Rotation);
        pair(recipe.Scale);
        number(recipe.Speed);
        mix(recipe.Health);
        name(recipe.Sprite);
        name(recipe.Target);
        pair(recipe.HalfExtent);
        name(recipe.NextLevel);
    }
    // Scatter content and the accepted placements are simulation state; cosmetic tints are not.
    mix(mLevel.HasScatter);
    if (mLevel.HasScatter == true)
    {
        const RoomScatter& scatter = mLevel.Scatter;
        name(scatter.Id);
        mix(scatter.UnitsPerMetre);
        pair(scatter.Minimum);
        pair(scatter.Maximum);
        number(scatter.KeepOutMargin);
        mix(scatter.LayerCount);
        for (usize index = 0; index < scatter.LayerCount; ++index)
        {
            const ScatterLayerRecipe& layer = scatter.Layers[index];
            name(layer.Id);
            mix(layer.Layer);
            mix(layer.CellSize);
            mix(layer.Candidates);
            mix(layer.MinimumScale);
            mix(layer.MaximumScale);
            mix(layer.VariantCount);
            for (usize variant = 0; variant < layer.VariantCount; ++variant)
            {
                const ScatterVariantRecipe& recipe = layer.Variants[variant];
                name(recipe.Id);
                mix(recipe.ExclusionRadius);
                pair(recipe.HalfExtent);
                for (const float32 channel : recipe.Color)
                {
                    number(channel);
                }
            }
        }
    }
    mix(mPlacements.GetSize());
    for (const Placement& placement : GetPlacements())
    {
        mix(placement.Scope);
        mix(static_cast<uint32>(placement.LatticeX));
        mix(static_cast<uint32>(placement.LatticeY));
        mix(placement.Rotation);
        mix(placement.Scale);
        mix(placement.Layer);
        mix(placement.Variant);
    }
    name(mNextLevel);
    EntityId entities[MAX_WORLD_ENTITIES] = {};
    usize count = 0;
    for (auto entity : mTransforms.GetOwners())
    {
        entities[count++] = entity;
    }
    foundation::Sort(entities, count, [&](EntityId first, EntityId second) {
        return mRegistry.GetCreationSequence(first) < mRegistry.GetCreationSequence(second);
    });
    mix(count);
    for (usize index = 0; index < count; ++index)
    {
        const auto entity = entities[index];
        mix(mRegistry.GetCreationSequence(entity));
        mix(mRegistry.IsActive(entity));
        const auto* transform = mTransforms.Find(entity);
        number(transform->Current.X);
        number(transform->Current.Y);
        number(transform->Rotation);
        number(transform->Scale.X);
        number(transform->Scale.Y);
        if (const auto* motion = mMotion.Find(entity))
        {
            mix(1);
            number(motion->Velocity.X);
            number(motion->Velocity.Y);
            number(motion->Speed);
            mix(motion->Revision);
            mix(motion->PendingRequest);
            mix(motion->Override);
        }
        else
        {
            mix(0);
        }
        if (const auto* collider = mColliders.Find(entity))
        {
            mix(1);
            pair(collider->HalfExtent);
        }
        else
        {
            mix(0);
        }
        if (const auto* health = mHealth.Find(entity))
        {
            mix(health->Current);
        }
        else
        {
            mix(0);
        }
        if (const auto* player = mPlayers.Find(entity))
        {
            mix(1);
            mix(player->AttackReady);
        }
        else
        {
            mix(0);
        }
        if (const auto* enemy = mEnemies.Find(entity))
        {
            mix(1);
            mix(mRegistry.GetCreationSequence(enemy->Target));
            mix(enemy->WakeTick);
            mix(static_cast<uint64>(enemy->Phase));
        }
        else
        {
            mix(0);
        }
        if (const auto* exit = mExits.Find(entity))
        {
            mix(1);
            mix(exit->Entered);
            mix(exit->Touching);
            for (char ch : exit->NextLevel.View())
            {
                mix(static_cast<uint8>(ch));
            }
        }
        else
        {
            mix(0);
        }
    }
    return hash;
}
Status TickDriver::Configure(const TickPolicy& policy) noexcept
{
    if (policy.TicksPerSecond == 0 || policy.MaximumCatchUpTicks == 0)
    {
        return Status::InvalidConfiguration;
    }
    // Advance adds at most one clamped frame to a remainder below one tick; that sum must fit.
    uint64 largestPhase = 0;
    if (foundation::TryMultiply(policy.MaximumFrameDelta.Nanoseconds, policy.TicksPerSecond, largestPhase) == false ||
        foundation::TryAdd(largestPhase, PHASE_UNITS_PER_TICK - 1, largestPhase) == false)
    {
        return Status::InvalidConfiguration;
    }
    mPolicy = policy;
    mFrameClock = foundation::time::FrameClock{policy.MaximumFrameDelta};
    dropTiming();
    return Status::Success;
}
float32 TickDriver::GetAlpha() const noexcept
{
    // The largest float32 below one: rounding (PHASE_UNITS_PER_TICK - 1) / PHASE_UNITS_PER_TICK
    // to float32 would otherwise yield exactly one.
    constexpr float32 LARGEST_ALPHA = 1.0F - 0x1.0p-24F;
    if (mMode != Mode::Playing)
    {
        return 1.0F;
    }
    const float64 alpha = static_cast<float64>(mTickPhase) / static_cast<float64>(PHASE_UNITS_PER_TICK);
    return foundation::math::Min(static_cast<float32>(alpha), LARGEST_ALPHA);
}
void TickDriver::dropTiming() noexcept
{
    mFrameClock.Reset();
    mTickPhase = 0;
    mInput.Cancel();
}
void TickDriver::recordFrame(foundation::time::Duration interval) noexcept
{
    using foundation::time::Duration;
    // Every observed interval is accepted, clamped or excluded; failed sums keep the previous totals.
    const uint64 accounted = mFrameSample.Elapsed.Nanoseconds + mFrameSample.Discarded.Nanoseconds;
    const Duration excluded{interval.Nanoseconds >= accounted ? interval.Nanoseconds - accounted : 0};
    (void)foundation::time::TryAdd(mAccepted, mFrameSample.Elapsed, mAccepted);
    (void)foundation::time::TryAdd(mClamped, mFrameSample.Discarded, mClamped);
    (void)foundation::time::TryAdd(mExcluded, excluded, mExcluded);
}
Status
TickDriver::Advance(GameWorld& inoutWorld, foundation::time::Timestamp now, TickInput input, bool isVisible) noexcept
{
    using foundation::time::Duration;
    using foundation::time::TimeStatus;
    // The high-water mark survives frame resets, so a regressed sample can never become a new baseline.
    Duration interval;
    const TimeStatus observed =
        mHasObserved == true ? foundation::time::TryElapsed(mObserved, now, interval) : TimeStatus::Ok;
    if (observed == TimeStatus::Ok)
    {
        mObserved = now;
        mHasObserved = true;
    }
    else
    {
        ++mClockRegressions;
    }
    mFrameSample = { .Status = observed };
    if (isVisible == false || mMode != Mode::Playing)
    {
        dropTiming();
        recordFrame(interval);
        return Status::Success;
    }
    mInput.Sample(input);
    if (observed == TimeStatus::Ok)
    {
        mFrameSample = mFrameClock.Sample(now);
    }
    recordFrame(interval);
    // The FrameClock is the only clamp; its discarded excess is already recorded. Configure proved that a
    // clamped frame plus the carried remainder fits, so these checks only guard that invariant.
    uint64 phase = 0;
    if (foundation::TryMultiply(mFrameSample.Elapsed.Nanoseconds, mPolicy.TicksPerSecond, phase) == false ||
        foundation::TryAdd(phase, mTickPhase, phase) == false)
    {
        SetMode(Mode::Faulted);
        return Status::InvalidConfiguration;
    }
    mTickPhase = phase;
    uint32 count = 0;
    while (mTickPhase >= PHASE_UNITS_PER_TICK && count < mPolicy.MaximumCatchUpTicks)
    {
        const Status status = inoutWorld.RunTick(mInput.Consume());
        if (status != Status::Success)
        {
            // The world did not publish the failed tick; the session stops with its debt dropped.
            SetMode(Mode::Faulted);
            return status;
        }
        mTickPhase -= PHASE_UNITS_PER_TICK;
        ++count;
    }
    // Discard whole ticks beyond the catch-up budget; only the fractional remainder carries.
    (void)foundation::TryAdd(mDiscardedTicks, mTickPhase / PHASE_UNITS_PER_TICK, mDiscardedTicks);
    mTickPhase %= PHASE_UNITS_PER_TICK;
    return Status::Success;
}
Status TickDriver::Step(GameWorld& world) noexcept
{
    if (mMode != Mode::Paused)
    {
        return Status::InvalidPhase;
    }
    const auto status = world.RunTick({});
    if (status != Status::Success)
    {
        SetMode(Mode::Faulted);
    }
    return status;
}
void TickDriver::SetMode(Mode mode) noexcept
{
    mMode = mode;
    dropTiming();
}
void TickDriver::Interrupt() noexcept
{
    dropTiming();
}
void TickDriver::Reset() noexcept
{
    mMode = Mode::Playing;
    dropTiming();
}
LevelResult Session::RequestLoad(std::string_view source) noexcept
{
    if (mTransition == std::numeric_limits<uint64>::max())
    {
        return {LevelError::Capacity, 0};
    }
    ++mTransition;
    mCandidate = GameWorld{};
    const auto result = ReadLevel(source, mDefinition);
    mStage = result.Error == LevelError::None ? LoadStage::Preparing : LoadStage::Failed;
    return result;
}
Status Session::PollLoad() noexcept
{
    Status status = Status::Success;
    if (mStage == LoadStage::Preparing)
    {
        status = mCandidate.Prepare(mDefinition, MAX_WORLD_ENTITIES, Seed);
        if (status == Status::Success)
        {
            mStage = LoadStage::Building;
        }
    }
    else if (mStage == LoadStage::Building)
    {
        status = mCandidate.BuildNext();
        if (status == Status::Success && mCandidate.IsBuilt())
        {
            mStage = LoadStage::Ready;
        }
    }
    if (status != Status::Success)
    {
        mCandidate = GameWorld{};
        mStage = LoadStage::Failed;
    }
    return status;
}
bool Session::Activate() noexcept
{
    if (mStage != LoadStage::Ready)
    {
        return false;
    }
    mActive = Move(mCandidate);
    mStage = LoadStage::Empty;
    Driver.Reset();
    return true;
}
void Session::CancelLoad() noexcept
{
    mCandidate = GameWorld{};
    mStage = LoadStage::Cancelled;
}
} // namespace ludus::world_demo
