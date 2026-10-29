// Import the renderer-owned salesin-jarosz-lines template (point-line design D8b) into a draft.
#include "internal/render_graph_configuration_template.h"

namespace ludus::editor
{
// File and configuration identify distinct authored records.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus PreparePointLineConfiguration(
    RenderGraphSnapshot& inoutSnapshot,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): root path, library path and configuration identity
    const QString& rootGraph,
    const QString& file,
    const QString& configuration,
    QString& outError,
    RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    const SceneComputeTemplate selected{
        QStringLiteral("salesin-jarosz-lines"),
        QStringLiteral("PointLine"),
        QStringLiteral("Salesin and Jarosz point and line MIS"),
    };
    return PrepareSceneComputeTemplateConfiguration(inoutSnapshot,
                                                    rootGraph,
                                                    file,
                                                    configuration,
                                                    outError,
                                                    selected,
                                                    inoutBudgetOrNull);
}
} // namespace ludus::editor
