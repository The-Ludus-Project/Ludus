#pragma once

// Independent tasks of the best-effort cook (texture halving, surface-map row bands),
// run optionally on a caller-owned FoundationThreading JobSystem
// (docs/architecture/threading.md: the caller budgets workers; there is no hidden pool).
//
// Each task writes only its own output, so the result does not depend on which
// thread runs a task or in what order. The owner then merges in task order. A task
// the job system did not run (no system, a busy or refusing system, or a call from a
// job) runs inline on the owner after the system's Wait, so every worker count gives
// the same outputs as the serial path.

#include <ludus/foundation/base/core.h>

#include <ludus/content/scene_format/limits.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/threading/jobs.hpp>

namespace ludus::content::scene_cook::importers
{
using foundation::Array;
using foundation::uint32;
using foundation::usize;

/// Most tasks submitted to the job system by one Run (further tasks run inline): one
/// per texture an imported bundle can hold.
inline constexpr uint32 MAX_PARALLEL_TASKS = scene_format::MAX_IMPORT_TEXTURES;

/// Row bands per job-system thread for row-parallel image work, so unequal bands balance.
inline constexpr uint32 IMAGE_BANDS_PER_THREAD = 4;

/// Runs independent tasks, on an optional job system, with an inline fallback.
///
/// Used on one thread. Reuses its job graph and task storage across runs.
class ParallelTasks final
{
public:
    /// Task callback: runs task index @p task of the run with the run's context.
    using TaskFunction = void (*)(void* context, usize task) noexcept;

    ParallelTasks() noexcept = default;
    ~ParallelTasks() noexcept = default;
    ParallelTasks(const ParallelTasks&) = delete;
    ParallelTasks& operator=(const ParallelTasks&) = delete;
    ParallelTasks(ParallelTasks&&) = delete;
    ParallelTasks& operator=(ParallelTasks&&) = delete;

    /// Call function(context, i) exactly once for every i < count, and return after all calls.
    /// @param jobsOrNull Caller-owned system, not running another graph of this thread's
    ///        owner; nullptr runs every task inline in index order.
    /// @param name Job label; must outlive the call (normally a string literal).
    /// @return false when task storage cannot be allocated, in which case no task ran.
    [[nodiscard]] bool Run(foundation::threading::JobSystem* jobsOrNull,
                           usize count,
                           TaskFunction function,
                           void* context,
                           const char* name) noexcept;

private:
    /// One submitted task; the job writes only its own record.
    struct Task final
    {
        TaskFunction Function = nullptr;
        void* ContextOrNull = nullptr;
        usize Index = 0;
        bool IsDone = false;
    };

    static foundation::threading::JobOutcome runTaskJob(void* context) noexcept;
    void submitTasks(foundation::threading::JobSystem& inoutJobs, usize count, const char* name) noexcept;

    /// Declared before mGraph, whose destructor drains a submission that still references them.
    Array<Task> mTasks;
    foundation::threading::JobGraph mGraph;
    bool mIsGraphInitialized = false;
};

/// Row bands for an image of @p rows rows on @p jobsOrNull: one without a system,
/// otherwise IMAGE_BANDS_PER_THREAD per thread (workers and the helping caller),
/// never more than the rows or MAX_PARALLEL_TASKS.
[[nodiscard]] usize GetImageBandCount(const foundation::threading::JobSystem* jobsOrNull, usize rows) noexcept;
} // namespace ludus::content::scene_cook::importers
