#include "internal/offscreen_frame_reader.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/profiling/profiling.hpp>

namespace ludus::editor
{
using namespace foundation;
namespace rhi = graphics::rhi;
namespace
{
/// Bytes per RGBA8Unorm texel.
constexpr usize BYTES_PER_TEXEL = 4;
/// Row alignment the RHI applies to its staging slices (see RequestTextureReadback).
constexpr usize STAGING_ROW_ALIGNMENT = 256;
} // namespace
bool OffscreenFrameReader::Begin(rhi::DeviceHandle device, rhi::TextureHandle texture, uint32 width, uint32 height)
{
    if (mIsBusy == true || width == 0 || height == 0)
    {
        return false;
    }
    rhi::LifetimeCapabilities capabilities;
    if (rhi::GetLifetimeCapabilities(device, capabilities) != rhi::RasterStatus::Ready ||
        capabilities.TransfersPerDirection == 0)
    {
        return false;
    }
    const usize rowBytes = static_cast<usize>(width) * BYTES_PER_TEXEL;
    const usize stagingRowBytes =
        (rowBytes + STAGING_ROW_ALIGNMENT - 1) / STAGING_ROW_ALIGNMENT * STAGING_ROW_ALIGNMENT;
    const usize rowsPerSlice = capabilities.TransferBytes / stagingRowBytes;
    if (rowsPerSlice == 0)
    {
        return false;
    }
    QImage image(static_cast<int32>(width), static_cast<int32>(height), QImage::Format_RGBA8888);
    if (image.isNull() == true || static_cast<usize>(image.bytesPerLine()) != rowBytes)
    {
        return false;
    }
    mImage = image;
    mDevice = device;
    mTexture = texture;
    mRowsPerBand = static_cast<uint32>(math::Min(rowsPerSlice, static_cast<usize>(height)));
    mBandsAllowed = math::Min(capabilities.TransfersPerDirection, MAX_BANDS_IN_FLIGHT);
    mNextRow = 0;
    mRowsReceived = 0;
    mHasFailed = false;
    mIsBusy = true;
    return true;
}
bool OffscreenFrameReader::requestBand(Band& outBand) noexcept
{
    LUDUS_PROFILE_SCOPE(EditorReadbackRequestBand);
    const uint32 height = static_cast<uint32>(mImage.height());
    const uint32 rows = math::Min(mRowsPerBand, height - mNextRow);
    rhi::ReadbackTicket ticket;
    const rhi::RasterStatus requested = rhi::RequestTextureReadback(
        mDevice,
        mTexture,
        { .Mip = 0, .X = 0, .Y = mNextRow, .Width = static_cast<uint32>(mImage.width()), .Height = rows },
        static_cast<usize>(mImage.bytesPerLine()),
        ticket);
    if (requested == rhi::RasterStatus::NotReady || requested == rhi::RasterStatus::CapacityExceeded)
    {
        LUDUS_PROFILE_MARK(EditorReadbackAwaitTransferCapacity);
        // Ring slices are still retiring; retry on the next poll.
        return true;
    }
    if (requested != rhi::RasterStatus::Pending && requested != rhi::RasterStatus::Ready)
    {
        return false;
    }
    outBand = { .Ticket = ticket, .FirstRow = mNextRow, .RowCount = rows, .IsInFlight = true };
    mNextRow += rows;
    return true;
}
OffscreenReadStatus OffscreenFrameReader::Poll() noexcept
{
    LUDUS_PROFILE_SCOPE(EditorReadbackPoll);
    if (mIsBusy == false)
    {
        return OffscreenReadStatus::Idle;
    }
    if (mHasFailed == true)
    {
        return OffscreenReadStatus::Failed;
    }
    const uint32 height = static_cast<uint32>(mImage.height());
    for (uint32 i = 0; i < mBandsAllowed; ++i)
    {
        Band& band = mBands[i];
        if (band.IsInFlight == true)
        {
            const rhi::RasterStatus status = rhi::GetStatus(mDevice, band.Ticket);
            if (status == rhi::RasterStatus::Pending)
            {
                LUDUS_PROFILE_MARK(EditorReadbackAwaitGpu);
                continue;
            }
            LUDUS_PROFILE_SCOPE(EditorReadbackCopyBand);
            const usize offset = static_cast<usize>(band.FirstRow) * static_cast<usize>(mImage.bytesPerLine());
            const usize capacity = static_cast<usize>(band.RowCount) * static_cast<usize>(mImage.bytesPerLine());
            const bool isCopied =
                status == rhi::RasterStatus::Ready &&
                rhi::CopyReadback(mDevice, band.Ticket, mImage.bits() + offset, capacity) == rhi::RasterStatus::Ready;
            const uint32 rowsCopied = band.RowCount;
            static_cast<void>(rhi::Release(mDevice, band.Ticket));
            band = {};
            if (isCopied == false)
            {
                mHasFailed = true;
                return OffscreenReadStatus::Failed;
            }
            mRowsReceived += rowsCopied;
        }
    }
    for (uint32 i = 0; i < mBandsAllowed && mNextRow < height; ++i)
    {
        if (mBands[i].IsInFlight == false && requestBand(mBands[i]) == false)
        {
            mHasFailed = true;
            return OffscreenReadStatus::Failed;
        }
    }
    return mRowsReceived == height ? OffscreenReadStatus::Complete : OffscreenReadStatus::Pending;
}
QImage OffscreenFrameReader::TakeImage() noexcept
{
    QImage image = mImage;
    mImage = QImage();
    mIsBusy = false;
    return image;
}
void OffscreenFrameReader::Cancel() noexcept
{
    for (Band& band : mBands)
    {
        if (band.IsInFlight == true)
        {
            static_cast<void>(rhi::Release(mDevice, band.Ticket));
        }
        band = {};
    }
    mImage = QImage();
    mIsBusy = false;
    mHasFailed = false;
    mNextRow = 0;
    mRowsReceived = 0;
}
} // namespace ludus::editor
