#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/threading/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

using namespace ludus::foundation;
using namespace ludus::foundation::threading;

namespace
{
JobOutcome Increment(void* context) noexcept
{
    ++*static_cast<uint32*>(context);
    return JobOutcome::Succeeded;
}
JobOutcome Fail(void*) noexcept
{
    return JobOutcome::Failed;
}
JobOutcome InvalidResult(void*) noexcept
{
    return JobOutcome::Running;
}
JobOutcome Succeed(void*) noexcept
{
    return JobOutcome::Succeeded;
}
JobOutcome Cancelled(void*) noexcept
{
    return JobOutcome::Cancelled;
}

struct Inputs
{
    uint32 Left = 0;
    uint32 Right = 0;
    uint32 Sum = 0;
};
JobOutcome Sum(void* context) noexcept
{
    Inputs& inputs = *static_cast<Inputs*>(context);
    inputs.Sum = inputs.Left + inputs.Right;
    return inputs.Sum == 2 ? JobOutcome::Succeeded : JobOutcome::Failed;
}
struct HeldJob
{
    std::atomic<bool> HasEntered{false};
    std::atomic<bool> IsReleased{false};
};
JobOutcome Hold(void* context) noexcept
{
    HeldJob& held = *static_cast<HeldJob*>(context);
    held.HasEntered.store(true);
    held.HasEntered.notify_all();
    held.IsReleased.wait(false);
    return JobOutcome::Succeeded;
}
struct Reentrant
{
    JobSystem& System;
    JobGraph& Graph;
    JobStatus WaitStatus = JobStatus::Ok;
    JobStatus SubmitStatus = JobStatus::Ok;
    JobStatus ShutdownStatus = JobStatus::Ok;
    JobStatus InitializeStatus = JobStatus::Ok;
};
JobOutcome Reenter(void* context) noexcept
{
    Reentrant& data = *static_cast<Reentrant*>(context);
    JobOutcome outcome{};
    data.WaitStatus = data.System.Wait(outcome);
    data.SubmitStatus = data.System.Submit(data.Graph);
    data.ShutdownStatus = data.System.Shutdown();
    data.InitializeStatus = data.System.Initialize();
    return JobOutcome::Succeeded;
}
JobOutcome GetOutcome(const JobGraph& graph, JobHandle handle)
{
    JobOutcome outcome{};
    REQUIRE(graph.GetOutcome(handle, outcome) == JobStatus::Ok);
    return outcome;
}
} // namespace

TEST_CASE("Graph construction validates handles, capacity and cycles", "[threading]")
{
    JobGraph graph;
    JobHandle first{};
    JobHandle second{};
    JobHandle third{};
    REQUIRE(graph.Add(Succeed, nullptr, first) == JobStatus::NotInitialized);
    REQUIRE(graph.Initialize({0, 0}) == JobStatus::InvalidArgument);
    REQUIRE(graph.Initialize({2, 2}) == JobStatus::Ok);
    REQUIRE(graph.Initialize() == JobStatus::AlreadyInitialized);
    REQUIRE(graph.Add(nullptr, nullptr, first) == JobStatus::InvalidArgument);
    REQUIRE(graph.Add(Succeed, nullptr, first) == JobStatus::Ok);
    REQUIRE(graph.Add(Succeed, nullptr, second) == JobStatus::Ok);
    REQUIRE(graph.Add(Succeed, nullptr, third) == JobStatus::CapacityExceeded);
    REQUIRE(third.GraphId == 0);
    REQUIRE(graph.DependsOn(first, first) == JobStatus::CycleDetected);
    REQUIRE(graph.DependsOn(second, first) == JobStatus::Ok);
    REQUIRE(graph.DependsOn(second, first) == JobStatus::DuplicateDependency);
    REQUIRE(graph.DependsOn(first, second) == JobStatus::Ok);
    REQUIRE(graph.Seal() == JobStatus::CycleDetected);
    JobSystem system;
    REQUIRE(system.Initialize() == JobStatus::Ok);
    REQUIRE(system.Submit(graph) == JobStatus::NotSealed);
    REQUIRE(graph.Reset() == JobStatus::Ok);
    REQUIRE(graph.DependsOn(second, first) == JobStatus::InvalidHandle);
    JobOutcome result{};
    REQUIRE(graph.GetOutcome(first, result) == JobStatus::InvalidHandle);

    JobGraph other;
    REQUIRE(other.Initialize({1, 0}) == JobStatus::Ok);
    REQUIRE(other.Add(Succeed, nullptr, third) == JobStatus::Ok);
    REQUIRE(graph.Add(Succeed, nullptr, first) == JobStatus::Ok);
    REQUIRE(graph.DependsOn(first, third) == JobStatus::InvalidHandle);
    REQUIRE(graph.Add(Succeed, nullptr, second) == JobStatus::Ok);
    REQUIRE(graph.Seal() == JobStatus::Ok);
    REQUIRE(graph.Add(Succeed, nullptr, third) == JobStatus::Busy);
}

