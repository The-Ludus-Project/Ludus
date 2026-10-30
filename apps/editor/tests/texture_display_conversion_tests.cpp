#include "internal/texture_display_conversion.h"
#include "internal/viewport_frame_pump.h"

#include <ludus/foundation/base/types.h>

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QObject>
#include <QPointer>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>

#include <atomic>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
constexpr int32 WAIT_MILLISECONDS = 5000;
constexpr uint32 POLL_MICROSECONDS = 200;

// The first call deliberately ignores cancellation until released. This proves
// that owner cancellation/destruction never waits even for an unresponsive worker.
struct ConversionGate final
{
    QSemaphore Started;
    QSemaphore Continue;
    QThread* OwnerThread = QThread::currentThread();
    std::atomic<uint32> Calls{0};
    std::atomic<uint32> Active{0};
    std::atomic<bool> IsWorkerThread{true};
    std::atomic<QThread*> FirstWorkerOrNull{nullptr};
    std::atomic<bool> HasReusedWorker{false};
    bool HasRequested = false;

    ~ConversionGate()
    {
        Continue.release();
        while (HasRequested == true && (Calls.load() == 0 || Active.load() != 0))
        {
            QThread::usleep(POLL_MICROSECONDS);
        }
    }
};

QImage ConvertGated(const InspectedTexture& texture,
                    const TextureDisplaySettings& settings,
                    TextureValueRange& outRange,
                    const TextureConversionCancellation& cancellation,
                    void* contextOrNull)
{
    if (contextOrNull == nullptr)
    {
        return {};
    }
    ConversionGate& gate = *static_cast<ConversionGate*>(contextOrNull);
    gate.Active.fetch_add(1);
    const uint32 call = gate.Calls.fetch_add(1);
    gate.IsWorkerThread.store(QThread::currentThread() != gate.OwnerThread);
    if (call == 0)
    {
        gate.FirstWorkerOrNull.store(QThread::currentThread());
        gate.Started.release();
        gate.Continue.acquire();
    }
    else
    {
        gate.HasReusedWorker.store(QThread::currentThread() == gate.FirstWorkerOrNull.load());
    }
    QImage image = MakeDisplayImage(texture, settings, outRange, &cancellation);
    gate.Active.fetch_sub(1);
    return image;
}

InspectedTexture MakeTexture(uint8 red, uint8 green)
{
    InspectedTexture texture;
    texture.Width = 1;
    texture.Height = 1;
    texture.Bytes.append(static_cast<char>(red));
    texture.Bytes.append(static_cast<char>(green));
    texture.Bytes.append(static_cast<char>(0));
    texture.Bytes.append(static_cast<char>(255));
    return texture;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (predicate() == false && timer.elapsed() < WAIT_MILLISECONDS)
    {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::usleep(POLL_MICROSECONDS);
    }
    return predicate();
}
} // namespace

TEST_CASE("Texture conversion runs off the GUI thread and coalesces to the latest snapshot and controls",
          "[editor][texture-viewer][conversion-worker]")
{
    ConversionGate gate;
    QObject owner;
    TextureDisplayConversion* conversion = new TextureDisplayConversion(&owner, ConvertGated, &gate);
    QSignalSpy completed(conversion, &TextureDisplayConversion::Completed);
    conversion->Request(MakeTexture(255, 0), {});
    gate.HasRequested = conversion->IsBusy();
    REQUIRE(gate.Started.tryAcquire(1, WAIT_MILLISECONDS) == true);
    CHECK(gate.IsWorkerThread.load() == true);
    CHECK(completed.count() == 0);

    bool hasServicedGui = false;
    QTimer::singleShot(0, conversion, [&]() { hasServicedGui = true; });
    QCoreApplication::processEvents();
    CHECK(hasServicedGui == true);
    CHECK(conversion->IsBusy() == true);
    ViewportFramePump pump;
    QSignalSpy serviced(&pump, &ViewportFramePump::WorkRequested);
    pump.Start();
    pump.Wake();
    REQUIRE(WaitUntil([&]() { return serviced.count() > 0; }) == true);
    CHECK(conversion->IsBusy() == true);
    pump.Stop();
    conversion->Request(MakeTexture(100, 0), {});
    TextureDisplaySettings settings;
    settings.Channel = TextureChannel::Green;
    conversion->Request(MakeTexture(0, 255), settings);
    gate.Continue.release();
    REQUIRE(WaitUntil([&]() { return conversion->IsBusy() == false; }) == true);
    REQUIRE(completed.count() == 1);
    CHECK(gate.Calls.load() == 2);
    CHECK(gate.HasReusedWorker.load() == true);
    CHECK(conversion->GetResult().Image.pixelColor(0, 0) == QColor(255, 255, 255));
    CHECK(conversion->GetResult().Texture.Bytes == MakeTexture(0, 255).Bytes);
    CHECK(conversion->GetResult().Settings.Channel == TextureChannel::Green);
    CHECK(conversion->GetResult().WorkMilliseconds > 0);
    CHECK(conversion->GetResult().ElapsedMilliseconds >= conversion->GetResult().WorkMilliseconds);
    // A later control/snapshot request also reuses the sleeping worker after it became idle.
    conversion->Request(MakeTexture(100, 0), {});
    REQUIRE(WaitUntil([&]() { return conversion->IsBusy() == false; }) == true);
    CHECK(completed.count() == 2);
    CHECK(gate.Calls.load() == 3);
    CHECK(gate.HasReusedWorker.load() == true);
    CHECK(conversion->GetResult().Image.pixelColor(0, 0) == QColor(100, 0, 0));
    const QPointer<QThread> worker(gate.FirstWorkerOrNull.load());
    delete conversion;
    REQUIRE(WaitUntil([&]() { return worker.isNull() == true; }) == true);
}

