#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <QElapsedTimer>
#include <QVector>

namespace ludus::editor
{
/// Keypress-to-paint latency measurement for the code editor.
///
/// The view calls MarkKeyHandled() at the end of a text-modifying key event and
/// MarkPainted() at the end of the paint that first shows that edit. The probe
/// records the elapsed time of each pending keystroke and keeps a bounded ring
/// of samples for a p50/p95 summary. It is idle and free until Start() is
/// called, so ordinary editing performs no measurement and no timer wakeups,
/// honouring the "idle editing performs no polling" budget.
///
/// This records relative, in-process timing through QElapsedTimer's monotonic
/// clock. It is the mechanism behind the native latency run in M9 and the
/// interactive runs; it does not itself prove the reference-machine budget.
/// GUI thread only.
class CodeLatencyProbe final
{
public:
    /// Bounded number of retained samples; the ring overwrites the oldest. One
    /// owner constant, read wherever the ring is sized or summarized.
    static constexpr foundation::int32 CODE_LATENCY_SAMPLE_CAPACITY = 1024;

    CodeLatencyProbe() noexcept = default;

    /// Begin recording. Clears any previous samples. Cheap; allocates the ring
    /// lazily on the first sample.
    void Start() noexcept;
    /// Stop recording. Retains the collected samples for the summary.
    void Stop() noexcept;
    [[nodiscard]] LUDUS_INLINE bool IsRecording() const noexcept
    {
        return mIsRecording;
    }

    /// Note that a text-modifying key event was handled. Starts the timer for
    /// the keystroke whose glyph the next paint should show. A no-op unless
    /// recording.
    void MarkKeyHandled() noexcept;
    /// Note that a paint completed. If a keystroke is pending, records the
    /// elapsed keypress-to-paint time and clears the pending mark. A no-op
    /// unless recording and a keystroke is pending.
    void MarkPainted() noexcept;

    /// The number of recorded samples, capped at the ring capacity.
    [[nodiscard]] foundation::int32 GetSampleCount() const noexcept;
    /// The p50 and p95 of the recorded samples in nanoseconds, or 0 when empty.
    [[nodiscard]] foundation::int64 GetPercentileNanoseconds(foundation::int32 percentile) const;

private:
    QVector<foundation::int64> mSamples;
    QElapsedTimer mPendingKey;
    bool mIsRecording = false;
    bool mHasPendingKey = false;
    foundation::int32 mWriteIndex = 0;
    foundation::int32 mCount = 0;
};
} // namespace ludus::editor
