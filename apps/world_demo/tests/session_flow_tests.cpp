// Gameplay G1 acceptance: typed addressed commands, cancellation scopes and the session flow state machine
// (docs/architecture/gameplay-foundations.md#debugging-performance-and-acceptance).
#include "internal/world.h"
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <string_view>
using namespace ludus::world_demo;
namespace foundation = ludus::foundation;
using ludus::foundation::time::NANOSECONDS_PER_SECOND;
using ludus::foundation::time::Timestamp;
namespace
{
// A long-uptime epoch: integer intervals must stay exact far from zero.
constexpr uint64 EPOCH = uint64{1} << 62U;
// The shortest frame that accepts one 60 Hz tick: 16666667 ns * 60 >= one second of phase units.
constexpr uint64 ONE_TICK_FRAME = (NANOSECONDS_PER_SECOND + TICKS_PER_SECOND - 1) / TICKS_PER_SECOND;
constexpr usize PARTITIONED_TICKS = 4;
Timestamp At(uint64 nanoseconds)
{
    return {EPOCH + nanoseconds};
}
GameWorld Build(uint32 capacity = MAX_WORLD_ENTITIES)
{
    Level level;
    REQUIRE(ReadLevel(ExampleLevel(), level).Error == LevelError::None);
    GameWorld world;
    REQUIRE(world.Prepare(level, capacity) == Status::Success);
    while (world.IsBuilt() == false)
    {
        REQUIRE(world.BuildNext() == Status::Success);
    }
    return world;
}
Recipe ExtraGuard()
{
    Level level;
    REQUIRE(ReadLevel(ExampleLevel(), level).Error == LevelError::None);
    Recipe recipe = level.Entities[1];
    recipe.Id = { .Text = "extra-guard" };
    return recipe;
}
uint32 GetHealth(const GameWorld& world, EntityId entity)
{
    const Health* healthOrNull = world.HealthValues().Find(entity);
    REQUIRE(healthOrNull != nullptr);
    return healthOrNull->Current;
}
uint64 PostDamage(GameWorld& inoutWorld, EntityId target, uint32 amount, CancellationToken scope = {})
{
    uint64 token = 0;
    REQUIRE(inoutWorld.PostDamage(target, { .Amount = amount }, scope, token) == Status::Success);
    REQUIRE(token != 0);
    return token;
}
const CommandOutcome& FindOutcome(foundation::BorrowedView<const CommandOutcome> outcomes, uint64 token)
{
    const CommandOutcome* foundOrNull = nullptr;
    for (const CommandOutcome& outcome : outcomes)
    {
        if (outcome.Token == token)
        {
            REQUIRE(foundOrNull == nullptr);
            foundOrNull = &outcome;
        }
    }
    REQUIRE(foundOrNull != nullptr);
    return *foundOrNull;
}
LevelReference Builtin(std::string_view id)
{
    LevelReference reference;
    REQUIRE(TryMakeBuiltinLevelReference(id, reference));
    return reference;
}
uint64 RequestFlow(Session& inoutSession, const FlowRequest& request)
{
    uint64 token = 0;
    REQUIRE(inoutSession.RequestFlow(request, token) == Status::Success);
    return token;
}
void RunBoundary(Session& inoutSession)
{
    REQUIRE(inoutSession.ProcessFlowBoundary() == Status::Success);
}
FlowRequestResult GetFlowResult(const Session& session, uint64 token)
{
    const FlowRequestOutcome* foundOrNull = nullptr;
    for (const FlowRequestOutcome& outcome : session.GetFlow().GetOutcomes())
    {
        if (outcome.Token == token)
        {
            REQUIRE(foundOrNull == nullptr);
            foundOrNull = &outcome;
        }
    }
    REQUIRE(foundOrNull != nullptr);
    return foundOrNull->Result;
}
// Poll the candidate until it is ready, failed or cancelled; the result is observed at the next boundary.
void SettleLoad(Session& inoutSession)
{
    constexpr usize MAXIMUM_POLLS = 64;
    for (usize poll = 0; poll < MAXIMUM_POLLS; ++poll)
    {
        const LoadStage stage = inoutSession.GetLoadStage();
        if (stage != LoadStage::Preparing && stage != LoadStage::Building)
        {
            return;
        }
        (void)inoutSession.PollLoad();
    }
    FAIL("candidate did not settle");
}
void LoadThroughFlow(Session& inoutSession, const LevelReference& level)
{
    const uint64 token = RequestFlow(inoutSession, { .Kind = FlowRequestKind::Load, .Level = level });
    RunBoundary(inoutSession);
    REQUIRE(GetFlowResult(inoutSession, token) == FlowRequestResult::Applied);
    REQUIRE(inoutSession.GetFlowState() == FlowState::Preparing);
    SettleLoad(inoutSession);
    RunBoundary(inoutSession);
}
void StartPlaying(Session& inoutSession)
{
    const uint64 services = RequestFlow(inoutSession, { .Kind = FlowRequestKind::ServicesReady });
    RunBoundary(inoutSession);
    REQUIRE(GetFlowResult(inoutSession, services) == FlowRequestResult::Applied);
    REQUIRE(inoutSession.GetFlowState() == FlowState::Frontend);
    LoadThroughFlow(inoutSession, Builtin(EXAMPLE_LEVEL_ID));
    REQUIRE(inoutSession.GetFlowState() == FlowState::Playing);
    REQUIRE(inoutSession.World().IsBuilt());
}
// The example room with the catalog lookup broken: it parses, then fails world preparation.
std::string UnknownRoomLevel()
{
    std::string source(ExampleLevel());
    const std::string_view known = "\"id\": \"first-room\"";
    const usize offset = source.find(known);
    REQUIRE(offset != std::string::npos);
    source.replace(offset, known.size(), "\"id\": \"third-room\"");
    return source;
}
struct CommandRun final
{
    CommandOutcome Outcomes[MAX_ADDRESSED_COMMANDS] = {};
    usize Count = 0;
    uint64 Hash = 0;
};
// Post the same commands, then cover PARTITIONED_TICKS of accepted time in `frames` equal frames.
CommandRun RunPartitioned(usize frames)
{
    GameWorld world = Build();
    TickDriver driver;
    REQUIRE(driver.Advance(world, At(0), {}) == Status::Success);
    const EntityId player = world.FindAuthored("player-start");
    const EntityId guard = world.FindAuthored("guard-a");
    const EntityId exit = world.FindAuthored("exit-a");
    (void)PostDamage(world, guard, 1);
    (void)PostDamage(world, exit, 1);
    (void)PostDamage(world, player, 1);
    (void)PostDamage(world, guard, 1);
    const uint64 frameLength = ONE_TICK_FRAME * (PARTITIONED_TICKS / frames);
    for (usize frame = 1; frame <= frames; ++frame)
    {
        REQUIRE(driver.Advance(world, At(frameLength * frame), {}) == Status::Success);
    }
    REQUIRE(world.GetTick() == PARTITIONED_TICKS);
    CommandRun run;
    for (const CommandOutcome& outcome : world.GetCommandOutcomes())
    {
        run.Outcomes[run.Count++] = outcome;
    }
    run.Hash = world.GetReplayHash();
    return run;
}
} // namespace

