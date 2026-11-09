#include "internal/renderer_workspace.h"

#include "internal/render_graph_availability.h"
#include "internal/render_graph_canvas.h"

#include <QAction>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>

namespace ludus::editor
{
void RendererWorkspace::addAvailabilityActions(QMenu& inoutMenu, const QJsonObject& node)
{
    const RenderGraphAvailabilityFeedback feedback =
        mCanvas->GetNodeAvailability(node.value(QStringLiteral("id")).toString());
    if (feedback.Availability == RenderGraphAvailability::Available)
    {
        return;
    }
    // The cause stays readable where the author acted; the item itself performs nothing.
    QAction* cause = inoutMenu.addAction(feedback.Label);
    cause->setObjectName(QStringLiteral("renderGraphAvailabilityCause"));
    cause->setEnabled(false);
    cause->setToolTip(feedback.Explanation);
    if (feedback.Repair == RenderGraphAvailabilityRepair::UseComputeBvh)
    {
        QAction* repair = inoutMenu.addAction(QStringLiteral("Use Compute BVH Traversal…"));
        repair->setObjectName(QStringLiteral("renderGraphUseComputeBvhAction"));
        repair->setEnabled(CanCreateSubgraph());
        repair->setToolTip(QStringLiteral("Run this pass's ray callbacks in ordinary GPU compute so it works without "
                                          "ray-tracing hardware. Asks first; one undoable edit."));
        connect(repair, &QAction::triggered, this, &RendererWorkspace::useComputeBvhTraversal);
    }
    inoutMenu.addSeparator();
}

void RendererWorkspace::useComputeBvhTraversal()
{
    const QJsonObject node = selectedNode();
    QJsonObject candidate;
    if (MakeComputeBvhRayNode(node, candidate) == false)
    {
        mStatus->setText(QStringLiteral("Select a ray pass that uses native ray queries or the hardware RT pipeline. "
                                        "The draft was preserved."));
        Q_EMIT NodeOperationFailed(mStatus->text());
        return;
    }
    const bool isHardwarePipeline =
        node.value(QStringLiteral("invocation")).toObject().value(QStringLiteral("type")).toString() ==
        QStringLiteral("RayTracing");
    QMessageBox confirmation(QMessageBox::Question,
                             QStringLiteral("Use Compute BVH Traversal"),
                             QStringLiteral("Change %1 from %2 to a Compute pipeline with Compute BVH traversal?")
                                 .arg(mSelectedNode,
                                      isHardwarePipeline == true ? QStringLiteral("the hardware RT pipeline")
                                                                 : QStringLiteral("native ray query traversal")),
                             QMessageBox::NoButton,
                             this);
    confirmation.setObjectName(QStringLiteral("renderGraphComputeBvhConfirmation"));
    confirmation.setInformativeText(
        QStringLiteral("Only this node's invocation type and traversal change. Its ray callbacks, inputs, outputs and "
                       "wires stay as they are, and it runs in ordinary GPU compute, including on GPUs without "
                       "ray-tracing hardware. Undo restores the previous invocation; Save publishes the change."));
    QPushButton* apply = confirmation.addButton(QStringLiteral("Switch to Compute BVH"), QMessageBox::AcceptRole);
    apply->setObjectName(QStringLiteral("renderGraphComputeBvhApply"));
    QPushButton* cancel = confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(cancel);
    confirmation.setEscapeButton(cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != apply)
    {
        return;
    }
    replaceSelectedNode(candidate);
    if (selectedNode() == candidate)
    {
        mStatus->setText(QStringLiteral("%1 now uses Compute BVH traversal. Build to preview it; Undo restores the "
                                        "previous invocation.")
                             .arg(mSelectedNode));
    }
}
} // namespace ludus::editor
