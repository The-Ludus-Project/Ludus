#include "internal/backend.hpp"

#include "internal/log_record.hpp"
#include "internal/sink.hpp"

#include <chrono>

namespace ludus::foundation::logging::internal
{

void AsyncBackend::RenderAndWrite(const QueuedRecord& rec) noexcept
{
    // Reconstruct the sink view over the dequeued OWNED bytes. Message points
    // into the record's own storage (valid for this call); category/file are
    // process-lifetime views; thread name was copied into the record so it is
    // valid even after the producer thread exits (requirements R24/R36).
    LogRecordView view;
    view.Level = rec.Level;
    view.Category = LogCategory{rec.CategoryId, rec.CategoryName};
    view.Message = std::string_view(rec.Message, rec.MessageLen);
    view.File = rec.File;
    view.Function = std::string_view{};
    view.Line = rec.Line;
    view.ThreadName = rec.ThreadNameView();
    view.ThreadId = rec.ThreadId;
    view.NativeThreadId = rec.NativeThreadId;
    view.MonotonicTicks = rec.MonotonicTicks;
    view.Sequence = rec.Sequence;

    for (auto& sink : mSinks)
    {
        (void)sink->Write(view);
    }
    // Error/Fatal flush promptly so their visibility promise holds in async mode
    // too (requirements R54).
    if (rec.Level >= LogLevel::Error)
    {
        for (auto& sink : mSinks)
        {
            (void)sink->Flush();
        }
    }
}

void AsyncBackend::DrainBatch() noexcept
{
    constexpr int kBatch = 256; // bounded so flush/control work is not starved (R39)
    QueuedRecord rec;
    for (int i = 0; i < kBatch; ++i)
    {
        if (mQueue.TryDequeue(rec) == false)
        {
            break;
        }
        RenderAndWrite(rec);
        mWritten.fetch_add(1, std::memory_order_relaxed);
    }
}

void AsyncBackend::ServiceFlush() noexcept
{
    bool durable = false;
    uint64 fence = 0;
    bool requested = false;
    {
        std::lock_guard<std::mutex> lock(mControlMutex);
        requested = mFlushRequested;
        durable = mFlushDurable;
        fence = mFlushFence;
    }
    if (requested == false)
    {
        return;
    }
    // Complete the fence only once the consumer cursor has passed it (all records
    // reserved before the fence are drained). A paused producer before the fence
    // keeps us from acking; the requester's deadline makes that observable
    // (requirements R29).
    if (mQueue.DequeuePos() < fence)
    {
        return;
    }
    bool ok = true;
    for (auto& sink : mSinks)
    {
        const SinkStatus s = durable ? sink->FlushDurable() : sink->Flush();
        if (s != SinkStatus::Ok && s != SinkStatus::Unsupported)
        {
            ok = false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mControlMutex);
        mFlushAckedPos = mQueue.DequeuePos();
        mLastFlushOk = ok;
        mFlushRequested = false;
        mFlushDurable = false;
        mFlushRequestedFlag.store(false, std::memory_order_release);
    }
    mFlushDone.notify_all();
}

void AsyncBackend::Run() noexcept
{
    uint64 seen = 0;
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(mWakeMutex);
            mWakeCv.wait_for(lock,
                             std::chrono::milliseconds(mFlushIntervalMs == 0 ? 50 : mFlushIntervalMs),
                             [this, &seen] {
                                 return mStop.load(std::memory_order_acquire) ||
                                        mPublished.load(std::memory_order_acquire) != seen ||
                                        mFlushRequestedFlag.load(std::memory_order_acquire);
                             });
            seen = mPublished.load(std::memory_order_acquire);
        }

        DrainBatch();
        ServiceFlush();

        if (mFlushIntervalMs != 0)
        {
            for (auto& sink : mSinks)
            {
                (void)sink->Flush();
            }
        }

        if (mStop.load(std::memory_order_acquire))
        {
            QueuedRecord rec;
            while (mQueue.TryDequeue(rec))
            {
                RenderAndWrite(rec);
                mWritten.fetch_add(1, std::memory_order_relaxed);
            }
            for (auto& sink : mSinks)
            {
                (void)sink->Flush();
            }
            ServiceFlush();
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mControlMutex);
        mExited = true;
    }
    mExitedCv.notify_all();
}

void AsyncBackend::Start(foundation::Array<foundation::UniquePtr<ILogSink>>&& sinks, uint32 flushIntervalMs) noexcept
{
    mSinks = std::move(sinks);
    mFlushIntervalMs = flushIntervalMs;
    mStop.store(false, std::memory_order_relaxed);
    mWorker = std::thread([this] { Run(); });
}

bool AsyncBackend::Enqueue(const QueuedRecord& record) noexcept
{
    const bool ok = mQueue.TryEnqueue(record);
    if (ok)
    {
        mPublished.fetch_add(1, std::memory_order_release);
        mWakeCv.notify_one();
    }
    return ok;
}

AsyncBackend::FlushOutcome AsyncBackend::Flush(bool durable, uint32 timeoutMs) noexcept
{
    FlushOutcome outcome;
    std::unique_lock<std::mutex> lock(mControlMutex);
    const uint64 fence = mQueue.EnqueuePos();
    mFlushFence = fence;
    mFlushDurable = mFlushDurable || durable;
    mFlushRequested = true;
    mFlushRequestedFlag.store(true, std::memory_order_release);
    mWakeCv.notify_one();

    const bool completed = mFlushDone.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this, fence] {
        return mFlushAckedPos >= fence && mFlushRequested == false;
    });

    outcome.AcknowledgedPos = mFlushAckedPos;
    outcome.TimedOut = completed == false;
    outcome.Ok = completed && mLastFlushOk;
    return outcome;
}

bool AsyncBackend::Stop(uint32 joinTimeoutMs) noexcept
{
    mStop.store(true, std::memory_order_release);
    mWakeCv.notify_all();
    if (mWorker.joinable() == false)
    {
        return true;
    }
    {
        std::unique_lock<std::mutex> lock(mControlMutex);
        const bool exited =
            mExitedCv.wait_for(lock, std::chrono::milliseconds(joinTimeoutMs), [this] { return mExited; });
        if (exited == false)
        {
            return false; // caller retains storage; do not free (requirements R40)
        }
    }
    mWorker.join();
    for (auto& sink : mSinks)
    {
        (void)sink->Flush();
    }
    mSinks.Clear();
    return true;
}

uint64 AsyncBackend::Dropped() const noexcept
{
    return mQueue.Dropped();
}

uint64 AsyncBackend::Written() const noexcept
{
    return mWritten.load(std::memory_order_relaxed);
}

} // namespace ludus::foundation::logging::internal