TEST_CASE("Addressed mailbox delivers by admission tick, producer and sequence", "[world-demo][g1]")
{
    constexpr usize CAPACITY = 4;
    constexpr uint64 EARLIER_TICK = 4;
    constexpr uint64 LATER_TICK = 5;
    AddressedMailbox<DamageCommand, CAPACITY> mailbox;
    const EntityId target{ .World = 1, .Slot = 1, .Generation = 1 };
    uint64 gameplayLater = 0;
    uint64 sessionLater = 0;
    uint64 sessionEarlier = 0;
    uint64 sessionLaterSecond = 0;
    REQUIRE(mailbox.Admit(target, {}, MessageProducer::Gameplay, LATER_TICK, {}, gameplayLater) == Status::Success);
    REQUIRE(mailbox.Admit(target, {}, MessageProducer::Session, LATER_TICK, {}, sessionLater) == Status::Success);
    REQUIRE(mailbox.Admit(target, {}, MessageProducer::Session, EARLIER_TICK, {}, sessionEarlier) == Status::Success);
    REQUIRE(mailbox.Admit(target, {}, MessageProducer::Session, LATER_TICK, {}, sessionLaterSecond) == Status::Success);

    // Admission is bounded: the rejected message changes neither its token nor the mailbox.
    uint64 rejected = 0;
    REQUIRE(mailbox.Admit(target, {}, MessageProducer::Session, LATER_TICK, {}, rejected) == Status::CapacityExceeded);
    REQUIRE(rejected == 0);
    REQUIRE(mailbox.GetOccupancy() == CAPACITY);

    REQUIRE(mailbox.Capture() == Status::Success);
    const uint64 expected[] = {sessionEarlier, sessionLater, sessionLaterSecond, gameplayLater};
    REQUIRE(mailbox.GetCaptured().GetSize() == CAPACITY);
    for (usize index = 0; index < CAPACITY; ++index)
    {
        CHECK(mailbox.GetCaptured()[index].Token == expected[index]);
    }
    // Captured messages still occupy capacity until the delivery is released.
    REQUIRE(mailbox.Admit(target, {}, MessageProducer::Session, LATER_TICK, {}, rejected) == Status::CapacityExceeded);
    mailbox.ReleaseCaptured();
    REQUIRE(mailbox.GetOccupancy() == 0);

    // A message admitted while a batch is captured waits for the next capture: delivery never drains recursively.
    uint64 first = 0;
    uint64 duringDelivery = 0;
    REQUIRE(mailbox.Admit(target, {}, MessageProducer::Session, LATER_TICK, {}, first) == Status::Success);
    REQUIRE(mailbox.Capture() == Status::Success);
    REQUIRE(mailbox.Admit(target, {}, MessageProducer::Gameplay, LATER_TICK, {}, duringDelivery) == Status::Success);
    REQUIRE(mailbox.Capture() == Status::InvalidPhase);
    REQUIRE(mailbox.GetCaptured().GetSize() == 1);
    REQUIRE(mailbox.GetCaptured()[0].Token == first);
    mailbox.ReleaseCaptured();
    REQUIRE(mailbox.Capture() == Status::Success);
    REQUIRE(mailbox.GetCaptured().GetSize() == 1);
    REQUIRE(mailbox.GetCaptured()[0].Token == duringDelivery);

    constexpr uint8 UNKNOWN_PRODUCER = 7;
    REQUIRE(mailbox.Admit(target, {}, static_cast<MessageProducer>(UNKNOWN_PRODUCER), LATER_TICK, {}, rejected) ==
            Status::InvalidConfiguration);
    REQUIRE(rejected == 0);
}

