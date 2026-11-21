#include <ludus/foundation/threading/thread.hpp>

#include "../internal/lifecycle.h"

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::smoke::lifecycle;
namespace threading = ludus::foundation::threading;
namespace
{
constexpr uint32 TEST_DOMAIN = 7;
struct Fixture;
struct FakeModule
{
    Fixture* OwnerOrNull = nullptr;
    usize Id = 0;
    StartResult Start;
    StartResult StartPoll;
    StopResult Stop;
    StopResult StopPoll;
    bool IsLive = false;
    bool ShouldCancelOnStart = false;
    bool ShouldReenter = false;
    usize AttemptsObserved = 0;
};
struct Fixture
{
    FakeModule Modules[3];
    usize Provider0[1]{0};
    usize Provider1[1]{1};
    Node Nodes[3];
    NodeRecord Records[3];
    usize Journal[3]{};
    uint32 Events[64]{};
    usize EventCount = 0;
    bool AreProvidersLive = true;
    bool IsReentryRejected = false;
    Runner Lifecycle{Nodes, Records, Journal};

    Fixture() noexcept
    {
        for (usize id = 0; id < 3; ++id)
        {
            Modules[id].OwnerOrNull = this;
            Modules[id].Id = id;
            Nodes[id] =
            {
                .Id = id,
                .Name = "Fake",
                .ContextOrNull = &Modules[id],
                .Providers = {},
                .BeginStart = BeginStart,
                .PollStart = PollStart,
                .BeginStop = BeginStop,
                .PollStop = PollStop,
            };
        }
        Nodes[1].Providers = Provider0;
        Nodes[2].Providers = Provider1;
    }
    void Record(const FakeModule& module, uint32 operation) noexcept
    {
        if (EventCount < 64)
        {
            Events[EventCount++] = operation + static_cast<uint32>(module.Id);
        }
        else
        {
            AreProvidersLive = false;
        }
        for (const usize provider : Nodes[Records[module.Id].Position].Providers)
        {
            AreProvidersLive = AreProvidersLive && Modules[provider].IsLive;
        }
    }
    static StartResult BeginStart(void* context) noexcept
    {
        FakeModule& module = *static_cast<FakeModule*>(context);
        Fixture& owner = *module.OwnerOrNull;
        owner.Record(module, 100);
        module.IsLive = true; // Even a Failed result acquired something to retire.
        module.AttemptsObserved = owner.Lifecycle.GetReport().AttemptedCount;
        if (module.ShouldCancelOnStart)
        {
            owner.Lifecycle.RequestStop();
        }
        if (module.ShouldReenter)
        {
            owner.IsReentryRejected =
                owner.Lifecycle.Begin() == BeginStatus::Busy && owner.Lifecycle.Advance(3) == State::Starting;
        }
        return module.Start;
    }
    static StartResult PollStart(void* context) noexcept
    {
        FakeModule& module = *static_cast<FakeModule*>(context);
        module.OwnerOrNull->Record(module, 200);
        return module.StartPoll;
    }
    static StopResult BeginStop(void* context) noexcept
    {
        FakeModule& module = *static_cast<FakeModule*>(context);
        module.OwnerOrNull->Record(module, 300);
        if (module.Stop.Status == StopStatus::Stopped)
        {
            module.IsLive = false;
        }
        return module.Stop;
    }
    static StopResult PollStop(void* context) noexcept
    {
        FakeModule& module = *static_cast<FakeModule*>(context);
        module.OwnerOrNull->Record(module, 400);
        if (module.StopPoll.Status == StopStatus::Stopped)
        {
            module.IsLive = false;
        }
        return module.StopPoll;
    }
};
} // namespace

