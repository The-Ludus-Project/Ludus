#pragma once
#include "level.h"
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
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
/// Render records: every entity, every collision box and every placement of a full room.
inline constexpr usize MAX_RENDER_DRAWS = MAX_WORLD_ENTITIES + MAX_WORLD_COLLISION + MAX_ROOM_PLACEMENTS;
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
    /// Index into that layer's Variants.
    uint8 Variant = 0;
    [[nodiscard]] constexpr bool operator==(const Placement&) const noexcept = default;
};
class GameWorld final
{
public:
    /// Validate the level, reserve pool capacity and, when the level authors a scatter, generate the room's
    /// placements from `worldSeed` and the checked-in room catalog. The only allocations happen here.
    /// @return InvalidConfiguration for an invalid level, capacity, unknown room or unusable recipe;
    /// CapacityExceeded when the recipe enumerates more candidates or accepts more placements than the room
    /// limits; AllocationFailure when storage is unavailable. Nothing is published on failure.
    [[nodiscard]] Status
    Prepare(const Level& level, uint32 capacity = MAX_WORLD_ENTITIES, WorldSeed worldSeed = {}) noexcept;
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
    [[nodiscard]] Status prepareScatter(WorldSeed worldSeed) noexcept;
    [[nodiscard]] Status Commit() noexcept;
    void Intent(TickInput input) noexcept;
    void MoveAndCollide() noexcept;
    [[nodiscard]] Status Gameplay(TickInput input, Name& nextLevel) noexcept;
    void Stage(EventKind kind, Vec2 anchor) noexcept;
    [[nodiscard]] Status Damage(EntityId entity) noexcept;
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
    uint64 mPlacementDigest = 0;
    uint64 mCosmeticKey = 0;
    WorldSeed mWorldSeed;
    uint16 mRoom = 0;
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
class Session final
{
public:
    [[nodiscard]] LevelResult RequestLoad(std::string_view source) noexcept;
    [[nodiscard]] Status PollLoad() noexcept;
    [[nodiscard]] bool Activate() noexcept;
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

private:
    GameWorld mActive;
    GameWorld mCandidate;
    Level mDefinition;
    LoadStage mStage = LoadStage::Empty;
    uint64 mTransition = 0;
};
} // namespace ludus::world_demo