TEST_CASE("Cancellation scopes cancel once, retire generations and follow their owners", "[world-demo][g1]")
{
    constexpr usize CAPACITY = 2;
    CancellationScopes<CAPACITY> scopes;
    const EntityId owner{ .World = 1, .Slot = 2, .Generation = 1 };
    CancellationToken unowned{};
    CancellationToken owned{};
    REQUIRE(scopes.Open({}, unowned) == Status::Success);
    REQUIRE(scopes.Open(owner, owned) == Status::Success);
    CancellationToken overflow{};
    REQUIRE(scopes.Open({}, overflow) == Status::CapacityExceeded);
    REQUIRE(overflow == CancellationToken{});

    REQUIRE_FALSE(scopes.IsCancelled({}));
    REQUIRE_FALSE(scopes.IsCancelled(unowned));
    REQUIRE(scopes.Cancel(unowned));
    REQUIRE_FALSE(scopes.Cancel(unowned));
    REQUIRE(scopes.IsCancelled(unowned));
    REQUIRE(scopes.CancelOwnedBy(owner) == 1);
    REQUIRE(scopes.CancelOwnedBy(owner) == 0);
    REQUIRE(scopes.IsCancelled(owned));
    REQUIRE(scopes.GetCancellations() == 2);

    // Closing retires the generation: the old token stays cancelled after its slot is reused.
    REQUIRE(scopes.Close(unowned) == Status::Success);
    REQUIRE(scopes.Close(unowned) == Status::InvalidEntity);
    CancellationToken reused{};
    REQUIRE(scopes.Open({}, reused) == Status::Success);
    REQUIRE(reused.Slot == unowned.Slot);
    REQUIRE(reused.Generation != unowned.Generation);
    REQUIRE(scopes.IsCancelled(unowned));
    REQUIRE_FALSE(scopes.IsOpen(unowned));
    REQUIRE_FALSE(scopes.IsCancelled(reused));
    REQUIRE_FALSE(scopes.Cancel(unowned));
    REQUIRE(scopes.GetOpenCount() == CAPACITY);
}

TEST_CASE("Addressed commands resolve once, in admission order, when their tick commits", "[world-demo][g1]")
{
    GameWorld world = Build();
    const EntityId guard = world.FindAuthored("guard-a");
    const EntityId exit = world.FindAuthored("exit-a");
    REQUIRE(GetHealth(world, guard) == 2);
    const uint64 first = PostDamage(world, guard, 1);
    const uint64 lethal = PostDamage(world, guard, 1);
    const uint64 late = PostDamage(world, guard, 1);
    const uint64 noHealth = PostDamage(world, exit, 1);
    // Queued commands belong to the next tick: nothing resolves and the boundary has no canonical hash.
    REQUIRE(world.GetCommandOutcomes().IsEmpty());
    REQUIRE(world.GetUnresolvedCommandCount() == 4);
    REQUIRE(world.GetReplayHash() == 0);

    REQUIRE(world.RunTick({}) == Status::Success);
    const foundation::BorrowedView<const CommandOutcome> outcomes = world.GetCommandOutcomes();
    REQUIRE(outcomes.GetSize() == 4);
    const uint64 order[] = {first, lethal, late, noHealth};
    for (usize index = 0; index < outcomes.GetSize(); ++index)
    {
        CHECK(outcomes[index].Token == order[index]);
        CHECK(outcomes[index].Tick == 1);
    }
    CHECK(FindOutcome(outcomes, first).Result == CommandResult::Applied);
    CHECK(FindOutcome(outcomes, lethal).Result == CommandResult::Applied);
    // The lethal command queued destruction; a later command in the same batch finds a stale target.
    CHECK(FindOutcome(outcomes, late).Result == CommandResult::StaleTarget);
    CHECK(FindOutcome(outcomes, noHealth).Result == CommandResult::MissingCapability);
    REQUIRE_FALSE(world.Registry().IsAlive(guard));
    REQUIRE(world.GetUnresolvedCommandCount() == 0);
    REQUIRE(world.GetReplayHash() != 0);

    // Further ticks publish nothing new; unconsumed outcomes stay until the consumer takes them.
    REQUIRE(world.RunTick({}) == Status::Success);
    REQUIRE(world.GetCommandOutcomes().GetSize() == 4);
    world.ConsumeCommandOutcomes();
    REQUIRE(world.GetCommandOutcomes().IsEmpty());
    REQUIRE(world.RunTick({}) == Status::Success);
    REQUIRE(world.GetCommandOutcomes().IsEmpty());
}

TEST_CASE("Command outcomes and replay are independent of how frames partition fixed ticks", "[world-demo][g1]")
{
    const CommandRun catchUp = RunPartitioned(1);
    const CommandRun split = RunPartitioned(PARTITIONED_TICKS);
    REQUIRE(catchUp.Count == 4);
    REQUIRE(split.Count == catchUp.Count);
    for (usize index = 0; index < catchUp.Count; ++index)
    {
        CHECK(split.Outcomes[index].Token == catchUp.Outcomes[index].Token);
        CHECK(split.Outcomes[index].Result == catchUp.Outcomes[index].Result);
        // Every command resolves in the first tick, even when one frame runs all four.
        CHECK(catchUp.Outcomes[index].Tick == 1);
        CHECK(split.Outcomes[index].Tick == 1);
    }
    REQUIRE(catchUp.Hash != 0);
    REQUIRE(split.Hash == catchUp.Hash);
}

