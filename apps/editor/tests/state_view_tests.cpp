// StateView: one icon, one sentence, at most one primary action, and for errors the cause, the fix and Copy
// Details. The Start Page, the viewport and the content browser all use it.

#include "internal/controller.h"
#include "internal/main_window.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/state_view.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace ludus::editor;

namespace
{
QLabel* FindLabel(const StateView& view, const char* name)
{
    return view.findChild<QLabel*>(QString::fromLatin1(name));
}
} // namespace

TEST_CASE("State view shows only the rows it was given", "[editor][state]")
{
    StateView view;
    view.show();
    StateViewContent content;
    content.Message = QStringLiteral("No world open.");
    view.SetContent(content);
    CHECK(FindLabel(view, "stateMessage")->isVisibleTo(&view) == true);
    CHECK(FindLabel(view, "stateCause")->isVisibleTo(&view) == false);
    CHECK(FindLabel(view, "stateFix")->isVisibleTo(&view) == false);
    CHECK(view.findChild<QPushButton*>(QStringLiteral("stateAction"))->isVisibleTo(&view) == false);
    CHECK(view.findChild<QPushButton*>(QStringLiteral("stateCopyDetails"))->isVisibleTo(&view) == false);
    CHECK(view.accessibleName() == QStringLiteral("No world open."));

    content.Cause = QStringLiteral("The file is missing.");
    content.Fix = QStringLiteral("Restore it.");
    content.ActionLabel = QStringLiteral("Refresh");
    content.ShortcutHint = QStringLiteral("F5");
    content.Details = QStringLiteral("raw detail");
    view.SetContent(content);
    CHECK(FindLabel(view, "stateCause")->text() == QStringLiteral("The file is missing."));
    CHECK(FindLabel(view, "stateFix")->text() == QStringLiteral("Restore it."));
    CHECK(FindLabel(view, "stateShortcut")->text() == QStringLiteral("F5"));
    QPushButton* action = view.findChild<QPushButton*>(QStringLiteral("stateAction"));
    REQUIRE(action->isVisibleTo(&view) == true);
    CHECK(action->text() == QStringLiteral("Refresh"));
    CHECK(view.findChild<QPushButton*>(QStringLiteral("stateCopyDetails"))->isVisibleTo(&view) == true);
    QSignalSpy triggered(&view, &StateView::ActionTriggered);
    action->click();
    CHECK(triggered.count() == 1);
}

TEST_CASE("Copy Details puts the sentence, cause, fix and raw details on the clipboard", "[editor][state]")
{
    StateViewContent content;
    content.Message = QStringLiteral("The import did not finish.");
    content.Cause = QStringLiteral("Bad header.");
    content.Fix = QStringLiteral("Use a WAV file.");
    content.Details = QStringLiteral("offset 12");
    const QString expected = QStringLiteral("The import did not finish.\nBad header.\nUse a WAV file.\noffset 12");
    CHECK(GetStateDetailsText(content) == expected);
    StateView view;
    view.show();
    view.SetContent(content);
    view.findChild<QPushButton*>(QStringLiteral("stateCopyDetails"))->click();
    CHECK(QGuiApplication::clipboard()->text() == expected);
}

TEST_CASE("A viewport failure names the cause and the fix", "[editor][state]")
{
    const StateViewContent content = MakeViewportProblem(QStringLiteral("No GPU device meets the budgets."));
    CHECK(content.Message.isEmpty() == false);
    CHECK(content.Cause == QStringLiteral("No GPU device meets the budgets."));
    CHECK(content.Fix.isEmpty() == false);
    CHECK(content.Details == content.Cause);
}

TEST_CASE("The viewport swaps its prompt for a state view on failure and back", "[editor][state][viewport]")
{
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(800, 600);
    viewport.show();
    QApplication::processEvents();
    StateView* problem = viewport.findChild<StateView*>(QStringLiteral("viewportProblem"));
    REQUIRE(problem != nullptr);
    CHECK(problem->isVisibleTo(&viewport) == false);
    Q_EMIT preview.Failed(QStringLiteral("No GPU device meets the raster profile."));
    QApplication::processEvents();
    CHECK(problem->isVisibleTo(&viewport) == true);
    CHECK(problem->GetContent().Cause == QStringLiteral("No GPU device meets the raster profile."));
    viewport.ClearFrame(QStringLiteral("Open a World to render it here."));
    QApplication::processEvents();
    CHECK(problem->isVisibleTo(&viewport) == false);
}

TEST_CASE("The Start Page says what to do when there are no recent projects", "[editor][state][shell]")
{
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    QApplication::processEvents();
    StateView* empty = window.findChild<StateView*>(QStringLiteral("recentProjectsEmpty"));
    REQUIRE(empty != nullptr);
    CHECK(empty->isVisible() == true);
    CHECK(empty->GetContent().Message == QStringLiteral("No recent projects yet."));
    CHECK(empty->GetContent().ShortcutHint.contains(QStringLiteral("Open a project")) == true);
}