TEST_CASE("Lifecycle construction is inert and callbacks follow the authored order", "[lifecycle]")
{
    Fixture fixture;
    CHECK(fixture.Lifecycle.GetState() == State::Idle);
    CHECK(fixture.EventCount == 0);
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    CHECK(fixture.EventCount == 0);
    CHECK(fixture.Lifecycle.Advance(0) == State::Starting);
    CHECK(fixture.Lifecycle.Advance(2) == State::Starting);
    CHECK(fixture.EventCount == 2);
    CHECK(fixture.Lifecycle.Advance() == State::Ready);
    CHECK(fixture.Events[0] == 100);
    CHECK(fixture.Events[1] == 101);
    CHECK(fixture.Events[2] == 102);
    CHECK(fixture.Modules[2].AttemptsObserved == 3);
    fixture.Lifecycle.RequestStop();
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
    CHECK(fixture.Events[3] == 302);
    CHECK(fixture.Events[4] == 301);
    CHECK(fixture.Events[5] == 300);
    CHECK(fixture.AreProvidersLive);
    CHECK(fixture.Lifecycle.GetOutcome() == Outcome::Completed);
    CHECK(fixture.Lifecycle.GetReport().RemainingCount == 0);
    fixture.Lifecycle.RequestStop();
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
    CHECK(fixture.EventCount == 6);
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    CHECK(fixture.Lifecycle.GetGeneration() == 2);
    CHECK(fixture.Lifecycle.GetOutcome() == Outcome::None);
    CHECK(fixture.Lifecycle.Advance(3) == State::Ready);
}

TEST_CASE("Lifecycle IDs identify records independently of plan positions", "[lifecycle]")
{
    Fixture fixture;
    const Node node = fixture.Nodes[0];
    fixture.Nodes[0] = fixture.Nodes[2];
    fixture.Nodes[2] = node;
    for (Node& entry : fixture.Nodes)
    {
        entry.Providers = {};
    }
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    CHECK(fixture.Lifecycle.Advance(3) == State::Ready);
    CHECK(fixture.Records[2].Position == 0);
    CHECK(fixture.Events[0] == 102);
    fixture.Lifecycle.RequestStop();
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
    CHECK(fixture.Events[3] == 300);
    CHECK(fixture.Events[5] == 302);
}

TEST_CASE("Invalid lifecycle plans invoke no callbacks", "[lifecycle]")
{
    Fixture fixture;
    PlanError expected = PlanError::None;
    SECTION("ID out of range")
    {
        fixture.Nodes[2].Id = 3;
        expected = PlanError::InvalidId;
    }
    SECTION("duplicate ID")
    {
        fixture.Nodes[2].Id = 1;
        expected = PlanError::DuplicateId;
    }
    SECTION("missing name")
    {
        fixture.Nodes[2].Name = {};
        expected = PlanError::MissingName;
    }
    SECTION("missing begin start")
    {
        fixture.Nodes[2].BeginStart = nullptr;
        expected = PlanError::MissingCallback;
    }
    SECTION("missing poll start")
    {
        fixture.Nodes[2].PollStart = nullptr;
        expected = PlanError::MissingCallback;
    }
    SECTION("missing begin stop")
    {
        fixture.Nodes[2].BeginStop = nullptr;
        expected = PlanError::MissingCallback;
    }
    SECTION("missing poll stop")
    {
        fixture.Nodes[2].PollStop = nullptr;
        expected = PlanError::MissingCallback;
    }
    SECTION("provider out of range")
    {
        fixture.Provider1[0] = 3;
        expected = PlanError::InvalidProvider;
    }
    SECTION("self dependency")
    {
        fixture.Provider1[0] = 2;
        expected = PlanError::SelfProvider;
    }
    SECTION("cycle")
    {
        fixture.Nodes[0].Providers = fixture.Provider1;
        expected = PlanError::ProviderCycle;
    }
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::InvalidPlan);
    CHECK(fixture.Lifecycle.GetPlanDiagnostic().Error == expected);
    CHECK(fixture.Lifecycle.GetState() == State::Idle);
    CHECK(fixture.EventCount == 0);
    CHECK(fixture.Lifecycle.GetGeneration() == 0);
}

TEST_CASE("Lifecycle storage is checked before writing or invoking a callback", "[lifecycle]")
{
    Fixture fixture;
    SECTION("records too small")
    {
        Runner runner(fixture.Nodes, std::span(fixture.Records).first(2), fixture.Journal);
        CHECK(runner.Begin() == BeginStatus::InvalidPlan);
        CHECK(runner.GetPlanDiagnostic().Error == PlanError::Capacity);
    }
    SECTION("journal too small")
    {
        Runner runner(fixture.Nodes, fixture.Records, std::span(fixture.Journal).first(2));
        CHECK(runner.Begin() == BeginStatus::InvalidPlan);
        CHECK(runner.GetPlanDiagnostic().Error == PlanError::Capacity);
    }
    CHECK(fixture.EventCount == 0);
}