TEST_CASE("Dependency publication and graph reuse agree across serial and workers", "[threading]")
{
    for (const uint32 workers : {0u, 1u, 4u})
    {
        JobSystem system;
        REQUIRE(system.Initialize({workers}) == JobStatus::Ok);
        JobGraph graph;
        REQUIRE(graph.Initialize({3, 2}) == JobStatus::Ok);
        Inputs inputs;
        JobHandle left{};
        JobHandle right{};
        JobHandle sum{};
        REQUIRE(graph.Add(Increment, &inputs.Left, left) == JobStatus::Ok);
        REQUIRE(graph.Add(Increment, &inputs.Right, right) == JobStatus::Ok);
        REQUIRE(graph.Add(Sum, &inputs, sum) == JobStatus::Ok);
        REQUIRE(graph.DependsOn(sum, left) == JobStatus::Ok);
        REQUIRE(graph.DependsOn(sum, right) == JobStatus::Ok);
        REQUIRE(graph.Seal() == JobStatus::Ok);
        for (uint32 iteration = 0; iteration < 100; ++iteration)
        {
            // Resubmission reuses the context pointers; only the data they reference changes.
            inputs = {};
            REQUIRE(system.Submit(graph) == JobStatus::Ok);
            REQUIRE(graph.Reset() == JobStatus::Busy);
            REQUIRE(system.Submit(graph) == JobStatus::Busy);
            JobOutcome result{};
            REQUIRE(system.Wait(result, iteration % 2 == 0) == JobStatus::Ok);
            REQUIRE(result == JobOutcome::Succeeded);
            REQUIRE(inputs.Sum == 2);
            REQUIRE(GetOutcome(graph, sum) == JobOutcome::Succeeded);
        }
    }
}

TEST_CASE("Failure and cancellation block dependent callbacks without losing independent work", "[threading]")
{
    for (const JobFunction failure : {Fail, InvalidResult, Cancelled})
    {
        for (const uint32 workers : {0u, 3u})
        {
            JobSystem system;
            REQUIRE(system.Initialize({workers}) == JobStatus::Ok);
            JobGraph graph;
            REQUIRE(graph.Initialize({4, 2}) == JobStatus::Ok);
            uint32 calls = 0;
            uint32 independentCalls = 0;
            JobHandle failed{};
            JobHandle child{};
            JobHandle grandchild{};
            JobHandle independent{};
            REQUIRE(graph.Add(failure, nullptr, failed) == JobStatus::Ok);
            REQUIRE(graph.Add(Increment, &calls, child) == JobStatus::Ok);
            REQUIRE(graph.Add(Increment, &calls, grandchild) == JobStatus::Ok);
            REQUIRE(graph.Add(Increment, &independentCalls, independent) == JobStatus::Ok);
            REQUIRE(graph.DependsOn(child, failed) == JobStatus::Ok);
            REQUIRE(graph.DependsOn(grandchild, child) == JobStatus::Ok);
            REQUIRE(graph.Seal() == JobStatus::Ok);
            REQUIRE(system.Submit(graph) == JobStatus::Ok);
            JobOutcome result{};
            REQUIRE(system.Wait(result) == JobStatus::Ok);
            REQUIRE(result == (failure == Cancelled ? JobOutcome::Cancelled : JobOutcome::Failed));
            REQUIRE(GetOutcome(graph, child) == JobOutcome::Blocked);
            REQUIRE(GetOutcome(graph, grandchild) == JobOutcome::Blocked);
            REQUIRE(calls == 0);
            REQUIRE(independentCalls == 1);
        }
    }
}