TEST_CASE("Command admission is bounded and reserves each eventual outcome", "[world-demo][g1]")
{
    GameWorld world = Build();
    const EntityId player = world.FindAuthored("player-start");
    const EntityId exit = world.FindAuthored("exit-a");
    uint64 token = 0;
    REQUIRE(world.PostDamage(exit, { .Amount = 0 }, {}, token) == Status::InvalidConfiguration);
    REQUIRE(world.PostDamage(exit, { .Amount = MAX_DAMAGE_AMOUNT + 1 }, {}, token) == Status::InvalidConfiguration);
    EntityId foreign = player;
    ++foreign.World;
    REQUIRE(world.PostDamage(foreign, {}, {}, token) == Status::InvalidEntity);
    REQUIRE(world.PostDamage({}, {}, {}, token) == Status::InvalidEntity);
    const CancellationToken unknownScope{ .Slot = 1, .Generation = 1 };
    REQUIRE(world.PostDamage(exit, {}, unknownScope, token) == Status::InvalidConfiguration);
    REQUIRE(token == 0);

    for (usize index = 0; index < MAX_ADDRESSED_COMMANDS; ++index)
    {
        (void)PostDamage(world, exit, 1);
    }
    REQUIRE(world.PostDamage(exit, {}, {}, token) == Status::CapacityExceeded);
    REQUIRE(token == 0);
    REQUIRE(world.RunTick({}) == Status::Success);
    REQUIRE(world.GetCommandOutcomes().GetSize() == MAX_ADDRESSED_COMMANDS);
    // Published but unconsumed outcomes keep their reservation, so the outbox can never overflow.
    REQUIRE(world.PostDamage(exit, {}, {}, token) == Status::CapacityExceeded);
    world.ConsumeCommandOutcomes();
    REQUIRE(world.PostDamage(exit, {}, {}, token) == Status::Success);
}

TEST_CASE("Cancelling a scope before or after capture resolves its commands as cancelled", "[world-demo][g1]")
{
    GameWorld world = Build();
    const EntityId player = world.FindAuthored("player-start");
    const EntityId guard = world.FindAuthored("guard-a");
    const uint32 health = GetHealth(world, player);

    CancellationToken cancelled{};
    CancellationToken closed{};
    CancellationToken guardOwned{};
    REQUIRE(world.OpenCommandScope({}, cancelled) == Status::Success);
    REQUIRE(world.OpenCommandScope({}, closed) == Status::Success);
    REQUIRE(world.OpenCommandScope(guard, guardOwned) == Status::Success);
    const uint64 cancelledCommand = PostDamage(world, player, 1, cancelled);
    const uint64 closedCommand = PostDamage(world, player, 1, closed);
    REQUIRE(world.CancelCommandScope(cancelled));
    REQUIRE_FALSE(world.CancelCommandScope(cancelled));
    REQUIRE(world.CloseCommandScope(closed) == Status::Success);
    // Within one captured batch: the first command destroys the scope owner, which cancels the second.
    const uint64 lethal = PostDamage(world, guard, MAX_DAMAGE_AMOUNT);
    const uint64 afterCapture = PostDamage(world, player, 1, guardOwned);

    REQUIRE(world.RunTick({}) == Status::Success);
    const foundation::BorrowedView<const CommandOutcome> outcomes = world.GetCommandOutcomes();
    REQUIRE(outcomes.GetSize() == 4);
    CHECK(FindOutcome(outcomes, cancelledCommand).Result == CommandResult::Cancelled);
    CHECK(FindOutcome(outcomes, closedCommand).Result == CommandResult::Cancelled);
    CHECK(FindOutcome(outcomes, lethal).Result == CommandResult::Applied);
    CHECK(FindOutcome(outcomes, afterCapture).Result == CommandResult::Cancelled);
    REQUIRE(world.IsCommandScopeCancelled(guardOwned));
    REQUIRE(GetHealth(world, player) == health);
    // A closed scope cannot admit new work.
    uint64 token = 0;
    REQUIRE(world.PostDamage(player, {}, closed, token) == Status::InvalidConfiguration);
}

