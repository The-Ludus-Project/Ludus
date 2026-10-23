#pragma once

#include <ludus/foundation/base/types.h>

#include <QHash>
#include <QString>

namespace ludus::editor
{
class RenderGraphDocument;
/// Conservative draft dependency state; this is not execution/profile validation.
enum class RenderGraphActivity : foundation::uint8
{
    Retained, ///< Reaches an exposed output, explicit effect or active-camera obligation.
    Inactive, ///< No such route in the displayed configuration.
    Disabled, ///< Authored `"enabled": false`; the compiler forwards its input to its consumers.
    Unknown,  ///< Broken structure, unresolved imports or an analysis bound prevents classification.
};
/// Read-only canvas annotation. Explanations state the current configuration's scope.
struct RenderGraphActivityFeedback final
{
    RenderGraphActivity Activity = RenderGraphActivity::Unknown;
    QString Label;
    QString Explanation;
};
/// Inspect the displayed configuration's draft dependencies without changing document/history.
/// Matches the compiler's observable roots and conservative retained-subgraph input contract;
/// all exposed outputs are assumed requested when inspecting a library on its own. Structural
/// problems and bounded/cyclic imports produce Unknown, never a false inactive claim.
/// GUI thread only; no shader compilation, GPU work or file writes.
[[nodiscard]] QHash<QString, RenderGraphActivityFeedback>
InspectRenderGraphActivity(const RenderGraphDocument& document, const QString& file, const QString& configuration);
} // namespace ludus::editor
