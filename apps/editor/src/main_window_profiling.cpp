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
}
} // namespace ludus::editor
