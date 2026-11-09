// Guided first run: setup problems read as what is wrong and what to do, with one primary action, progress
// while it runs and a confirmation when it works. No internal result-code name reaches the screen.

#include "internal/controller.h"
#include "internal/main_window.h"
#include "internal/setup_guidance.h"
#include "internal/state_view.h"
#include "internal/workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>

using namespace ludus::editor;
using ludus::foundation::int32;

namespace
{
constexpr int32 SETUP_TIMEOUT_MILLISECONDS = 10000;
constexpr int32 LAST_RESULT_CODE = static_cast<int32>(ResultCode::CleanupUnknown);

void WriteFile(const QString& path, const QByteArray& bytes)
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes) == bytes.size());
}

/// Every piece of text a person can read in window: labels, buttons and their tooltips.
QStringList CollectVisibleText(const QWidget& window)
{
    QStringList texts;
    for (const QLabel* label : window.findChildren<QLabel*>())
    {
        if (label->isVisible() == true)
        {
            texts << label->text() << label->toolTip();
        }
    }
    for (const QAbstractButton* button : window.findChildren<QAbstractButton*>())
    {
        if (button->isVisible() == true)
        {
            texts << button->text() << button->toolTip();
        }
    }
    return texts;
}

/// A setup adapter that waits briefly, then reports success once a "ready" file sits beside the project and a
/// coded failure until then, the way a real repair first fails and then works.
QByteArray MakeAdapter()
{
    return QByteArrayLiteral(R"py(import json, sys, time
from pathlib import Path
print(json.dumps(dict(protocol=1, type='ready')), flush=True)
request = json.loads(sys.stdin.readline())
time.sleep(0.4)
is_ready = Path(request['project']).with_name('ready').exists()
print(json.dumps(dict(protocol=1, type='result', job=request['job'],
    outcome='success' if is_ready else 'failed', stage='configuring',
    code='Ok' if is_ready else 'MissingTools',
    message='Setup checked' if is_ready else 'MissingTools: slangc was not found',
    cleanup_confirmed=True, exit_code=None, signal=None)), flush=True)
)py");
}
} // namespace

TEST_CASE("Every result code has a plain phrase that names no code", "[editor][setup][guidance]")
{
    for (int32 index = 0; index <= LAST_RESULT_CODE; ++index)
    {
        const ResultCode code = static_cast<ResultCode>(index);
        const QString phrase = DescribeResultCode(code);
        INFO(ResultCodeName(code));
        CHECK(phrase.isEmpty() == false);
        CHECK(ContainsResultCodeName(phrase) == false);
        const SetupGuidance guidance =
            DescribeSetupProblem(QString::fromLatin1(ResultCodeName(code)) + QStringLiteral(": something specific"));
        CHECK(guidance.Headline == phrase + QStringLiteral("."));
        CHECK(guidance.Detail == QStringLiteral("something specific"));
        CHECK(ContainsResultCodeName(guidance.Headline) == false);
        CHECK(ContainsResultCodeName(guidance.Fix) == false);
    }
}

TEST_CASE("Setup guidance says what is wrong and whether repair helps", "[editor][setup][guidance]")
{
    const SetupGuidance tools = DescribeSetupProblem(QStringLiteral("MissingTools: slangc was not found"));
    CHECK(tools.Headline == QStringLiteral("Build tools need to be prepared."));
    CHECK(tools.Detail == QStringLiteral("slangc was not found"));
    CHECK(tools.IsRepairable == true);

    const SetupGuidance settings =
        DescribeSetupProblem(QStringLiteral("CMake setup needs attention: InvalidProject: 'preset' must be debug"));
    CHECK(settings.Headline == QStringLiteral("The project's settings file can't be read."));
    CHECK(settings.Detail == QStringLiteral("'preset' must be debug"));
    CHECK(settings.IsRepairable == false);

    // A message that already reads well is kept, with its later lines as the detail.
    const SetupGuidance plain =
        DescribeSetupProblem(QStringLiteral("Rendering tools need preparation.\nslangc was not found."));
    CHECK(plain.Headline == QStringLiteral("Rendering tools need preparation."));
    CHECK(plain.Detail == QStringLiteral("slangc was not found."));
    CHECK(plain.IsRepairable == true);

    CHECK(DescribeSetupProblem(QString()).Headline.isEmpty() == false);
    CHECK(ReplaceResultCodeNames(QStringLiteral("BuildFailed: exit 2")) ==
          DescribeResultCode(ResultCode::BuildFailed) + QStringLiteral(": exit 2"));
    CHECK(ContainsResultCodeName(QStringLiteral("last result: BootstrapStale")) == true);
    CHECK(ContainsResultCodeName(QStringLiteral("Setup needs repair")) == false);
}

