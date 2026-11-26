// Status bar activity chip: truthful labels, a static dot or a determinate bar, problem counts that open the
// Error List, and an update rate of at most 10 Hz.

#include "internal/activity_chip.h"
#include "internal/controller.h"
#include "internal/error_list.h"
#include "internal/main_window.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QList>
#include <QRect>
#include <QSignalSpy>
#include <QSize>
#include <QStatusBar>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

using namespace ludus::editor;

namespace
{
constexpr int BURST_UPDATES = 50;
/// Extra milliseconds that let a pending change land after its interval.
constexpr int SETTLE_MILLISECONDS = 50;
constexpr double HALF_DONE = 0.5;
/// The MacBook Air's usable window, and a narrower one.
constexpr QSize MACBOOK_AIR_WINDOW_SIZE(1470, 920);
constexpr QSize NARROW_WINDOW_SIZE(1180, 760);
} // namespace

TEST_CASE("Activity labels follow priority and say nothing when idle", "[editor][activity]")
{
    CHECK(GetActivityLabel(Phase::Idle, false, false).isEmpty() == true);
    CHECK(GetActivityLabel(Phase::Building, false, false) == QStringLiteral("Building"));
    CHECK(GetActivityLabel(Phase::Configuring, true, true) == QStringLiteral("Configuring"));
    CHECK(GetActivityLabel(Phase::Idle, true, true) == QStringLiteral("Building render graph"));
    CHECK(GetActivityLabel(Phase::Idle, true, false) == QStringLiteral("Importing content"));
    // A running game or an open debugger session is not a job, and unknown cleanup is not "in progress".
    CHECK(GetActivityLabel(Phase::Running, false, false).isEmpty() == true);
    CHECK(GetActivityLabel(Phase::Debugging, false, false).isEmpty() == true);
    CHECK(GetActivityLabel(Phase::CleanupUnknown, false, false).isEmpty() == true);
    CHECK(FormatProblemCount(1, QStringLiteral("Problem"), QStringLiteral("Problems")) == QStringLiteral("1 Problem"));
    CHECK(FormatProblemCount(0, QStringLiteral("Problem"), QStringLiteral("Problems")) == QStringLiteral("0 Problems"));
}

TEST_CASE("Activity chip says Not checked when there is no problem report", "[editor][activity]")
{
    StatusActivityBar bar;
    bar.show();
    bar.SetCounts(0, 0, false);
    bar.Flush();
    QToolButton* problems = bar.findChild<QToolButton*>(QStringLiteral("statusProblems"));
    QToolButton* warnings = bar.findChild<QToolButton*>(QStringLiteral("statusWarnings"));
    REQUIRE(problems != nullptr);
    REQUIRE(warnings != nullptr);
    CHECK(problems->text() == QStringLiteral("–"));
    CHECK(problems->accessibleName() == QStringLiteral("Not checked"));
    CHECK(problems->toolTip().startsWith(QStringLiteral("Not checked")) == true);
    CHECK(warnings->accessibleName() == QStringLiteral("Not checked"));
    // A real count is still a count, even if the graph has no report.
    bar.SetCounts(2, 0, false);
    bar.Flush();
    CHECK(problems->text() == QStringLiteral("2"));
    CHECK(problems->accessibleName() == QStringLiteral("2 Problems"));
    bar.SetCounts(0, 0, true);
    bar.Flush();
    CHECK(problems->text() == QStringLiteral("0"));
    CHECK(problems->accessibleName() == QStringLiteral("0 Problems"));
}

TEST_CASE("Activity chip shows a dot or a bar, hides when idle and opens the Error List", "[editor][activity]")
{
    StatusActivityBar bar;
    bar.show();
    bar.SetActivity(QStringLiteral("Building"), -1.0);
    CHECK(bar.IsActivityShown() == true);
    CHECK(bar.GetActivityText() == QStringLiteral("Building"));
    CHECK(bar.GetShownProgress() < 0.0);
    // Determinate progress replaces the dot with a bar once the rate limit allows the change.
    QTest::qWait(ACTIVITY_UPDATE_INTERVAL_MILLISECONDS + SETTLE_MILLISECONDS);
    bar.SetActivity(QStringLiteral("Importing content"), HALF_DONE);
    CHECK(bar.GetShownProgress() == HALF_DONE);
    QTest::qWait(ACTIVITY_UPDATE_INTERVAL_MILLISECONDS + SETTLE_MILLISECONDS);
    bar.SetActivity(QString(), HALF_DONE);
    CHECK(bar.IsActivityShown() == false);
    bar.SetCounts(2, 1);
    bar.Flush();
    QToolButton* problems = bar.findChild<QToolButton*>(QStringLiteral("statusProblems"));
    QToolButton* warnings = bar.findChild<QToolButton*>(QStringLiteral("statusWarnings"));
    REQUIRE(problems != nullptr);
    REQUIRE(warnings != nullptr);
    // Compact counts: the icon says which, the number how many; words stay in the name and tooltip.
    CHECK(problems->text() == QStringLiteral("2"));
    CHECK(warnings->text() == QStringLiteral("1"));
    CHECK(problems->accessibleName() == QStringLiteral("2 Problems"));
    CHECK(warnings->accessibleName() == QStringLiteral("1 Warning"));
    CHECK(problems->toolTip().startsWith(QStringLiteral("2 Problems")) == true);
    QSignalSpy requests(&bar, &StatusActivityBar::ProblemsRequested);
    problems->click();
    warnings->click();
    CHECK(requests.count() == 2);
}