TEST_CASE("Cancelled texture conversions cannot publish and destroying an owner never joins",
          "[editor][texture-viewer][conversion-worker]")
{
    ConversionGate gate;
    QObject owner;
    TextureDisplayConversion* conversion = new TextureDisplayConversion(&owner, ConvertGated, &gate);
    QSignalSpy completed(conversion, &TextureDisplayConversion::Completed);
    conversion->Request(MakeTexture(255, 0), {});
    gate.HasRequested = conversion->IsBusy();
    REQUIRE(gate.Started.tryAcquire(1, WAIT_MILLISECONDS) == true);
    const QPointer<QThread> worker(gate.FirstWorkerOrNull.load());
    conversion->Request(MakeTexture(0, 255), {});
    conversion->Cancel();
    CHECK(completed.count() == 0);
    SECTION("cancelled work retires without publication")
    {
        gate.Continue.release();
        REQUIRE(WaitUntil([&]() { return conversion->IsBusy() == false; }) == true);
        CHECK(completed.count() == 0);
        CHECK(conversion->GetResult().Image.isNull() == true);
        delete conversion;
    }
    SECTION("destruction leaves a blocked worker with its own inputs")
    {
        delete conversion;
        CHECK(gate.Active.load() == 1);
        gate.Continue.release();
        REQUIRE(WaitUntil([&]() { return gate.Active.load() == 0; }) == true);
    }
    REQUIRE(WaitUntil([&]() { return worker.isNull() == true; }) == true);
    CHECK(gate.Calls.load() == 1);
}

TEST_CASE("Texture conversion cancellation preserves range output for both passes",
          "[editor][texture-viewer][conversion-worker]")
{
    TextureConversionCancellation cancellation;
    cancellation.Cancel();
    TextureDisplaySettings settings;
    settings.Range = TextureRangeMode::Automatic;
    TextureValueRange range{ .Minimum = 2, .Maximum = 3, .HasValues = true };
    CHECK(MakeDisplayImage(MakeTexture(0, 255), settings, range, &cancellation).isNull() == true);
    CHECK(range.Minimum == 2);
    CHECK(range.Maximum == 3);
    CHECK(range.HasValues == true);
    CHECK(FindValueRange(MakeTexture(0, 255), TextureChannel::Green, true, &cancellation).HasValues == false);
}

TEST_CASE("A texture conversion completion may destroy its owner synchronously",
          "[editor][texture-viewer][conversion-worker]")
{
    ConversionGate gate;
    QObject owner;
    TextureDisplayConversion* conversionOrNull = new TextureDisplayConversion(&owner, ConvertGated, &gate);
    QObject::connect(conversionOrNull, &TextureDisplayConversion::Completed, &owner, [&]() {
        delete conversionOrNull;
        conversionOrNull = nullptr;
    });
    conversionOrNull->Request(MakeTexture(0, 255), {});
    gate.HasRequested = conversionOrNull->IsBusy();
    REQUIRE(gate.Started.tryAcquire(1, WAIT_MILLISECONDS) == true);
    const QPointer<QThread> worker(gate.FirstWorkerOrNull.load());
    gate.Continue.release();
    REQUIRE(WaitUntil([&]() { return conversionOrNull == nullptr; }) == true);
    REQUIRE(WaitUntil([&]() { return worker.isNull() == true; }) == true);
}