TEST_CASE("A faulted tick publishes no outcome and retirement supersedes each command once", "[world-demo][g1]")
{
    constexpr uint32 FULL_CAPACITY = 3;
    GameWorld world = Build(FULL_CAPACITY);
    const EntityId player = world.FindAuthored("player-start");
    const uint32 health = GetHealth(world, player);
    const uint64 captured = PostDamage(world, player, 1);
    REQUIRE(world.QueueDestroy(world.FindAuthored("guard-a")) == Status::Success);
    uint64 request = 0;
    REQUIRE(world.QueueSpawn(ExtraGuard(), request) == Status::Success);
    // Commit preflight fails after the command was delivered: its outcome is staged, never published.
    REQUIRE(world.RunTick({}) == Status::CapacityExceeded);
    REQUIRE(world.GetPhase() == Phase::Faulted);
    REQUIRE(world.GetCommandOutcomes().IsEmpty());
    REQUIRE(world.GetUnresolvedCommandCount() == 1);
    uint64 token = 0;
    REQUIRE(world.PostDamage(player, {}, {}, token) == Status::InvalidPhase);

    CommandOutcome retired[MAX_ADDRESSED_COMMANDS] = {};
    REQUIRE(world.RetireCommands({retired, 0}) == Status::CapacityExceeded);
    REQUIRE(world.GetRetiringCommandCount() == 1);
    REQUIRE(world.RetireCommands(retired) == Status::Success);
    REQUIRE(retired[0].Token == captured);
    REQUIRE(retired[0].Result == CommandResult::Superseded);
    REQUIRE(retired[0].Tick == 0);
    REQUIRE(world.GetRetiringCommandCount() == 0);
    // The partial world stays inspectable: its staged damage is visible, but nothing was published.
    REQUIRE(GetHealth(world, player) == health - 1);
}

TEST_CASE("World replacement hands every command exactly one outcome", "[world-demo][g1]")
{
    Session session;
    StartPlaying(session);
    const EntityId player = session.World().FindAuthored("player-start");
    const uint64 applied = PostDamage(session.World(), player, 1);
    REQUIRE(session.World().RunTick({}) == Status::Success);
    const uint64 pending = PostDamage(session.World(), player, 1);
    REQUIRE(session.World().GetCommandOutcomes().GetSize() == 1);

    LoadThroughFlow(session, Builtin(EXAMPLE_LEVEL_ID));
    REQUIRE(session.GetFlowState() == FlowState::Playing);
    REQUIRE(session.World().GetRetiringCommandCount() == 0);
    const foundation::BorrowedView<const CommandOutcome> retired = session.GetRetiredCommandOutcomes();
    REQUIRE(retired.GetSize() == 2);
    CHECK(FindOutcome(retired, applied).Result == CommandResult::Applied);
    CHECK(FindOutcome(retired, applied).Tick == 1);
    CHECK(FindOutcome(retired, pending).Result == CommandResult::Superseded);

    // Activation never drops an outcome: it waits while the retired list cannot take the old world's.
    const EntityId exit = session.World().FindAuthored("exit-a");
    for (usize index = retired.GetSize(); index < MAX_ADDRESSED_COMMANDS; ++index)
    {
        (void)PostDamage(session.World(), exit, 1);
    }
    LoadThroughFlow(session, Builtin(EXAMPLE_LEVEL_ID));
    REQUIRE(session.GetRetiredCommandOutcomes().GetSize() == MAX_ADDRESSED_COMMANDS);
    // The replaced world's handle is stale in its successor.
    uint64 stale = 0;
    REQUIRE(session.World().PostDamage(exit, {}, {}, stale) == Status::InvalidEntity);
    (void)PostDamage(session.World(), session.World().FindAuthored("exit-a"), 1);
    const uint64 load = RequestFlow(session, { .Kind = FlowRequestKind::Load, .Level = Builtin(EXAMPLE_LEVEL_ID) });
    RunBoundary(session);
    REQUIRE(GetFlowResult(session, load) == FlowRequestResult::Applied);
    SettleLoad(session);
    REQUIRE(session.GetLoadStage() == LoadStage::Ready);
    RunBoundary(session);
    REQUIRE(session.GetFlowState() == FlowState::Preparing);
    REQUIRE(session.GetFlow().GetOutcomes().GetSize() == 1);
    REQUIRE(session.GetFlow().GetOutcomes()[0].Result == FlowRequestResult::PreflightFailed);
    REQUIRE(session.World().GetRetiringCommandCount() == 1);
    session.ConsumeRetiredCommandOutcomes();
    RunBoundary(session);
    REQUIRE(session.GetFlowState() == FlowState::Playing);
    REQUIRE(session.GetRetiredCommandOutcomes().GetSize() == 1);
    REQUIRE(session.GetRetiredCommandOutcomes()[0].Result == CommandResult::Superseded);
}

TEST_CASE("Flow boots, prepares and plays through typed requests", "[world-demo][g1][flow]")
{
    Session session;
    REQUIRE(session.GetFlowState() == FlowState::Boot);
    REQUIRE(session.GetFlowPolicy().Simulation == SimulationPolicy::Stopped);
    // A load is not a Boot transition: it is rejected, not deferred.
    const uint64 early = RequestFlow(session, { .Kind = FlowRequestKind::Load, .Level = Builtin(EXAMPLE_LEVEL_ID) });
    RunBoundary(session);
    REQUIRE(GetFlowResult(session, early) == FlowRequestResult::Rejected);
    REQUIRE(session.GetFlowState() == FlowState::Boot);
    StartPlaying(session);
    REQUIRE(session.GetFlowPolicy() ==
            FlowPolicy{ .Simulation = SimulationPolicy::Running, .Input = InputRoute::Gameplay });
    REQUIRE(session.Driver.GetMode() == Mode::Playing);
    REQUIRE(session.GetFlow().GetLastChange().From == FlowState::Preparing);
    REQUIRE(session.GetFlow().GetLastChange().Cause == FlowRequestKind::CandidateReady);
    // Observed kinds belong to the session.
    uint64 token = 0;
    REQUIRE(session.RequestFlow({ .Kind = FlowRequestKind::CandidateReady }, token) == Status::InvalidConfiguration);
    REQUIRE(session.RequestFlow({ .Kind = FlowRequestKind::Fault }, token) == Status::InvalidConfiguration);
    REQUIRE(token == 0);
}

