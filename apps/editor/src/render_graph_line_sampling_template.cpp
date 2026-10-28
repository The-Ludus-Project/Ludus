// Import the renderer-owned line-sampling template (point-line design D8a) into a draft.
#include "internal/render_graph_configuration_template.h"

namespace ludus::editor
{
// File and configuration identify distinct authored records.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus PrepareLineSamplingConfiguration(
    RenderGraphSnapshot& inoutSnapshot,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): root path, library path and configuration identity
    const QString& rootGraph,
    const QString& file,
    const QString& configuration,
    QString& outError,
    RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    const SceneComputeTemplate selected{
        QStringLiteral("line-sampling"),
        QStringLiteral("LineSampling"),
        QStringLiteral("Line sampling"),
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
