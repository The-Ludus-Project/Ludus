#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/hosted_surface.h>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/platform/hosted_window.h>

#include "internal/viewport_frame_source.h"

#include <QString>
#include <QTimer>

namespace ludus::editor
{
/// Where a PlayViewportSource's session stands.
enum class PlaySourceState : foundation::uint8
{
    /// Not started, or stopped.
    Stopped,
    /// Waiting for the game to open its hosted window.
    Listening,
    /// The game's window is connected; no frame has arrived yet.
    Connected,
    /// At least one game frame has arrived; the viewport can show the game.
    Playing,
    /// The game's window closed or its process went away. Terminal until Stop().
    Disconnected,
    /// The game's images cannot be shown here (GetUnavailableReason() says why). Terminal until Stop().
    Unavailable,
};

/// The editor end of Play in the viewport: the game's hosted window, and its frames as a
/// ViewportFrameSource.
///
/// Start() opens a HostedWindowEndpoint and imports the game's surface onto the viewport's
/// own device with a HostedSurfaceConsumer. The game process, launched with
/// GetChannelAddress() in `LUDUS_HOSTED_WINDOW`, renders through its ordinary RHI into
/// that surface. Each frame read the viewport opens is a held game frame; ending a read
/// queues a marker on Qt's queue behind the sampling, and the image returns to the game once
/// the marker completes. Frames are shown 1:1 at the size Resize() requests.
///
/// The source pumps the endpoint and consumer on its own timer, also before a viewport
/// presents it, because the game blocks in window creation until the endpoint answers. Input
/// and focus go to the game through GetEndpoint(). GUI thread only; the device whose
/// identity Start() receives must outlive Stop(). See
/// docs/architecture/editor-gui-systems.md, "Play in the editor viewport".
class PlayViewportSource final : public ViewportFrameSource
{
    Q_OBJECT
public:
    /// @param parentOrNull Optional Qt owner.
    explicit PlayViewportSource(QObject* parentOrNull = nullptr);
    /// Stop(); outstanding reads must already have been ended.
    ~PlayViewportSource() override;
    /// @brief Start a session for one game window.
    /// @param parentDirectory Existing short absolute directory for the private socket directory.
    /// @param device The viewport's device: the identity Qt's scene graph samples with.
    /// @param width Initial window width in physical pixels.
    /// @param height Initial window height in physical pixels.
    /// @param scale Physical pixels per logical pixel.
    /// @return True when listening; false with GetUnavailableReason() set otherwise.
    [[nodiscard]] bool Start(const QString& parentDirectory,
                             const graphics::rhi::NativeDeviceIdentity& device,
                             foundation::uint32 width,
                             foundation::uint32 height,
                             foundation::float32 scale);
    /// Close the game's window and destroy the imported images. Emits ResettingFrames first so
    /// a viewport ends its reads; queued releases complete before the images go.
    void Stop() noexcept;
    /// Value for `LUDUS_HOSTED_WINDOW` in the game's environment; empty unless started.
    [[nodiscard]] QString GetChannelAddress() const;
    [[nodiscard]] PlaySourceState GetState() const noexcept
    {
        return mState;
    }
    /// Why the game's images cannot be shown, when GetState() is Unavailable.
    [[nodiscard]] QString GetUnavailableReason() const
    {
        return mUnavailableReason;
    }
    /// The game's hosted window: focus, keys, visibility and close go through it.
    [[nodiscard]] platform::HostedWindowEndpoint& GetEndpoint() noexcept
    {
        return mEndpoint;
    }
    /// Frames presented by the game and taken by a viewport since Start().
    [[nodiscard]] foundation::uint64 GetTakenFrames() const noexcept
    {
        return mTakenFrames;
    }
    /// Pump the endpoint and consumer now (the timer also does this).
    void Pump();
    /// Physical pixels per logical pixel sent with the next Resize().
    void SetScale(foundation::float32 scale) noexcept;

    [[nodiscard]] bool HasPendingFrame() const noexcept override;
    /// Game frames are not RHI images: readback presentation is not offered.
    [[nodiscard]] bool TakeFrame(graphics::renderer::ViewportFrame& outFrame,
                                 graphics::renderer::ViewportImage& outImage) noexcept override;
    void ReleaseFrame(graphics::renderer::ViewportFrame& inoutFrame) noexcept override;
    /// Resize the game's window to this many physical pixels.
    void Resize(foundation::int32 width, foundation::int32 height) override;
    [[nodiscard]] graphics::rhi::RasterStatus TakeFrameRead(graphics::rhi::NativeDeviceLease device,
                                                            const graphics::rhi::NativeDeviceIdentity& consumer,
                                                            ViewportFrameRead& outRead) noexcept override;
    [[nodiscard]] graphics::rhi::RasterStatus EndFrameRead(ViewportFrameRead& inoutRead) noexcept override;
    void DrainFrameRead(ViewportFrameRead& inoutRead) noexcept override;
Q_SIGNALS:
    /// GetState() changed.
    void StateChanged();

private:
    /// Frames a viewport may hold at once: two slots, plus one being replaced.
    static constexpr foundation::usize MAX_FRAME_READS = 4;
    struct HeldRead final
    {
        bool IsValid = false;
        foundation::uint64 Token = 0;
        graphics::rhi::HostedFrame Frame{};
    };
    void setState(PlaySourceState state);
    void makeUnavailable(const QString& reason);
    [[nodiscard]] HeldRead* findReadOrNull(const ViewportFrameRead& read) noexcept;
    platform::HostedWindowEndpoint mEndpoint;
    graphics::rhi::HostedSurfaceConsumer mConsumer;
    graphics::rhi::NativeDeviceIdentity mDevice{};
    foundation::StaticArray<HeldRead, MAX_FRAME_READS> mReads{};
    QTimer mPumpTimer;
    QString mUnavailableReason;
    PlaySourceState mState = PlaySourceState::Stopped;
    foundation::uint64 mNextToken = 1;
    foundation::uint64 mTakenFrames = 0;
    foundation::float32 mScale = 1;
};
} // namespace ludus::editor
