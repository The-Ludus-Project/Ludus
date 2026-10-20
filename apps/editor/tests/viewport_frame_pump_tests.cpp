#include "internal/viewport_frame_pump.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;

TEST_CASE("Viewport frame servicing survives continuous requests to start", "[editor][frame-pump]")
{
    ViewportFramePump pump;
    QSignalSpy work(&pump, &ViewportFramePump::WorkRequested);
    QTimer input;
    int32 starts = 0;
    QEventLoop loop;
    QObject::connect(&input, &QTimer::timeout, &pump, [&pump, &starts]() {
        ++starts;
        pump.Start();
    });
    QObject::connect(&pump, &ViewportFramePump::WorkRequested, &loop, [&loop, &work, &starts]() {
        if (starts > 20 && work.count() > 1)
        {
            loop.quit();
        }
    });
    pump.Start();
    input.start(2);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    input.stop();
    pump.Stop();
    CHECK(starts > 20);
    CHECK(work.count() > 1);
}

TEST_CASE("Viewport wakes defer submission and stopping cancels every wake and poll", "[editor][frame-pump]")
{
    ViewportFramePump pump;
    QSignalSpy work(&pump, &ViewportFramePump::WorkRequested);
    pump.Start();
    pump.Wake();
    pump.Wake();
    pump.SetProducerPending(true);
    CHECK(work.count() == 0);
    pump.Stop();
    pump.Wake();
    pump.SetProducerPending(true);
    QTest::qWait(20);
    CHECK(work.count() == 0);
    CHECK(pump.IsActive() == false);

    pump.Start();
    pump.SetProducerPending(true);
    REQUIRE(QTest::qWaitFor([&work]() { return work.count() > 0; }) == true);
    pump.SetProducerPending(false);
    pump.Stop();
    const isize stoppedCount = static_cast<isize>(work.count());
    QTest::qWait(20);
    CHECK(work.count() == stoppedCount);
}

TEST_CASE("Viewport GPU servicing cannot reenter through a nested event loop", "[editor][frame-pump]")
{
    ViewportFramePump pump;
    int32 calls = 0;
    QObject::connect(&pump, &ViewportFramePump::WorkRequested, &pump, [&pump, &calls]() {
        ++calls;
        if (calls == 1)
        {
            pump.Wake();
            QCoreApplication::processEvents();
            CHECK(calls == 1);
            pump.Stop();
        }
    });
    pump.Start();
    pump.SetProducerPending(true);
    REQUIRE(QTest::qWaitFor([&calls]() { return calls > 0; }) == true);
    QTest::qWait(20);
    CHECK(calls == 1);
}
