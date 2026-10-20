#include "internal/play_viewport_source.h"

#include <ludus/foundation/base/core.h>

#include <string_view>

namespace ludus::editor
{
namespace
{
namespace rhi = graphics::rhi;
namespace rr = graphics::renderer;
using foundation::float32;
using foundation::int32;
using foundation::uint32;
/// How often the source pumps the game's window channel, in milliseconds. The game waits for
/// the endpoint during window creation, and each pump returns released images to it.
constexpr int32 PUMP_INTERVAL_MILLISECONDS = 4;

QString DescribeRefusal(rhi::HostedConsumerRefusal refusal)
{
    switch (refusal)
    {
        case rhi::HostedConsumerRefusal::AdapterMismatch:
            return QStringLiteral("the game renders on another GPU adapter or driver");
        case rhi::HostedConsumerRefusal::UnsupportedHandle:
            return QStringLiteral("the game shares its images in a form this editor cannot import");
        case rhi::HostedConsumerRefusal::ImportFailed:
            return QStringLiteral("the game's images could not be imported");
        case rhi::HostedConsumerRefusal::ProtocolViolation:
            return QStringLiteral("the game announced a frame it did not share");
        case rhi::HostedConsumerRefusal::None:
            break;
    }
    return QStringLiteral("the game's images were refused");
}

bool IsSameContext(const rhi::NativeDeviceIdentity& first, const rhi::NativeDeviceIdentity& second) noexcept
{
    return first.Api == second.Api && first.Device.GetObject() == second.Device.GetObject() &&
           first.CommandQueue.GetObject() == second.CommandQueue.GetObject();
}
} // namespace

PlayViewportSource::PlayViewportSource(QObject* parentOrNull) : ViewportFrameSource(parentOrNull)
{
    mPumpTimer.setTimerType(Qt::PreciseTimer);
    mPumpTimer.setInterval(PUMP_INTERVAL_MILLISECONDS);
    connect(&mPumpTimer, &QTimer::timeout, this, &PlayViewportSource::Pump);
}

PlayViewportSource::~PlayViewportSource()
{
    Stop();
}

bool PlayViewportSource::Start(const QString& parentDirectory,
                               const rhi::NativeDeviceIdentity& device,
                               uint32 width,
                               uint32 height,
                               float32 scale)
{
    if (mState != PlaySourceState::Stopped)
    {
        return false;
    }
    mUnavailableReason.clear();
    if (device.Api != rhi::Backend::Metal)
    {
        // Vulkan needs VK_KHR_external_memory_fd on Qt's device, the external queue-family
        // acquire and a fence on Qt's queue; until then Play opens in its own window.
        makeUnavailable(QStringLiteral("Play in the viewport is not yet available with Vulkan."));
        return false;
    }
    const QByteArray parent = parentDirectory.toUtf8();
    platform::HostedWindowEndpoint::StartInfo info;
    info.ParentDirectory = std::string_view(parent.constData(), static_cast<foundation::usize>(parent.size()));
    info.Width = width;
    info.Height = height;
    info.Scale = scale;
    const platform::HostedEndpointStatus started = mEndpoint.Start(info);
    if (started != platform::HostedEndpointStatus::Ok)
    {
        makeUnavailable(QStringLiteral("Play in the viewport could not open the game's window channel (status %1).")
                            .arg(static_cast<uint32>(started)));
        return false;
    }
    if (mConsumer.Start(mEndpoint, device) != rhi::HostedConsumerStatus::Ok)
    {
        mEndpoint.Stop();
        makeUnavailable(QStringLiteral("Play in the viewport cannot import game images on this GPU device."));
        return false;
    }
    mDevice = device;
    mScale = scale;
    mTakenFrames = 0;
    setState(PlaySourceState::Listening);
    mPumpTimer.start();
    return true;
}

void PlayViewportSource::Stop() noexcept
{
    if (mState == PlaySourceState::Stopped)
    {
        return;
    }
    // A viewport ends its reads first; their markers are queued behind Qt's sampling.
    Q_EMIT ResettingFrames();
    for (HeldRead& held : mReads)
    {
        if (held.IsValid == true)
        {
            static_cast<void>(mConsumer.ReleaseFrameAfterQueue(held.Frame));
            held = {};
        }
    }
    mPumpTimer.stop();
    // Waits for queued markers before destroying the imports; closing the channel closes the game's window.
    mConsumer.Stop();
    mEndpoint.Stop();
    mDevice = {};
    setState(PlaySourceState::Stopped);
}

QString PlayViewportSource::GetChannelAddress() const
{
    const std::string_view address = mEndpoint.GetChannelAddress();
    return QString::fromUtf8(address.data(), static_cast<qsizetype>(address.size()));
}

void PlayViewportSource::Pump()
{
    if (mState == PlaySourceState::Stopped || mState == PlaySourceState::Unavailable)
    {
        return;
    }
    static_cast<void>(mEndpoint.Pump());
    const rhi::HostedConsumerStatus pumped = mConsumer.Pump();
    if (pumped == rhi::HostedConsumerStatus::Refused)
    {
        makeUnavailable(QStringLiteral("Play in the viewport is unavailable: ") +
                        DescribeRefusal(mConsumer.GetRefusal()) + QStringLiteral("."));
        return;
    }
    const platform::HostedConnectionState connection = mEndpoint.GetState();
    if (connection == platform::HostedConnectionState::Disconnected && mState != PlaySourceState::Disconnected)
    {
        setState(PlaySourceState::Disconnected);
        return;
    }
    if (connection == platform::HostedConnectionState::Connected && mState == PlaySourceState::Listening)
    {
        setState(PlaySourceState::Connected);
    }
    if (mConsumer.HasFrame() == true && mState != PlaySourceState::Disconnected)
    {
        if (mState == PlaySourceState::Connected)
        {
            setState(PlaySourceState::Playing);
        }
        Q_EMIT FrameReady();
    }
}

void PlayViewportSource::SetScale(float32 scale) noexcept
{
    mScale = scale;
}

bool PlayViewportSource::HasPendingFrame() const noexcept
{
    return (mState == PlaySourceState::Connected || mState == PlaySourceState::Playing) && mConsumer.HasFrame();
}

bool PlayViewportSource::TakeFrame([[maybe_unused]] rr::ViewportFrame& outFrame,
                                   [[maybe_unused]] rr::ViewportImage& outImage) noexcept
{
    return false;
}

void PlayViewportSource::ReleaseFrame([[maybe_unused]] rr::ViewportFrame& inoutFrame) noexcept {}

void PlayViewportSource::Resize(int32 width, int32 height)
{
    if (width <= 0 || height <= 0 || mState == PlaySourceState::Stopped)
    {
        return;
    }
    // Before admission the endpoint records the extent and sends it in its Welcome.
    static_cast<void>(mEndpoint.SendResize(static_cast<uint32>(width), static_cast<uint32>(height), mScale));
}

rhi::RasterStatus PlayViewportSource::TakeFrameRead(rhi::NativeDeviceLease device,
                                                    const rhi::NativeDeviceIdentity& consumer,
                                                    ViewportFrameRead& outRead) noexcept
{
    if (HasPendingFrame() == false)
    {
        return rhi::RasterStatus::NotReady;
    }
    if (IsSameContext(consumer, mDevice) == false)
    {
        return rhi::RasterStatus::InvalidState;
    }
    HeldRead* freeOrNull = nullptr;
    uint32 slot = 0;
    for (uint32 i = 0; i < MAX_FRAME_READS; ++i)
    {
        if (mReads[i].IsValid == false)
        {
            freeOrNull = &mReads[i];
            slot = i;
            break;
        }
    }
    if (freeOrNull == nullptr)
    {
        return rhi::RasterStatus::NotReady;
    }
    rhi::NativeDeviceInfo info;
    if (rhi::GetNativeDeviceInfo(device, info) != rhi::RasterStatus::Ready)
    {
        return rhi::RasterStatus::InvalidHandle;
    }
    rhi::HostedFrame frame;
    const rhi::HostedConsumerStatus acquired = mConsumer.AcquireFrame(frame);
    if (acquired != rhi::HostedConsumerStatus::Ok)
    {
        return acquired == rhi::HostedConsumerStatus::Empty ? rhi::RasterStatus::NotReady : rhi::RasterStatus::Failed;
    }
    ViewportFrameRead read;
    read.Image.Device = info;
    read.Image.Revision = frame.Sequence;
    read.Image.ProducerOrdinal = frame.Sequence;
    read.Image.Format = rhi::RasterFormat::Rgba8Unorm;
    read.Image.Width = frame.Width;
    read.Image.Height = frame.Height;
    read.Image.MetalTexture = frame.MetalTexture;
    read.Image.VulkanImage = frame.VulkanImage;
    read.Image.VulkanLayout = frame.VulkanLayout;
    read.Image.SampledReadOnly = true;
    read.Token = mNextToken++;
    read.Slot = slot;
    *freeOrNull = { .IsValid = true, .Token = read.Token, .Frame = frame };
    ++mTakenFrames;
    outRead = read;
    return rhi::RasterStatus::Ready;
}

rhi::RasterStatus PlayViewportSource::EndFrameRead(ViewportFrameRead& inoutRead) noexcept
{
    HeldRead* heldOrNull = findReadOrNull(inoutRead);
    if (heldOrNull == nullptr)
    {
        // Already reclaimed by Stop(): nothing is held any more.
        inoutRead = {};
        return rhi::RasterStatus::Ready;
    }
    const rhi::HostedConsumerStatus released = mConsumer.ReleaseFrameAfterQueue(heldOrNull->Frame);
    if (released == rhi::HostedConsumerStatus::Unavailable)
    {
        return rhi::RasterStatus::Unsupported;
    }
    // Ok queues the release behind Qt's submitted sampling; InvalidState means the consumer
    // already forgot the frame (its window went away). Either way the read is over.
    *heldOrNull = {};
    inoutRead = {};
    return rhi::RasterStatus::Ready;
}

void PlayViewportSource::DrainFrameRead(ViewportFrameRead& inoutRead) noexcept
{
    // The queued marker completes behind Qt's sampling; Stop() waits for it before destroying images.
    static_cast<void>(EndFrameRead(inoutRead));
    inoutRead = {};
}

void PlayViewportSource::setState(PlaySourceState state)
{
    if (state == mState)
    {
        return;
    }
    mState = state;
    Q_EMIT StateChanged();
}

void PlayViewportSource::makeUnavailable(const QString& reason)
{
    mUnavailableReason = reason;
    setState(PlaySourceState::Unavailable);
}

PlayViewportSource::HeldRead* PlayViewportSource::findReadOrNull(const ViewportFrameRead& read) noexcept
{
    if (read.Slot >= MAX_FRAME_READS || read.Token == 0)
    {
        return nullptr;
    }
    HeldRead& held = mReads[read.Slot];
    return held.IsValid == true && held.Token == read.Token ? &held : nullptr;
}
} // namespace ludus::editor
