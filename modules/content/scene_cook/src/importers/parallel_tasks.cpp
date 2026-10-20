// Independent cook tasks on an optional job system; see parallel_tasks.h.

#include "parallel_tasks.h"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::scene_cook::importers
{
namespace
{
namespace threading = foundation::threading;
namespace math = foundation::math;
} // namespace

bool ParallelTasks::Run(threading::JobSystem* jobsOrNull,
                        usize count,
                        TaskFunction function,
                        void* context,
                        const char* name) noexcept
{
    if (jobsOrNull == nullptr || count <= 1)
    {
        for (usize i = 0; i < count; ++i)
        {
            function(context, i);
        }
        return true;
    }
    if (mTasks.TryResize(count) == false)
    {
        return false;
    }
    for (usize i = 0; i < count; ++i)
    {
        mTasks[i] = { .Function = function, .ContextOrNull = context, .Index = i, .IsDone = false };
    }
    submitTasks(*jobsOrNull, count, name);
    // Runs any task the job system did not, after Wait made every callback write visible.
    for (Task& task : mTasks)
    {
        if (task.IsDone == false)
        {
            task.Function(task.ContextOrNull, task.Index);
            task.IsDone = true;
        }
    }
    return true;
}

threading::JobOutcome ParallelTasks::runTaskJob(void* context) noexcept
{
    Task& task = *static_cast<Task*>(context);
    task.Function(task.ContextOrNull, task.Index);
    task.IsDone = true;
    return threading::JobOutcome::Succeeded;
}

void ParallelTasks::submitTasks(threading::JobSystem& inoutJobs, usize count, const char* name) noexcept
{
    if (mIsGraphInitialized == false)
    {
        if (mGraph.Initialize(
        {
            .MaxJobs = MAX_PARALLEL_TASKS,
            .MaxDependencies = 0,
        }) != threading::JobStatus::Ok)
        {
            return;
        }
        mIsGraphInitialized = true;
    }
    if (mGraph.Reset() != threading::JobStatus::Ok)
    {
        return;
    }
    // A full graph leaves the remaining tasks to the inline pass in Run.
    const usize submitted = math::Min(count, static_cast<usize>(MAX_PARALLEL_TASKS));
    for (usize i = 0; i < submitted; ++i)
    {
        threading::JobHandle handle;
        if (mGraph.Add(runTaskJob, &mTasks[i], handle, { .NameOrNull = name }) != threading::JobStatus::Ok)
        {
            break;
        }
    }
    // A job system that refuses the graph (busy with another graph, or called from a
    // job) leaves every task to the inline pass.
    if (mGraph.Seal() != threading::JobStatus::Ok || inoutJobs.Submit(mGraph) != threading::JobStatus::Ok)
    {
        return;
    }
    threading::JobOutcome outcome = threading::JobOutcome::Pending;
    const threading::JobStatus waited = inoutJobs.Wait(outcome);
    // Submit already refused a call from a job. NotInitialized or InvalidArgument means an
    // owner Shutdown drained the graph; either way every callback write is now visible.
    LUDUS_REQUIRE(waited != threading::JobStatus::InsideJob);
}

usize GetImageBandCount(const threading::JobSystem* jobsOrNull, usize rows) noexcept
{
    if (jobsOrNull == nullptr)
    {
        return 1;
    }
    const usize threads = usize{jobsOrNull->GetWorkerCount()} + 1;
    const usize bands = math::Min(threads * IMAGE_BANDS_PER_THREAD, static_cast<usize>(MAX_PARALLEL_TASKS));
    return math::Max(math::Min(bands, rows), usize{1});
}
} // namespace ludus::content::scene_cook::importers
