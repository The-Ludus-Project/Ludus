#pragma once

#include <ludus/foundation/base/types.h>

#include <atomic>

namespace ludus::editor
{
/// Import stages in the order they run.
enum class ContentImportStage : foundation::uint8
{
    Waiting,
    Reading,
    Validating,
    Checking,
    Publishing,
};

/// Number of values in ContentImportStage.
inline constexpr foundation::usize CONTENT_IMPORT_STAGE_COUNT = 5;

/// A snapshot of an import's progress: its stage and how many of that stage's steps are done.
struct ContentImportProgress final
{
    ContentImportStage Stage = ContentImportStage::Waiting;
    foundation::uint32 CompletedSteps = 0;
    foundation::uint32 TotalSteps = 0;
    [[nodiscard]] bool operator==(const ContentImportProgress&) const = default;
};

/// Overall completed fraction in [0, 1] of an import at this progress. Stages take fixed shares of the bar
/// (validation, which visits every dependent definition, the largest), and steps fill their stage evenly.
[[nodiscard]] double GetContentImportFraction(const ContentImportProgress& progress) noexcept;

// Private publication gate shared by native import entry points. Cancellation
// wins before publication or reports that publication has already begun. The
// importing thread also reports progress here; the UI thread reads it while polling.
class ContentImportGate final
{
public:
    [[nodiscard]] bool Cancel() noexcept
    {
        auto expected = Phase::Preparing;
        return Phase_.compare_exchange_strong(expected, Phase::Cancelled) || expected == Phase::Cancelled;
    }
    [[nodiscard]] bool Cancelled() const noexcept
    {
        return Phase_.load() == Phase::Cancelled;
    }
    [[nodiscard]] bool Publish() noexcept
    {
        auto expected = Phase::Preparing;
        return Phase_.compare_exchange_strong(expected, Phase::Publishing);
    }
    /// Record that stage has completedSteps of totalSteps done. Steps above STEP_MASK are clamped.
    void
    ReportProgress(ContentImportStage stage, foundation::uint32 completedSteps, foundation::uint32 totalSteps) noexcept
    {
        const foundation::uint64 completed = completedSteps < STEP_MASK ? completedSteps : STEP_MASK;
        const foundation::uint64 total = totalSteps < STEP_MASK ? totalSteps : STEP_MASK;
        mProgress.store(static_cast<foundation::uint64>(stage) | (completed << STEP_SHIFT) |
                            (total << (STEP_SHIFT + STEP_BITS)),
                        std::memory_order_relaxed);
    }
    /// The last reported progress.
    [[nodiscard]] ContentImportProgress GetProgress() const noexcept
    {
        const foundation::uint64 packed = mProgress.load(std::memory_order_relaxed);
        return
        {
            .Stage = static_cast<ContentImportStage>(packed & STAGE_MASK),
            .CompletedSteps = static_cast<foundation::uint32>((packed >> STEP_SHIFT) & STEP_MASK),
            .TotalSteps = static_cast<foundation::uint32>((packed >> (STEP_SHIFT + STEP_BITS)) & STEP_MASK),
        };
    }

private:
    enum class Phase : foundation::uint8
    {
        Preparing,
        Cancelled,
        Publishing
    };
    /// Packed progress: stage in the low byte, then completed and total steps.
    static constexpr foundation::uint64 STAGE_MASK = 0xFF;
    static constexpr foundation::uint32 STEP_SHIFT = 8;
    static constexpr foundation::uint32 STEP_BITS = 24;
    static constexpr foundation::uint32 STEP_MASK = (foundation::uint32{1} << STEP_BITS) - 1;
    std::atomic<Phase> Phase_{Phase::Preparing};
    std::atomic<foundation::uint64> mProgress{0};
};

} // namespace ludus::editor