TEST_CASE("Every partial startup failure uses the same reverse attempt journal", "[lifecycle]")
{
    for (usize failed = 0; failed < 3; ++failed)
    {
        Fixture fixture;
        fixture.Modules[failed].Start =
        {
            .Status = StartStatus::Failed,
            .Error =
            {
                .Domain = TEST_DOMAIN,
                .Value = 42,
            },
        };
        fixture.Modules[failed].ShouldCancelOnStart = true;
        REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
        CHECK(fixture.Lifecycle.Advance(3) == State::Stopping);
        CHECK(fixture.Lifecycle.GetReport().RemainingCount == failed + 1);
        CHECK(fixture.Modules[failed].IsLive);
        CHECK(fixture.Lifecycle.GetFailure().NodeId == failed);
        CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
        CHECK(fixture.EventCount == 2 * (failed + 1));
        CHECK(fixture.Events[failed + 1] == 300 + failed);
        CHECK(fixture.AreProvidersLive);
        CHECK(fixture.Lifecycle.GetOutcome() == Outcome::StartFailed);
        CHECK(fixture.Lifecycle.GetFailure().Error.Value == 42);
        CHECK_FALSE(fixture.Lifecycle.ReportFailure(0, { .Domain = TEST_DOMAIN, .Value = 99 }));
        CHECK(fixture.Lifecycle.GetFailure().Error.Value == 42);
    }
}

TEST_CASE("Pending startup yields once and blocks dependent starts", "[lifecycle]")
{
    Fixture fixture;
    fixture.Modules[1].Start = { .Status = StartStatus::Pending, .WaitReason = 7 };
    fixture.Modules[1].StartPoll = { .Status = StartStatus::Pending, .WaitReason = 8 };
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    CHECK(fixture.Lifecycle.Advance(50) == State::Starting);
    CHECK(fixture.EventCount == 2);
    CHECK(fixture.Records[1].WaitReason == 7);
    CHECK(fixture.Lifecycle.Advance(50) == State::Starting);
    CHECK(fixture.EventCount == 3);
    CHECK(fixture.Records[1].WaitReason == 8);
    CHECK_FALSE(fixture.Modules[2].IsLive);
    SECTION("poll succeeds")
    {
        fixture.Modules[1].StartPoll = {};
        CHECK(fixture.Lifecycle.Advance(3) == State::Ready);
        CHECK(fixture.Events[3] == 201);
        CHECK(fixture.Events[4] == 102);
    }
    SECTION("poll fails")
    {
        fixture.Modules[1].StartPoll =
        {
            .Status = StartStatus::Failed,
            .Error =
            {
                .Domain = TEST_DOMAIN,
                .Value = 17,
            },
        };
        CHECK(fixture.Lifecycle.Advance(3) == State::Stopping);
        CHECK(fixture.Lifecycle.GetFailure().Error.Value == 17);
        CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
        CHECK(fixture.Events[4] == 301);
        CHECK(fixture.Events[5] == 300);
    }
    CHECK(fixture.AreProvidersLive);
}

TEST_CASE("Cancellation before work and during pending startup never starts a consumer", "[lifecycle]")
{
    Fixture fixture;
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    SECTION("before first callback")
    {
        fixture.Lifecycle.RequestStop();
        CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
        CHECK(fixture.EventCount == 0);
    }
    SECTION("pending callback")
    {
        fixture.Modules[1].Start = { .Status = StartStatus::Pending };
        CHECK(fixture.Lifecycle.Advance(3) == State::Starting);
        fixture.Lifecycle.RequestStop();
        CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
        CHECK(fixture.Events[2] == 301);
        CHECK(fixture.Events[3] == 300);
        CHECK_FALSE(fixture.Modules[2].IsLive);
    }
    CHECK(fixture.Lifecycle.GetOutcome() == Outcome::Cancelled);
}

TEST_CASE("A start success racing with cancellation is still retired", "[lifecycle]")
{
    Fixture fixture;
    fixture.Modules[1].ShouldCancelOnStart = true;
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopping);
    CHECK(fixture.Modules[1].IsLive);
    CHECK_FALSE(fixture.Modules[2].IsLive);
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
    CHECK(fixture.Events[2] == 301);
    CHECK(fixture.Events[3] == 300);
    CHECK(fixture.AreProvidersLive);
}

