#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/world_graph_view.h"

#include <ludus/foundation/base/types.h>

#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QImage>
#include <QList>
#include <QMenu>
#include <QObject>
#include <QPixmap>
#include <QRect>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;

constexpr int32 VIEWPORT_HEIGHT = 480;
constexpr int32 SETTLE_MILLISECONDS = 50;
/// From a roomy World document down to one squeezed beside docked panels.
constexpr int32 ROOMY_WIDTH = 1400;
constexpr int32 MEDIUM_WIDTH = 900;
constexpr int32 COMPACT_WIDTH = 640;
constexpr int32 NARROW_WIDTH = 420;
constexpr int32 NARROWEST_WIDTH = 300;
constexpr int32 WIDTHS[] = {ROOMY_WIDTH, MEDIUM_WIDTH, COMPACT_WIDTH, NARROW_WIDTH, NARROWEST_WIDTH};

/// Rectangle of @p control in toolbar coordinates.
QRect GetToolbarRect(const QToolBar& toolbar, const QWidget& control)
{
    return QRect(control.mapTo(&toolbar, QPoint()), control.size());
}

/// Controls a reader sees on the toolbar row: buttons and combo boxes, including those inside a
/// grouped container, skipping anything hidden or moved behind the overflow chevron.
QList<QWidget*> GetShownControls(const QToolBar& toolbar)
{
    QList<QWidget*> controls;
    for (QAction* action : toolbar.actions())
    {
        QWidget* widgetOrNull = action->isSeparator() == true ? nullptr : toolbar.widgetForAction(action);
        if (widgetOrNull == nullptr || widgetOrNull->isVisible() == false)
        {
            continue;
        }
        if (qobject_cast<QComboBox*>(widgetOrNull) != nullptr ||
            qobject_cast<QAbstractButton*>(widgetOrNull) != nullptr)
        {
            controls.append(widgetOrNull);
            continue;
        }
        for (QWidget* child : widgetOrNull->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
        {
            if (child->isVisible() == true)
            {
                controls.append(child);
            }
        }
    }
    return controls;
}

/// Every control on the row sits inside the toolbar, shows a non-empty label at its full width,
/// and none overlaps another.
void CheckToolbarRow(const QToolBar& toolbar)
{
    const QList<QWidget*> controls = GetShownControls(toolbar);
    CHECK(controls.isEmpty() == false);
    for (int32 i = 0; i < controls.size(); ++i)
    {
        const QWidget& control = *controls[i];
        INFO(control.objectName().toStdString());
        const QRect rect = GetToolbarRect(toolbar, control);
        CHECK(toolbar.rect().contains(rect) == true);
        if (const QComboBox* comboBox = qobject_cast<const QComboBox*>(&control); comboBox != nullptr)
        {
            INFO(comboBox->currentText().toStdString());
            CHECK(comboBox->currentText().isEmpty() == false);
            CHECK(control.width() >= ComputeComboBoxWidth(*comboBox, comboBox->currentText()));
        }
        else if (const QAbstractButton* button = qobject_cast<const QAbstractButton*>(&control); button != nullptr)
        {
            INFO(button->text().toStdString());
            CHECK((button->text().isEmpty() == false || button->icon().isNull() == false));
            CHECK(control.width() >= control.sizeHint().width());
        }
        for (int32 j = i + 1; j < controls.size(); ++j)
        {
            INFO("overlaps " << controls[j]->objectName().toStdString());
            CHECK(rect.intersects(GetToolbarRect(toolbar, *controls[j])) == false);
        }
    }
}

bool IsOnRow(const QToolBar& toolbar, const char* objectName)
{
    const QWidget* controlOrNull = toolbar.findChild<QWidget*>(QString::fromLatin1(objectName));
    return controlOrNull != nullptr && controlOrNull->isVisible() == true &&
           toolbar.rect().contains(GetToolbarRect(toolbar, *controlOrNull)) == true;
}
} // namespace