TEST_CASE("Failure dominates cancellation in the graph outcome", "[threading]")
{
    for (const uint32 workers : {0u, 3u})
    {
        for (const bool isFailureFirst : {false, true})
        {
            JobSystem system;
            JobGraph graph;
            REQUIRE(system.Initialize({workers}) == JobStatus::Ok);
            REQUIRE(graph.Initialize({2, 0}) == JobStatus::Ok);
            JobHandle first{};
            JobHandle second{};
            REQUIRE(graph.Add(isFailureFirst == true ? Fail : Cancelled, nullptr, first) == JobStatus::Ok);
            REQUIRE(graph.Add(isFailureFirst == true ? Cancelled : Fail, nullptr, second) == JobStatus::Ok);
            REQUIRE(graph.Seal() == JobStatus::Ok);
            REQUIRE(system.Submit(graph) == JobStatus::Ok);
            JobOutcome result{};
            REQUIRE(system.Wait(result) == JobStatus::Ok);
            REQUIRE(result == JobOutcome::Failed);
        }
    }
}

TEST_CASE("Cancellation skips queued work and preserves a running callback", "[threading]")
{
    HeldJob held;
    uint32 calls = 0;
    JobGraph graph;
    JobSystem system;
    REQUIRE(graph.Initialize({3, 1}) == JobStatus::Ok);
    REQUIRE(system.Initialize({1}) == JobStatus::Ok);
    JobHandle running{};
    JobHandle queued{};
    JobHandle dependent{};
    REQUIRE(graph.Add(Hold, &held, running) == JobStatus::Ok);
    REQUIRE(graph.Add(Increment, &calls, queued) == JobStatus::Ok);
    REQUIRE(graph.Add(Increment, &calls, dependent) == JobStatus::Ok);
    REQUIRE(graph.DependsOn(dependent, running) == JobStatus::Ok);
    REQUIRE(graph.Seal() == JobStatus::Ok);
    REQUIRE(system.Submit(graph) == JobStatus::Ok);
    held.HasEntered.wait(false);
    JobSnapshot snapshot{};
    REQUIRE(system.Snapshot(snapshot) == JobStatus::Ok);
    REQUIRE(snapshot.Running == 1);
    REQUIRE(snapshot.Pending == 2);
    REQUIRE(system.Cancel() == JobStatus::Ok);
    held.IsReleased.store(true);
    held.IsReleased.notify_all();
    JobOutcome result{};
    REQUIRE(system.Wait(result) == JobStatus::Ok);
    REQUIRE(result == JobOutcome::Cancelled);
    REQUIRE(GetOutcome(graph, running) == JobOutcome::Succeeded);
    REQUIRE(GetOutcome(graph, queued) == JobOutcome::Cancelled);
    REQUIRE(GetOutcome(graph, dependent) == JobOutcome::Cancelled);
    REQUIRE(calls == 0);
}

namespace
{
struct Canceller
{
    JobSystem& System;
    JobStatus CancelStatus = JobStatus::InvalidArgument;
};
JobOutcome CancelSystem(void* context) noexcept
{
    Canceller& data = *static_cast<Canceller*>(context);
    data.CancelStatus = data.System.Cancel();
    return JobOutcome::Failed;
}
} // namespace

