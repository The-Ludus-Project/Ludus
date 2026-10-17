#include "internal/timing_panel.h"

#include "internal/controller.h"
#include "internal/timing_view.h"

#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QShowEvent>
#include <QTableWidget>
#include <QVBoxLayout>

namespace ludus::editor
{
QString DescribeTimingState(const PlaySnapshot& play)
{
    const bool isRunning = play.Phase == PlayPhase::Running || play.Phase == PlayPhase::Paused;
    if (play.HasTiming == true)
    {
        return isRunning == true ? QStringLiteral("Runtime-reported frame timing, refreshed every second.")
                                 : QStringLiteral("Last runtime report; the session is not running.");
    }
    if (isRunning == false)
    {
        return QStringLiteral("No running session. Play the project to inspect frame timing.");
    }
    return play.TimingMessage.isEmpty() == true ? QStringLiteral("Waiting for the runtime's timing report.")
                                                : play.TimingMessage;
}

TimingPanel::TimingPanel(EditorController* controller, QWidget* parentOrNull)
    : QWidget(parentOrNull), mController(controller), mStatus(new QLabel(this)), mRows(new QTableWidget(this))
{
    setObjectName(QStringLiteral("timingPanel"));
    mStatus->setObjectName(QStringLiteral("timingStatus"));
    mStatus->setWordWrap(true);
    mRows->setObjectName(QStringLiteral("timingRows"));
    mRows->setAccessibleName(QStringLiteral("Runtime frame timing"));
    mRows->setColumnCount(2);
    mRows->setHorizontalHeaderLabels({QStringLiteral("Measure"), QStringLiteral("Runtime value")});
    mRows->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    mRows->horizontalHeader()->setStretchLastSection(true);
    mRows->verticalHeader()->hide();
    mRows->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mRows->setSelectionMode(QAbstractItemView::NoSelection);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(mStatus);
    layout->addWidget(mRows, 1);
    mRefresh.setInterval(TIMING_REFRESH_MILLISECONDS);
    connect(&mRefresh, &QTimer::timeout, this, [this]() { mController->RefreshTiming(); });
    Render();
}

void TimingPanel::Render()
{
    const PlaySnapshot& play = mController->PlayState();
    mStatus->setText(DescribeTimingState(play));
    const QList<TimingRow> rows = play.HasTiming == true ? MakeTimingRows(play.Timing) : QList<TimingRow>{};
    mRows->setRowCount(static_cast<foundation::int32>(rows.size()));
    for (foundation::int32 row = 0; row < rows.size(); ++row)
    {
        mRows->setItem(row, 0, new QTableWidgetItem(rows.at(row).Label));
        mRows->setItem(row, 1, new QTableWidgetItem(rows.at(row).Value));
    }
}

void TimingPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    mRefresh.start();
    mController->RefreshTiming();
}

void TimingPanel::hideEvent(QHideEvent* event)
{
    mRefresh.stop();
    QWidget::hideEvent(event);
}
} // namespace ludus::editor