TEST_CASE("Pending stop preserves providers and calls BeginStop once", "[lifecycle]")
{
    Fixture fixture;
    fixture.Modules[2].Stop = { .Status = StopStatus::Pending, .WaitReason = 9 };
    fixture.Modules[2].StopPoll = { .Status = StopStatus::Pending, .WaitReason = 10 };
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    REQUIRE(fixture.Lifecycle.Advance(3) == State::Ready);
    fixture.Lifecycle.RequestStop();
    CHECK(fixture.Lifecycle.Advance(50) == State::Stopping);
    CHECK(fixture.EventCount == 4);
    CHECK(fixture.Records[2].WaitReason == 9);
    CHECK(fixture.Lifecycle.Begin() == BeginStatus::Busy);
    CHECK(fixture.Lifecycle.Advance(50) == State::Stopping);
    CHECK(fixture.Events[4] == 402);
    CHECK(fixture.Records[2].WaitReason == 10);
    CHECK(fixture.Modules[0].IsLive);
    CHECK(fixture.Modules[1].IsLive);
    fixture.Modules[2].StopPoll = {};
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
    CHECK(fixture.Events[5] == 402);
    CHECK(fixture.Events[6] == 301);
    CHECK(fixture.Events[7] == 300);
    CHECK(fixture.AreProvidersLive);
}

TEST_CASE("Unsafe cleanup or an owner deadline pins providers and preserves the first failure", "[lifecycle]")
{
    Fixture fixture;
    fixture.Modules[1].Start = { .Status = StartStatus::Failed, .Error = { .Domain = TEST_DOMAIN, .Value = 42 } };
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    REQUIRE(fixture.Lifecycle.Advance(3) == State::Stopping);
    SECTION("unsafe stop")
    {
        fixture.Modules[1].Stop = { .Status = StopStatus::Unsafe, .Error = { .Domain = TEST_DOMAIN, .Value = 99 } };
        CHECK(fixture.Lifecycle.Advance(3) == State::CleanupBlocked);
    }
    SECTION("deadline after pending stop")
    {
        CHECK_FALSE(fixture.Lifecycle.BlockCleanup({ .Domain = TEST_DOMAIN, .Value = 99 }));
        fixture.Modules[1].Stop = { .Status = StopStatus::Pending };
        CHECK(fixture.Lifecycle.Advance(3) == State::Stopping);
        CHECK(fixture.Lifecycle.BlockCleanup({ .Domain = TEST_DOMAIN, .Value = 99 }));
    }
    const usize events = fixture.EventCount;
    CHECK(fixture.Lifecycle.Advance(3) == State::CleanupBlocked);
    CHECK(fixture.EventCount == events);
    CHECK(fixture.Lifecycle.Begin() == BeginStatus::Busy);
    CHECK(fixture.Lifecycle.GetReport().RemainingCount == 2);
    CHECK(fixture.Modules[0].IsLive);
    CHECK(fixture.Modules[1].IsLive);
    CHECK(fixture.Lifecycle.GetFailure().Error.Value == 42);
    CHECK(fixture.Lifecycle.GetCleanupFailure().NodeId == 1);
    CHECK(fixture.Lifecycle.GetCleanupFailure().Error.Value == 99);
    REQUIRE(fixture.Lifecycle.ResumeCleanup());
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
    CHECK(fixture.Events[events] == 401);
    CHECK(fixture.AreProvidersLive);
    CHECK(fixture.Lifecycle.GetFailure().Error.Value == 42);
    CHECK(fixture.Lifecycle.GetCleanupFailure().Error.Value == 99);
}

TEST_CASE("Runtime failure and cross-thread stop requests retire on the owner thread", "[lifecycle]")
{
    Fixture fixture;
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    REQUIRE(fixture.Lifecycle.Advance(3) == State::Ready);
    SECTION("runtime failure")
    {
        CHECK_FALSE(fixture.Lifecycle.ReportFailure(3, { .Domain = TEST_DOMAIN, .Value = 42 }));
        CHECK(fixture.Lifecycle.ReportFailure(1, { .Domain = TEST_DOMAIN, .Value = 42 }));
        CHECK_FALSE(fixture.Lifecycle.ReportFailure(2, { .Domain = TEST_DOMAIN, .Value = 99 }));
        CHECK(fixture.Lifecycle.GetOutcome() == Outcome::RuntimeFailed);
        CHECK(fixture.Lifecycle.GetFailure().Error.Value == 42);
    }
    SECTION("worker requests stop")
    {
        threading::Thread worker;
        REQUIRE(worker.Start([&fixture] { fixture.Lifecycle.RequestStop(); }) == threading::ThreadStatus::Ok);
        REQUIRE(worker.Join() == threading::ThreadStatus::Ok);
        CHECK(fixture.EventCount == 3);
    }
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
    CHECK(fixture.AreProvidersLive);
}