TEST_CASE("A boundary applies at most one transition by priority, then admission order", "[world-demo][g1][flow]")
{
    Session session;
    StartPlaying(session);
    const uint64 hash = session.World().GetReplayHash();
    const uint64 changes = session.GetFlow().GetChangeCount();
    const uint64 resume = RequestFlow(session, { .Kind = FlowRequestKind::Resume });
    const uint64 pause = RequestFlow(session, { .Kind = FlowRequestKind::Pause });
    const uint64 firstLoad = RequestFlow(session,
                                         {
                                             .Kind = FlowRequestKind::Load,
                                             .Level = Builtin(EXAMPLE_LEVEL_ID),
                                         });
    const uint64 secondLoad = RequestFlow(session, { .Kind = FlowRequestKind::Load, .Level = Builtin("second-room") });
    RunBoundary(session);
    REQUIRE(session.GetFlow().GetOutcomes().GetSize() == 4);
    CHECK(GetFlowResult(session, resume) == FlowRequestResult::Rejected);
    CHECK(GetFlowResult(session, pause) == FlowRequestResult::Superseded);
    CHECK(GetFlowResult(session, firstLoad) == FlowRequestResult::Applied);
    CHECK(GetFlowResult(session, secondLoad) == FlowRequestResult::Superseded);
    REQUIRE(session.GetFlowState() == FlowState::Preparing);
    REQUIRE(session.GetFlow().GetChangeCount() == changes + 1);
    // The superseded requests are gone: the next boundary has nothing to apply.
    RunBoundary(session);
    REQUIRE(session.GetFlow().GetOutcomes().IsEmpty());
    REQUIRE(session.GetFlowState() == FlowState::Preparing);
    REQUIRE(session.World().GetReplayHash() == hash);
    SettleLoad(session);
    RunBoundary(session);
    REQUIRE(session.GetFlowState() == FlowState::Playing);
    REQUIRE(session.World().GetRoom() == 1);
}

TEST_CASE("Requests made after a boundary captured its batch wait for the next boundary", "[world-demo][g1][flow]")
{
    SessionFlow flow;
    uint64 services = 0;
    REQUIRE(flow.Request({ .Kind = FlowRequestKind::ServicesReady }, services) == Status::Success);
    REQUIRE(flow.BeginBoundary() == Status::Success);
    REQUIRE(flow.BeginBoundary() == Status::InvalidPhase);
    // An entry action or notification raising a request during the boundary.
    uint64 quit = 0;
    REQUIRE(flow.Request({ .Kind = FlowRequestKind::Quit }, quit) == Status::Success);
    FlowTransition transition;
    REQUIRE(flow.SelectTransition(transition));
    REQUIRE(transition.Token == services);
    REQUIRE(transition.To == FlowState::Frontend);
    REQUIRE_FALSE(flow.SelectTransition(transition));
    REQUIRE(flow.Observe({ .Kind = FlowRequestKind::Fault }) == Status::InvalidPhase);
    flow.ApplyTransition(transition);
    flow.EndBoundary();
    REQUIRE(flow.GetState() == FlowState::Frontend);
    REQUIRE(flow.GetOutcomes().GetSize() == 1);
    REQUIRE(flow.BeginBoundary() == Status::Success);
    REQUIRE(flow.SelectTransition(transition));
    REQUIRE(transition.Token == quit);
    flow.ApplyTransition(transition);
    flow.EndBoundary();
    REQUIRE(flow.GetState() == FlowState::Shutdown);
    REQUIRE(flow.GetLastChange().Boundary == flow.GetBoundaryCount());
}

TEST_CASE("Pause holds ticks and commands until a step advances exactly one tick", "[world-demo][g1][flow]")
{
    Session session;
    StartPlaying(session);
    REQUIRE(session.StepFlow() == Status::InvalidPhase);
    REQUIRE(session.Driver.Advance(session.World(), At(0), {}) == Status::Success);
    const uint64 pause = RequestFlow(session, { .Kind = FlowRequestKind::Pause });
    RunBoundary(session);
    REQUIRE(GetFlowResult(session, pause) == FlowRequestResult::Applied);
    REQUIRE(session.GetFlowState() == FlowState::Paused);
    REQUIRE(session.GetFlowPolicy().CanStep);
    const uint64 tick = session.World().GetTick();
    const EntityId player = session.World().FindAuthored("player-start");
    const uint64 command = PostDamage(session.World(), player, 1);
    constexpr uint64 PAUSED_FRAMES = 10;
    for (uint64 frame = 1; frame <= PAUSED_FRAMES; ++frame)
    {
        REQUIRE(session.Driver.Advance(session.World(), At(ONE_TICK_FRAME * frame), {}) == Status::Success);
        RunBoundary(session);
    }
    REQUIRE(session.World().GetTick() == tick);
    REQUIRE(session.World().GetCommandOutcomes().IsEmpty());
    REQUIRE(session.World().GetUnresolvedCommandCount() == 1);

    REQUIRE(session.StepFlow() == Status::Success);
    REQUIRE(session.World().GetTick() == tick + 1);
    REQUIRE(session.World().GetCommandOutcomes().GetSize() == 1);
    REQUIRE(FindOutcome(session.World().GetCommandOutcomes(), command).Result == CommandResult::Applied);
    REQUIRE(FindOutcome(session.World().GetCommandOutcomes(), command).Tick == tick + 1);
    REQUIRE(session.GetFlowState() == FlowState::Paused);

    // Resume does not catch up paused time.
    const uint64 resume = RequestFlow(session, { .Kind = FlowRequestKind::Resume });
    RunBoundary(session);
    REQUIRE(GetFlowResult(session, resume) == FlowRequestResult::Applied);
    REQUIRE(session.Driver.GetMode() == Mode::Playing);
    REQUIRE(session.Driver.Advance(session.World(), At(ONE_TICK_FRAME * (PAUSED_FRAMES + 1)), {}) == Status::Success);
    REQUIRE(session.World().GetTick() == tick + 1);
    REQUIRE(session.Driver.Advance(session.World(), At(ONE_TICK_FRAME * (PAUSED_FRAMES + 2)), {}) == Status::Success);
    REQUIRE(session.World().GetTick() == tick + 2);
    REQUIRE(session.World().GetCommandOutcomes().GetSize() == 1);
}