TEST_CASE("Explicit cancellation takes precedence over blocking for jobs that have not started", "[threading]")
{
    // Serial execution is FIFO, so the failing canceller resolves before its
    // dependent becomes ready and before the independent job starts.
    JobSystem system;
    JobGraph graph;
    REQUIRE(system.Initialize() == JobStatus::Ok);
    REQUIRE(graph.Initialize({3, 1}) == JobStatus::Ok);
    uint32 calls = 0;
    Canceller canceller{ .System = system };
    JobHandle failing{};
    JobHandle dependent{};
    JobHandle independent{};
    REQUIRE(graph.Add(CancelSystem, &canceller, failing) == JobStatus::Ok);
    REQUIRE(graph.Add(Increment, &calls, independent) == JobStatus::Ok);
    REQUIRE(graph.Add(Increment, &calls, dependent) == JobStatus::Ok);
    REQUIRE(graph.DependsOn(dependent, failing) == JobStatus::Ok);
    REQUIRE(graph.Seal() == JobStatus::Ok);
    REQUIRE(system.Submit(graph) == JobStatus::Ok);
    JobOutcome result{};
    REQUIRE(system.Wait(result) == JobStatus::Ok);
    REQUIRE(canceller.CancelStatus == JobStatus::Ok);
    REQUIRE(result == JobOutcome::Failed);
    REQUIRE(GetOutcome(graph, failing) == JobOutcome::Failed);
    REQUIRE(GetOutcome(graph, independent) == JobOutcome::Cancelled);
    REQUIRE(GetOutcome(graph, dependent) == JobOutcome::Cancelled);
    REQUIRE(calls == 0);
}

TEST_CASE("Cancelling after the last job resolved keeps a successful outcome", "[threading]")
{
    for (const uint32 workers : {0u, 2u})
    {
        JobSystem system;
        JobGraph graph;
        REQUIRE(system.Initialize({workers}) == JobStatus::Ok);
        REQUIRE(graph.Initialize({2, 1}) == JobStatus::Ok);
        uint32 calls = 0;
        JobHandle first{};
        JobHandle second{};
        REQUIRE(graph.Add(Increment, &calls, first) == JobStatus::Ok);
        REQUIRE(graph.Add(Increment, &calls, second) == JobStatus::Ok);
        REQUIRE(graph.DependsOn(second, first) == JobStatus::Ok);
        REQUIRE(graph.Seal() == JobStatus::Ok);
        REQUIRE(system.Submit(graph) == JobStatus::Ok);
        JobOutcome outcome = JobOutcome::Pending;
        while (outcome != JobOutcome::Succeeded)
        {
            REQUIRE(graph.GetOutcome(second, outcome) == JobStatus::Ok);
            std::this_thread::yield();
        }
        REQUIRE(system.Cancel() == JobStatus::Ok);
        JobOutcome result{};
        REQUIRE(system.Wait(result) == JobStatus::Ok);
        REQUIRE(result == JobOutcome::Succeeded);
        REQUIRE(calls == 2);
    }
}

namespace
{
struct Observer
{
    JobSystem& System;
    JobSnapshot Snapshot{};
    JobStatus SnapshotStatus = JobStatus::InvalidArgument;
};
JobOutcome Observe(void* context) noexcept
{
    Observer& data = *static_cast<Observer*>(context);
    data.SnapshotStatus = data.System.Snapshot(data.Snapshot);
    return JobOutcome::Succeeded;
}
} // namespace

