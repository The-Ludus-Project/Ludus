# FoundationThreading

Bounded reusable CPU task graphs, with native worker pools and the same serial
executor on native/browser builds. Link `Ludus::FoundationThreading` and include
`<ludus/foundation/threading/jobs.hpp>`. See the
[architecture and reference review](../../architecture/threading.md).

```cpp
#include <ludus/foundation/threading/jobs.hpp>

namespace jobs = ludus::foundation::threading;

jobs::JobOutcome Compute(void* context) noexcept
{
    ++*static_cast<ludus::foundation::uint32*>(context);
    return jobs::JobOutcome::Succeeded;
}

int main()
{
    ludus::foundation::uint32 output = 0;
    // Contexts must outlive the graph/system, including failure paths.
    jobs::JobGraph graph;
    jobs::JobSystem system;
    jobs::JobHandle handle;
    if (graph.Initialize({1, 0}) != jobs::JobStatus::Ok ||
        system.Initialize({2}) != jobs::JobStatus::Ok ||
        graph.Add(Compute, &output, handle) != jobs::JobStatus::Ok ||
        graph.Seal() != jobs::JobStatus::Ok ||
        system.Submit(graph) != jobs::JobStatus::Ok)
    {
        return 1;
    }
    jobs::JobOutcome result;
    if (system.Wait(result) != jobs::JobStatus::Ok || result != jobs::JobOutcome::Succeeded)
    {
        return 1;
    }
    return output == 1 ? 0 : 1;
}
```

The default worker budget is zero. Native consumers explicitly select their CPU
budget; a component lent a system reads that budget with `GetWorkerCount()` to size
its graphs instead of taking a second configuration value. Browser builds reject a nonzero worker count. One graph remains admitted
until Wait, even after callbacks finish. Submit of a sealed graph may be repeated;
each job keeps the context pointer given to Add, so update the data it references
between Wait and the next Submit. Reset invalidates its handles before rebuilding.
Cancel skips callbacks that have not started and reports them Cancelled, even when
a failed prerequisite would otherwise report them Blocked; running callbacks finish.
The graph outcome is Failed if any job failed, otherwise Cancelled if any job was
cancelled. GetOutcome and Snapshot expose progress; Cancel and Snapshot may also be
called from callbacks.

Build/reset and pool lifecycle belong to one application owner. Wait may help
execute CPU work on its caller; use `help=false` to preserve caller responsiveness
or TLS expectations. Jobs never wait on jobs or synchronous I/O and never call
pool lifecycle methods. The owner retains immutable inputs and exclusive output
ranges through Wait, then applies results in stable order. All errors use statuses;
the engine library compiles with exceptions disabled.

## Execution traces

Jobs can carry an optional `JobLabel` (a borrowed name and a site ID), given as the
last argument of `Add`. A `JobSystem` can observe its own scheduling through
`JobSystemConfig::Hooks`: worker start/stop hooks that run once on each worker
during `Initialize` and `Shutdown`, a trace function and a per-submission filter.
Hooks run outside the scheduler lock and cannot call pool lifecycle methods. The
[architecture](../../architecture/threading.md#task-identity-and-execution-traces)
gives the event order and the lock and lifetime argument.

To record Perfetto traces, link `Ludus::FoundationThreadingProfiling` and use the
profiler hooks:

```cpp
#include <ludus/foundation/profiling/trace_system.hpp>
#include <ludus/foundation/threading/job_profiling.hpp>

namespace jobs = ludus::foundation::threading;
namespace profiling = ludus::foundation::profiling;

profiling::BeginCapture();
jobs::JobSystem system;
// Workers register as profiler threads "Bake worker 0", "Bake worker 1", ...
if (system.Initialize({ .WorkerCount = 3, .Hooks = jobs::MakeProfilerJobHooks("Bake worker") }) !=
    jobs::JobStatus::Ok)
{
    return 1;
}
jobs::JobHandle handle;
if (graph.Add(Compute, &output, handle, LUDUS_JOB_LABEL(Compute)) != jobs::JobStatus::Ok)
{
    return 1;
}
// ... Seal, Submit and Wait as above ...
profiling::EndCapture(); // publishes every thread's partial trace chunk
profiling::ExportPerfettoTrace("jobs.json");
```

Submissions are traced only while a profiler capture is active; otherwise each job
costs one predictable branch. `LUDUS_JOB_LABEL(Name)` registers `Name` as a profiler
site once and never allocates. A capture contains:

| Track | Event | Meaning |
| --- | --- | --- |
| Owner thread | `JobSystem::Submit` slice | Admission; starts the dispatch flow of every job without prerequisites. |
| Owner thread | `JobSystem::Wait` slice | Time the owner spends in `Wait`, including jobs it helps execute. |
| Executing thread | Slice named by the job's label (`Job` when unlabelled) | The callback's execution. |
| Flow arrows | `JobDispatch` | From Submit to a root's start, and from a succeeded prerequisite to the start of each dependent it released. The arrow's length is the dispatch-to-start latency. |
| Counter | `JobReadyDepth` | Jobs waiting in the ready queue, sampled at Submit and at each job start. |

Jobs skipped as Blocked or Cancelled have no slice. A dependent with several
prerequisites receives the arrow of the prerequisite that released it.

## Dedicated worker and owner completions

`<ludus/foundation/threading/worker_queue.hpp>` provides `WorkerQueue` for native
blocking preparation that belongs outside the CPU dependency scheduler, including
driver pipeline creation. `Initialize` allocates fixed storage and starts one
thread. `Submit(execute, complete, context)` admits at most
`MAX_WORKER_QUEUE_TASKS` tasks, including running and uncollected completions.
It returns an explicit status when unavailable or full; it never executes work
on the caller. Browser initialization returns Unavailable without a serial fallback.

The execution callback runs on that worker. Call `Poll` between frames to deliver
at most one completed callback on the owner thread, with the execution writes
visible. Keep context storage alive through completion. Neither callback may
reenter its queue. Submission and polling allocate nothing and never wait for
execution; their internal gate protects only bounded bookkeeping, never a driver
call. `Shutdown` joins the worker and delivers remaining completions on the owner;
use it during orderly shutdown, outside frame-critical work.