TEST_CASE("The setup notice guides a repair from problem to progress to ready", "[editor][setup][notice][guidance]")
{
    QTemporaryDir directory;
    QFile fixture(QStringLiteral(LUDUS_EDITOR_SOURCE_DIR) + QStringLiteral("/tests/fixtures/valid_v2_cmake.json"));
    REQUIRE(fixture.open(QIODevice::ReadOnly));
    const QString descriptor = directory.filePath(QStringLiteral("ludus.project.json"));
    WriteFile(descriptor, fixture.readAll());
    WriteFile(directory.filePath(QStringLiteral("CMakeLists.txt")),
              QByteArrayLiteral("cmake_minimum_required(VERSION 3.29)\n"));
    const QString adapter = directory.filePath(QStringLiteral("setup_adapter.py"));
    WriteFile(adapter, MakeAdapter());
    ToolingPaths tooling;
    tooling.PythonPath = QStringLiteral("/usr/bin/python3");
    tooling.AdapterPath = adapter;
    tooling.ToolingRoot = directory.path();
    EditorController controller(tooling);
    MainWindow window(&controller);
    window.show();
    QToolBar* noticeOrNull = window.findChild<QToolBar*>(QStringLiteral("projectSetupNotice"));
    QLabel* headlineOrNull = window.findChild<QLabel*>(QStringLiteral("projectSetupNoticeText"));
    QLabel* detailOrNull = window.findChild<QLabel*>(QStringLiteral("projectSetupNoticeDetail"));
    QProgressBar* progressOrNull = window.findChild<QProgressBar*>(QStringLiteral("projectSetupNoticeProgress"));
    QPushButton* repairOrNull = window.findChild<QPushButton*>(QStringLiteral("projectSetupNoticeRepair"));
    QPushButton* checkOrNull = window.findChild<QPushButton*>(QStringLiteral("projectSetupNoticeCheck"));
    QToolButton* dismissOrNull = window.findChild<QToolButton*>(QStringLiteral("projectSetupNoticeDismiss"));
    REQUIRE(noticeOrNull != nullptr);
    REQUIRE(headlineOrNull != nullptr);
    REQUIRE(detailOrNull != nullptr);
    REQUIRE(progressOrNull != nullptr);
    REQUIRE(repairOrNull != nullptr);
    REQUIRE(checkOrNull != nullptr);
    REQUIRE(dismissOrNull != nullptr);

    // 1. What is wrong, in words, with one primary action.
    controller.OpenProject(descriptor);
    REQUIRE(QTest::qWaitFor(
        [&controller, checkOrNull]() {
            return controller.State().SetupProblem.isEmpty() == false && controller.Caps().CanCloseImmediately &&
                   checkOrNull->isEnabled() == true;
        },
        SETUP_TIMEOUT_MILLISECONDS));
    REQUIRE(noticeOrNull->isVisible() == true);
    CHECK(headlineOrNull->text() == QStringLiteral("Build tools need to be prepared."));
    CHECK(detailOrNull->text() == QStringLiteral("slangc was not found"));
    CHECK(repairOrNull->text() == QStringLiteral("Repair project"));
    CHECK(repairOrNull->isVisible() == true);
    CHECK(repairOrNull->property("visualRole").toString() == QStringLiteral("primary"));
    CHECK(progressOrNull->isVisible() == false);
    for (const QString& text : CollectVisibleText(window))
    {
        INFO(text.toStdString());
        CHECK(ContainsResultCodeName(text) == false);
    }

    // 2. Progress while it runs: no second action to press, a bar and the step in words.
    WriteFile(directory.filePath(QStringLiteral("ready")), QByteArrayLiteral("1"));
    controller.SetupProject(QString(), QString(), false, true);
    CHECK(headlineOrNull->text() == QStringLiteral("Repairing your project…"));
    CHECK(progressOrNull->isVisible() == true);
    CHECK(repairOrNull->isVisible() == false);
    CHECK(checkOrNull->isVisible() == false);
    CHECK(detailOrNull->text().isEmpty() == false);
    CHECK(ContainsResultCodeName(detailOrNull->text()) == false);

    // 3. Success says so, and goes away on its own (or when dismissed).
    REQUIRE(QTest::qWaitFor(
        [&controller, progressOrNull]() {
            return controller.Caps().CanCloseImmediately && progressOrNull->isVisible() == false;
        },
        SETUP_TIMEOUT_MILLISECONDS));
    CHECK(controller.State().SetupProblem.isEmpty() == true);
    CHECK(noticeOrNull->isVisible() == true);
    CHECK(headlineOrNull->text() == QStringLiteral("Your project is ready."));
    CHECK(repairOrNull->isVisible() == false);
    CHECK(dismissOrNull->isVisible() == true);
    dismissOrNull->click();
    CHECK(noticeOrNull->isVisible() == false);
}

