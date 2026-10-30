#include "internal/texture_display_conversion.h"

#include <ludus/foundation/base/compiler.h>

#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>

#include <QPointer>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <new>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1'000'000.0;

QImage ConvertTexture(const InspectedTexture& texture,
                      const TextureDisplaySettings& settings,
                      TextureValueRange& outRange,
                      const TextureConversionCancellation& cancellation,
                      void* /*contextOrNull*/)
{
    return MakeDisplayImage(texture, settings, outRange, &cancellation);
}
} // namespace

// Like ContentWorker, this Qt adapter owns every byte touched by run(). No callback
// captures a widget. Owner-thread completion delivery is explicit and never joins.
class TextureDisplayWorker final : public QThread
{
    Q_OBJECT
public:
    TextureDisplayConversionFunction Convert;
    void* ContextOrNull = nullptr;
    uint64 Serial = 0;
    TextureConversionCancellation Cancellation;
    QElapsedTimer Requested;
    TextureDisplayConversionResult Result;

    // GUI writes request fields only before Wake, or after Converted. The semaphore
    // publishes them to the worker; the queued signal publishes the immutable result back.
    LUDUS_INLINE void Wake() noexcept
    {
        mPending.release();
    }
    LUDUS_INLINE void Stop() noexcept
    {
        Cancellation.Cancel();
        mShouldStop.store(true, std::memory_order_relaxed);
        mPending.release();
    }

Q_SIGNALS:
    void Converted();

private:
    void run() override
    {
        profiling::RegisterThreadForTrace("Texture conversion");
        for (;;)
        {
            mPending.acquire();
            if (mShouldStop.load(std::memory_order_relaxed) == true)
            {
                return;
            }
            {
                LUDUS_PROFILE_SCOPE(EditorTextureInspectionDisplayConversion);
                QElapsedTimer timer;
                timer.start();
                Result.Image = Convert(Result.Texture, Result.Settings, Result.Range, Cancellation, ContextOrNull);
                Result.WorkMilliseconds = static_cast<float64>(timer.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND;
            }
            Q_EMIT Converted();
        }
    }

    QSemaphore mPending;
    std::atomic<bool> mShouldStop{false};
};

TextureDisplayConversion::TextureDisplayConversion(QObject* parentOrNull,
                                                   TextureDisplayConversionFunction convertOrNull,
                                                   void* contextOrNull)
    : QObject(parentOrNull), mConvert(convertOrNull != nullptr ? convertOrNull : ConvertTexture),
      mContextOrNull(contextOrNull)
{
}

TextureDisplayConversion::~TextureDisplayConversion()
{
    Cancel();
    if (mWorkerOrNull != nullptr)
    {
        mWorkerOrNull->Stop();
    }
}

void TextureDisplayConversion::Request(const InspectedTexture& texture, const TextureDisplaySettings& settings)
{
    ++mRequestSerial;
    mPendingTexture = texture;
    mPendingSettings = settings;
    mPendingTimer.start();
    mHasPending = true;
    if (mIsBusy == true)
    {
        mWorkerOrNull->Cancellation.Cancel();
    }
    else
    {
        startPending();
    }
}

void TextureDisplayConversion::Cancel() noexcept
{
    ++mRequestSerial;
    mHasPending = false;
    mPendingTexture = {};
    if (mWorkerOrNull != nullptr)
    {
        mWorkerOrNull->Cancellation.Cancel();
    }
}

bool TextureDisplayConversion::IsBusy() const noexcept
{
    return mIsBusy == true || mHasPending == true;
}

const TextureDisplayConversionResult& TextureDisplayConversion::GetResult() const noexcept
{
    return mResult;
}

void TextureDisplayConversion::startPending()
{
    mHasPending = false;
    if (mWorkerOrNull == nullptr)
    {
        mWorkerOrNull = new (std::nothrow) TextureDisplayWorker;
        if (mWorkerOrNull != nullptr)
        {
            TextureDisplayWorker* worker = mWorkerOrNull;
            worker->Convert = mConvert;
            worker->ContextOrNull = mContextOrNull;
            connect(
                worker,
                &TextureDisplayWorker::Converted,
                this,
                [this, worker]() { finish(worker); },
                Qt::QueuedConnection);
            connect(worker, &QThread::finished, worker, &QObject::deleteLater);
            worker->start(QThread::LowPriority);
        }
    }
    if (mWorkerOrNull == nullptr)
    {
        mResult = {};
        mResult.ElapsedMilliseconds = static_cast<float64>(mPendingTimer.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND;
        mPendingTexture = {};
        Q_EMIT Completed();
        return;
    }
    mWorkerOrNull->Serial = mRequestSerial;
    mWorkerOrNull->Requested = mPendingTimer;
    mWorkerOrNull->Cancellation.Reset();
    mWorkerOrNull->Result = {};
    mWorkerOrNull->Result.Texture = mPendingTexture;
    mWorkerOrNull->Result.Settings = mPendingSettings;
    mPendingTexture = {};
    mIsBusy = true;
    mWorkerOrNull->Wake();
}

void TextureDisplayConversion::finish(TextureDisplayWorker* worker)
{
    mIsBusy = false;
    if (worker->Serial == mRequestSerial && worker->Cancellation.IsCancelled() == false)
    {
        mResult = worker->Result;
        mResult.ElapsedMilliseconds =
            static_cast<float64>(worker->Requested.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND;
        worker->Result = {};
        const QPointer<TextureDisplayConversion> owner(this);
        Q_EMIT Completed();
        if (owner.isNull() == true)
        {
            return;
        }
    }
    else
    {
        worker->Result = {};
    }
    if (mIsBusy == false && mHasPending == true)
    {
        startPending();
    }
}
} // namespace ludus::editor

#include "texture_display_conversion.moc"
