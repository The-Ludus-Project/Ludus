#include "internal/viewport_frame_pump.h"

namespace ludus::editor
{
ViewportFramePump::ViewportFramePump()
{
    mService.setParent(this);
    mCompletion.setParent(this);
    mWake.setParent(this);
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
    if (mIsRunning == false)
    {
        mIsRunning = true;
        mService.start();
    }
}
void ViewportFramePump::Stop() noexcept
{
    mIsRunning = false;
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
        mService.stop();
        if (mCompletion.isActive() == false)
        {
            mCompletion.start();
        }
    }
    else
    {
        mCompletion.stop();
        if (mIsRunning == true && mService.isActive() == false)
        {
            mService.start();
        }
    }
}
void ViewportFramePump::dispatchWork()
{
    if (mIsDispatchingWork == true)
    {
        mHasDeferredWork = true;
        return;
    }
    mWake.stop(); // This service consumes any already queued wake.
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