TEST_CASE("Activity chip changes at most ten times a second", "[editor][activity]")
{
    StatusActivityBar bar;
    bar.show();
    QTest::qWait(ACTIVITY_UPDATE_INTERVAL_MILLISECONDS);
    const int before = bar.GetAppliedChangeCount();
    for (int update = 0; update < BURST_UPDATES; ++update)
    {
        bar.SetActivity(QStringLiteral("Building %1").arg(update), -1.0);
    }
    // The first change in a burst lands at once; the rest coalesce into one more after the interval.
    CHECK(bar.GetAppliedChangeCount() - before == 1);
    REQUIRE(QTest::qWaitFor([&bar, before]() { return bar.GetAppliedChangeCount() - before == 2; }) == true);
    CHECK(bar.GetAppliedChangeCount() - before == 2);
    CHECK(bar.GetActivityText() == QStringLiteral("Building %1").arg(BURST_UPDATES - 1));
}

TEST_CASE("Main window status bar counts problems and opens the Error List", "[editor][activity][shell]")
{
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    QApplication::processEvents();
    StatusActivityBar* bar = window.findChild<StatusActivityBar*>(QStringLiteral("statusActivityBar"));
    REQUIRE(bar != nullptr);
    CHECK(bar->IsActivityShown() == false);
    ErrorList* list = window.findChild<ErrorList*>();
    REQUIRE(list != nullptr);
    QList<ErrorListEntry> entries;
    ErrorListEntry error;
    error.Severity = ErrorListSeverity::Error;
    error.Producer = ErrorListProducer::Build;
    error.Code = QStringLiteral("E1");
    error.Description = QStringLiteral("broken");
    entries.append(error);
    ErrorListEntry warning = error;
    warning.Severity = ErrorListSeverity::Warning;
    entries.append(warning);
    list->ReplaceEntries(ErrorListProducer::Build, entries);
    bar->Flush();
    CHECK(bar->findChild<QToolButton*>(QStringLiteral("statusProblems"))->accessibleName() ==
          QStringLiteral("1 Problem"));
    CHECK(bar->findChild<QToolButton*>(QStringLiteral("statusWarnings"))->accessibleName() ==
          QStringLiteral("1 Warning"));
    QDockWidget* dock = window.findChild<QDockWidget*>(QStringLiteral("errorListDock"));
    REQUIRE(dock != nullptr);
    dock->hide();
    bar->findChild<QToolButton*>(QStringLiteral("statusProblems"))->click();
    CHECK(dock->isHidden() == false);
}

TEST_CASE("Status bar counts and toolbar labels stay whole at a MacBook Air width and narrower",
          "[editor][activity][shell][layout]")
{
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    for (const QSize size : {MACBOOK_AIR_WINDOW_SIZE, NARROW_WINDOW_SIZE})
    {
        INFO(size.width());
        window.resize(size);
        QApplication::processEvents();
        QApplication::processEvents();
        StatusActivityBar* bar = window.findChild<StatusActivityBar*>(QStringLiteral("statusActivityBar"));
        REQUIRE(bar != nullptr);
        // The permanent widgets keep their whole width inside the window; the status line gives way first.
        const QRect barRect(bar->mapTo(&window, QPoint(0, 0)), bar->size());
        CHECK(window.rect().contains(barRect) == true);
        for (QToolButton* button : bar->findChildren<QToolButton*>())
        {
            INFO(button->objectName().toStdString());
            CHECK(button->width() >= button->sizeHint().width());
            CHECK(button->width() >= button->minimumWidth());
        }
        // Every main-window dock tab shows its whole label or only its icon; none is cut mid-word.
        for (QTabBar* tabs : window.findChildren<QTabBar*>(Qt::FindDirectChildrenOnly))
        {
            if (tabs->isVisible() == false)
            {
                continue;
            }
            CHECK(tabs->elideMode() == Qt::ElideRight);
            int width = 0;
            for (int index = 0; index < tabs->count(); ++index)
            {
                width += tabs->tabRect(index).width();
            }
            CHECK(width <= tabs->width());
        }
    }
}
