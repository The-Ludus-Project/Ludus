#include "viewport_pacing.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::editor::tests;

namespace
{
constexpr uint64 WINDOW = time::NANOSECONDS_PER_SECOND;
constexpr usize PAIR_COUNT = 5;
constexpr usize WINDOW_COUNT = PAIR_COUNT * 2;
/// Images per one-second window of a 48 Hz hosted macOS runner (batch 42912855925).
constexpr usize STEADY_IMAGES = 48;

/// Where each window's images fall, as offsets from the window start.
enum class Cadence : uint8
{
    Even,
    /// The same number of images, but all of them compressed around one long stall.
    Stalled,
};
struct Timeline final
{
    Array<uint64> Ticks;
    StaticArray<ViewportPacingWindow, WINDOW_COUNT> Windows;
};
/// Interleave idle and input windows, starting with idle, at the given cadences and image counts.
Timeline MakeTimeline(usize idleImages, usize inputImages, Cadence inputCadence, uint64 stallNanoseconds = 0)
{
    Timeline timeline;
    for (usize index = 0; index < WINDOW_COUNT; ++index)
    {
        ViewportPacingWindow& window = timeline.Windows[index];
        const bool isInput = index % 2 == 1;
        window.Kind = isInput == true ? ViewportPacingWindowKind::Input : ViewportPacingWindowKind::Idle;
        window.BeginTicks = WINDOW * (index + 1);
        window.EndTicks = window.BeginTicks + WINDOW;
        window.InputCount = isInput == true ? MIN_PACING_INPUT_EVENTS * 10 : 0;
        const usize images = isInput == true ? inputImages : idleImages;
        const bool isStalled = isInput == true && inputCadence == Cadence::Stalled;
        const uint64 active = isStalled == true ? WINDOW - stallNanoseconds : WINDOW;
        const uint64 step = images == 0 ? 0 : active / images;
        for (usize image = 0; image < images; ++image)
        {
            uint64 offset = step / 2 + step * image;
            if (isStalled == true && image >= images / 2)
            {
                offset += stallNanoseconds;
            }
            REQUIRE(timeline.Ticks.TryAdd(window.BeginTicks + offset) == true);
        }
    }
    return timeline;
}
ViewportPacingVerdict Evaluate(const Timeline& timeline)
{
    ViewportPacingVerdict verdict;
    const BorrowedView<const ViewportPacingWindow> windows(timeline.Windows.GetData(), timeline.Windows.GetSize());
    REQUIRE(EvaluateViewportPacing(timeline.Ticks.AsView(), windows, verdict) == true);
    return verdict;
}
} // namespace

TEST_CASE("Viewport pacing accepts steady input at the idle cadence", "[editor][viewport][navigation-cadence]")
{
    const ViewportPacingVerdict verdict = Evaluate(MakeTimeline(STEADY_IMAGES, STEADY_IMAGES, Cadence::Even));
    CHECK(verdict.IdleMedianImages == STEADY_IMAGES);
    CHECK(verdict.InputMedianImages == STEADY_IMAGES);
    CHECK(verdict.IsRatioJudged == true);
    CHECK(IsViewportPacingAccepted(verdict) == true);
}

TEST_CASE("Viewport pacing detects a long stall when every input window keeps its image count",
          "[editor][viewport][navigation-cadence]")
{
    // Controlled delayed presentation: each input window presents as many images as an idle one,
    // but half of them arrive only after one stall. Throughput alone cannot see it.
    const uint64 stall = time::Milliseconds(400).Nanoseconds;
    const ViewportPacingVerdict verdict = Evaluate(MakeTimeline(STEADY_IMAGES, STEADY_IMAGES, Cadence::Stalled, stall));
    CHECK(verdict.InputMedianImages == verdict.IdleMedianImages);
    CHECK(verdict.IsThroughputKept == true);
    CHECK(verdict.HasProgress == true);
    CHECK(verdict.InputMedianLongestGap > stall);
    CHECK(verdict.IsCadenceKept == false);
    CHECK(IsViewportPacingAccepted(verdict) == false);
}

