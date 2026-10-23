#include "internal/render_graph_resolution_settings.h"
#include "internal/renderer_workspace.h"

namespace ludus::editor
{
bool RendererWorkspace::CanEditResolutionSettings() const
{
    return mDocument.IsLoaded() == true && IsJobActive() == false && mSourceProblem.isEmpty() == true;
}

bool RendererWorkspace::SetResolutionSettings(const RenderGraphResolutionSelection& selection,
                                              const QJsonValue& extent,
                                              const QJsonArray& scale,
                                              QString& outError)
{
    if (CanEditResolutionSettings() == false)
    {
        outError =
            QStringLiteral("Finish the renderer job or resolve external source changes before editing resolution.");
        return false;
    }
    if (WriteRenderGraphResolutionSettings(mDocument, selection, extent, scale, outError) == false)
    {
        return false;
    }
    documentEdited(RenderGraphDocumentStatus::Ok);
    return true;
}

bool RendererWorkspace::EnableResolutionSettings(QString& outError)
{
    if (CanEditResolutionSettings() == false)
    {
        outError =
            QStringLiteral("Finish the renderer job or resolve external source changes before editing resolution.");
        return false;
    }
    if (EnableRenderGraphResolutionSettings(mDocument, outError) == false)
    {
        return false;
    }
    documentEdited(RenderGraphDocumentStatus::Ok);
    return true;
}
} // namespace ludus::editor
