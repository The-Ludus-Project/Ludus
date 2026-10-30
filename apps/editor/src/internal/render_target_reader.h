#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

#include "internal/offscreen_frame_reader.h"
#include "internal/texture_inspection.h"

namespace ludus::editor
{
/// Result of advancing an asynchronous render-target read.
enum class RenderTargetReadStatus : foundation::uint8
{
    Idle,      ///< No read is in progress.
    Pending,   ///< GPU work or band copies are in flight; poll again on a later tick.
    Complete,  ///< TakeTexture publishes the read subresource.
    Undefined, ///< The subresource holds no stored contents (never written, or its last pass discarded it).
    Failed,    ///< The RHI rejected a request or a copy failed.
};

/// Reads any render-graph colour or depth attachment back to the CPU without waiting for the GPU.
///
/// Colour subresources (RGBA8Unorm) use OffscreenFrameReader, optionally from an owned GPU
/// snapshot. RHI readback is RGBA8-only, so a Depth32Float subresource is first drawn by an editor-owned shader that
/// packs each texel's IEEE bits into the bytes of an RGBA8 attachment, which is then copied the same way and
/// reassembled bit-exactly. Every step is a nonblocking request polled on later ticks, so the GUI thread never waits on
/// a fence. Owner-thread use only, between frames: Begin may encode one direct frame on the borrowed surface, and the
/// source texture must not be written or released until IsSourcePending returns false. Snapshot reads then use owned
/// storage.
class RenderTargetReader final
{
public:
    RenderTargetReader() = default;
    ~RenderTargetReader();
    RenderTargetReader(const RenderTargetReader&) = delete;
    RenderTargetReader& operator=(const RenderTargetReader&) = delete;

    /// Start reading mip 0 of an RGBA8Unorm colour or Depth32Float depth aspect.
    /// @param surface Headless surface of the device; used only to encode the depth packing frame.
    /// @param shouldSnapshotSource Copy colour into owned GPU storage so band reads allow source reuse.
    /// False preserves the output reader's existing direct readback path. Depth always packs into owned storage.
    /// @pre Every earlier read used the same device, or Release ran since.
    /// @return Pending when started, Undefined when the aspect holds no stored contents (nothing
    /// is started) or Failed when the device rejected setup. A read already in progress is cancelled.
    [[nodiscard]] RenderTargetReadStatus Begin(graphics::rhi::DeviceHandle device,
                                               graphics::rhi::SurfaceHandle surface,
                                               graphics::rhi::TextureHandle texture,
                                               graphics::rhi::TextureAspect aspect,
                                               foundation::uint32 width,
                                               foundation::uint32 height,
                                               bool shouldSnapshotSource = false);
    /// True while the source must remain unchanged: through snapshot capture, or the entire direct read.
    [[nodiscard]] LUDUS_INLINE bool IsSourcePending() const noexcept
    {
        return mIsBusy == true && (mIsSnapshotPending == true || mIsBorrowingSource == true);
    }
    /// Collect finished copies without waiting. False defers new band requests while preserving
    /// the unfinished snapshot, yielding shared staging capacity to the viewport output reader.
    [[nodiscard]] RenderTargetReadStatus Poll(bool shouldRequestBands = true) noexcept;
    /// Move the completed subresource out and return to Idle. Valid only after Poll returned Complete.
    [[nodiscard]] InspectedTexture TakeTexture() noexcept;
    /// Drop the read in progress; the RHI retires in-flight staging after GPU completion.
    void Cancel() noexcept;
    /// Cancel and release every device object (packing pipeline and snapshot). Call before the
    /// device is destroyed or a different device is used; the next depth read recreates them.
    void Release() noexcept;
    /// True while a read started by Begin has not been taken, failed or cancelled.
    [[nodiscard]] bool IsBusy() const noexcept
    {
        return mIsBusy;
    }
    /// Polls that returned Pending for the read in progress or last completed; tests use it
    /// to prove a read spans ticks instead of blocking.
    [[nodiscard]] foundation::uint32 GetPendingPolls() const noexcept
    {
        return mPendingPolls;
    }

private:
    [[nodiscard]] bool preparePacking(foundation::uint32 width, foundation::uint32 height) noexcept;
    [[nodiscard]] bool encodePacking(graphics::rhi::SurfaceHandle surface,
                                     graphics::rhi::TextureHandle texture) noexcept;
    graphics::rhi::DeviceHandle mDevice{};
    OffscreenFrameReader mReader;
    graphics::rhi::RasterShaderHandle mVertex{};
    graphics::rhi::RasterShaderHandle mFragment{};
    graphics::rhi::BindingLayoutHandle mLayout{};
    graphics::rhi::RasterPipelineHandle mPipeline{};
    graphics::rhi::BufferHandle mVertices{};
    graphics::rhi::BufferHandle mIndices{};
    graphics::rhi::TextureHandle mSnapshot{};
    graphics::rhi::TransferTicket mSnapshotCopy{};
    graphics::rhi::SubmissionToken mPackingProducer{};
    bool mIsSnapshotPending = false;
    bool mIsBorrowingSource = false;
    foundation::uint32 mSnapshotWidth = 0;
    foundation::uint32 mSnapshotHeight = 0;
    TexelEncoding mEncoding = TexelEncoding::Rgba8Unorm;
    foundation::uint32 mWidth = 0;
    foundation::uint32 mHeight = 0;
    foundation::uint32 mPendingPolls = 0;
    InspectedTexture mTexture;
    bool mHasDevice = false;
    bool mIsPackingReady = false;
    bool mIsBusy = false;
    bool mIsComplete = false;
};
} // namespace ludus::editor