TEST_CASE("Viewport pacing rejects input that keeps only a few images per window",
          "[editor][viewport][navigation-cadence]")
{
    // A few images per second still advance the viewport; the former progress-only check passed this.
    constexpr usize THROTTLED_IMAGES = 3;
    const ViewportPacingVerdict verdict = Evaluate(MakeTimeline(STEADY_IMAGES, THROTTLED_IMAGES, Cadence::Even));
    CHECK(verdict.HasProgress == true);
    CHECK(verdict.IsRatioJudged == true);
    CHECK(verdict.IsThroughputKept == false);
    CHECK(verdict.IsCadenceKept == false);
    CHECK(IsViewportPacingAccepted(verdict) == false);
}

TEST_CASE("Viewport pacing judges only progress and gaps below the idle frame floor",
          "[editor][viewport][navigation-cadence]")
{
    // A host that presents too few idle images says little about the throughput ratio.
    constexpr usize SLOW_IDLE_IMAGES = PACING_IDLE_IMAGE_FLOOR - 1;
    // Below a third of the idle median, which would fail the ratio if it were judged.
    constexpr usize SLOW_INPUT_IMAGES = 5;
    const ViewportPacingVerdict verdict = Evaluate(MakeTimeline(SLOW_IDLE_IMAGES, SLOW_INPUT_IMAGES, Cadence::Even));
    CHECK(verdict.IsRatioJudged == false);
    CHECK(verdict.IsThroughputKept == true);
    CHECK(verdict.HasProgress == true);
    CHECK(verdict.IsCadenceKept == true);
    CHECK(IsViewportPacingAccepted(verdict) == true);
    const ViewportPacingVerdict stalled =
        Evaluate(MakeTimeline(SLOW_IDLE_IMAGES, MIN_PACING_WINDOW_IMAGES, Cadence::Even));
    CHECK(stalled.HasProgress == false);
}

TEST_CASE("Viewport pacing reports gaps that cross a window boundary", "[editor][viewport][navigation-cadence]")
{
    constexpr uint64 MILLISECOND = time::NANOSECONDS_PER_MILLISECOND;
    const ViewportPacingWindow window =
    {
        .Kind = ViewportPacingWindowKind::Input,
        .BeginTicks = 1000 * MILLISECOND,
        .EndTicks = 2000 * MILLISECOND,
    };
    // One image before the window, three inside, none after the last until past the window end.
    const StaticArray<uint64, 5> ticks = {
        900 * MILLISECOND,
        1300 * MILLISECOND,
        1310 * MILLISECOND,
        1330 * MILLISECOND,
        2600 * MILLISECOND,
    };
    ViewportPacingWindowSummary summary;
    REQUIRE(SummarizeViewportPacingWindow(BorrowedView<const uint64>(ticks.GetData(), ticks.GetSize()),
                                          window,
                                          summary) == true);
    CHECK(summary.Images == 3);
    CHECK(summary.MedianInterval == 20 * MILLISECOND);
    CHECK(summary.P95Interval == 20 * MILLISECOND);
    // Open gap from the last image inside the window to its end.
    CHECK(summary.LongestGap == 670 * MILLISECOND);

    const ViewportPacingWindow empty = { .BeginTicks = 1400 * MILLISECOND, .EndTicks = 1900 * MILLISECOND };
    REQUIRE(SummarizeViewportPacingWindow(BorrowedView<const uint64>(ticks.GetData(), ticks.GetSize()),
                                          empty,
                                          summary) == true);
    CHECK(summary.Images == 0);
    CHECK(summary.LongestGap == 500 * MILLISECOND);

    const ViewportPacingWindowSummary unchanged = summary;
    const ViewportPacingWindow reversed = { .BeginTicks = 2, .EndTicks = 1 };
    CHECK(SummarizeViewportPacingWindow(BorrowedView<const uint64>(ticks.GetData(), ticks.GetSize()),
                                        reversed,
                                        summary) == false);
    CHECK(summary.LongestGap == unchanged.LongestGap);
}
