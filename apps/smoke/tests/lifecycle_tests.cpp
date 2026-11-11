#include <ludus/foundation/threading/thread.hpp>

#include "../internal/lifecycle.h"

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::smoke::lifecycle;
namespace threading = ludus::foundation::threading;
namespace
{
struct Fixture;
struct FakeModule
{
    Fixture* Owner = nullptr;
    usize Id = 0;
    StartResult Start;
    StartResult StartPoll;
    StopResult Stop;
    StopResult StopPoll;
    bool Live = false;
    bool CancelOnStart = false;
    bool Reenter = false;
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
    bool ProvidersLive = true;
    bool ReentryRejected = false;
    Runner Lifecycle{Nodes, Records, Journal};

    Fixture() noexcept
    {
        for (usize id = 0; id < 3; ++id)
        {
            Modules[id].Owner = this;
            Modules[id].Id = id;
            Nodes[id] =
            {
                .Id = id,
                .Name = "Fake",
                .Context = &Modules[id],
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
    void Record(FakeModule& module, uint32 operation) noexcept
    {
        if (EventCount < 64)
        {
            Events[EventCount++] = operation + static_cast<uint32>(module.Id);
        }
        else
        {
            ProvidersLive = false;
        }
        for (const auto provider : Nodes[Records[module.Id].Position].Providers)
        {
            ProvidersLive = ProvidersLive && Modules[provider].Live;
        }
    }
    static StartResult BeginStart(void* context) noexcept
    {
        auto& module = *static_cast<FakeModule*>(context);
        auto& owner = *module.Owner;
        owner.Record(module, 100);
        module.Live = true; // Even a Failed result acquired something to retire.
        module.AttemptsObserved = owner.Lifecycle.GetAttemptedCount();
        if (module.CancelOnStart)
        {
            owner.Lifecycle.RequestStop();
        }
        if (module.Reenter)
        {
            owner.ReentryRejected =
                owner.Lifecycle.Begin() == BeginStatus::Busy && owner.Lifecycle.Advance(3) == State::Starting;
        }
        return module.Start;
    }
    static StartResult PollStart(void* context) noexcept
    {
        auto& module = *static_cast<FakeModule*>(context);
        module.Owner->Record(module, 200);
        return module.StartPoll;
    }
    static StopResult BeginStop(void* context) noexcept
    {
        auto& module = *static_cast<FakeModule*>(context);
        module.Owner->Record(module, 300);
        if (module.Stop.Status == StopStatus::Stopped)
        {
            module.Live = false;
        }
        return module.Stop;
    }
    static StopResult PollStop(void* context) noexcept
    {
        auto& module = *static_cast<FakeModule*>(context);
        module.Owner->Record(module, 400);
        if (module.StopPoll.Status == StopStatus::Stopped)
        {
            module.Live = false;
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
    CHECK(fixture.ProvidersLive);
    CHECK(fixture.Lifecycle.GetOutcome() == Outcome::Completed);
    CHECK(fixture.Lifecycle.GetAttemptedCount() == 0);
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
    const auto node = fixture.Nodes[0];
    fixture.Nodes[0] = fixture.Nodes[2];
    fixture.Nodes[2] = node;
    for (auto& entry : fixture.Nodes)
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
        expected = PlanError::ProviderOrder;
    }
    SECTION("backward edge or cycle")
    {
        fixture.Nodes[0].Providers = fixture.Provider1;
        expected = PlanError::ProviderOrder;
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
        fixture.Modules[failed].Start = { .Status = StartStatus::Failed, .Error = 42 };
        fixture.Modules[failed].CancelOnStart = true;
        REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
        CHECK(fixture.Lifecycle.Advance(3) == State::Stopping);
        CHECK(fixture.Lifecycle.GetAttemptedCount() == failed + 1);
        CHECK(fixture.Modules[failed].Live);
        CHECK(fixture.Lifecycle.GetFailure().NodeId == failed);
        CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
        CHECK(fixture.EventCount == 2 * (failed + 1));
        CHECK(fixture.Events[failed + 1] == 300 + failed);
        CHECK(fixture.ProvidersLive);
        CHECK(fixture.Lifecycle.GetOutcome() == Outcome::StartFailed);
        CHECK(fixture.Lifecycle.GetFailure().Error == 42);
        CHECK_FALSE(fixture.Lifecycle.ReportFailure(0, 99));
        CHECK(fixture.Lifecycle.GetFailure().Error == 42);
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
    CHECK_FALSE(fixture.Modules[2].Live);
    SECTION("poll succeeds")
    {
        fixture.Modules[1].StartPoll = {};
        CHECK(fixture.Lifecycle.Advance(3) == State::Ready);
        CHECK(fixture.Events[3] == 201);
        CHECK(fixture.Events[4] == 102);
    }
    SECTION("poll fails")
    {
        fixture.Modules[1].StartPoll = { .Status = StartStatus::Failed, .Error = 17 };
        CHECK(fixture.Lifecycle.Advance(3) == State::Stopping);
        CHECK(fixture.Lifecycle.GetFailure().Error == 17);
        CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
        CHECK(fixture.Events[4] == 301);
        CHECK(fixture.Events[5] == 300);
    }
    CHECK(fixture.ProvidersLive);
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
        CHECK_FALSE(fixture.Modules[2].Live);
    }
    CHECK(fixture.Lifecycle.GetOutcome() == Outcome::Cancelled);
}

TEST_CASE("A start success racing with cancellation is still retired", "[lifecycle]")
{
    Fixture fixture;
    fixture.Modules[1].CancelOnStart = true;
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopping);
    CHECK(fixture.Modules[1].Live);
    CHECK_FALSE(fixture.Modules[2].Live);
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
    CHECK(fixture.Events[2] == 301);
    CHECK(fixture.Events[3] == 300);
    CHECK(fixture.ProvidersLive);
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
    CHECK(fixture.Modules[0].Live);
    CHECK(fixture.Modules[1].Live);
    fixture.Modules[2].StopPoll = {};
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
    CHECK(fixture.Events[5] == 402);
    CHECK(fixture.Events[6] == 301);
    CHECK(fixture.Events[7] == 300);
    CHECK(fixture.ProvidersLive);
}

TEST_CASE("Unsafe cleanup or an owner deadline pins providers and preserves the first failure", "[lifecycle]")
{
    Fixture fixture;
    fixture.Modules[1].Start = { .Status = StartStatus::Failed, .Error = 42 };
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    REQUIRE(fixture.Lifecycle.Advance(3) == State::Stopping);
    SECTION("unsafe stop")
    {
        fixture.Modules[1].Stop = { .Status = StopStatus::Unsafe, .Error = 99 };
        CHECK(fixture.Lifecycle.Advance(3) == State::CleanupBlocked);
    }
    SECTION("deadline after pending stop")
    {
        CHECK_FALSE(fixture.Lifecycle.BlockCleanup(99));
        fixture.Modules[1].Stop = { .Status = StopStatus::Pending };
        CHECK(fixture.Lifecycle.Advance(3) == State::Stopping);
        CHECK(fixture.Lifecycle.BlockCleanup(99));
    }
    const auto events = fixture.EventCount;
    CHECK(fixture.Lifecycle.Advance(3) == State::CleanupBlocked);
    CHECK(fixture.EventCount == events);
    CHECK(fixture.Lifecycle.Begin() == BeginStatus::Busy);
    CHECK(fixture.Lifecycle.GetAttemptedCount() == 2);
    CHECK(fixture.Modules[0].Live);
    CHECK(fixture.Modules[1].Live);
    CHECK(fixture.Lifecycle.GetFailure().Error == 42);
    CHECK(fixture.Lifecycle.GetCleanupFailure().NodeId == 1);
    CHECK(fixture.Lifecycle.GetCleanupFailure().Error == 99);
    REQUIRE(fixture.Lifecycle.ResumeCleanup());
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
    CHECK(fixture.Events[events] == 401);
    CHECK(fixture.ProvidersLive);
    CHECK(fixture.Lifecycle.GetFailure().Error == 42);
    CHECK(fixture.Lifecycle.GetCleanupFailure().Error == 99);
}

TEST_CASE("Runtime failure and cross-thread stop requests retire on the owner thread", "[lifecycle]")
{
    Fixture fixture;
    REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
    REQUIRE(fixture.Lifecycle.Advance(3) == State::Ready);
    SECTION("runtime failure")
    {
        CHECK_FALSE(fixture.Lifecycle.ReportFailure(3, 42));
        CHECK(fixture.Lifecycle.ReportFailure(1, 42));
        CHECK_FALSE(fixture.Lifecycle.ReportFailure(2, 99));
        CHECK(fixture.Lifecycle.GetOutcome() == Outcome::RuntimeFailed);
        CHECK(fixture.Lifecycle.GetFailure().Error == 42);
    }
    SECTION("worker requests stop")
    {
        threading::Thread worker;
        REQUIRE(worker.Start([&fixture] { fixture.Lifecycle.RequestStop(); }) == threading::ThreadStatus::Ok);
        REQUIRE(worker.Join() == threading::ThreadStatus::Ok);
        CHECK(fixture.EventCount == 3);
    }
    CHECK(fixture.Lifecycle.Advance(3) == State::Stopped);
    CHECK(fixture.ProvidersLive);
}

TEST_CASE("Lifecycle rejects reentry and handles an empty profile", "[lifecycle]")
{
    SECTION("callback reentry")
    {
        Fixture fixture;
        fixture.Modules[0].Reenter = true;
        REQUIRE(fixture.Lifecycle.Begin() == BeginStatus::Started);
        CHECK(fixture.Lifecycle.Advance(3) == State::Ready);
        CHECK(fixture.ReentryRejected);
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
