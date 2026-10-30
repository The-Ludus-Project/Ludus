#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>

#include <ludus/content/render_graph_format/plan.h>

#include <ludus/graphics/rhi/device.h>

#include <ludus/graphics/renderer/renderer.hpp>

#include "internal/texture_inspection.h"

#include <QImage>
#include <QSize>
#include <QString>

namespace ludus::editor
{
class RenderGraphPreview;
struct RenderScene;

/// Presented frames after which a raster capture stops waiting for an accumulating graph to go idle.
inline constexpr foundation::uint32 RASTER_CAPTURE_MAXIMUM_FRAMES = 64;

/// Project-graph selection copied from the World viewport's live producer.
struct ViewportRasterSource final
{
    /// Cooked runtime package the viewport previews.
    QString PackagePath;
    /// Raster-only package used when the package needs unavailable ray tracing; may be empty.
    QString FallbackPath;
    /// Display attachment; empty selects the exported output.
    QString OutputTarget;
    /// Logical resource of OutputTarget in the live generation's catalogue; ABSENT selects the first exported
    /// colour resource, as the viewport does.
    foundation::uint32 OutputResource = content::render_graph_format::ABSENT;
    /// Use raster even when the package and device support hardware ray tracing.
    bool ShouldForceRaster = false;
};

/// Copy the live producer's package, fallback, output and renderer choice.
[[nodiscard]] ViewportRasterSource GetViewportRasterSource(const RenderGraphPreview& liveGraph);

/// Progress of one ViewportRasterCapture.
enum class ViewportRasterPhase : foundation::uint8
{
    Idle,      ///< Nothing started, or released.
    Rendering, ///< The private graph producer is preparing, drawing or copying.
    Complete,  ///< GetImage and GetTexture hold the captured output.
    Failed,    ///< GetError explains why; nothing was captured.
};

/// One offscreen frame of the World viewport's project graph at a chosen extent and frozen camera.
///
/// A private RenderGraphPreview on the viewport's device loads the same package, fallback, output
/// and renderer choice as the live producer, so the image is the current rendering scheme's, not
/// the built-in raster renderer's. It reads the output back to the CPU and never presents.
/// A graph that keeps accumulating (a progressive ray-traced graph) is captured after
/// RASTER_CAPTURE_MAXIMUM_FRAMES frames. Owner (GUI) thread only; Release before the device stops.
class ViewportRasterCapture final
{
public:
    ViewportRasterCapture() noexcept;
    ~ViewportRasterCapture() noexcept;
    ViewportRasterCapture(const ViewportRasterCapture&) = delete;
    ViewportRasterCapture& operator=(const ViewportRasterCapture&) = delete;

    /// Attach a private producer to the viewport's device, load the graph and resolve its output
    /// extent for requestedSize. Any earlier capture is released first. Describe the camera at
    /// outExtent next and pass it to Begin; the frozen camera then belongs to this capture.
    /// @param outExtent Receives the graph's output extent, which differs from requestedSize when
    /// the graph renders at a scaled resolution. Compare the reference at this extent.
    /// @return False with outError when the package cannot be loaded or has no colour extent.
    [[nodiscard]] bool Load(graphics::rhi::DeviceHandle device,
                            graphics::rhi::SurfaceHandle surface,
                            const ViewportRasterSource& source,
                            const QSize& requestedSize,
                            QSize& outExtent,
                            QString& outError);
    /// Submit the scene with a conventional-depth view of the loaded extent and start rendering.
    /// @param view From RenderPreview::DescribeConventionalView (or RenderScene::View with
    /// ShouldUseConventionalDepth) at the extent Load returned.
    [[nodiscard]] bool
    Begin(const RenderScene& scene, const graphics::renderer::ViewDescription& view, QString& outError);
    /// Advance the private producer without waiting; returns the resulting phase.
    ViewportRasterPhase Service() noexcept;
    /// Detach and destroy the private producer, keeping a completed image. Idempotent.
    void Release() noexcept;

    [[nodiscard]] LUDUS_INLINE ViewportRasterPhase GetPhase() const noexcept
    {
        return mPhase;
    }
    [[nodiscard]] LUDUS_INLINE const QString& GetError() const noexcept
    {
        return mError;
    }
    /// Display image of the captured output, as the viewport shows it.
    [[nodiscard]] LUDUS_INLINE const QImage& GetImage() const noexcept
    {
        return mImage;
    }
    /// Raw output texels; RGBA16F outputs keep linear radiance.
    [[nodiscard]] LUDUS_INLINE const InspectedTexture& GetTexture() const noexcept
    {
        return mTexture;
    }
    /// Frames presented for the captured camera; above one only for accumulating graphs.
    [[nodiscard]] LUDUS_INLINE foundation::uint32 GetFrameCount() const noexcept
    {
        return mFrameCount;
    }
    /// True when the graph was still accumulating at RASTER_CAPTURE_MAXIMUM_FRAMES.
    [[nodiscard]] LUDUS_INLINE bool WasStillAccumulating() const noexcept
    {
        return mWasStillAccumulating;
    }

private:
    void fail(const QString& message) noexcept;
    foundation::UniquePtr<RenderGraphPreview> mGraph;
    QImage mImage;
    InspectedTexture mTexture;
    QString mError;
    foundation::uint64 mRevision = 0;
    foundation::uint32 mFrameCount = 0;
    ViewportRasterPhase mPhase = ViewportRasterPhase::Idle;
    bool mWasStillAccumulating = false;
};
} // namespace ludus::editor
