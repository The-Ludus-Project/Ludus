#include "internal/code_latency_probe.h"

#include <catch2/catch_test_macros.hpp>

#include <QElapsedTimer>

using namespace ludus::editor;
using namespace ludus::foundation;

TEST_CASE("the latency probe is idle until started", "[editor][code-editor]")
{
    CodeLatencyProbe probe;
    REQUIRE(probe.IsRecording() == false);
    // Marks before Start() record nothing: idle editing performs no measurement.
    probe.MarkKeyHandled();
    probe.MarkPainted();
    REQUIRE(probe.GetSampleCount() == 0);
    REQUIRE(probe.GetPercentileNanoseconds(95) == 0);
}

TEST_CASE("a paint only records when a keystroke is pending", "[editor][code-editor]")
{
    CodeLatencyProbe probe;
    probe.Start();
    REQUIRE(probe.IsRecording() == true);

    // A paint with no pending key is not a keypress-to-paint sample.
    probe.MarkPainted();
    REQUIRE(probe.GetSampleCount() == 0);

    // One key then one paint yields exactly one sample.
    probe.MarkKeyHandled();
    probe.MarkPainted();
    REQUIRE(probe.GetSampleCount() == 1);

    // A second paint without a new key adds nothing.
    probe.MarkPainted();
    REQUIRE(probe.GetSampleCount() == 1);
}

TEST_CASE("starting clears the previous samples", "[editor][code-editor]")
{
    CodeLatencyProbe probe;
    probe.Start();
    probe.MarkKeyHandled();
    probe.MarkPainted();
    REQUIRE(probe.GetSampleCount() == 1);
    probe.Start();
    REQUIRE(probe.GetSampleCount() == 0);
}

TEST_CASE("a sample measures from the key's arrival, including handling", "[editor][code-editor]")
{
    constexpr int64 HANDLING_NANOSECONDS = 2000000;
    CodeLatencyProbe probe;
    probe.Start();
    probe.MarkKeyArrived();
    QElapsedTimer handling;
    handling.start();
    while (handling.nsecsElapsed() < HANDLING_NANOSECONDS)
    {
    }
    probe.MarkKeyHandled();
    probe.MarkPainted();
    REQUIRE(probe.GetSampleCount() == 1);
    REQUIRE(probe.GetPercentileNanoseconds(50) >= HANDLING_NANOSECONDS);
}

TEST_CASE("a key that changes no text records no sample", "[editor][code-editor]")
{
    CodeLatencyProbe probe;
    probe.Start();
    probe.MarkKeyArrived();
    probe.MarkPainted();
    REQUIRE(probe.GetSampleCount() == 0);
}