TEST_CASE("Lifecycle rejects reentry and handles an empty profile", "[lifecycle]")
{
    SECTION("callback reentry")
    {
        Fixture fixture;
        fixture.Modules[0].ShouldReenter = true;
        REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
        CHECK(fixture.Lifecycle.Advance(3) == State::Ready);
        CHECK(fixture.IsReentryRejected);
        CHECK(fixture.EventCount == 3);
    }
    SECTION("empty profile")
    {
        Runner runner({}, {}, {});
        REQUIRE(runner.Begin() == BeginStatus::Started);
        CHECK(runner.Advance() == State::Ready);
        runner.RequestStop();
        CHECK(runner.Advance() == State::Stopped);
    }
}

TEST_CASE("Plan diagnostics name misordered, cyclic and repeated provider edges", "[lifecycle]")
{
    Fixture fixture;
    usize providerTwo[1]{2};
    usize repeatedProvider[2]{1, 1};
    PlanDiagnostic expected;
    SECTION("misordered provider without a cycle")
    {
        fixture.Nodes[1].Providers = {};
        fixture.Nodes[2].Providers = {};
        fixture.Nodes[0].Providers = providerTwo;
        expected = { .Error = PlanError::ProviderOrder, .NodeId = 0, .ProviderId = 2 };
    }
    SECTION("transitive cycle")
    {
        fixture.Nodes[0].Providers = providerTwo;
        expected = { .Error = PlanError::ProviderCycle, .NodeId = 0, .ProviderId = 2 };
    }
    SECTION("repeated provider")
    {
        fixture.Nodes[2].Providers = repeatedProvider;
        expected = { .Error = PlanError::DuplicateProvider, .NodeId = 2, .ProviderId = 1 };
    }
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::InvalidPlan);
    const PlanDiagnostic diagnostic = fixture.Lifecycle.GetPlanDiagnostic();
    CHECK(diagnostic.Error == expected.Error);
    CHECK(diagnostic.NodeId == expected.NodeId);
    CHECK(diagnostic.ProviderId == expected.ProviderId);
    CHECK(fixture.EventCount == 0);
    CHECK(fixture.Lifecycle.GetState() == State::Idle);
    for (const NodeRecord& record : fixture.Lifecycle.GetRecords())
    {
        CHECK(record.State == NodeState::Dormant);
    }
    // Repairing the plan makes the same storage usable without stale marks.
    fixture.Nodes[0].Providers = {};
    fixture.Nodes[1].Providers = fixture.Provider0;
    fixture.Nodes[2].Providers = fixture.Provider1;
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    CHECK(fixture.Lifecycle.Advance(3) == State::Ready);
}

namespace
{
// Diamond plan A <- {B, C} <- D. Dense IDs are deliberately not plan positions.
constexpr usize DIAMOND_NODE_COUNT = 4;
constexpr usize DIAMOND_IDS[DIAMOND_NODE_COUNT]{2, 0, 3, 1};
constexpr usize MAXIMUM_ADVANCES = 64;

enum class Boundary : uint8
{
    BeginStart,
    PollStart
};

struct Diamond;
struct DiamondModule
{
    Diamond* OwnerOrNull = nullptr;
    usize Id = 0;
    StartResult BeginResult;
    StartResult PollResult;
    usize StartCalls = 0;
    usize BeginStopCalls = 0;
    usize PollStopCalls = 0;
    bool IsLive = false;
};

struct Diamond
{
    DiamondModule Modules[DIAMOND_NODE_COUNT];
    usize ProvidersOfB[1]{DIAMOND_IDS[0]};
    usize ProvidersOfC[1]{DIAMOND_IDS[0]};
    usize ProvidersOfD[2]{DIAMOND_IDS[1], DIAMOND_IDS[2]};
    Node Nodes[DIAMOND_NODE_COUNT];
    NodeRecord Records[DIAMOND_NODE_COUNT];
    usize Journal[DIAMOND_NODE_COUNT]{};
    usize StopOrder[DIAMOND_NODE_COUNT]{};
    usize StopCount = 0;
    usize Violations = 0;
    Runner Lifecycle{Nodes, Records, Journal};

