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
#include <QPlainTextEdit>
#include <QPoint>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>

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
    QPushButton* copy = problem->findChild<QPushButton*>(QStringLiteral("stateCopyDetails"));
    REQUIRE(copy != nullptr);
    const QPoint positionViewport = viewport.mapFromGlobal(copy->mapToGlobal(copy->rect().center()));
    QWidget* targetOrNull = viewport.childAt(positionViewport);
    REQUIRE(targetOrNull == copy);
    QGuiApplication::clipboard()->clear();
    QTest::mouseClick(targetOrNull, Qt::LeftButton);
    CHECK(QGuiApplication::clipboard()->text() == GetStateDetailsText(problem->GetContent()));
    viewport.ClearFrame(QStringLiteral("Open a World to render it here."));
    QApplication::processEvents();
    CHECK(problem->isVisibleTo(&viewport) == false);
    QWidget* promptOrNull = viewport.findChild<QWidget*>(QStringLiteral("emptyWorldView"));
    REQUIRE(promptOrNull != nullptr);
    CHECK(promptOrNull->testAttribute(Qt::WA_TransparentForMouseEvents) == true);
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

TEST_CASE("A cause with IDs and hashes fits a narrow dock: paths wrap, a word too long for one line is shortened",
          "[editor][state][layout]")
{
    constexpr int NARROW_WIDTH = 220;
    constexpr int WIDE_WIDTH = 900;
    constexpr int HEIGHT = 600;
    const QString id = QStringLiteral("material/0b2d8a4c-1f3e-4b7a-9c21-5d6e7f80448a");
    // A hash has no slash to wrap at, so in a narrow dock it can only be shortened.
    const QString digest = QStringLiteral("0b2d8a4c1f3e4b7a9c215d6e7f80448a0b2d8a4c");
    StateViewContent content;
    content.Icon = EditorIcon::Error;
    content.Message = QStringLiteral("The content catalog could not be read.");
    content.Cause = QStringLiteral("Cooked material %1 (assets/objects/brick1.bin) has a stale binding for "
                                   "texture/checker, digest %2.")
                        .arg(id, digest);
    content.Fix = QStringLiteral("Recook the scene, then refresh.");
    content.ActionLabel = QStringLiteral("Refresh");
    content.Details = content.Cause;
    QWidget host;
    QVBoxLayout* layout = new QVBoxLayout(&host);
    StateView* viewOrNull = new StateView(&host);
    layout->addWidget(viewOrNull);
    viewOrNull->SetContent(content);
    LongWordLabel* causeOrNull = viewOrNull->findChild<LongWordLabel*>(QStringLiteral("stateCause"));
    REQUIRE(causeOrNull != nullptr);

    host.resize(NARROW_WIDTH, HEIGHT);
    host.show();
    QApplication::processEvents();
    // The view fits its host. The path wraps at its slashes; the ID after the slash and the hash are too wide for
    // one line, so each is shortened in the middle with the whole text in the tooltip, and the label has the height
    // its wrapped text needs.
    CHECK(viewOrNull->width() <= NARROW_WIDTH);
    CHECK(causeOrNull->GetFullText() == content.Cause);
    CHECK(causeOrNull->IsShortened() == true);
    CHECK(causeOrNull->toolTip() == content.Cause);
    CHECK(causeOrNull->text().contains(QStringLiteral("…")) == true);
    // The start and the end of each long word stay visible around its gap.
    CHECK(causeOrNull->text().contains(QStringLiteral("0b2d8a4c-")) == true);
    CHECK(causeOrNull->text().contains(QStringLiteral("5d6e7f80448a")) == true);
    CHECK(causeOrNull->text().contains(id) == false);
    CHECK(causeOrNull->text().contains(digest) == false);
    CHECK(causeOrNull->text().contains(QStringLiteral("stale binding")) == true);
    CHECK(causeOrNull->height() >= causeOrNull->heightForWidth(causeOrNull->width()));
    CHECK(causeOrNull->minimumSizeHint().width() <= NARROW_WIDTH);
    for (const QWidget* widget : viewOrNull->findChildren<QWidget*>())
    {
        if (widget->isVisibleTo(viewOrNull) == true && widget->objectName().isEmpty() == false)
        {
            INFO(widget->objectName().toStdString());
            const QRect inView(widget->mapTo(viewOrNull, QPoint(0, 0)), widget->size());
            CHECK(inView.left() >= 0);
            CHECK(inView.right() <= viewOrNull->width() - 1);
        }
    }
    // The action row stacks instead of clipping a button.
    for (const char* name : {"stateAction", "stateCopyDetails", "stateShowDetails"})
    {
        const QPushButton* buttonOrNull = viewOrNull->findChild<QPushButton*>(QString::fromLatin1(name));
        REQUIRE(buttonOrNull != nullptr);
        INFO(name);
        CHECK(buttonOrNull->isVisibleTo(viewOrNull) == true);
        CHECK(buttonOrNull->width() >= buttonOrNull->sizeHint().width());
    }

    // Details holds the whole text with nothing shortened, and is hidden until asked for.
    QPlainTextEdit* detailsOrNull = viewOrNull->findChild<QPlainTextEdit*>(QStringLiteral("stateDetails"));
    QPushButton* toggleOrNull = viewOrNull->findChild<QPushButton*>(QStringLiteral("stateShowDetails"));
    REQUIRE(detailsOrNull != nullptr);
    REQUIRE(toggleOrNull != nullptr);
    CHECK(detailsOrNull->isVisibleTo(viewOrNull) == false);
    toggleOrNull->click();
    CHECK(detailsOrNull->isVisibleTo(viewOrNull) == true);
    CHECK(detailsOrNull->toPlainText() == GetStateDetailsText(content));
    CHECK(detailsOrNull->toPlainText().contains(digest) == true);
    CHECK(toggleOrNull->text() == QStringLiteral("Hide Details"));
    toggleOrNull->click();
    CHECK(detailsOrNull->isVisibleTo(viewOrNull) == false);

    // Wide enough for every word: nothing is shortened and the tooltip is gone.
    host.resize(WIDE_WIDTH, HEIGHT);
    QApplication::processEvents();
    CHECK(causeOrNull->IsShortened() == false);
    CHECK(causeOrNull->text() == content.Cause);
    CHECK(causeOrNull->toolTip().isEmpty() == true);
}