TEST_CASE("Snapshot counts every outcome and is callable from a callback", "[threading]")
{
    // Serial FIFO order: failed, succeeded, blocked (released by failed), then
    // the observer (released by succeeded).
    JobSystem system;
    JobGraph graph;
    REQUIRE(system.Initialize() == JobStatus::Ok);
    REQUIRE(graph.Initialize({5, 2}) == JobStatus::Ok);
    Observer observer{ .System = system };
    uint32 calls = 0;
    JobHandle failed{};
    JobHandle succeeded{};
    JobHandle blocked{};
    JobHandle observing{};
    JobHandle cancelled{};
    REQUIRE(graph.Add(Fail, nullptr, failed) == JobStatus::Ok);
    REQUIRE(graph.Add(Succeed, nullptr, succeeded) == JobStatus::Ok);
    REQUIRE(graph.Add(Increment, &calls, blocked) == JobStatus::Ok);
    REQUIRE(graph.Add(Observe, &observer, observing) == JobStatus::Ok);
    REQUIRE(graph.DependsOn(blocked, failed) == JobStatus::Ok);
    REQUIRE(graph.DependsOn(observing, succeeded) == JobStatus::Ok);
    REQUIRE(graph.Seal() == JobStatus::Ok);
    REQUIRE(system.Submit(graph) == JobStatus::Ok);
    JobOutcome result{};
    REQUIRE(system.Wait(result) == JobStatus::Ok);
    REQUIRE(result == JobOutcome::Failed);
    REQUIRE(observer.SnapshotStatus == JobStatus::Ok);
    REQUIRE(observer.Snapshot.Pending == 0);
    REQUIRE(observer.Snapshot.Running == 1);
    REQUIRE(observer.Snapshot.Succeeded == 1);
    REQUIRE(observer.Snapshot.Failed == 1);
    REQUIRE(observer.Snapshot.Cancelled == 0);
    REQUIRE(observer.Snapshot.Blocked == 1);
    REQUIRE(calls == 0);
    JobSnapshot idle{};
    REQUIRE(system.Snapshot(idle) == JobStatus::Ok);
    REQUIRE(idle.Succeeded == 0);
    REQUIRE(idle.Failed == 0);
    REQUIRE(idle.Blocked == 0);

    REQUIRE(graph.Reset() == JobStatus::Ok);
    REQUIRE(graph.Add(Cancelled, nullptr, cancelled) == JobStatus::Ok);
    REQUIRE(graph.Add(Succeed, nullptr, succeeded) == JobStatus::Ok);
    REQUIRE(graph.Add(Observe, &observer, observing) == JobStatus::Ok);
    REQUIRE(graph.DependsOn(observing, succeeded) == JobStatus::Ok);
    REQUIRE(graph.Seal() == JobStatus::Ok);
    REQUIRE(system.Submit(graph) == JobStatus::Ok);
    REQUIRE(system.Wait(result) == JobStatus::Ok);
    REQUIRE(result == JobOutcome::Cancelled);
    REQUIRE(observer.Snapshot.Cancelled == 1);
    REQUIRE(observer.Snapshot.Succeeded == 1);
    REQUIRE(observer.Snapshot.Running == 1);
}

TEST_CASE("Cancel and Snapshot run concurrently with a non-helping owner Wait", "[threading]")
{
    HeldJob held;
    uint32 calls = 0;
    JobGraph graph;
    JobSystem system;
    REQUIRE(graph.Initialize({2, 1}) == JobStatus::Ok);
    REQUIRE(system.Initialize({2}) == JobStatus::Ok);
    JobHandle running{};
    JobHandle dependent{};
    REQUIRE(graph.Add(Hold, &held, running) == JobStatus::Ok);
    REQUIRE(graph.Add(Increment, &calls, dependent) == JobStatus::Ok);
    REQUIRE(graph.DependsOn(dependent, running) == JobStatus::Ok);
    REQUIRE(graph.Seal() == JobStatus::Ok);
    REQUIRE(system.Submit(graph) == JobStatus::Ok);
    std::atomic<JobStatus> snapshotStatus{JobStatus::InvalidArgument};
    std::atomic<JobStatus> cancelStatus{JobStatus::InvalidArgument};
    std::atomic<uint32> observedRunning{0};
    std::thread controller([&] {
        held.HasEntered.wait(false);
        JobSnapshot snapshot{};
        snapshotStatus.store(system.Snapshot(snapshot));
        observedRunning.store(snapshot.Running);
        cancelStatus.store(system.Cancel());
        held.IsReleased.store(true);
        held.IsReleased.notify_all();
    });
    JobOutcome result{};
    const JobStatus waitStatus = system.Wait(result, false);
    controller.join();
    REQUIRE(waitStatus == JobStatus::Ok);
    REQUIRE(snapshotStatus.load() == JobStatus::Ok);
    REQUIRE(observedRunning.load() == 1);
    REQUIRE(cancelStatus.load() == JobStatus::Ok);
    REQUIRE(result == JobOutcome::Cancelled);
    REQUIRE(GetOutcome(graph, running) == JobOutcome::Succeeded);
    REQUIRE(GetOutcome(graph, dependent) == JobOutcome::Cancelled);
    REQUIRE(calls == 0);
}