TEST_CASE("Changed SDK or tool inputs offer a repair, not the settings file", "[editor][setup][guidance]")
{
    // The setup check reports a stale recorded setup as BootstrapStale; it used to say InvalidProject, which named
    // an unreadable settings file and offered only Open Project Settings, a page that cannot refresh the setup.
    const SetupGuidance stale = DescribeSetupProblem(
        QStringLiteral("BootstrapStale: SDK or tool inputs changed; run repair to refresh presets and caches"));
    CHECK(stale.Headline == QStringLiteral("Build tools are out of date."));
    CHECK(stale.Detail == QStringLiteral("SDK or tool inputs changed; run repair to refresh presets and caches"));
    CHECK(stale.IsRepairable == true);

    QTemporaryDir directory;
    QFile fixture(QStringLiteral(LUDUS_EDITOR_SOURCE_DIR) + QStringLiteral("/tests/fixtures/valid_v2_cmake.json"));
    REQUIRE(fixture.open(QIODevice::ReadOnly));
    const QString descriptor = directory.filePath(QStringLiteral("ludus.project.json"));
    WriteFile(descriptor, fixture.readAll());
    EditorController controller(ToolingPaths{});
    MainWindow window(&controller);
    window.show();
    controller.OpenProject(descriptor);
    QCoreApplication::processEvents();
    controller.ReportProjectSetupProblem(
        QStringLiteral("BootstrapStale: SDK or tool inputs changed; run repair to refresh presets and caches"));
    QCoreApplication::processEvents();
    QPushButton* primaryOrNull = window.findChild<QPushButton*>(QStringLiteral("projectSetupNoticeRepair"));
    QLabel* headlineOrNull = window.findChild<QLabel*>(QStringLiteral("projectSetupNoticeText"));
    REQUIRE(primaryOrNull != nullptr);
    REQUIRE(headlineOrNull != nullptr);
    CHECK(headlineOrNull->text() == QStringLiteral("Build tools are out of date."));
    CHECK(primaryOrNull->text() == QStringLiteral("Repair project"));
    CHECK(primaryOrNull->property("opensSettings").toBool() == false);
}

TEST_CASE("A settings problem offers Project Settings, not a repair that cannot help", "[editor][setup][guidance]")
{
    QTemporaryDir directory;
    QFile fixture(QStringLiteral(LUDUS_EDITOR_SOURCE_DIR) + QStringLiteral("/tests/fixtures/valid_v2_cmake.json"));
    REQUIRE(fixture.open(QIODevice::ReadOnly));
    const QString descriptor = directory.filePath(QStringLiteral("ludus.project.json"));
    WriteFile(descriptor, fixture.readAll());
    EditorController controller(ToolingPaths{});
    MainWindow window(&controller);
    window.show();
    controller.OpenProject(descriptor);
    QCoreApplication::processEvents();
    controller.ReportProjectSetupProblem(QStringLiteral("InvalidProject: 'preset' must be linux-clang-debug"));
    QCoreApplication::processEvents();
    QToolBar* noticeOrNull = window.findChild<QToolBar*>(QStringLiteral("projectSetupNotice"));
    QPushButton* primaryOrNull = window.findChild<QPushButton*>(QStringLiteral("projectSetupNoticeRepair"));
    QLabel* headlineOrNull = window.findChild<QLabel*>(QStringLiteral("projectSetupNoticeText"));
    REQUIRE(noticeOrNull != nullptr);
    REQUIRE(primaryOrNull != nullptr);
    REQUIRE(headlineOrNull != nullptr);
    CHECK(noticeOrNull->isVisible() == true);
    CHECK(headlineOrNull->text() == QStringLiteral("The project's settings file can't be read."));
    CHECK(primaryOrNull->text() == QStringLiteral("Open Project Settings"));
    for (const QString& text : CollectVisibleText(window))
    {
        INFO(text.toStdString());
        CHECK(ContainsResultCodeName(text) == false);
    }
}

TEST_CASE("The Start Page explains a project that could not be opened", "[editor][setup][welcome][guidance]")
{
    QTemporaryDir directory;
    const QString descriptor = directory.filePath(QStringLiteral("ludus.project.json"));
    WriteFile(descriptor, QByteArrayLiteral("{ not json"));
    EditorController controller(ToolingPaths{});
    MainWindow window(&controller);
    window.show();
    StateView* problemOrNull = window.findChild<StateView*>(QStringLiteral("welcomeProblem"));
    REQUIRE(problemOrNull != nullptr);
    CHECK(problemOrNull->isVisible() == false);
    controller.OpenProject(descriptor);
    QCoreApplication::processEvents();
    REQUIRE(problemOrNull->isVisible() == true);
    const StateViewContent& content = problemOrNull->GetContent();
    CHECK(content.Message == QStringLiteral("That project couldn't be opened."));
    CHECK(content.Cause.isEmpty() == false);
    CHECK(content.Fix.isEmpty() == false);
    CHECK(content.ActionLabel == QStringLiteral("Choose Another Project…"));
    CHECK(ContainsResultCodeName(content.Message) == false);
    CHECK(ContainsResultCodeName(content.Cause) == false);
    CHECK(ContainsResultCodeName(content.Fix) == false);
    for (const QString& text : CollectVisibleText(window))
    {
        INFO(text.toStdString());
        CHECK(ContainsResultCodeName(text) == false);
    }
}
