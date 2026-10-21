#include "internal/editor_commands.h"
#include "internal/main_window.h"
#include "internal/profiling_panel.h"
#include "internal/world_workspace.h"

#include <QAction>
#include <QDockWidget>

namespace ludus::editor
{
void MainWindow::setupProfiling()
{
    mProfilingDock->setObjectName(QStringLiteral("profilingDock"));
    mProfilingPanel = new ProfilingPanel(&World_->GetRenderPreview(), mRecordProfileAction, mProfilingDock);
    mProfilingDock->setWidget(mProfilingPanel);
    addDockWidget(Qt::BottomDockWidgetArea, mProfilingDock);
    tabifyDockWidget(TimingDock_, mProfilingDock);
    mProfilingDock->hide();
    // Follow the same visible World source as the renderer. An in-flight recording
    // retains its source; the completion notification retries the binding afterward.
    const auto updatePreview = [this]() {
        static_cast<void>(mProfilingPanel->GetController().SetPreviewSource(&World_->GetRenderPreview()));
    };
    connect(World_, &WorldWorkspace::RenderPreviewChanged, mProfilingPanel, updatePreview);
    connect(&mProfilingPanel->GetController(), &ProfilingController::Changed, World_, updatePreview);
    connect(mRecordProfileAction, &QAction::triggered, this, [this]() { showToolWindow(mProfilingDock); });
}
} // namespace ludus::editor
