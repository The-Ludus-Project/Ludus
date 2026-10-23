#include "internal/viewport_raster_capture.h"

#include "internal/render_graph_preview.h"
#include "internal/render_scene.h"

#include <ludus/foundation/base/core.h>

#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include "editor_default_graph.h"
#include "renderer_composite.h"

#include <QObject>

#include <new>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace renderer = graphics::renderer;
namespace default_graph = render_graph_packages::editor_default_graph;

bool HasFailedPermanently(const RenderGraphPreview& graph) noexcept
{
    const renderer::RenderGraphActivationReport report = graph.GetReport();
    return report.ActiveGeneration == 0 && (report.Candidate == renderer::RenderGraphCandidateState::Failed ||
                                            report.Candidate == renderer::RenderGraphCandidateState::RestartRequired);
}
} // namespace

ViewportRasterSource GetViewportRasterSource(const RenderGraphPreview& liveGraph)
{
    ViewportRasterSource source;
    source.PackagePath = liveGraph.GetRequestedPackagePath();
    source.FallbackPath = liveGraph.GetRasterFallbackPath();
    source.OutputTarget = liveGraph.GetOutputTarget();
    source.ShouldForceRaster = liveGraph.ShouldForceRaster();
    return source;
}

ViewportRasterCapture::ViewportRasterCapture() noexcept = default;
ViewportRasterCapture::~ViewportRasterCapture() noexcept
{
    Release();
}

void ViewportRasterCapture::fail(const QString& message) noexcept
{
    mError = message;
    mPhase = ViewportRasterPhase::Failed;
    if (mGraph.Get() != nullptr)
    {
        mGraph->Detach();
        mGraph.Reset();
    }
}

bool ViewportRasterCapture::Load(graphics::rhi::DeviceHandle device,
                                 graphics::rhi::SurfaceHandle surface,
                                 const ViewportRasterSource& source,
                                 const QSize& requestedSize,
                                 QSize& outExtent,
                                 QString& outError)
{
    Release();
    mPhase = ViewportRasterPhase::Idle;
    mImage = {};
    mTexture = {};
    mError.clear();
    mFrameCount = 0;
    mWasStillAccumulating = false;
    if (source.PackagePath.isEmpty() == true || requestedSize.isEmpty() == true)
    {
        outError = QStringLiteral("The viewport has no project graph output to compare with yet.");
        return false;
    }
    mGraph.Reset(new (std::nothrow) RenderGraphPreview(
        {default_graph::INTERFACE_IDENTITY.GetData(), default_graph::INTERFACE_IDENTITY.GetSize()},
        shaders::renderer_composite::Vertex(),
        shaders::renderer_composite::Fragment()));
    if (mGraph.Get() == nullptr)
    {
        outError = QStringLiteral("Raster capture allocation failed.");
        return false;
    }
    RenderGraphPreview& graph = *mGraph;
    QObject::connect(&graph, &RenderGraphPreview::FramePresented, &graph, [this]() {
        if (mGraph.Get() != nullptr && mGraph->GetPresentedRevision() == mRevision)
        {
            ++mFrameCount;
        }
    });
    graph.SetForceRaster(source.ShouldForceRaster);
    graph.SetOutputTarget(source.OutputTarget);
    if (graph.Attach(device, surface) == false)
    {
        mGraph.Reset();
        outError = QStringLiteral("The raster capture could not attach to the viewport device.");
        return false;
    }
    const RenderGraphPreviewLoad loaded = graph.Load(source.PackagePath, source.FallbackPath);
    if (loaded != RenderGraphPreviewLoad::Staged && loaded != RenderGraphPreviewLoad::Waiting)
    {
        outError = QStringLiteral("The viewport's render graph could not be loaded for capture: %1")
                       .arg(graph.GetDisplayStatus());
        Release();
        return false;
    }
    graph.SetReferenceSize(requestedSize);
    const QSize extent = graph.GetTargetSize();
    if (extent.isEmpty() == true)
    {
        outError = QStringLiteral("The viewport's render graph has no colour output extent to capture.");
        Release();
        return false;
    }
    outExtent = extent;
    return true;
}

bool ViewportRasterCapture::Begin(const RenderScene& scene, const renderer::ViewDescription& view, QString& outError)
{
    if (mGraph.Get() == nullptr || mPhase != ViewportRasterPhase::Idle)
    {
        outError = QStringLiteral("Load the viewport's render graph before beginning the raster capture.");
        return false;
    }
    // A fixed nonzero revision identifies this capture's single frozen extraction.
    mRevision = 1;
    RenderGraphExtraction extraction;
    QString error;
    if (MakeRenderGraphExtraction(scene, view, mRevision, extraction, error) == false)
    {
        outError = QStringLiteral("The scene cannot be prepared for the raster capture: %1").arg(error);
        Release();
        return false;
    }
    extraction.ContentRevision = mRevision;
    mGraph->SetExtraction(extraction);
    mPhase = ViewportRasterPhase::Rendering;
    return true;
}

ViewportRasterPhase ViewportRasterCapture::Service() noexcept
{
    if (mPhase != ViewportRasterPhase::Rendering || mGraph.Get() == nullptr)
    {
        return mPhase;
    }
    RenderGraphPreview& graph = *mGraph;
    graph.Tick();
    if (HasFailedPermanently(graph) == true)
    {
        fail(QStringLiteral("The viewport's render graph failed to prepare for capture: %1")
                 .arg(graph.GetDisplayStatus()));
        return mPhase;
    }
    if (graph.GetPresentedRevision() != mRevision || graph.GetOutputTexture().IsValid() == false)
    {
        return mPhase;
    }
    const bool isIdle = graph.HasPendingWork() == false;
    if (isIdle == true || mFrameCount >= RASTER_CAPTURE_MAXIMUM_FRAMES)
    {
        if (graph.GetOutputTexture().Encoding == TexelEncoding::Depth32Float)
        {
            fail(QStringLiteral("The viewport shows a depth output; choose a colour output to compare."));
            return mPhase;
        }
        mWasStillAccumulating = isIdle == false;
        mImage = graph.GetImage();
        mTexture = graph.GetOutputTexture();
        mPhase = ViewportRasterPhase::Complete;
        graph.Detach();
        mGraph.Reset();
    }
    return mPhase;
}

void ViewportRasterCapture::Release() noexcept
{
    if (mGraph.Get() != nullptr)
    {
        mGraph->Detach();
        mGraph.Reset();
    }
    if (mPhase == ViewportRasterPhase::Rendering)
    {
        mPhase = ViewportRasterPhase::Idle;
    }
}
} // namespace ludus::editor