TEST_CASE("Callbacks cannot recursively initialize, wait, submit or shut down", "[threading]")
{
    for (const uint32 workers : {0u, 2u})
    {
        JobSystem system;
        JobGraph graph;
        REQUIRE(system.Initialize({workers}) == JobStatus::Ok);
        REQUIRE(graph.Initialize({1, 0}) == JobStatus::Ok);
        Reentrant data{ .System = system, .Graph = graph };
        JobHandle handle{};
        REQUIRE(graph.Add(Reenter, &data, handle) == JobStatus::Ok);
        REQUIRE(graph.Seal() == JobStatus::Ok);
        REQUIRE(system.Submit(graph) == JobStatus::Ok);
        JobOutcome result{};
        REQUIRE(system.Wait(result) == JobStatus::Ok);
        REQUIRE(data.WaitStatus == JobStatus::InsideJob);
        REQUIRE(data.SubmitStatus == JobStatus::InsideJob);
        REQUIRE(data.ShutdownStatus == JobStatus::InsideJob);
        REQUIRE(data.InitializeStatus == JobStatus::InsideJob);
    }
}

TEST_CASE("Callbacks cannot drive a different pool", "[threading]")
{
    for (const uint32 workers : {0u, 2u})
    {
        JobSystem outer;
        JobSystem inner;
        JobSystem uninitialized;
        JobGraph outerGraph;
        JobGraph innerGraph;
        REQUIRE(outer.Initialize({workers}) == JobStatus::Ok);
        REQUIRE(inner.Initialize({1}) == JobStatus::Ok);
        REQUIRE(innerGraph.Initialize({1, 0}) == JobStatus::Ok);
        JobHandle handle{};
        REQUIRE(innerGraph.Add(Succeed, nullptr, handle) == JobStatus::Ok);
        REQUIRE(innerGraph.Seal() == JobStatus::Ok);
        Reentrant other{ .System = inner, .Graph = innerGraph };
        Reentrant fresh{ .System = uninitialized, .Graph = innerGraph };
        REQUIRE(outerGraph.Initialize({2, 0}) == JobStatus::Ok);
        REQUIRE(outerGraph.Add(Reenter, &other, handle) == JobStatus::Ok);
        REQUIRE(outerGraph.Add(Reenter, &fresh, handle) == JobStatus::Ok);
        REQUIRE(outerGraph.Seal() == JobStatus::Ok);
        REQUIRE(outer.Submit(outerGraph) == JobStatus::Ok);
        JobOutcome result{};
        REQUIRE(outer.Wait(result) == JobStatus::Ok);
        REQUIRE(result == JobOutcome::Succeeded);
        for (const Reentrant* data : {&other, &fresh})
        {
            REQUIRE(data->WaitStatus == JobStatus::InsideJob);
            REQUIRE(data->SubmitStatus == JobStatus::InsideJob);
            REQUIRE(data->ShutdownStatus == JobStatus::InsideJob);
            REQUIRE(data->InitializeStatus == JobStatus::InsideJob);
        }
        // The rejected calls left the inner pool usable from its owner.
        REQUIRE(inner.Submit(innerGraph) == JobStatus::Ok);
        REQUIRE(inner.Wait(result) == JobStatus::Ok);
        REQUIRE(result == JobOutcome::Succeeded);
    }
}

