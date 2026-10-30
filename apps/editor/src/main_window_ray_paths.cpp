#include "internal/main_window.h"
#include "internal/ray_path_inspection.h"
#include "internal/ray_path_inspector.h"
#include "internal/render_graph_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/world_workspace.h"

#include <QDockWidget>
#include <QSizePolicy>

namespace ludus::editor
{
void MainWindow::setupRayPaths()
{
    RayPathInspection* rayPathsOrNull = Renderer_->GetRayPathInspectionOrNull();
    if (rayPathsOrNull == nullptr)
    {
        return;
    }
    mRayPathsDockOrNull = new QDockWidget(QStringLiteral("Ray Paths"), this);
    mRayPathsDockOrNull->setObjectName(QStringLiteral("rayPathsDock"));
    RayPathInspector* inspector = new RayPathInspector(*rayPathsOrNull, mRayPathsDockOrNull);
    // Keep controls readable in the shared dock; the event/details splitter owns vertical sizing.
    inspector->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    mRayPathsDockOrNull->setWidget(inspector);
    // Docked beside Properties only once opened, so the default layout keeps its dock sizes.
    addDockWidget(Qt::RightDockWidgetArea, mRayPathsDockOrNull);
    mRayPathsDockOrNull->hide();
    // Open the panel once per request: when picking starts, or when capture reports it is unavailable.
    connect(rayPathsOrNull, &RayPathInspection::Changed, this, [this, rayPathsOrNull]() {
        const RenderGraphPreview* previewOrNull = rayPathsOrNull->GetPreviewOrNull();
        const bool isUnavailable = previewOrNull != nullptr &&
                                   previewOrNull->GetRayPathCapture().GetState() == RayPathCaptureState::Unavailable;
        const bool shouldShow = rayPathsOrNull->GetStage() != RayPathInspectionStage::Inactive || isUnavailable;
        if (shouldShow == true && mHasShownRayPaths == false)
        {
            if (mRayPathsDockOrNull->isHidden() == true && mRayPathsDockOrNull->isFloating() == false)
            {
                tabifyDockWidget(mPropertiesDock, mRayPathsDockOrNull);
            }
            showToolWindow(mRayPathsDockOrNull);
        }
        mHasShownRayPaths = shouldShow;
    });
    connect(rayPathsOrNull, &RayPathInspection::ObjectSelectionRequested, World_, [this](const QString& object) {
        static_cast<void>(World_->SelectEntity(object));
    });
}

void MainWindow::hideIdleRayPaths()
{
    const RayPathInspection* rayPathsOrNull = Renderer_->GetRayPathInspectionOrNull();
    if (mRayPathsDockOrNull == nullptr || rayPathsOrNull == nullptr ||
        rayPathsOrNull->GetStage() != RayPathInspectionStage::Inactive)
    {
        return;
    }
    mRayPathsDockOrNull->hide();
    // The next capture opens it again.
    mHasShownRayPaths = false;
}
} // namespace ludus::editor