    Diamond() noexcept
    {
        constexpr std::string_view NAMES[DIAMOND_NODE_COUNT]{"A", "B", "C", "D"};
        for (usize position = 0; position < DIAMOND_NODE_COUNT; ++position)
        {
            const usize id = DIAMOND_IDS[position];
            Modules[id].OwnerOrNull = this;
            Modules[id].Id = id;
            Nodes[position] =
            {
                .Id = id,
                .Name = NAMES[position],
                .ContextOrNull = &Modules[id],
                .Providers = {},
                .BeginStart = BeginStart,
                .PollStart = PollStart,
                .BeginStop = BeginStop,
                .PollStop = PollStop,
            };
        }
        Nodes[1].Providers = ProvidersOfB;
        Nodes[2].Providers = ProvidersOfC;
        Nodes[3].Providers = ProvidersOfD;
    }

    // At every callback, exactly the nodes before the caller are Ready and live:
    // its declared providers and every earlier node stay available. Later nodes
    // are untouched during startup and already Stopped (or never attempted)
    // during cleanup.
    void CheckAvailability(const DiamondModule& module, bool isStop) noexcept
    {
        const usize position = Records[module.Id].Position;
        for (usize other = 0; other < DIAMOND_NODE_COUNT; ++other)
        {
            const usize id = DIAMOND_IDS[other];
            const NodeState state = Records[id].State;
            bool isExpected = true;
            if (other < position)
            {
                isExpected = state == NodeState::Ready && Modules[id].IsLive == true;
            }
            else if (other > position)
            {
                const bool isUntouched = state == NodeState::Dormant && Modules[id].StartCalls == 0;
                const bool isRetired = state == NodeState::Stopped && Modules[id].IsLive == false;
                isExpected = isStop == true ? (isUntouched == true || isRetired == true) : isUntouched;
            }
            if (isExpected == false)
            {
                ++Violations;
            }
        }
        for (const usize provider : Nodes[position].Providers)
        {
            if (Records[provider].State != NodeState::Ready || Modules[provider].IsLive == false)
            {
                ++Violations;
            }
        }
    }
    static StartResult BeginStart(void* context) noexcept
    {
        DiamondModule& module = *static_cast<DiamondModule*>(context);
        module.OwnerOrNull->CheckAvailability(module, false);
        ++module.StartCalls;
        module.IsLive = true; // Even a Failed result acquired something to retire.
        return module.BeginResult;
    }
    static StartResult PollStart(void* context) noexcept
    {
        DiamondModule& module = *static_cast<DiamondModule*>(context);
        module.OwnerOrNull->CheckAvailability(module, false);
        ++module.StartCalls;
        return module.PollResult;
    }
    static StopResult BeginStop(void* context) noexcept
    {
        DiamondModule& module = *static_cast<DiamondModule*>(context);
        Diamond& owner = *module.OwnerOrNull;
        owner.CheckAvailability(module, true);
        ++module.BeginStopCalls;
        if (owner.StopCount < DIAMOND_NODE_COUNT)
        {
            owner.StopOrder[owner.StopCount++] = owner.Records[module.Id].Position;
        }
        // Every stop stays Pending once so providers must outlive a polled retirement.
        return { .Status = StopStatus::Pending };
    }
    static StopResult PollStop(void* context) noexcept
    {
        DiamondModule& module = *static_cast<DiamondModule*>(context);
        module.OwnerOrNull->CheckAvailability(module, true);
        ++module.PollStopCalls;
        module.IsLive = false;
        return {};
    }
    // Advances one callback at a time and checks the structured cleanup results
    // whenever cleanup is in progress.
    State RunToRest() noexcept
    {
        for (usize i = 0; i < MAXIMUM_ADVANCES; ++i)
        {
            const State state = Lifecycle.Advance();
            if (state == State::Stopping)
            {
                const Report report = Lifecycle.GetReport();
                const std::span<const usize> remaining = Lifecycle.GetRemainingCleanup();
                if (report.Cleanup != CleanupOutcome::Pending || remaining.size() != report.RemainingCount ||
                    report.StoppedCount + report.RemainingCount != report.AttemptedCount)
                {
                    ++Violations;
                }
                for (usize entry = 0; entry < remaining.size(); ++entry)
                {
                    Violations += remaining[entry] == entry ? 0 : 1;
                }
            }
            if (state == State::Ready || state == State::Stopped || state == State::CleanupBlocked)
            {
                return state;
            }
        }
        return Lifecycle.GetState();
    }
};
} // namespace