TEST_CASE("A failed replacement keeps the retained session and its return state", "[world-demo][g1][flow]")
{
    Session session;
    StartPlaying(session);
    REQUIRE(session.World().RunTick({}) == Status::Success);
    const EntityId player = session.World().FindAuthored("player-start");
    const uint64 hash = session.World().GetReplayHash();

    // Preflight failure: an unknown room is rejected before Playing exits.
    const uint64 unknown = RequestFlow(session, { .Kind = FlowRequestKind::Load, .Level = Builtin("missing-room") });
    RunBoundary(session);
    REQUIRE(GetFlowResult(session, unknown) == FlowRequestResult::PreflightFailed);
    REQUIRE(session.GetFlowState() == FlowState::Playing);
    REQUIRE(session.Driver.GetMode() == Mode::Playing);
    REQUIRE(session.GetLoadStage() == LoadStage::Empty);

    // Candidate failure: a parsed level that cannot be prepared returns to the paused session it left.
    const uint64 pause = RequestFlow(session, { .Kind = FlowRequestKind::Pause });
    RunBoundary(session);
    REQUIRE(GetFlowResult(session, pause) == FlowRequestResult::Applied);
    const std::string unknownRoom = UnknownRoomLevel();
    session.SetProvidedLevel(unknownRoom);
    LoadThroughFlow(session, { .Source = LevelSource::Provided });
    REQUIRE(session.GetFlowState() == FlowState::Paused);
    REQUIRE(session.GetFlow().GetLastChange().Cause == FlowRequestKind::CandidateFailed);
    REQUIRE(session.Driver.GetMode() == Mode::Paused);
    REQUIRE(session.World().Registry().IsAlive(player));
    REQUIRE(session.World().GetReplayHash() == hash);
    REQUIRE(session.GetCancelledLoads() == 0);
    REQUIRE(session.StepFlow() == Status::Success);

    // Without a retained session the guard sends the failure back to Frontend.
    Session fresh;
    const uint64 services = RequestFlow(fresh, { .Kind = FlowRequestKind::ServicesReady });
    RunBoundary(fresh);
    REQUIRE(GetFlowResult(fresh, services) == FlowRequestResult::Applied);
    fresh.SetProvidedLevel(unknownRoom);
    LoadThroughFlow(fresh, { .Source = LevelSource::Provided });
    REQUIRE(fresh.GetFlowState() == FlowState::Frontend);
    REQUIRE_FALSE(fresh.World().IsBuilt());
}

TEST_CASE("Leaving Preparing cancels its owned load exactly once", "[world-demo][g1][flow]")
{
    Session session;
    StartPlaying(session);
    const EntityId player = session.World().FindAuthored("player-start");
    const uint64 load = RequestFlow(session, { .Kind = FlowRequestKind::Load, .Level = Builtin("second-room") });
    RunBoundary(session);
    REQUIRE(GetFlowResult(session, load) == FlowRequestResult::Applied);
    REQUIRE(session.GetFlowPolicy().Simulation == SimulationPolicy::Held);
    REQUIRE(session.PollLoad() == Status::Success);

    // A second load restarts Preparing: the in-flight candidate is cancelled once and replaced.
    const uint64 restart = RequestFlow(session, { .Kind = FlowRequestKind::Load, .Level = Builtin(EXAMPLE_LEVEL_ID) });
    RunBoundary(session);
    REQUIRE(GetFlowResult(session, restart) == FlowRequestResult::Applied);
    REQUIRE(session.GetFlowState() == FlowState::Preparing);
    REQUIRE(session.GetCancelledLoads() == 1);
    REQUIRE(session.PollLoad() == Status::Success);

    const uint64 leave = RequestFlow(session, { .Kind = FlowRequestKind::Leave });
    RunBoundary(session);
    REQUIRE(GetFlowResult(session, leave) == FlowRequestResult::Applied);
    REQUIRE(session.GetFlowState() == FlowState::Frontend);
    REQUIRE(session.GetCancelledLoads() == 2);
    REQUIRE(session.GetLoadStage() == LoadStage::Cancelled);
    // A late poll cannot revive the cancelled candidate, and later boundaries cancel nothing again.
    REQUIRE(session.PollLoad() == Status::Success);
    REQUIRE_FALSE(session.Activate());
    RunBoundary(session);
    RunBoundary(session);
    REQUIRE(session.GetCancelledLoads() == 2);
    REQUIRE(session.GetFlowState() == FlowState::Frontend);
    REQUIRE(session.World().Registry().IsAlive(player));
}

