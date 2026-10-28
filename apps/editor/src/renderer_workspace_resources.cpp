#include "internal/render_graph_history.h"
#include "internal/render_graph_resource_inspection.h"
#include "internal/renderer_workspace.h"

#include <QTabWidget>

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/render_graph_preview.h"
#    include "internal/render_graph_targets.h"
#    include "internal/texture_viewer.h"
#endif

namespace ludus::editor
{
using namespace foundation;
void RendererWorkspace::setupResourceInspection()
{
    mResourceInspectorOrNull = new RenderGraphResourceInspector(mDocument, *this);
    const auto makeRequest = [this](const QString& endpoint, bool isOutput) {
        return RenderGraphResourceRequest{mCanvas->GetFile(),
                                          mCanvas->GetConfiguration(),
                                          endpoint,
                                          isOutput,
                                          mCanvas->GetHistory().GetPath()};
    };
    connect(mCanvas,
            &RenderGraphCanvas::ResourceInspectionRequested,
            this,
            [this, makeRequest](const QString& endpoint, bool isOutput) {
                mResourceInspectorOrNull->Inspect(makeRequest(endpoint, isOutput));
            });
    connect(mCanvas,
            &RenderGraphCanvas::ResourceHovered,
            this,
            [this, makeRequest](const QString& endpoint, bool isOutput) {
                mResourceInspectorOrNull->Follow(makeRequest(endpoint, isOutput));
            });
    connect(mCanvas,
            &RenderGraphCanvas::ReferenceSizeChanged,
            mResourceInspectorOrNull,
            &RenderGraphResourceInspector::SetReferenceSize);
    connect(mResourceInspectorOrNull,
            &RenderGraphResourceInspector::PreviewRequested,
            this,
            &RendererWorkspace::updateResourceInspection);
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    connect(mResourceInspectorOrNull,
            &RenderGraphResourceInspector::OpenInTexturesRequested,
            this,
            [this](const QString& name, uint64 generation) {
                if (mPreviewOrNull != nullptr && mTextureViewerOrNull != nullptr &&
                    mPreviewOrNull->IsAttached() == true && mPreviewOrNull->HasPresentedCatalogue() == true &&
                    mPreviewOrNull->GetReport().ActiveGeneration == generation &&
                    mPreviewOrNull->GetPresentedGeneration() == generation &&
                    mPreviewOrNull->GetTargetCatalogue().HasExactRuntimeNames == true)
                {
                    const RenderGraphTargetCatalogue& catalogue = mPreviewOrNull->GetTargetCatalogue();
                    const qsizetype index = catalogue.Find(name);
                    if (index >= 0 && catalogue.Targets.at(index).IsBuffer == false &&
                        catalogue.Targets.at(index).IsSurface == false)
                    {
                        mTextureViewerOrNull->SelectTarget(name);
                        mViews->setCurrentWidget(mTextureViewerOrNull);
                    }
                }
            });
#endif
}
void RendererWorkspace::updateResourceInspection()
{
    if (mResourceInspectorOrNull == nullptr || mResourceInspectorOrNull->isVisible() == false)
    {
        return;
    }
    RenderGraphResourcePreviewSnapshot snapshot;
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mPreviewOrNull != nullptr)
    {
        snapshot.IsAttached = mPreviewOrNull->IsAttached();
        snapshot.ActiveGeneration = mPreviewOrNull->GetReport().ActiveGeneration;
        snapshot.PresentedGeneration = mPreviewOrNull->GetPresentedGeneration();
        snapshot.InputIdentity = mPreviewOrNull->GetActiveSource().InputIdentity;
        snapshot.Configuration = mPreviewOrNull->GetActiveSource().Configuration;
        snapshot.RootFile = mPreviewOrNull->GetActiveSource().RootFile;
        const RenderGraphTargetCatalogue& catalogue = mPreviewOrNull->GetTargetCatalogue();
        snapshot.HasNames = catalogue.HasExactRuntimeNames;
        if (catalogue.HasNames == true && catalogue.HasExactRuntimeNames == false)
        {
            snapshot.Problem =
                QStringLiteral("Build & Preview to bind resource names to the exact runtime configuration.");
        }
        snapshot.HasPresentedCatalogue = mPreviewOrNull->HasPresentedCatalogue();
        snapshot.Problem += QLatin1Char('\n') + mPreviewOrNull->GetDisplayStatus();
        if (catalogue.NamesProblem.isEmpty() == false)
        {
            snapshot.Problem += QLatin1Char('\n') + catalogue.NamesProblem;
        }
        for (const RenderGraphTarget& target : catalogue.Targets)
        {
            const QString description =
                target.IsBuffer == true ? QStringLiteral("Buffer · no texture image") : DescribeTarget(target);
            snapshot.Targets.append({target.Name, description, target.IsBuffer == false && target.IsSurface == false});
        }
    }
#endif
    mResourceInspectorOrNull->SetPreviewSnapshot(snapshot);
}
} // namespace ludus::editor