namespace
{
constexpr uint32 ORDER_JOBS = 6;
struct OrderRecord
{
    uint32 Sequence[ORDER_JOBS]{};
    uint32 Count = 0;
};
// Only the serial executor touches the record, on the test thread.
OrderRecord gOrder{};
uint32 gOrderIds[ORDER_JOBS]{0, 1, 2, 3, 4, 5};
JobOutcome RecordOrder(void* context) noexcept
{
    gOrder.Sequence[gOrder.Count++] = *static_cast<const uint32*>(context);
    return JobOutcome::Succeeded;
}
void BuildOrderGraph(JobGraph& inoutGraph)
{
    JobHandle handles[ORDER_JOBS]{};
    for (uint32 index = 0; index < ORDER_JOBS; ++index)
    {
        REQUIRE(inoutGraph.Add(RecordOrder, &gOrderIds[index], handles[index]) == JobStatus::Ok);
    }
    // A diamond plus an independent chain, declared in a fixed order.
    REQUIRE(inoutGraph.DependsOn(handles[1], handles[0]) == JobStatus::Ok);
    REQUIRE(inoutGraph.DependsOn(handles[2], handles[0]) == JobStatus::Ok);
    REQUIRE(inoutGraph.DependsOn(handles[3], handles[1]) == JobStatus::Ok);
    REQUIRE(inoutGraph.DependsOn(handles[3], handles[2]) == JobStatus::Ok);
    REQUIRE(inoutGraph.DependsOn(handles[5], handles[4]) == JobStatus::Ok);
    REQUIRE(inoutGraph.Seal() == JobStatus::Ok);
}
} // namespace

TEST_CASE("Serial execution order repeats for a fixed construction and edge order", "[threading]")
{
    JobSystem system;
    REQUIRE(system.Initialize() == JobStatus::Ok);
    OrderRecord reference{};
    for (uint32 build = 0; build < 3; ++build)
    {
        JobGraph graph;
        REQUIRE(graph.Initialize({ORDER_JOBS, ORDER_JOBS}) == JobStatus::Ok);
        BuildOrderGraph(graph);
        for (uint32 repeat = 0; repeat < 4; ++repeat)
        {
            gOrder = {};
            REQUIRE(system.Submit(graph) == JobStatus::Ok);
            JobOutcome result{};
            REQUIRE(system.Wait(result) == JobStatus::Ok);
            REQUIRE(result == JobOutcome::Succeeded);
            REQUIRE(gOrder.Count == ORDER_JOBS);
            if (build == 0 && repeat == 0)
            {
                reference = gOrder;
            }
            for (uint32 index = 0; index < ORDER_JOBS; ++index)
            {
                REQUIRE(gOrder.Sequence[index] == reference.Sequence[index]);
            }
        }
    }
}

TEST_CASE("Shutdown drains accepted work and supports repeated initialization", "[threading]")
{
    JobGraph graph;
    REQUIRE(graph.Initialize({512, 0}) == JobStatus::Ok);
    Array<uint32> calls(512, 0);
    JobHandle handle{};
    for (uint32& count : calls)
    {
        REQUIRE(graph.Add(Increment, &count, handle) == JobStatus::Ok);
    }
    REQUIRE(graph.Seal() == JobStatus::Ok);
    JobSystem system;
    REQUIRE(system.GetWorkerCount() == 0);
    for (uint32 repeat = 0; repeat < 10; ++repeat)
    {
        REQUIRE(system.Initialize({4}) == JobStatus::Ok);
        REQUIRE(system.GetWorkerCount() == 4);
        REQUIRE(system.Submit(graph) == JobStatus::Ok);
        REQUIRE(system.Shutdown() == JobStatus::Ok);
        REQUIRE(system.GetWorkerCount() == 0);
        for (const uint32 count : calls)
        {
            REQUIRE(count == repeat + 1);
        }
        REQUIRE(system.Shutdown() == JobStatus::Ok);
    }
    REQUIRE(graph.Reset() == JobStatus::Ok);
}

