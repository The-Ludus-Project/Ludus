#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <QObject>
#include <QTimer>

namespace ludus::editor
{
/// GUI-thread service clock for one preview. Polls submitted GPU work promptly;
/// presentation returns wake the producer without waiting for the fallback service clock.
class ViewportFramePump final : public QObject
{
    Q_OBJECT
public:
    /// Retry interval while submitted producer/consumer GPU work is pending, in milliseconds.
    static constexpr foundation::int32 COMPLETION_POLL_MILLISECONDS = 1;
    ViewportFramePump();
    /// Start fallback servicing; an already running clock keeps its deadline.
    void Start() noexcept;
    /// Cancel fallback servicing, GPU polls and queued wakes.
    void Stop() noexcept;
    /// Coalesce an asynchronous owner-thread wake after a consumer returns an image.
    void Wake() noexcept;
    /// Poll only while the active preview has submitted GPU work awaiting completion.
    void SetProducerPending(bool hasPendingProducer) noexcept;
    [[nodiscard]] LUDUS_INLINE bool IsActive() const noexcept
    {
        return mIsRunning;
    }
Q_SIGNALS:
    /// Service the preview on its GUI/device owner thread; callbacks never nest.
    void WorkRequested();

private:
    static constexpr foundation::int32 FALLBACK_SERVICE_MILLISECONDS = 16;
    QTimer mService;
    QTimer mCompletion;
    QTimer mWake;
    bool mIsRunning = false;
    bool mIsDispatchingWork = false;
    bool mHasDeferredWork = false;
    void dispatchWork();
};
} // namespace ludus::editor
