#include "internal/viewport_frame_pump.h"

namespace ludus::editor
{
ViewportFramePump::ViewportFramePump()
{
    mService.setInterval(FALLBACK_SERVICE_MILLISECONDS);
    mService.setTimerType(Qt::PreciseTimer);
    mCompletion.setInterval(COMPLETION_POLL_MILLISECONDS);
    mCompletion.setTimerType(Qt::PreciseTimer);
    mWake.setInterval(0);
    mWake.setSingleShot(true);
    connect(&mService, &QTimer::timeout, this, &ViewportFramePump::dispatchWork);
    connect(&mCompletion, &QTimer::timeout, this, &ViewportFramePump::dispatchWork);
    connect(&mWake, &QTimer::timeout, this, &ViewportFramePump::dispatchWork);
}
void ViewportFramePump::Start() noexcept
{
    if (mService.isActive() == false)
    {
        mService.start();
    }
}
void ViewportFramePump::Stop() noexcept
{
    mService.stop();
    mCompletion.stop();
    mWake.stop();
    mHasDeferredWork = false;
}
void ViewportFramePump::Wake() noexcept
{
    if (IsActive() == true && mWake.isActive() == false)
    {
        mWake.start();
    }
}
void ViewportFramePump::SetProducerPending(bool hasPendingProducer) noexcept
{
    if (hasPendingProducer == true && IsActive() == true)
    {
        if (mCompletion.isActive() == false)
        {
            mCompletion.start();
        }
    }
    else
    {
        mCompletion.stop();
    }
}
void ViewportFramePump::dispatchWork()
{
    if (mIsDispatchingWork == true)
    {
        mHasDeferredWork = true;
        return;
    }
    mIsDispatchingWork = true;
    Q_EMIT WorkRequested();
    mIsDispatchingWork = false;
    if (mHasDeferredWork == true)
    {
        mHasDeferredWork = false;
        Wake();
    }
}
} // namespace ludus::editor