TEST_CASE("Empty graphs and invalid system operations have explicit results", "[threading]")
{
    JobSystem system;
    JobGraph graph;
    JobOutcome result{};
    REQUIRE(system.Wait(result) == JobStatus::NotInitialized);
    REQUIRE(system.Initialize({65}) == JobStatus::InvalidArgument);
    REQUIRE(system.GetWorkerCount() == 0);
    REQUIRE(system.Initialize() == JobStatus::Ok);
    REQUIRE(system.GetWorkerCount() == 0);
    REQUIRE(system.Initialize() == JobStatus::AlreadyInitialized);
    REQUIRE(system.Wait(result) == JobStatus::InvalidArgument);
    REQUIRE(system.Cancel() == JobStatus::InvalidArgument);
    REQUIRE(system.Submit(graph) == JobStatus::NotInitialized);
    REQUIRE(graph.Initialize({1, 0}) == JobStatus::Ok);
    REQUIRE(graph.Seal() == JobStatus::Ok);
    REQUIRE(system.Submit(graph) == JobStatus::Ok);
    REQUIRE(system.Wait(result, false) == JobStatus::Ok);
    REQUIRE(result == JobOutcome::Succeeded);
}

TEST_CASE("A graph cannot be admitted by two pools and graph destruction drains work", "[threading]")
{
    uint32 count = 0;
    JobSystem first;
    JobSystem second;
    REQUIRE(first.Initialize({2}) == JobStatus::Ok);
    REQUIRE(second.Initialize({2}) == JobStatus::Ok);
    JobHandle stale{};
    {
        JobGraph graph;
        REQUIRE(graph.Initialize({1, 0}) == JobStatus::Ok);
        REQUIRE(graph.Add(Increment, &count, stale) == JobStatus::Ok);
        REQUIRE(graph.Seal() == JobStatus::Ok);
        REQUIRE(first.Submit(graph) == JobStatus::Ok);
        REQUIRE(second.Submit(graph) == JobStatus::Busy);
    }
    REQUIRE(count == 1);
    JobGraph graph;
    REQUIRE(graph.Initialize({1, 0}) == JobStatus::Ok);
    JobHandle fresh{};
    REQUIRE(graph.Add(Succeed, nullptr, fresh) == JobStatus::Ok);
    JobOutcome result{};
    REQUIRE(graph.GetOutcome(stale, result) == JobStatus::InvalidHandle);
}

namespace
{
constexpr uint32 FAN_WIDTH = 128;
struct FanIn
{
    uint32 Values[FAN_WIDTH]{};
    uint32 Total = 0;
};
JobOutcome Reduce(void* context) noexcept
{
    FanIn& data = *static_cast<FanIn*>(context);
    data.Total = 0;
    for (const uint32 value : data.Values)
    {
        data.Total += value;
    }
    return data.Total == FAN_WIDTH ? JobOutcome::Succeeded : JobOutcome::Failed;
}
} // namespace

TEST_CASE("Wide fan-out and fan-in run exactly once across repeated sleeping workers", "[threading]")
{
    FanIn data{};
    JobGraph graph;
    JobSystem system;
    REQUIRE(graph.Initialize({FAN_WIDTH + 2, FAN_WIDTH * 2}) == JobStatus::Ok);
    REQUIRE(system.Initialize({4}) == JobStatus::Ok);
    JobHandle root{};
    JobHandle join{};
    REQUIRE(graph.Add(Succeed, nullptr, root) == JobStatus::Ok);
    REQUIRE(graph.Add(Reduce, &data, join) == JobStatus::Ok);
    for (uint32 index = 0; index < FAN_WIDTH; ++index)
    {
        JobHandle child{};
        REQUIRE(graph.Add(Increment, &data.Values[index], child) == JobStatus::Ok);
        REQUIRE(graph.DependsOn(child, root) == JobStatus::Ok);
        REQUIRE(graph.DependsOn(join, child) == JobStatus::Ok);
    }
    REQUIRE(graph.Seal() == JobStatus::Ok);
    for (uint32 repeat = 0; repeat < 100; ++repeat)
    {
        for (uint32& value : data.Values)
        {
            value = 0;
        }
        REQUIRE(system.Submit(graph) == JobStatus::Ok);
        // Force repeated full completion notifications to a non-helping owner.
        JobOutcome outcome{};
        REQUIRE(system.Wait(outcome, false) == JobStatus::Ok);
        REQUIRE(outcome == JobOutcome::Succeeded);
        REQUIRE(data.Total == FAN_WIDTH);
    }
}
