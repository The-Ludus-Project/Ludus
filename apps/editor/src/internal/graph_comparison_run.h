#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>

#include "internal/render_graph_library_capture.h"
#include "internal/render_scene.h"
#include "internal/viewport_graph_capture.h"
#include "internal/viewport_reference_capture.h"

#include <QJsonArray>
#include <QPointer>

namespace ludus::editor
{
class RendererWorkspace;
class RenderPreview;

/// Maximum rows authored by Compare graphs and admitted by its saved run viewer.
inline constexpr foundation::int32 MAXIMUM_GRAPH_COMPARISON_CANDIDATES = 16;

/// One candidate route and its requested budget. Zero expected bounces means no target was specified.
struct GraphComparisonCandidate final
{
    QString Label;
    RenderGraphCaptureTarget Target{};
    foundation::uint32 Samples = 0;
    foundation::uint32 ExpectedBounces = 0;
    bool HasSavedTarget = false;
    bool IsDisplayFrame = false;
};

/// Serial capture of one baseline and several candidates at one frozen scene, view and output extent.
/// Owns one candidate producer at a time; completed evidence is written to an owned results directory.
class GraphComparisonRun final
{
public:
    [[nodiscard]] bool Start(RenderPreview& inoutPreview,
                             RendererWorkspace& inoutWorkspace,
                             const ViewportReferenceSettings& settings,
                             const QVector<GraphComparisonCandidate>& candidates,
                             const QImage& displayBaseline,
                             const QString& baselineSource,
                             const QString& directory,
                             QString& outError);
    void Service();
    /// Device is still alive. Releases all captures and leaves completed files inspectable.
    void Stop();
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] const QString& GetStatus() const noexcept;
    [[nodiscard]] const QString& GetDirectory() const noexcept;
    [[nodiscard]] const QJsonArray& GetResults() const noexcept;

private:
    enum class Phase : foundation::uint8
    {
        Idle,
        Reference,
        NextCandidate,
        PreparingCandidate,
        Graph,
        DisplayFrame,
    };

    void finishReference();
    void prepareCandidate();
    void startCandidate(const ViewportRasterSource& source, const QString& profile);
    void finishCandidate(const QString& state, const QString& message);
    void finishRun(const QString& state, const QString& message);
    void writeManifest(const QString& state);

    QPointer<RendererWorkspace> mWorkspace;
    graphics::rhi::DeviceHandle mDevice{};
    graphics::rhi::SurfaceHandle mSurface{};
    RenderScene mScene;
    graphics::renderer::ViewDescription mView{};
    QSize mExtent;
    QByteArray mGenerationIdentity;
    QByteArray mViewportPackageIdentity;
    QByteArray mViewportFallbackIdentity;
    QString mViewportConfiguration;
    ViewportRasterSource mViewportSource;
    ViewportReferenceSettings mSettings;
    QVector<GraphComparisonCandidate> mCandidates;
    QString mBaselineSource;
    QImage mDisplayBaseline;
    foundation::Array<graphics::renderer::ReferencePixel> mReferencePixels;
    graphics::renderer::ReferenceCaptureInfo mReferenceInfo{};
    ViewportReferenceCapture mReference;
    ViewportGraphCapture mGraph;
    ViewportRasterCapture mRaster;
    QJsonArray mResults;
    QString mDirectory;
    QString mStatus;
    QString mBaselineState;
    QString mBaselineError;
    qsizetype mCandidateIndex = 0;
    Phase mPhase = Phase::Idle;
    bool mHasRequestedCandidate = false;
};
} // namespace ludus::editor