TEST_CASE("Viewport toolbar keeps every label whole and apart at every width", "[editor][viewport][toolbar][layout]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({});
    RenderPreview source;
    PreviewViewport viewport(&source, static_cast<QWidget*>(nullptr), false);
    viewport.SetTerrainActions();
    viewport.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&source);
    workspace.SetProject(project.path(), 1);
    // A selected terrain shows its editing command on the row, as in the default World.
    viewport.GetEditTerrainAction().setVisible(true);
    viewport.resize(ROOMY_WIDTH, VIEWPORT_HEIGHT);
    viewport.show();
    QTest::qWait(SETTLE_MILLISECONDS);
    QToolBar* toolbar = viewport.findChild<QToolBar*>(QStringLiteral("viewportControls"));
    REQUIRE(toolbar != nullptr);
    if (viewport.findChild<QComboBox*>(QStringLiteral("worldGraphTargets")) == nullptr)
    {
        SKIP("This build has no native project graph preview");
    }
    const QString screenshotDirectory = qEnvironmentVariable("LUDUS_VIEWPORT_TOOLBAR_SCREENSHOT_DIRECTORY");
    for (const int32 width : WIDTHS)
    {
        INFO("width " << width);
        viewport.resize(width, VIEWPORT_HEIGHT);
        QTest::qWait(SETTLE_MILLISECONDS);
        if (screenshotDirectory.isEmpty() == false)
        {
            REQUIRE(QDir().mkpath(screenshotDirectory) == true);
            const QRect band(0, 0, width, toolbar->geometry().bottom() + 1);
            CHECK(viewport.grab(band).save(
                      QDir(screenshotDirectory).filePath(QStringLiteral("toolbar-%1.png").arg(width))) == true);
        }
        CheckToolbarRow(*toolbar);
        if (width == NARROW_WIDTH)
        {
            // The chevron opens a menu of the commands that did not fit, so none becomes unreachable.
            QToolButton* chevron = toolbar->findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"));
            REQUIRE(chevron != nullptr);
            QMenu* menu = chevron->menu();
            REQUIRE(menu != nullptr);
            const QSignalSpy menuShown(menu, &QMenu::aboutToShow);
            QStringList overflowTexts;
            QTimer closeMenu;
            closeMenu.setSingleShot(true);
            QObject::connect(&closeMenu, &QTimer::timeout, menu, [menu, &overflowTexts, &screenshotDirectory, width]() {
                for (const QAction* action : menu->actions())
                {
                    overflowTexts.append(action->text());
                }
                if (screenshotDirectory.isEmpty() == false)
                {
                    const QString path =
                        QDir(screenshotDirectory).filePath(QStringLiteral("toolbar-%1-overflow-open.png").arg(width));
                    CHECK(menu->grab().save(path) == true);
                }
                menu->close();
            });
            closeMenu.start(SETTLE_MILLISECONDS);
            chevron->showMenu();
            REQUIRE(menuShown.size() == 1);
            INFO(overflowTexts.join(QStringLiteral(", ")).toStdString());
            for (QAction* action : toolbar->actions())
            {
                const QWidget* widgetOrNull = toolbar->widgetForAction(action);
                if (action->isSeparator() == false && action->isVisible() == true && widgetOrNull != nullptr &&
                    qobject_cast<const QToolButton*>(widgetOrNull) != nullptr && widgetOrNull->isVisible() == false)
                {
                    INFO(action->text().toStdString());
                    CHECK(overflowTexts.contains(action->text()) == true);
                }
            }
            CHECK(overflowTexts.contains(QStringLiteral("Navigation help")) == true);
            CHECK(overflowTexts.contains(QStringLiteral("Exposure")) == true);
        }
        // The choice of what the viewport shows stays on the row longest.
        CHECK(IsOnRow(*toolbar, "worldGraphTargets") == true);
    }
    // Narrowest: help and diagnostics wait behind the chevron so the output choice and size stay readable.
    const QToolButton* chevron = toolbar->findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"));
    REQUIRE(chevron != nullptr);
    CHECK(chevron->isVisible() == true);
    CHECK(IsOnRow(*toolbar, "viewportNavigationHint") == false);
    CHECK(IsOnRow(*toolbar, "viewportSize") == true);
    // The configuration menu appears only when there is a choice to make, and explains itself.
    const QComboBox* configurations = viewport.findChild<QComboBox*>(QStringLiteral("worldGraphConfigurations"));
    REQUIRE(configurations != nullptr);
    CHECK(configurations->isVisibleTo(&viewport) == (configurations->count() > 1));
    CHECK(configurations->toolTip().isEmpty() == false);
}

TEST_CASE("A fitted combo box grows and shrinks with its current choice", "[editor][viewport][toolbar][layout]")
{
    const QString shortChoice = QStringLiteral("Final output");
    const QString longChoice = QStringLiteral("Debug: Light count per cluster");
    QComboBox comboBox;
    comboBox.addItems({shortChoice, longChoice});
    FitComboBoxToCurrentText(comboBox);
    const int32 shortWidth = ComputeComboBoxWidth(comboBox, shortChoice);
    CHECK(comboBox.minimumWidth() == shortWidth);
    comboBox.setCurrentIndex(1);
    CHECK(comboBox.minimumWidth() == ComputeComboBoxWidth(comboBox, longChoice));
    CHECK(comboBox.minimumWidth() > shortWidth);
    // Fitting again keeps one connection: returning to the short choice still follows it.
    FitComboBoxToCurrentText(comboBox);
    comboBox.setCurrentIndex(0);
    CHECK(comboBox.minimumWidth() == shortWidth);
}
