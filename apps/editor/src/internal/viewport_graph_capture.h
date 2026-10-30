#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>

#include <ludus/graphics/rhi/device.h>

#include <ludus/graphics/renderer/render_graph_capture.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include "internal/render_graph_preview.h"
#include "internal/viewport_raster_capture.h"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

namespace ludus::editor
{
/// Samples between the progress read-backs a viewport graph capture shows while it renders.
inline constexpr foundation::uint32 VIEWPORT_GRAPH_CAPTURE_PREVIEW_INTERVAL = 16;

/// Why ViewportGraphCapture::Load would reject the live graph's current output, judged before a capture starts
/// from the active generation's route (RenderGraphPreview::FindActiveCaptureProblem), the selected output and
/// **Force raster**. Package reads, settings and GPU failures are found only by Load and later.
/// @return The explanation Load would give; empty when the output can be captured or while no generation of the
/// requested package is active yet.
[[nodiscard]] QString FindViewportGraphCaptureProblem(const RenderGraphPreview& liveGraph);

/// One World viewport render graph captured to a target sample count at a frozen camera, as linear radiance.
///
/// A RenderGraphCapture on the viewport's device decodes the same cooked package the viewport shows and reads
/// the linear mean of the Accumulate that feeds the selected output. The viewport keeps drawing; its image and
/// accumulation are unchanged. Only accumulating ray outputs qualify: a raster output, a ray output without an
/// Accumulate and **Force raster** are rejected with an explanation. See
/// docs/architecture/render-graph-frame-capture.md. Owner (GUI) thread only; Release before the device stops.
class ViewportGraphCapture final
{
public:
    ViewportGraphCapture() noexcept;
    ~ViewportGraphCapture() noexcept;
    ViewportGraphCapture(const ViewportGraphCapture&) = delete;
    ViewportGraphCapture& operator=(const ViewportGraphCapture&) = delete;

    /// Decode the source's package, resolve the captured route and begin initializing the private graph. Any
    /// earlier capture is released first.
    /// @param captureProfilePath The `.lurgcp` capture profile of the source package's generation
    /// (RenderGraphCaptureProfile), decoded in its place so the capture links the graph's capture budget;
    /// empty captures the source package itself.
    /// @param requestedSize Drawable extent for view-relative resources.
    /// @param samples Target samples per pixel, at most MAX_RENDER_GRAPH_CAPTURE_SAMPLES.
    /// @param outExtent Receives the captured output's extent, which differs from requestedSize when the graph
    /// renders at a scaled resolution. Describe the camera at this extent and pass it to Begin.
    /// @return False with outError when the package cannot be read or its route cannot be captured.
    [[nodiscard]] bool Load(graphics::rhi::DeviceHandle device,
                            graphics::rhi::SurfaceHandle surface,
                            const ViewportRasterSource& source,
                            const QString& captureProfilePath,
                            const QSize& requestedSize,
                            foundation::uint32 samples,
                            QSize& outExtent,
                            QString& outError);
    /// Freeze the scene and camera; they are submitted once the private graph is ready.
    /// @param view From RenderPreview::DescribeConventionalView at the extent Load returned.
    [[nodiscard]] bool
    Begin(const RenderScene& scene, const graphics::renderer::ViewDescription& view, QString& outError);
    /// Advance the capture without waiting; returns the resulting phase.
    graphics::renderer::RenderGraphCapturePhase Service() noexcept;
    /// Submit no further frames and keep the committed samples.
    void Stop() noexcept;
    /// Release the private graph and its pixels. Idempotent.
    void Release() noexcept;

    /// Current phase; Idle before Load or after Release.
    [[nodiscard]] graphics::renderer::RenderGraphCapturePhase GetPhase() const noexcept;
    /// True while the capture prepares, renders or reads back.
    [[nodiscard]] bool IsRunning() const noexcept;
    /// Explanation of a rejection or failure; empty otherwise.
    [[nodiscard]] LUDUS_INLINE const QString& GetError() const noexcept
    {
        return mError;
    }
    /// Progress and provenance of the capture.
    [[nodiscard]] graphics::renderer::RenderGraphCaptureInfo GetInfo() const noexcept;
    /// Linear means and committed counts of the last read-back, top-left rows; empty before one finished.
    [[nodiscard]] foundation::BorrowedView<const graphics::renderer::RenderGraphCapturePixel>
    GetPixels() const noexcept;
    /// Display image of the last read-back, encoded like the viewport (EncodeReferenceDisplayColor).
    [[nodiscard]] QImage MakeDisplayImage() const;
    /// Write the `.lurgcap` capture and, beside it, its display PNG (GetDisplayImagePath).
    [[nodiscard]] bool Save(const QString& capturePath, QString& outError) const;
    /// PNG path written beside a `.lurgcap` path by Save.
    [[nodiscard]] static QString GetDisplayImagePath(const QString& capturePath);
    /// Read the generation identity of the runtime package a capture of source would decode.
    /// @return False with outError when the package cannot be read or decoded; outIdentity is unchanged then.
    [[nodiscard]] static bool
    ReadGenerationIdentity(const ViewportRasterSource& source, QByteArray& outIdentity, QString& outError);
    /// True when the loaded capture decodes a capture profile rather than the runtime package.
    [[nodiscard]] LUDUS_INLINE bool IsCaptureProfile() const noexcept
    {
        return mIsCaptureProfile;
    }

private:
    void fail(const QString& message) noexcept;
    foundation::core::UniquePtr<graphics::renderer::RenderGraphPackage> mPackage;
    foundation::core::UniquePtr<graphics::renderer::RenderGraphPrograms> mProgramsOrNull;
    graphics::renderer::RenderGraphCapture mCapture;
    /// Frozen World converted to graph records; kept until the private graph accepts it.
    foundation::core::UniquePtr<RenderGraphExtraction> mExtractionOrNull;
    QString mError;
    bool mIsCaptureProfile = false;
};
} // namespace ludus::editor