TEST_CASE("Every failure boundary stops exactly the attempted nodes while providers stay available", "[lifecycle]")
{
    constexpr uint32 FAILURE_CODE = 23;
    for (usize failed = 0; failed < DIAMOND_NODE_COUNT; ++failed)
    {
        for (const Boundary boundary : {Boundary::BeginStart, Boundary::PollStart})
        {
            CAPTURE(failed, boundary == Boundary::BeginStart);
            Diamond diamond;
            DiamondModule& failing = diamond.Modules[DIAMOND_IDS[failed]];
            const StartResult failure =
            {
                .Status = StartStatus::Failed,
                .Error = { .Domain = TEST_DOMAIN, .Value = FAILURE_CODE },
            };
            if (boundary == Boundary::BeginStart)
            {
                failing.BeginResult = failure;
            }
            else
            {
                failing.BeginResult = { .Status = StartStatus::Pending };
                failing.PollResult = failure;
            }
            REQUIRE(diamond.Lifecycle.Begin() == BeginStatus::Started);
            CHECK(diamond.RunToRest() == State::Stopped);
            CHECK(diamond.Violations == 0);

            const Report report = diamond.Lifecycle.GetReport();
            CHECK(report.Generation == 1);
            CHECK(report.RunnerState == State::Stopped);
            CHECK(report.Primary == Outcome::StartFailed);
            CHECK(report.PrimaryFailure.NodeId == DIAMOND_IDS[failed]);
            CHECK(report.PrimaryFailure.Source ==
                  (boundary == Boundary::BeginStart ? FailureSource::BeginStart : FailureSource::PollStart));
            CHECK(report.PrimaryFailure.Error.Domain == TEST_DOMAIN);
            CHECK(report.PrimaryFailure.Error.Value == FAILURE_CODE);
            CHECK(report.Cleanup == CleanupOutcome::Complete);
            CHECK(report.CleanupFailure.Source == FailureSource::None);
            CHECK(report.AttemptedCount == failed + 1);
            CHECK(report.StoppedCount == failed + 1);
            CHECK(report.RemainingCount == 0);
            CHECK(diamond.Lifecycle.GetRemainingCleanup().empty());

            REQUIRE(diamond.StopCount == failed + 1);
            for (usize i = 0; i < diamond.StopCount; ++i)
            {
                CHECK(diamond.StopOrder[i] == failed - i);
            }
            for (usize position = 0; position < DIAMOND_NODE_COUNT; ++position)
            {
                const DiamondModule& module = diamond.Modules[DIAMOND_IDS[position]];
                const bool isAttempted = position <= failed;
                CHECK(module.BeginStopCalls == (isAttempted == true ? 1 : 0));
                CHECK(module.PollStopCalls == (isAttempted == true ? 1 : 0));
                CHECK(module.IsLive == false);
                CHECK(diamond.Records[module.Id].State ==
                      (isAttempted == true ? NodeState::Stopped : NodeState::Dormant));
                if (isAttempted == false)
                {
                    CHECK(module.StartCalls == 0);
                }
            }
        }
    }
}

