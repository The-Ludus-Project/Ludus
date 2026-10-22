#pragma once

#include "internal/viewport_frame_source.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/graph.h>

namespace ludus::editor
{
/// Owner-thread GPU snapshots of graph output. Graph attachments may be reused only after
/// PollCopy completes; Qt holds independent images until its submitted sampling retires.
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
    [[nodiscard]] graphics::rhi::RasterStatus Copy(graphics::rhi::SurfaceHandle surface,
                                                   graphics::rhi::TextureHandle source,
                                                   foundation::uint32 width,
                                                   foundation::uint32 height) noexcept;
    [[nodiscard]] graphics::rhi::RasterStatus PollCopy() noexcept;
    /// Commit only after validating the requesting output revision; superseded copies stay private.
    void PublishCopy() noexcept;
    void DiscardCopy() noexcept;
    [[nodiscard]] bool IsCopying() const noexcept;
    [[nodiscard]] bool HasPendingFrame() const noexcept override;
    [[nodiscard]] foundation::uint64 GetLatestSerial() const noexcept;
    /// CPU consumers use the graph's explicit readback fallback instead.
    [[nodiscard]] bool TakeFrame(graphics::renderer::ViewportFrame& outFrame,
                                 graphics::renderer::ViewportImage& outImage) noexcept override;
    void ReleaseFrame(graphics::renderer::ViewportFrame& inoutFrame) noexcept override;
    void Resize(foundation::int32 width, foundation::int32 height) override;
    [[nodiscard]] graphics::rhi::RasterStatus TakeFrameRead(graphics::rhi::NativeDeviceLease device,
                                                            const graphics::rhi::NativeDeviceIdentity& consumer,
                                                            ViewportFrameRead& outRead) noexcept override;
    [[nodiscard]] graphics::rhi::RasterStatus EndFrameRead(ViewportFrameRead& inoutRead) noexcept override;
    void DrainFrameRead(ViewportFrameRead& inoutRead) noexcept override;
Q_SIGNALS:
    /// A returned display slot can admit the next graph output without a service timer gap.
    void CapacityAvailable();

private:
    static constexpr foundation::usize DISPLAY_SLOT_COUNT = 3;
    struct DisplaySlot final
    {
        graphics::rhi::TextureHandle Texture{};
        graphics::rhi::SubmissionToken Producer{};
        foundation::uint32 Width = 0;
        foundation::uint32 Height = 0;
        foundation::uint32 Readers = 0;
        foundation::uint64 Serial = 0;
    };
    [[nodiscard]] graphics::rhi::RasterStatus prepareCopy() noexcept;
    void releaseCopyBinding() noexcept;
    [[nodiscard]] graphics::rhi::RasterStatus buildCopyGraph(graphics::rhi::TextureHandle source,
                                                             const DisplaySlot& destination,
                                                             const graphics::rhi::RasterDraw& draw) noexcept;
    void releaseRead(ViewportFrameRead& inoutRead) noexcept;
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
};
} // namespace ludus::editor
