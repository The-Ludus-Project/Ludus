#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

#include <QImage>

namespace ludus::editor
{
/// Result of advancing an asynchronous offscreen frame copy.
enum class OffscreenReadStatus : foundation::uint8
{
    /// No copy is in progress.
    Idle,
    /// Band copies are still in flight; poll again on a later tick.
    Pending,
    /// Every band arrived; TakeImage publishes the complete frame.
    Complete,
    /// The RHI rejected a request or a copy failed; Cancel before reuse.
    Failed,
};
/// Copies one RGBA8 viewport display image to a QImage in row bands.
///
/// Used when Qt cannot share the engine's GPU device (forced offscreen, or no Vulkan adapter
/// can present to the window with a shareable device). Each band
/// fits one RHI transfer slice, at most TransfersPerDirection bands are in flight, and the GUI
/// thread only polls, so no call waits for the GPU. Owner-thread use only, between frames.
class OffscreenFrameReader final
{
public:
    /// Start copying a Ready RGBA8Unorm texture of the given physical size.
    /// @pre No copy is in progress. The texture must stay leased until Complete, Failed or Cancel.
    /// @return False when the size is empty or the device reports no transfer budget.
    [[nodiscard]] bool Begin(graphics::rhi::DeviceHandle device,
                             graphics::rhi::TextureHandle texture,
                             foundation::uint32 width,
                             foundation::uint32 height);
    /// Collect completed bands and optionally issue more, without waiting.
    /// @param shouldRequestBands False yields shared staging capacity while retaining the unfinished image.
    [[nodiscard]] OffscreenReadStatus Poll(bool shouldRequestBands = true) noexcept;
    /// Move the completed frame out and return to Idle. Valid only after Poll returned Complete.
    [[nodiscard]] QImage TakeImage() noexcept;
    /// Drop every in-flight ticket; the RHI retires their staging after GPU completion.
    void Cancel() noexcept;
    /// True while a copy started by Begin has not been taken or cancelled.
    [[nodiscard]] LUDUS_INLINE bool IsBusy() const noexcept
    {
        return mIsBusy;
    }

private:
    /// Maximum simultaneously owned band tickets; the RHI allows at most four per direction.
    static constexpr foundation::uint32 MAX_BANDS_IN_FLIGHT = 4;
    struct Band final
    {
        graphics::rhi::ReadbackTicket Ticket{};
        foundation::uint32 FirstRow = 0;
        foundation::uint32 RowCount = 0;
        bool IsInFlight = false;
    };
    [[nodiscard]] bool requestBand(Band& outBand) noexcept;
    graphics::rhi::DeviceHandle mDevice{};
    graphics::rhi::TextureHandle mTexture{};
    QImage mImage;
    Band mBands[MAX_BANDS_IN_FLIGHT]{};
    foundation::uint32 mBandsAllowed = 0;
    foundation::uint32 mRowsPerBand = 0;
    foundation::uint32 mNextRow = 0;
    foundation::uint32 mRowsReceived = 0;
    bool mIsBusy = false;
    bool mHasFailed = false;
};
} // namespace ludus::editor