TEST_CASE("A completed diamond plan reports separate primary and cleanup results", "[lifecycle]")
{
    Diamond diamond;
    REQUIRE(diamond.Lifecycle.Begin() == BeginStatus::Started);
    REQUIRE(diamond.RunToRest() == State::Ready);
    Report report = diamond.Lifecycle.GetReport();
    CHECK(report.Primary == Outcome::None);
    CHECK(report.Cleanup == CleanupOutcome::None);
    CHECK(report.AttemptedCount == DIAMOND_NODE_COUNT);
    CHECK(report.RemainingCount == DIAMOND_NODE_COUNT);
    SECTION("stop request")
    {
        diamond.Lifecycle.RequestStop();
        CHECK(diamond.RunToRest() == State::Stopped);
        report = diamond.Lifecycle.GetReport();
        CHECK(report.Primary == Outcome::Completed);
        CHECK(report.PrimaryFailure.Source == FailureSource::None);
        CHECK(report.PrimaryFailure.NodeId == INVALID_NODE);
    }
    SECTION("runtime failure")
    {
        CHECK(diamond.Lifecycle.ReportFailure(DIAMOND_IDS[2], { .Domain = TEST_DOMAIN, .Value = 5 }));
        CHECK(diamond.RunToRest() == State::Stopped);
        report = diamond.Lifecycle.GetReport();
        CHECK(report.Primary == Outcome::RuntimeFailed);
        CHECK(report.PrimaryFailure.Source == FailureSource::Runtime);
        CHECK(report.PrimaryFailure.NodeId == DIAMOND_IDS[2]);
        CHECK(report.PrimaryFailure.Error.Value == 5);
    }
    CHECK(diamond.Violations == 0);
    CHECK(report.Cleanup == CleanupOutcome::Complete);
    CHECK(report.StoppedCount == DIAMOND_NODE_COUNT);
    CHECK(report.RemainingCount == 0);
    CHECK(diamond.StopOrder[0] == 3);
    CHECK(diamond.StopOrder[3] == 0);
}

TEST_CASE("Blocked cleanup reports its source and the retained journal without replacing the first cause",
          "[lifecycle]")
{
    Fixture fixture;
    fixture.Modules[1].Start = { .Status = StartStatus::Failed, .Error = { .Domain = TEST_DOMAIN, .Value = 42 } };
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    REQUIRE(fixture.Lifecycle.Advance(3) == State::Stopping);
    FailureSource expected = FailureSource::None;
    SECTION("unsafe begin stop")
    {
        fixture.Modules[1].Stop = { .Status = StopStatus::Unsafe, .Error = { .Domain = TEST_DOMAIN, .Value = 99 } };
        expected = FailureSource::BeginStop;
    }
    SECTION("unsafe poll stop")
    {
        fixture.Modules[1].Stop = { .Status = StopStatus::Pending };
        fixture.Modules[1].StopPoll = { .Status = StopStatus::Unsafe, .Error = { .Domain = TEST_DOMAIN, .Value = 99 } };
        CHECK(fixture.Lifecycle.Advance() == State::Stopping);
        expected = FailureSource::PollStop;
    }
    SECTION("owner deadline")
    {
        fixture.Modules[1].Stop = { .Status = StopStatus::Pending };
        CHECK(fixture.Lifecycle.Advance() == State::Stopping);
        CHECK(fixture.Lifecycle.GetReport().Cleanup == CleanupOutcome::Pending);
        CHECK(fixture.Lifecycle.BlockCleanup({ .Domain = TEST_DOMAIN, .Value = 99 }));
        expected = FailureSource::Deadline;
    }
    CHECK(fixture.Lifecycle.Advance(3) == State::CleanupBlocked);
    const Report report = fixture.Lifecycle.GetReport();
    CHECK(report.Cleanup == CleanupOutcome::Blocked);
    CHECK(report.Primary == Outcome::StartFailed);
    CHECK(report.PrimaryFailure.NodeId == 1);
    CHECK(report.PrimaryFailure.Source == FailureSource::BeginStart);
    CHECK(report.PrimaryFailure.Error.Value == 42);
    CHECK(report.CleanupFailure.NodeId == 1);
    CHECK(report.CleanupFailure.Source == expected);
    CHECK(report.CleanupFailure.Error.Domain == TEST_DOMAIN);
    CHECK(report.CleanupFailure.Error.Value == 99);
    CHECK(report.AttemptedCount == 2);
    CHECK(report.StoppedCount == 0);
    CHECK(report.RemainingCount == 2);
    const std::span<const usize> remaining = fixture.Lifecycle.GetRemainingCleanup();
    REQUIRE(remaining.size() == 2);
    CHECK(remaining[0] == 0);
    CHECK(remaining[1] == 1);
    CHECK(fixture.Records[1].State == NodeState::CleanupBlocked);
    CHECK(fixture.Modules[0].IsLive);
    CHECK(fixture.Modules[1].IsLive);
}
