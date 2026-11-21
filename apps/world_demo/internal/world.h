#pragma once
#include "addressed_mailbox.h"
#include "cancellation.h"
#include "level.h"
#include "session_flow.h"
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/random_checkpoint.hpp>
#include <ludus/foundation/time/timers.hpp>
#include <ludus/gameplay/camera/evaluation.hpp>
#include <ludus/gameplay/world/component_pool.hpp>
#include <ludus/gameplay/world/entity_registry.h>
#include <span>
namespace ludus::world_demo
{
using gameplay::world::EntityId;
using gameplay::world::Status;
namespace internal
{
struct WorldTestAccess;
}
struct Transform final
{
    Vec2 Current{};
    Vec2 Previous{};
    float32 Rotation = 0;
    Vec2 Scale{1, 1};
};
struct Motion final
{
    Vec2 Velocity{};
    float32 Speed = 0;
    uint64 Revision = 0;
    uint64 PendingRequest = 0;
    bool Override = false;
};
struct Collider final
{
    Vec2 HalfExtent{};
};
struct Visual final
{
    Vec2 HalfExtent{};
    float32 Color[4] = {};
};
struct Health final
{
    uint32 Current = 0;
};
struct PlayerState final
{
    uint64 AttackReady = 0;
};
enum class ActionPhase : uint8
{
    SelectTarget,
    Approach,
    Attack,
    Recover
};
struct EnemyState final
{
    EntityId Target{};
    uint64 WakeTick = 0;
    ActionPhase Phase = ActionPhase::SelectTarget;
};
struct ExitState final
{
    Name NextLevel{};
    bool Entered = false;
    bool Touching = false;
};
struct TickInput final
{
    Vec2 Axis{};
    bool Held = false;
    bool Pressed = false;
    bool Released = false;
    bool Cancelled = false;
};
class InputBridge final
{
public:
    void Sample(TickInput input) noexcept;
    void Cancel() noexcept;
    [[nodiscard]] TickInput Consume() noexcept;

private:
    TickInput mPending;
    bool mGate = false;
};
enum class EventKind : uint8
{
    Damage,
    Death,
    Exit,
    Spawn
};
struct PresentationEvent final
{
    uint64 World = 0;
    uint64 Tick = 0;
    uint64 Sequence = 0;
    EventKind Kind = EventKind::Damage;
    Vec2 Anchor{};
};
struct Draw final
{
    Vec2 Position{};
    Vec2 HalfExtent{};
    float32 Rotation = 0;
    float32 Color[4] = {};
};
/// Most entities one world reserves.
inline constexpr uint32 MAX_WORLD_ENTITIES = 48;
/// Most collision boxes one level authors.
inline constexpr usize MAX_WORLD_COLLISION = 16;
/// Channels of a draw colour: linear RGB followed by alpha.
inline constexpr usize DRAW_COLOR_CHANNELS = 4;
/// Draws Extract emits before the collision boxes: the room floor over the level bounds.
inline constexpr usize ROOM_FLOOR_DRAWS = 1;
/// Linear RGBA of the room floor; outside the level bounds the renderer's clear shows.
inline constexpr float32 ROOM_FLOOR_COLOR[DRAW_COLOR_CHANNELS] = {0.025F, 0.04F, 0.08F, 1};
/// Render records: the floor, every entity, every collision box and every placement of a full room.
inline constexpr usize MAX_RENDER_DRAWS =
    ROOM_FLOOR_DRAWS + MAX_WORLD_ENTITIES + MAX_WORLD_COLLISION + MAX_ROOM_PLACEMENTS;
struct RenderFrame final
{
    Draw Draws[MAX_RENDER_DRAWS] = {};
    usize Count = 0;
    gameplay::camera::CameraSample Camera{};
    gameplay::camera::CameraTrace CameraTrace{};
    uint64 Tick = 0;
};
struct SpawnOutcome final
{
    uint64 Request = 0;
    EntityId Entity{};
};
enum class CompletionState : uint8
{
    Ready,
    Cancelled,
    Failed
};
struct Completion final
{
    EntityId Entity{};
    uint64 Request = 0;
    uint64 Revision = 0;
    Vec2 Velocity{};
    CompletionState State = CompletionState::Ready;
};
enum class ContactPhase : uint8
{
    Enter,
    Stay,
    Exit
};
struct ContactEvent final
{
    EntityId First;
    EntityId Second;
    ContactPhase Kind = ContactPhase::Enter;
};
enum class Phase : uint8
{
    Construction,
    BeginTick,
    Intent,
    Motion,
    Gameplay,
    Commit,
    Idle,
    Faulted
};
struct TickTrace final
{
    uint64 Tick = 0;
    TickInput Input;
    Phase LastPhase = Phase::Idle;
    Status Result = Status::Success;
    usize Commands = 0;
    usize Contacts = 0;
    usize Presentation = 0;
    usize Completions = 0;
};
/// Fixed default session seed; a session records the seed it actually used.
inline constexpr uint64 DEFAULT_WORLD_SEED = 0x4c75647573000001ULL;
/// The session's root seed for addressed randomness (docs/architecture/randomness.md#milestone-1-procedural-placement).
/// Every room derives its scatter keys from it; a level's authored Seed stays content data.
struct WorldSeed final
{
    uint64 Value = DEFAULT_WORLD_SEED;
};
/// One accepted scatter placement, kept as compact data rather than an entity.
struct Placement final
{
    /// Packed identity: room, layer ID, candidate, cell X, cell Y (ProceduralEnvironment's PackScatterScope).
    uint64 Scope = 0;
    /// Position in lattice units; divide by the room's UnitsPerMetre for metres.
    int32 LatticeX = 0;
    int32 LatticeY = 0;
    /// Rotation in 1/65536 turns.
    uint16 Rotation = 0;
    /// Scale in SCATTER_SCALE_UNIT thousandths.
    uint16 Scale = 0;
    /// Index into Level::Scatter.Layers.
    uint8 Layer = 0;
    /// Index into that layer's Variants: the base generation variant (content folded into the golden digest).
    uint8 Variant = 0;
    [[nodiscard]] constexpr bool operator==(const Placement&) const noexcept = default;
};
/// Dimension of a regrow variant draw in the regrow domain; the ordinal is the address event.
inline constexpr uint16 REGROW_VARIANT_DIMENSION = 0;
/// Ticks after a committed harvest before a placement becomes eligible to regrow (one second at TICKS_PER_SECOND).
inline constexpr uint64 REGROW_DELAY_TICKS = 60;
/// Harvest and regrow state of one accepted placement, keyed positionally to the placement's packed scatter scope.
///
/// A tombstone is a world mutation, not a random event; only a committed regrow advances RegrowOrdinal and draws a
/// variant (docs/architecture/randomness.md#milestone-1-procedural-placement).
struct PlacementState final
{
    /// Current variant index to draw: the base variant until a regrow commits a new one.
    uint8 Variant = 0;
    /// Regrow ordinal, starting at 1; the next committed regrow draws (regrow domain, scope, RegrowOrdinal, 0),
    /// then advances this once. It never wraps past UINT32_MAX.
    uint32 RegrowOrdinal = 1;
    /// Whether a tombstone suppresses the placement (harvested and not yet regrown).
    bool IsHarvested = false;
    /// Tick at which a harvested placement becomes eligible to regrow; meaningful only while IsHarvested.
    uint64 RegrowAtTick = 0;
    [[nodiscard]] constexpr bool operator==(const PlacementState&) const noexcept = default;
};
/// One session-persistent harvest record, keyed by a placement's packed scatter scope, so a harvested placement
/// stays harvested across leaving and re-entering its room within a session.
struct Tombstone final
{
    /// Packed scatter scope (persistent identity) of the placement.
    uint64 Scope = 0;
    /// Regrow ordinal carried across the room round-trip.
    uint32 RegrowOrdinal = 1;
    /// Current variant carried across the room round-trip.
    uint8 Variant = 0;
    /// Whether the placement is currently harvested.
    bool IsHarvested = false;
};
/// Most addressed commands one world holds at once: pending, captured, and delivered with an outcome not yet
/// consumed. Admission reserves the eventual outcome, so outcome publication never overflows.
inline constexpr usize MAX_ADDRESSED_COMMANDS = 16;
/// Most open command cancellation scopes in one world.
inline constexpr usize MAX_COMMAND_SCOPES = 8;
/// Largest health decrement one damage command may request.
inline constexpr uint32 MAX_DAMAGE_AMOUNT = 8;
/// Typed payload of an addressed damage command.
struct DamageCommand final
{
    /// Health units to remove, from 1 to MAX_DAMAGE_AMOUNT; destruction at zero follows the combat rules.
    uint32 Amount = 1;
};
/// Outcome of one admitted command; every admitted token receives exactly one.
enum class CommandResult : uint8
{
    /// The command was applied in the Gameplay phase of a committed tick.
    Applied,
    /// The target was destroyed, pending destruction or otherwise inactive at delivery.
    StaleTarget,
    /// The target was active but lacks the capability the command needs.
    MissingCapability,
    /// The command's cancellation scope was cancelled or closed before delivery.
    Cancelled,
    /// The world was replaced before the command's outcome was published.
    Superseded,
};
/// Published outcome of one addressed command.
struct CommandOutcome final
{
    /// Token returned by admission.
    uint64 Token = 0;
    EntityId Target{};
    CommandResult Result = CommandResult::Applied;
    /// Completed tick that resolved the command; for Superseded, the replaced world's last completed tick.
    uint64 Tick = 0;
};
/// The world's damage mailbox: one typed domain buffer.
using DamageMailbox = AddressedMailbox<DamageCommand, MAX_ADDRESSED_COMMANDS>;
/// The world's command cancellation scopes.
using CommandScopes = CancellationScopes<MAX_COMMAND_SCOPES>;
/// Session tombstone capacity: one record per placement of every catalog room.
inline constexpr usize MAX_SESSION_TOMBSTONES = MAX_ROOM_PLACEMENTS * 2;
/// Leading word of a session checkpoint: the bytes `LWDS` in file order.
inline constexpr uint32 SESSION_CHECKPOINT_MAGIC = 0x5344574cu;
/// Version of the session checkpoint envelope below.
inline constexpr uint32 SESSION_CHECKPOINT_VERSION = 1;
/// uint32 envelope fields before the random checkpoint: magic, version and total bytes.
inline constexpr usize SESSION_CHECKPOINT_ENVELOPE_FIELDS = 3;
/// Encoded bytes of one tombstone override: packed scope uint64, variant uint8 and harvested flag uint8.
inline constexpr usize SESSION_TOMBSTONE_OVERRIDE_BYTES = sizeof(uint64) + sizeof(uint8) + sizeof(uint8);
/// @brief Largest session checkpoint: the envelope, a random checkpoint with one regrow counter per tombstone, the
/// override count and one override per tombstone. Session::TrySaveCheckpoint never writes more.
inline constexpr usize MAX_SESSION_CHECKPOINT_BYTES =
    SESSION_CHECKPOINT_ENVELOPE_FIELDS * sizeof(uint32) + foundation::math::RANDOM_CHECKPOINT_HEADER_BYTES +
    MAX_SESSION_TOMBSTONES * foundation::math::RANDOM_CHECKPOINT_EVENT_RECORD_BYTES + sizeof(uint32) +
    MAX_SESSION_TOMBSTONES * SESSION_TOMBSTONE_OVERRIDE_BYTES;
class GameWorld final
{
public:
    /// Validate the level, reserve pool capacity and, when the level authors a scatter, generate the room's
    /// placements from `worldSeed` and the checked-in room catalog, then reapply any session tombstones. The
    /// only allocations happen here.
    /// @param level The validated level to prepare.
    /// @param capacity Entity capacity, at least the level's entity count and at most MAX_WORLD_ENTITIES.
    /// @param worldSeed The session root seed recorded with the world; drives every scatter draw.
    /// @param tombstones Session harvest records for this room, reapplied by packed scope after generation;
    ///        empty by default. Scopes with no placement are ignored.
    /// @return InvalidConfiguration for an invalid level, capacity, unknown room or unusable recipe, or a tombstone
    /// whose variant is outside its placement's layer or whose regrow ordinal is zero;
    /// CapacityExceeded when the recipe enumerates more candidates or accepts more placements than the room
    /// limits; AllocationFailure when storage is unavailable. Nothing is published on failure.
    [[nodiscard]] Status Prepare(const Level& level,
                                 uint32 capacity = MAX_WORLD_ENTITIES,
                                 WorldSeed worldSeed = {},
                                 foundation::BorrowedView<const Tombstone> tombstones = {}) noexcept;
    [[nodiscard]] Status BuildNext() noexcept;
    [[nodiscard]] bool IsBuilt() const noexcept
    {
        return mBuilt == mLevel.EntityCount && mRegistry.GetWorld() != 0;
    }
    [[nodiscard]] Status RunTick(TickInput input) noexcept;
    [[nodiscard]] Status QueueSpawn(Recipe recipe, uint64& request) noexcept;
    [[nodiscard]] Status QueueDestroy(EntityId entity) noexcept;
    [[nodiscard]] Status SubmitCompletion(Completion completion) noexcept;
    [[nodiscard]] Status BeginRequest(EntityId entity, Completion& ticket) noexcept;
    /// Admit an addressed damage command at an idle boundary for the next tick.
    ///
    /// The next BeginTick captures every admitted command; the Gameplay phase delivers the batch in MessageKey order
    /// before player and enemy combat, revalidating the target and the cancellation scope. The outcome publishes only
    /// when that tick commits; a faulted tick publishes none, and the command stays unresolved until the world is
    /// retired (RetireCommands). Paused worlds hold admitted commands until a tick runs.
    /// @param target Active entity of this world.
    /// @param command Owned payload; Amount must be from 1 to MAX_DAMAGE_AMOUNT.
    /// @param scope The default token, or an open scope from OpenCommandScope.
    /// @param outToken Receives the command token; unchanged on failure.
    /// @return InvalidPhase outside an idle built world; InvalidEntity for a foreign or inactive target;
    /// InvalidConfiguration for an out-of-range amount or a closed scope; CapacityExceeded when commands and
    /// unconsumed outcomes fill MAX_ADDRESSED_COMMANDS; IdentityExhausted when tokens would wrap.
    [[nodiscard]] Status
    PostDamage(EntityId target, DamageCommand command, CancellationToken scope, uint64& outToken) noexcept;
    /// Open a command cancellation scope at an idle boundary.
    /// @param owner The default EntityId, or an active entity whose committed destruction cancels the scope.
    /// @param outToken Receives the scope token; unchanged on failure.
    /// @return InvalidPhase outside an idle world; InvalidEntity for an inactive owner; CapacityExceeded when
    /// MAX_COMMAND_SCOPES are open.
    [[nodiscard]] Status OpenCommandScope(EntityId owner, CancellationToken& outToken) noexcept;
    /// Cancel a command scope; captured and pending commands carrying it resolve as Cancelled at delivery.
    /// @return true only for the call that cancelled the open scope.
    [[nodiscard]] bool CancelCommandScope(CancellationToken token) noexcept;
    /// Close a command scope; commands still carrying it resolve as Cancelled.
    /// @return InvalidEntity for a closed or stale token.
    [[nodiscard]] Status CloseCommandScope(CancellationToken token) noexcept;
    /// Whether a command scope is cancelled or closed.
    [[nodiscard]] bool IsCommandScopeCancelled(CancellationToken token) const noexcept
    {
        return mCommandScopes.IsCancelled(token);
    }
    /// Outcomes of committed ticks not yet consumed, in tick then delivery order.
    [[nodiscard]] foundation::BorrowedView<const CommandOutcome> GetCommandOutcomes() const noexcept
    {
        return {mCommandOutcomes.GetData(), mCommandOutcomeCount};
    }
    /// Consume the published command outcomes, releasing their reserved admission capacity.
    void ConsumeCommandOutcomes() noexcept
    {
        mCommandOutcomeCount = 0;
    }
    /// Commands admitted or captured and not yet resolved by a committed tick.
    [[nodiscard]] usize GetUnresolvedCommandCount() const noexcept
    {
        return mDamageMailbox.GetOccupancy();
    }
    /// Unconsumed outcomes plus unresolved commands: what RetireCommands writes.
    [[nodiscard]] usize GetRetiringCommandCount() const noexcept
    {
        return mCommandOutcomeCount + mDamageMailbox.GetOccupancy();
    }
    /// Retire every command before the world is replaced: copy the unconsumed outcomes, then resolve each
    /// unresolved command as Superseded in delivery order, and leave the world with none of either.
    /// @param outOutcomes Receives GetRetiringCommandCount outcomes; must hold at least that many.
    /// @return CapacityExceeded, changing nothing, when outOutcomes is too small.
    [[nodiscard]] Status RetireCommands(foundation::BorrowedView<CommandOutcome> outOutcomes) noexcept;
    [[nodiscard]] bool Extract(float32 alpha,
                               RenderFrame& frame,
                               bool followPlayer = false,
                               uint64 presentationSequence = 0) const noexcept;
    [[nodiscard]] EntityId FindAuthored(std::string_view id) const noexcept;
    [[nodiscard]] const gameplay::world::EntityRegistry& Registry() const noexcept
    {
        return mRegistry;
    }
    [[nodiscard]] const gameplay::world::ComponentPool<Transform>& Transforms() const noexcept
    {
        return mTransforms;
    }
    [[nodiscard]] const gameplay::world::ComponentPool<EnemyState>& Enemies() const noexcept
    {
        return mEnemies;
    }
    [[nodiscard]] const gameplay::world::ComponentPool<Health>& HealthValues() const noexcept
    {
        return mHealth;
    }
    [[nodiscard]] std::span<const PresentationEvent> GetOutbox() const noexcept
    {
        return {mOutbox, mOutboxCount};
    }
    [[nodiscard]] std::span<const SpawnOutcome> GetSpawnOutcomes() const noexcept
    {
        return {mOutcomes, mOutcomeCount};
    }
    void ConsumeOutbox() noexcept
    {
        mOutboxCount = 0;
    }
    [[nodiscard]] std::span<const TickTrace> GetTraceRing() const noexcept
    {
        return {mTrace, mTraceCount};
    }
    [[nodiscard]] std::span<const ContactEvent> GetContacts() const noexcept
    {
        return {mContacts, mContactCount};
    }
    [[nodiscard]] uint64 GetTick() const noexcept
    {
        return mTick;
    }
    [[nodiscard]] uint64 GetReplayHash() const noexcept;
    [[nodiscard]] Phase GetPhase() const noexcept
    {
        return mPhase;
    }
    /// Placements generated by Prepare, in layer order then the kernel's canonical cell order.
    [[nodiscard]] std::span<const Placement> GetPlacements() const noexcept
    {
        return {mPlacements.GetData(), mPlacements.GetSize()};
    }
    /// FNV-1a digest of GetPlacements in the scatter golden's field order; zero without placements.
    [[nodiscard]] uint64 GetPlacementDigest() const noexcept
    {
        return mPlacementDigest;
    }
    /// Harvest and regrow state of each placement, positionally keyed to GetPlacements.
    [[nodiscard]] std::span<const PlacementState> GetPlacementStates() const noexcept
    {
        return {mPlacementStates.GetData(), mPlacementStates.GetSize()};
    }
    /// Regrows that failed because the ordinal reached UINT32_MAX; the placements stayed harvested.
    [[nodiscard]] uint64 GetRegrowExhaustions() const noexcept
    {
        return mRegrowExhaustions;
    }
    /// The seed Prepare recorded.
    [[nodiscard]] WorldSeed GetWorldSeed() const noexcept
    {
        return mWorldSeed;
    }
    /// Catalog ordinal of the prepared room; zero when the level authors no scatter.
    [[nodiscard]] uint16 GetRoom() const noexcept
    {
        return mRoom;
    }
    [[nodiscard]] const Name& GetNextLevel() const noexcept
    {
        return mNextLevel;
    }
    [[nodiscard]] uint64 GetStaleCompletions() const noexcept
    {
        return mStaleCompletions;
    }
    [[nodiscard]] uint64 GetDroppedPresentation() const noexcept
    {
        return mDroppedPresentation;
    }

private:
    friend struct internal::WorldTestAccess;
    enum class CommandKind : uint8
    {
        Spawn,
        Destroy
    };
    struct Command final
    {
        CommandKind Kind = CommandKind::Spawn;
        Recipe Spawn{};
        EntityId Entity{};
        uint64 Request = 0;
    };
    [[nodiscard]] Status Spawn(const Recipe& recipe, EntityId& output) noexcept;
    [[nodiscard]] Status prepareScatter(WorldSeed worldSeed,
                                        foundation::BorrowedView<const Tombstone> tombstones) noexcept;
    void harvestAndSchedule() noexcept;
    void commitHarvestAndRegrow() noexcept;
    [[nodiscard]] Status Commit() noexcept;
    void Intent(TickInput input) noexcept;
    void MoveAndCollide() noexcept;
    [[nodiscard]] Status Gameplay(TickInput input, Name& nextLevel) noexcept;
    void Stage(EventKind kind, Vec2 anchor) noexcept;
    [[nodiscard]] Status Damage(EntityId entity) noexcept;
    /// Deliver the captured damage batch in key order and stage one outcome per command.
    [[nodiscard]] Status deliverCommands() noexcept;
    /// Publish the staged outcomes and release the captured batch after a committed tick.
    void publishCommandOutcomes() noexcept;
    gameplay::world::EntityRegistry mRegistry;
    gameplay::world::ComponentPool<Transform> mTransforms;
    gameplay::world::ComponentPool<Motion> mMotion;
    gameplay::world::ComponentPool<Collider> mColliders;
    gameplay::world::ComponentPool<Visual> mVisuals;
    gameplay::world::ComponentPool<Health> mHealth;
    gameplay::world::ComponentPool<PlayerState> mPlayers;
    gameplay::world::ComponentPool<EnemyState> mEnemies;
    gameplay::world::ComponentPool<ExitState> mExits;
    Level mLevel;
    EntityId mAuthored[32] = {};
    EntityId mPlayer;
    usize mBuilt = 0;
    Command mCommands[32] = {};
    usize mCommandCount = 0;
    SpawnOutcome mOutcomes[32] = {};
    usize mOutcomeCount = 0;
    PresentationEvent mStaged[64] = {};
    usize mStagedCount = 0;
    PresentationEvent mOutbox[64] = {};
    usize mOutboxCount = 0;
    struct RequestLease final
    {
        uint64 Id = 0;
        EntityId Entity;
        uint64 Revision = 0;
        uint64 CreationSequence = 0;
        bool Completed = false;
    };
    RequestLease mRequests[16] = {};
    Completion mCompletions[16] = {};
    usize mCompletionCount = 0;
    ContactEvent mContacts[32] = {};
    usize mContactCount = 0;
    TickTrace mTrace[64] = {};
    usize mTraceCount = 0;
    usize mTraceNext = 0;
    uint64 mNextRequest = 1;
    uint64 mTick = 0;
    uint64 mStaleCompletions = 0;
    uint64 mDroppedPresentation = 0;
    Phase mPhase = Phase::Construction;
    Name mNextLevel;
    foundation::Array<Placement> mPlacements;
    foundation::Array<PlacementState> mPlacementStates;
    uint64 mPlacementDigest = 0;
    uint64 mCosmeticKey = 0;
    foundation::math::RandomKey mRegrowKey;
    foundation::math::PreparedBound32 mRegrowVariantBound;
    uint8 mHarvestableLayer = 0;
    bool mHasHarvestableLayer = false;
    uint64 mRegrowExhaustions = 0;
    usize mPendingHarvests[MAX_ROOM_PLACEMENTS] = {};
    usize mPendingHarvestCount = 0;
    usize mPendingRegrows[MAX_ROOM_PLACEMENTS] = {};
    usize mPendingRegrowCount = 0;
    WorldSeed mWorldSeed;
    uint16 mRoom = 0;
    DamageMailbox mDamageMailbox;
    CommandScopes mCommandScopes;
    /// Outcomes staged by the running tick; published only when it commits.
    foundation::StaticArray<CommandOutcome, MAX_ADDRESSED_COMMANDS> mStagedCommandOutcomes{};
    usize mStagedCommandOutcomeCount = 0;
    foundation::StaticArray<CommandOutcome, MAX_ADDRESSED_COMMANDS> mCommandOutcomes{};
    usize mCommandOutcomeCount = 0;
};
enum class Mode : uint8
{
    Playing,
    Paused,
    Faulted
};
/// Largest presentation interval accepted as simulation debt; the excess is reported as discarded.
inline constexpr foundation::time::Duration MAXIMUM_FRAME_DELTA{foundation::time::NANOSECONDS_PER_SECOND / 4};
/// Simulation rate the world reference's RunTick step is authored for: each tick advances 1/60 second.
inline constexpr uint64 TICKS_PER_SECOND = 60;
/// Most ticks one presentation callback may run before whole-tick debt is discarded.
inline constexpr uint32 MAXIMUM_CATCH_UP_TICKS = 4;
/// Phase units one tick consumes. A frame adds elapsed nanoseconds times ticks per second,
/// so a tick is exactly 1/rate second without rounding the tick length to whole nanoseconds.
inline constexpr uint64 PHASE_UNITS_PER_TICK = foundation::time::NANOSECONDS_PER_SECOND;
/// Fixed-step policy; record it with replay metadata. RunTick's step stays 1/TICKS_PER_SECOND,
/// so another rate changes the simulation's real-time speed rather than its step.
struct TickPolicy final
{
    /// Simulation ticks per second of accepted frame time; must be nonzero.
    uint64 TicksPerSecond = TICKS_PER_SECOND;
    /// FrameClock cap: the only clamp on a presentation interval. Zero freezes accepted time.
    foundation::time::Duration MaximumFrameDelta = MAXIMUM_FRAME_DELTA;
    /// Catch-up budget per presentation callback; must be nonzero.
    uint32 MaximumCatchUpTicks = MAXIMUM_CATCH_UP_TICKS;
};
/// Frame-to-simulation boundary: owns the one FrameClock, so every debt reset also drops the frame baseline.
/// Pause/resume, suspension, faults and session replacement therefore never catch up excluded time.
/// Debt is a rational phase in checked uint64 units (see PHASE_UNITS_PER_TICK), so tick and remainder
/// accounting is exact and independent of how accepted time is partitioned into frames.
class TickDriver final
{
public:
    /// Validate and adopt a policy, then drop debt, pending input edges and the frame baseline.
    /// The largest frame contribution plus the largest carried remainder,
    /// MaximumFrameDelta * TicksPerSecond + PHASE_UNITS_PER_TICK - 1, must fit uint64.
    /// @return InvalidConfiguration for a zero rate, a zero catch-up budget or that overflow;
    /// failure preserves the previous policy and all timing state.
    [[nodiscard]] Status Configure(const TickPolicy& policy) noexcept;
    /// Sample the timestamp read once at this presentation callback and run bounded fixed ticks.
    /// Hidden or non-Playing frames reset debt, input edges and the frame baseline instead.
    /// A sample below the highest observed timestamp counts a regression and adds no time.
    /// Each successful tick consumes PHASE_UNITS_PER_TICK; whole ticks beyond the catch-up budget are
    /// discarded and counted, and only the fractional remainder carries to the next frame.
    /// @return A failed tick's status; the driver then enters Faulted and drops its debt. The world's
    /// completed tick advances only for successful ticks, so tick exhaustion stops the session.
    [[nodiscard]] Status
    Advance(GameWorld& inoutWorld, foundation::time::Timestamp now, TickInput input, bool isVisible = true) noexcept;
    [[nodiscard]] Status Step(GameWorld& world) noexcept;
    void SetMode(Mode mode) noexcept;
    /// Drop debt, pending input edges and the frame baseline without changing mode.
    void Interrupt() noexcept;
    /// Return to Playing and drop debt, pending input edges and the frame baseline; the policy is kept.
    void Reset() noexcept;
    [[nodiscard]] Mode GetMode() const noexcept
    {
        return mMode;
    }
    /// Interpolation factor in [0, 1) derived from the carried phase remainder while Playing;
    /// 1 (exact current transforms) otherwise.
    [[nodiscard]] float32 GetAlpha() const noexcept;
    /// Policy adopted by the latest successful Configure (the defaults until then).
    [[nodiscard]] LUDUS_INLINE const TickPolicy& GetPolicy() const noexcept
    {
        return mPolicy;
    }
    /// Carried fractional debt in phase units, always below PHASE_UNITS_PER_TICK between callbacks.
    [[nodiscard]] LUDUS_INLINE uint64 GetTickPhase() const noexcept
    {
        return mTickPhase;
    }
    /// Whole ticks of accepted time discarded because they exceeded the catch-up budget.
    /// Reported separately from GetClampedTime, the frame clamp applied before accumulation.
    [[nodiscard]] LUDUS_INLINE uint64 GetDiscardedTicks() const noexcept
    {
        return mDiscardedTicks;
    }
    /// Sample from the latest Advance, for presentation and diagnostics.
    [[nodiscard]] LUDUS_INLINE const foundation::time::FrameSample& GetFrameSample() const noexcept
    {
        return mFrameSample;
    }
    /// Total frame time accepted as simulation debt.
    [[nodiscard]] LUDUS_INLINE foundation::time::Duration GetAcceptedTime() const noexcept
    {
        return mAccepted;
    }
    /// Total frame time beyond the policy's MaximumFrameDelta, discarded before accumulation.
    [[nodiscard]] LUDUS_INLINE foundation::time::Duration GetClampedTime() const noexcept
    {
        return mClamped;
    }
    /// Time between observed frames excluded by baselines, pause, suspension, faults or replacement.
    [[nodiscard]] LUDUS_INLINE foundation::time::Duration GetExcludedTime() const noexcept
    {
        return mExcluded;
    }
    /// Number of Advance timestamps below the highest observed timestamp.
    [[nodiscard]] LUDUS_INLINE uint64 GetClockRegressions() const noexcept
    {
        return mClockRegressions;
    }

private:
    void dropTiming() noexcept;
    void recordFrame(foundation::time::Duration interval) noexcept;
    InputBridge mInput;
    TickPolicy mPolicy;
    foundation::time::FrameClock mFrameClock{MAXIMUM_FRAME_DELTA};
    foundation::time::FrameSample mFrameSample;
    foundation::time::Timestamp mObserved;
    foundation::time::Duration mAccepted;
    foundation::time::Duration mClamped;
    foundation::time::Duration mExcluded;
    uint64 mClockRegressions = 0;
    uint64 mTickPhase = 0;
    uint64 mDiscardedTicks = 0;
    bool mHasObserved = false;
    Mode mMode = Mode::Playing;
};
enum class LoadStage : uint8
{
    Empty,
    Preparing,
    Building,
    Ready,
    Failed,
    Cancelled
};
/// Most flow cancellation scopes: the load the Preparing state owns and its preflighted replacement.
inline constexpr usize FLOW_SCOPE_CAPACITY = 2;
/// The session: active and candidate worlds, the tick driver and the game flow state machine.
///
/// Two control surfaces share it. A game drives the flow: RequestFlow, then ProcessFlowBoundary once per frame
/// between ticks, with PollLoad advancing a candidate. Tools and headless runners may instead call RequestLoad,
/// PollLoad and Activate directly; they then leave the flow in Boot. Do not mix both surfaces in one session.
class Session final
{
public:
    [[nodiscard]] LevelResult RequestLoad(std::string_view source) noexcept;
    [[nodiscard]] Status PollLoad() noexcept;
    /// Replace the active world with the ready candidate at a frame boundary. Retires the old world's commands
    /// first (GetRetiredCommandOutcomes), so every admitted command keeps exactly one outcome.
    /// @return false, changing nothing, without a ready candidate or when the retired outcomes would not fit
    /// beside unconsumed ones.
    [[nodiscard]] bool Activate() noexcept;
    /// Borrow the source that LevelSource::Provided loads resolve to. The caller keeps the bytes alive while a
    /// load request may still be preflighted from it.
    void SetProvidedLevel(std::string_view source) noexcept
    {
        mProvidedLevel = source;
    }
    /// Admit a flow request for the next ProcessFlowBoundary; see SessionFlow::Request.
    [[nodiscard]] Status RequestFlow(const FlowRequest& request, uint64& outToken) noexcept
    {
        return mFlow.Request(request, outToken);
    }
    /// Run one flow boundary between ticks.
    ///
    /// Captures the admitted requests, observes completed state (a ready or failed candidate while Preparing, an
    /// authoritative fault, and a level outcome while Playing), selects at most one transition and preflights it
    /// before any exit action runs. A Load preflight parses its source and reserves the next load's cancellation
    /// scope; an activation preflight checks the candidate and the retired outcome capacity. A failed preflight
    /// leaves the state active. Otherwise the session exits leaf to Session superstate, performs the transition,
    /// then enters superstate to leaf: Preparing's exit cancels its owned load exactly once unless the candidate
    /// activated or failed; entries apply the state's FlowPolicy to the tick driver. Requests made during the
    /// boundary are eligible at the next one; outcomes are published in GetFlow().GetOutcomes().
    /// @return The SessionFlow::BeginBoundary failure; Success otherwise.
    [[nodiscard]] Status ProcessFlowBoundary() noexcept;
    /// Advance exactly one fixed tick while the flow is Paused.
    /// @return InvalidPhase in every other flow state; otherwise TickDriver::Step's status, whose fault the next
    /// boundary observes.
    [[nodiscard]] Status StepFlow() noexcept;
    [[nodiscard]] LUDUS_INLINE FlowState GetFlowState() const noexcept
    {
        return mFlow.GetState();
    }
    /// Policy snapshot of the current flow state.
    [[nodiscard]] FlowPolicy GetFlowPolicy() const noexcept
    {
        return world_demo::GetFlowPolicy(mFlow.GetState());
    }
    /// Request batching, outcomes and change records.
    [[nodiscard]] LUDUS_INLINE const SessionFlow& GetFlow() const noexcept
    {
        return mFlow;
    }
    /// Loads the flow cancelled by leaving Preparing, each counted once.
    [[nodiscard]] LUDUS_INLINE uint64 GetCancelledLoads() const noexcept
    {
        return mCancelledLoads;
    }
    /// Outcomes retired from replaced worlds: their unconsumed outcomes and Superseded for unresolved commands.
    [[nodiscard]] foundation::BorrowedView<const CommandOutcome> GetRetiredCommandOutcomes() const noexcept
    {
        return {mRetiredOutcomes.GetData(), mRetiredOutcomeCount};
    }
    /// Consume the retired outcomes so the next activation can retire more.
    void ConsumeRetiredCommandOutcomes() noexcept
    {
        mRetiredOutcomeCount = 0;
    }
    void CancelLoad() noexcept;
    [[nodiscard]] GameWorld& World() noexcept
    {
        return mActive;
    }
    [[nodiscard]] const GameWorld& World() const noexcept
    {
        return mActive;
    }
    [[nodiscard]] LoadStage GetLoadStage() const noexcept
    {
        return mStage;
    }
    [[nodiscard]] uint64 GetTransition() const noexcept
    {
        return mTransition;
    }
    TickDriver Driver;
    /// Root seed the next candidate world is prepared with; record it with the session.
    WorldSeed Seed;
    /// Session harvest records across every visited room, so tombstones survive leaving and re-entering a room.
    [[nodiscard]] std::span<const Tombstone> GetTombstones() const noexcept
    {
        return {mTombstones, mTombstoneCount};
    }
    /// @brief Identity of the content a session checkpoint belongs to: the random domain manifest, the room
    /// catalog's ordinals and the scatter layout and regrow address versions. A checkpoint from other content is
    /// rejected rather than reinterpreted.
    [[nodiscard]] static uint64 GetCheckpointContentIdentity() noexcept;
    /// @brief Save the session's random state: the root seed, each tombstone's regrow ordinal as an event counter of
    /// the regrow domain, and each tombstone's harvest and variant override.
    ///
    /// The active world's placements are first recorded into the tombstone store, as leaving its room would.
    /// Layout: "Session checkpoints" in docs/examples/world-demo.md.
    /// @param destination Receives the checkpoint in its first bytes; at most MAX_SESSION_CHECKPOINT_BYTES are needed.
    /// @param outWritten Receives the encoded length on success; unchanged on failure.
    /// @return Success, or CapacityExceeded when destination is too short, with destination untouched.
    [[nodiscard]] foundation::math::RandomCheckpointStatus TrySaveCheckpoint(std::span<uint8> destination,
                                                                             usize& outWritten) noexcept;
    /// @brief Restore a checkpoint written by TrySaveCheckpoint, as a reload: the seed and tombstone store are
    /// replaced together, the active world and any load in progress are discarded, and the next RequestLoad
    /// regenerates its room from the restored state.
    ///
    /// The whole checkpoint is validated first, so any rejection leaves the session unchanged. The discarded active
    /// world's commands are retired as on Activate (GetRetiredCommandOutcomes).
    /// @param source The checkpoint bytes; bytes after its recorded length are ignored.
    /// @return Success; Malformed or UnsupportedFormat for an unknown envelope; the random checkpoint's rejection
    /// (UnsupportedContract, IncompatibleContent, ...); OwnerMismatch when a counter has no matching override or
    /// belongs to another domain; InvalidRecord for a zero ordinal or a harvested flag other than 0 or 1;
    /// CapacityExceeded when the retired outcomes would not fit beside unconsumed ones.
    [[nodiscard]] foundation::math::RandomCheckpointStatus TryRestoreCheckpoint(std::span<const uint8> source) noexcept;

private:
    /// Snapshot the active world's harvested/regrown placements into the session tombstone store, keyed by scope.
    void recordTombstones() noexcept;
    [[nodiscard]] bool canActivate() const noexcept;
    /// Raise the boundary's observations from completed state.
    void observeFlow() noexcept;
    /// Check and reserve everything the selected transition needs before any exit action.
    [[nodiscard]] bool preflightTransition(const FlowTransition& transition) noexcept;
    /// Parse the load's source into the staged definition and reserve its cancellation scope.
    [[nodiscard]] bool preflightLoad(const LevelReference& level) noexcept;
    /// Run exit actions, the transition action and entry actions; cannot fail after preflight.
    void executeTransition(const FlowTransition& transition) noexcept;
    /// Begin the candidate preflightLoad staged, owned by the reserved scope.
    void startStagedLoad() noexcept;
    void enterFlowState(FlowState state) noexcept;
    GameWorld mActive;
    GameWorld mCandidate;
    Level mDefinition;
    LoadStage mStage = LoadStage::Empty;
    uint64 mTransition = 0;
    /// Bounded tombstone store across visited rooms; a scope's entry is updated in place, so it is at most one
    /// per placement of every catalog room.
    Tombstone mTombstones[MAX_SESSION_TOMBSTONES] = {};
    usize mTombstoneCount = 0;
    SessionFlow mFlow;
    CancellationScopes<FLOW_SCOPE_CAPACITY> mFlowScopes;
    /// Scope owned by Preparing for its in-flight load.
    CancellationToken mLoadScope{};
    /// Scope a Load preflight reserved for the next load.
    CancellationToken mPreflightScope{};
    /// Definition a Load preflight parsed; adopted only when the transition is applied.
    Level mStagedDefinition;
    std::string_view mProvidedLevel;
    foundation::StaticArray<CommandOutcome, MAX_ADDRESSED_COMMANDS> mRetiredOutcomes{};
    usize mRetiredOutcomeCount = 0;
    uint64 mCancelledLoads = 0;
    /// Whether the active world's level outcome already raised a Load.
    bool mHasObservedNextLevel = false;
};
} // namespace ludus::world_demo
