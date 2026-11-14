#pragma once

#include "internal/selection_outline_request.h"
#include "internal/viewport_frame_source.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/graphics/renderer/selection_outline.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <QSize>

namespace ludus::editor
{
/// Owner-thread GPU snapshots of graph output. Graph attachments may be reused only after
/// PollCopy completes; Qt holds independent images until its submitted sampling retires.
///
/// While a selection outline is requested, each snapshot is also composed with the outline on the GPU: the clean
/// snapshot stays in its slot and the shown image is a second slot holding the snapshot with the outline drawn over
/// it. Moving or changing the outline recomposes from the clean snapshot without a new graph frame.
/// The admitted device outlives Reset. Reset drains consumers through ResettingFrames.
class GraphDisplayFrames final : public ViewportFrameSource
{
    Q_OBJECT
public:
    explicit GraphDisplayFrames(QObject* parentOrNull = nullptr);
    ~GraphDisplayFrames() override;
    void Initialize(graphics::rhi::DeviceHandle device,
                    const graphics::rhi::RasterShaderDescription& vertex,
                    const graphics::rhi::RasterShaderDescription& fragment) noexcept;
    void Reset() noexcept;
    /// Copy an encoded RGBA8 attachment into a free display slot without CPU transfer.
    /// Pending retains source resources; retry until Ready or failure before reusing the graph.
    /// NotReady applies bounded consumer backpressure and preserves the last published image.
    /// @param worldToClipOrNull World to clip matrix of the camera that rendered source, which places the selection
    /// outline on the snapshot; null when the source has no camera, and the snapshot is then never outlined.
    [[nodiscard]] graphics::rhi::RasterStatus
    Copy(graphics::rhi::SurfaceHandle surface,
         graphics::rhi::TextureHandle source,
         foundation::uint32 width,
         foundation::uint32 height,
         const foundation::math::Matrix4* worldToClipOrNull = nullptr) noexcept;
    [[nodiscard]] graphics::rhi::RasterStatus PollCopy() noexcept;
    /// Commit only after validating the requesting output revision; superseded copies stay private.
    void PublishCopy() noexcept;
    void DiscardCopy() noexcept;
    [[nodiscard]] bool IsCopying() const noexcept;
    /// Whether the GPU can draw the selection outline: the shaders were accepted and no outline draw has failed since
    /// Initialize. A caller that needs an outline falls back to drawing it itself while this is false.
    [[nodiscard]] bool CanDrawSelectionOutline() const noexcept;
    /// Outline one mesh over the shown image from now on, replacing any earlier request. The next PollSelectionOutline
    /// recomposes the shown image; a snapshot copied later is composed the same way.
    /// @return False when the GPU is not drawing this outline: the outline is unavailable, the mesh has no usable
    /// triangle, or the RHI refused the mesh (which also makes CanDrawSelectionOutline false). The caller then draws
    /// the outline itself.
    [[nodiscard]] bool SetSelectionOutline(const SelectionOutlineRequest& request) noexcept;
    /// Stop outlining; the clean snapshot is shown again by the next PollSelectionOutline.
    void ClearSelectionOutline() noexcept;
    /// Advance recomposition of the shown image after a request change, between graph copies. Ready when nothing is
    /// outstanding, Pending while composition is in flight, NotReady while a graph copy or free capacity is awaited.
    [[nodiscard]] graphics::rhi::RasterStatus PollSelectionOutline() noexcept;
    /// Schedule a readback of the shown image's texels, with any outline drawn into it, for inspection and tests.
    /// Pending once accepted: poll rhi::GetStatus on the ticket, copy with rhi::CopyReadback (rows are
    /// GetShownSize().width() * 4 bytes), and release the ticket. NotReady when no image is shown.
    [[nodiscard]] graphics::rhi::RasterStatus RequestShownReadback(graphics::rhi::ReadbackTicket& outTicket) noexcept;
    /// Pixel size of the shown image, or an empty size when none is shown.
    [[nodiscard]] QSize GetShownSize() const noexcept;
    /// Whether PollSelectionOutline has work: a request the shown image does not yet reflect, or composition in flight.
    [[nodiscard]] bool HasPendingSelectionOutline() const noexcept;
    [[nodiscard]] bool HasPendingFrame() const noexcept override;
    [[nodiscard]] foundation::uint64 GetLatestSerial() const noexcept;
    /// CPU consumers use the graph's explicit readback fallback instead.
    [[nodiscard]] bool TakeFrame(graphics::renderer::ViewportFrame& outFrame,
                                 graphics::renderer::ViewportImage& outImage) noexcept override;
    void ReleaseFrame(graphics::renderer::ViewportFrame& inoutFrame) noexcept override;
    void Resize(foundation::int32 width, foundation::int32 height, foundation::float32 pixelRatio) override;
    [[nodiscard]] graphics::rhi::RasterStatus TakeFrameRead(graphics::rhi::NativeDeviceLease device,
                                                            const graphics::rhi::NativeDeviceIdentity& consumer,
                                                            ViewportFrameRead& outRead) noexcept override;
    [[nodiscard]] graphics::rhi::RasterStatus EndFrameRead(ViewportFrameRead& inoutRead) noexcept override;
    void DrainFrameRead(ViewportFrameRead& inoutRead) noexcept override;
Q_SIGNALS:
    /// A returned display slot can admit the next graph output without a service timer gap.
    void CapacityAvailable();
    /// An outline request changed or availability changed; the owner should poll soon.
    void SelectionOutlineWorkRequested();
    /// CanDrawSelectionOutline changed.
    void SelectionOutlineAvailabilityChanged();

private:
    /// Images in flight without an outline; the consumer's backpressure applies at this many.
    static constexpr foundation::usize PLAIN_SLOT_COUNT = 3;
    /// With an outline, the clean snapshot it is composed from and one spare join the images in flight.
    static constexpr foundation::usize DISPLAY_SLOT_COUNT = PLAIN_SLOT_COUNT + 2;
    struct DisplaySlot final
    {
        graphics::rhi::TextureHandle Texture{};
        graphics::rhi::SubmissionToken Producer{};
        foundation::uint32 Width = 0;
        foundation::uint32 Height = 0;
        foundation::uint32 Readers = 0;
        foundation::uint64 Serial = 0;
        /// World to clip of the camera that rendered the image; meaningful when HasView is true.
        foundation::math::Matrix4 WorldToClip{};
        bool HasView = false;
    };
    [[nodiscard]] graphics::rhi::RasterStatus prepareCopy() noexcept;
    void releaseCopyBinding() noexcept;
    [[nodiscard]] graphics::rhi::RasterStatus buildCopyGraph(graphics::rhi::TextureHandle source,
                                                             const DisplaySlot& destination,
                                                             const graphics::rhi::RasterDraw& draw) noexcept;
    void releaseRead(ViewportFrameRead& inoutRead) noexcept;
    /// Size slot's texture to width by height, creating it when it is missing or another size.
    [[nodiscard]] graphics::rhi::RasterStatus
    prepareSlotTexture(DisplaySlot& inoutSlot, foundation::uint32 width, foundation::uint32 height) noexcept;
    /// Index of a slot that is neither shown, read, copied, composed nor the clean snapshot, or -1.
    [[nodiscard]] foundation::int32 findFreeSlot() const noexcept;
    /// Slots a new copy or composition may use: all of them only while an outline is wanted or shown.
    [[nodiscard]] foundation::usize getSlotLimit() const noexcept;
    /// Submit one frame that copies the clean slot into the target slot and draws the outline over it. Pending once
    /// submitted; NotReady to retry; InvalidDescription when nothing can be drawn; another failure disables the
    /// outline.
    [[nodiscard]] graphics::rhi::RasterStatus beginCompose(foundation::int32 cleanIndex,
                                                           foundation::int32 targetIndex) noexcept;
    /// Completion of the submitted composition: Pending, Ready, or Failed.
    [[nodiscard]] graphics::rhi::RasterStatus pollCompose() noexcept;
    /// Compose a copy that just completed, when an outline is wanted. Ready means PublishCopy may run now.
    [[nodiscard]] graphics::rhi::RasterStatus startCopyCompose() noexcept;
    void disableSelectionOutline() noexcept;
    foundation::StaticArray<DisplaySlot, DISPLAY_SLOT_COUNT> mSlots;
    graphics::rhi::DeviceHandle mDevice{};
    graphics::rhi::RasterShaderDescription mVertexDescription{};
    graphics::rhi::RasterShaderDescription mFragmentDescription{};
    graphics::rhi::RasterShaderHandle mVertex{};
    graphics::rhi::RasterShaderHandle mFragment{};
    graphics::rhi::BindingLayoutHandle mLayout{};
    graphics::rhi::RasterPipelineHandle mPipeline{};
    graphics::rhi::BufferHandle mVertices{};
    graphics::rhi::BufferHandle mIndices{};
    graphics::rhi::TextureViewHandle mSourceView{};
    graphics::rhi::SamplerHandle mSampler{};
    graphics::rhi::BindingSetHandle mBinding{};
    foundation::int32 mLatest = -1;
    foundation::int32 mCopying = -1;
    foundation::int32 mCompleted = -1;
    foundation::uint64 mSerial = 0;
    bool mHasDevice = false;
    bool mIsPrepared = false;
    bool mHasPreparationFailed = false;
    graphics::rhi::OrderedGraph mCopyGraph{};
    graphics::rhi::SurfaceHandle mSurface{};
    bool mIsSubmitted = false;
    graphics::renderer::SelectionOutlineRenderer mOutline;
    bool mIsOutlineInitialized = false;
    bool mIsOutlineDisabled = false;
    bool mIsOutlineWanted = false;
    /// Scene and mesh whose geometry mOutline holds, to upload again only when they change.
    QByteArray mOutlineSceneIdentity;
    foundation::int32 mOutlineMesh = -1;
    foundation::math::Affine3 mOutlineMeshToWorld{};
    graphics::renderer::SelectionOutlineStyle mOutlineStyle{};
    foundation::float32 mOutlineStrokeScale = 1;
    foundation::float32 mOutlineLogicalOutlineWidth = 2;
    foundation::float32 mOutlineLogicalCasingWidth = mOutlineLogicalOutlineWidth * 2;
    /// Counts every change of what should be drawn; mComposedRevision is the count the shown image reflects.
    foundation::int32 mRejectedMesh = -1;
    QByteArray mRejectedSceneIdentity;
    foundation::uint64 mOutlineRevision = 0;
    foundation::uint64 mComposedRevision = 0;
    /// Slot holding the clean snapshot of the shown image; it is also the shown slot when no outline is drawn.
    foundation::int32 mClean = -1;
    /// Slot being composed into, or -1.
    foundation::int32 mComposing = -1;
    foundation::uint64 mComposingRevision = 0;
    /// Whether the composition in flight belongs to a graph copy (published by PublishCopy) or to a recomposition.
    bool mIsComposingCopy = false;
    /// Composed slot of a finished graph copy, or -1 when the copy is published clean.
    foundation::int32 mCompletedDisplay = -1;
};
} // namespace ludus::editor