TEST_CASE("An authoritative fault stops the flow until a load restarts it", "[world-demo][g1][flow]")
{
    Session session;
    StartPlaying(session);
    REQUIRE(session.Driver.Advance(session.World(), At(0), {}) == Status::Success);
    const TickInput invalid{ .Axis = {std::numeric_limits<float32>::quiet_NaN(), 0} };
    REQUIRE(session.Driver.Advance(session.World(), At(ONE_TICK_FRAME), invalid) == Status::InvalidConfiguration);
    const uint64 pause = RequestFlow(session, { .Kind = FlowRequestKind::Pause });
    RunBoundary(session);
    // The observed fault outranks the game's pause.
    REQUIRE(GetFlowResult(session, pause) == FlowRequestResult::Superseded);
    REQUIRE(session.GetFlowState() == FlowState::Faulted);
    REQUIRE(session.GetFlow().GetLastChange().Cause == FlowRequestKind::Fault);
    REQUIRE(session.Driver.GetMode() == Mode::Faulted);
    REQUIRE(session.StepFlow() == Status::InvalidPhase);
    const uint64 resume = RequestFlow(session, { .Kind = FlowRequestKind::Resume });
    RunBoundary(session);
    REQUIRE(GetFlowResult(session, resume) == FlowRequestResult::Rejected);

    LoadThroughFlow(session, Builtin(EXAMPLE_LEVEL_ID));
    REQUIRE(session.GetFlowState() == FlowState::Playing);
    REQUIRE(session.World().GetPhase() == Phase::Idle);
    REQUIRE(session.Driver.GetMode() == Mode::Playing);
}

TEST_CASE("Quit keeps a reserved slot and wins from every state", "[world-demo][g1][flow]")
{
    Session session;
    StartPlaying(session);
    uint64 token = 0;
    for (usize index = 0; index < MAX_FLOW_REQUESTS; ++index)
    {
        (void)RequestFlow(session, { .Kind = FlowRequestKind::Pause });
    }
    REQUIRE(session.RequestFlow({ .Kind = FlowRequestKind::Resume }, token) == Status::CapacityExceeded);
    REQUIRE(token == 0);
    const uint64 quit = RequestFlow(session, { .Kind = FlowRequestKind::Quit });
    RunBoundary(session);
    REQUIRE(session.GetFlow().GetOutcomes().GetSize() == MAX_FLOW_REQUESTS + 1);
    REQUIRE(GetFlowResult(session, quit) == FlowRequestResult::Applied);
    REQUIRE(session.GetFlowState() == FlowState::Shutdown);
    // Shutdown is terminal.
    const uint64 late = RequestFlow(session, { .Kind = FlowRequestKind::Load, .Level = Builtin(EXAMPLE_LEVEL_ID) });
    RunBoundary(session);
    REQUIRE(GetFlowResult(session, late) == FlowRequestResult::Rejected);

    // Quit while Preparing cancels the owned load on exit.
    Session loading;
    const uint64 services = RequestFlow(loading, { .Kind = FlowRequestKind::ServicesReady });
    RunBoundary(loading);
    REQUIRE(GetFlowResult(loading, services) == FlowRequestResult::Applied);
    const uint64 load = RequestFlow(loading, { .Kind = FlowRequestKind::Load, .Level = Builtin(EXAMPLE_LEVEL_ID) });
    RunBoundary(loading);
    REQUIRE(GetFlowResult(loading, load) == FlowRequestResult::Applied);
    const uint64 loadingQuit = RequestFlow(loading, { .Kind = FlowRequestKind::Quit });
    RunBoundary(loading);
    REQUIRE(GetFlowResult(loading, loadingQuit) == FlowRequestResult::Applied);
    REQUIRE(loading.GetFlowState() == FlowState::Shutdown);
    REQUIRE(loading.GetCancelledLoads() == 1);
    REQUIRE(loading.GetLoadStage() == LoadStage::Cancelled);
}

TEST_CASE("Restoring a checkpoint retires the discarded world's commands once", "[world-demo][g1]")
{
    Session session;
    StartPlaying(session);
    ludus::foundation::core::StaticArray<uint8, MAX_SESSION_CHECKPOINT_BYTES> checkpoint{};
    usize written = 0;
    REQUIRE(session.TrySaveCheckpoint({checkpoint.data(), checkpoint.size()}, written) ==
            foundation::math::RandomCheckpointStatus::Success);
    const uint64 pending = PostDamage(session.World(), session.World().FindAuthored("player-start"), 1);
    REQUIRE(session.TryRestoreCheckpoint({checkpoint.data(), written}) ==
            foundation::math::RandomCheckpointStatus::Success);
    REQUIRE_FALSE(session.World().IsBuilt());
    REQUIRE(session.GetRetiredCommandOutcomes().GetSize() == 1);
    REQUIRE(FindOutcome(session.GetRetiredCommandOutcomes(), pending).Result == CommandResult::Superseded);
}
