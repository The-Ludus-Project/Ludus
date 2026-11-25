// The Environment World tool: authoring and inspecting a climate with two regions through one World transaction,
// with Properties hidden, including refused edits in the panel, the region overlay, undo, save and reopen, and
// removal behind a confirmation.
#include "entropy_fixture.h"
#include "internal/preview_viewport.h"
#include "internal/render_panel.h"
#include "internal/viewport_object_interaction.h"
#include "internal/viewport_object_overlay.h"
#include "internal/world_climate.h"
#include "internal/world_environment_panel.h"
#include "internal/world_workspace.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRectF>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

namespace ludus::editor
{
namespace
{
using namespace Qt::StringLiterals;

template <typename WidgetType>
WidgetType& Find(const QWidget& parent, const QString& name)
{
    WidgetType* widgetOrNull = parent.findChild<WidgetType*>(name);
    REQUIRE(widgetOrNull != nullptr);
    return *widgetOrNull;
}

/// Click the named button of the next modal message box once it is shown.
void ClickNextDialogButton(const QString& buttonName, QMessageBox::StandardButton fallback)
{
    QTimer::singleShot(0, [buttonName, fallback]() {
        QMessageBox* boxOrNull = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (boxOrNull == nullptr)
        {
            ClickNextDialogButton(buttonName, fallback);
            return;
        }
        QAbstractButton* buttonOrNull = buttonName.isEmpty() == true
                                            ? boxOrNull->button(fallback)
                                            : boxOrNull->findChild<QAbstractButton*>(buttonName);
        if (buttonOrNull != nullptr)
        {
            buttonOrNull->click();
        }
        else
        {
            boxOrNull->reject();
        }
    });
}

QJsonArray GetRegions(const WorldWorkspace& workspace)
{
    return workspace.Document().Draft().value(WORLD_CLIMATE).toObject().value("regions"_L1).toArray();
}
} // namespace

TEST_CASE("The Environment tool authors both regions in one World transaction with Properties hidden",
          "[editor][world][climate][environment]")
{
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString path = QDir(root).filePath(QStringLiteral("valley.world.json"));
    WorldWorkspace workspace;
    workspace.SetEntropySource(tests::MakeFixedEntropySource());
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(path) == true);
    QWidget properties;
    workspace.TakePropertiesPanel(&properties)->hide();
    workspace.show();

    WorldEnvironmentPanel& panel = Find<WorldEnvironmentPanel>(workspace, u"worldEnvironmentPanel"_s);
    QTabWidget& tabs = Find<QTabWidget>(workspace, u"worldSidebarLists"_s);
    tabs.setCurrentWidget(&panel);
    const QLabel& status = Find<QLabel>(panel, u"worldEnvironmentStatus"_s);
    ViewportObjectInteraction& overlay = Find<RenderPanel>(workspace, QString{}).GetViewport().GetObjectInteraction();
    REQUIRE(workspace.Document().Draft().contains(WORLD_CLIMATE) == false);
    REQUIRE(Find<QPushButton>(panel, u"worldEnvironmentAddClimate"_s).isVisible() == true);
    const foundation::usize history = workspace.Document().GetHistory().GetCount();

    // A new climate and two regions stay a draft until Apply; the overlay follows the draft.
    Find<QPushButton>(panel, u"worldEnvironmentAddClimate"_s).click();
    Find<QPushButton>(panel, u"worldEnvironmentAddRegion"_s).click();
    Find<QPushButton>(panel, u"worldEnvironmentAddRegion"_s).click();
    CHECK(panel.HasPendingEdits() == true);
    CHECK(workspace.Document().Draft().contains(WORLD_CLIMATE) == false);
    CHECK(panel.GetDraft().toObject().value("regions"_L1).toArray().size() == 2);
    // The domain, and each region's edge and feather depth.
    CHECK(overlay.GetClimateOutlineCount() == 5);
    // With nothing selected, the viewport draws those outlines as dashed ground-plane segments.
    constexpr qreal FRAME_WIDTH = 640;
    constexpr qreal FRAME_HEIGHT = 480;
    // The offscreen test viewport presents no frames, so activate its interaction as a shown viewport does.
    overlay.SetActive(true);
    ViewportObjectOverlay drawn;
    overlay.BuildOverlay(QRectF(0, 0, FRAME_WIDTH, FRAME_HEIGHT), drawn);
    CHECK(drawn.OutlineLinesPixel.IsEmpty() == false);

    // The second region is raised above the first and moved east; the inspector sees it win at its centre.
    QComboBox& regions = Find<QComboBox>(panel, u"worldEnvironmentRegionChooser"_s);
    REQUIRE(regions.count() == 2);
    regions.setCurrentIndex(1);
    Find<QLineEdit>(panel, u"worldEnvironmentRegionName"_s).setText(u"Ridge"_s);
    Q_EMIT Find<QLineEdit>(panel, u"worldEnvironmentRegionName"_s).editingFinished();
    Find<QSpinBox>(panel, u"worldEnvironmentRegionPriority"_s).setValue(5);
    Find<QDoubleSpinBox>(panel, u"worldEnvironmentRegionBounds:maximumX"_s).setValue(400.0);
    Find<QDoubleSpinBox>(panel, u"worldEnvironmentInspectX"_s).setValue(50.0);
    Find<QDoubleSpinBox>(panel, u"worldEnvironmentInspectZ"_s).setValue(0.0);
    CHECK(panel.GetInspection().contains(u"Ridge"_s) == true);
    CHECK(panel.GetInspection().contains(u"°C"_s) == true);

    // A region outside the domain is refused in the panel with its repair, and nothing is committed.
    Find<QDoubleSpinBox>(panel, u"worldEnvironmentRegionBounds:maximumX"_s).setValue(5000.0);
    CHECK(status.text().contains(u"Region “Ridge”"_s) == true);
    CHECK(status.text().contains(u"enlarge the domain"_s) == true);
    CHECK(panel.GetInspection().contains(u"Fix the climate"_s) == true);
    Find<QPushButton>(panel, u"worldEnvironmentApply"_s).click();
    CHECK(workspace.Document().Draft().contains(WORLD_CLIMATE) == false);
    CHECK(workspace.Document().GetHistory().GetCount() == history);
    CHECK(status.text().contains(u"enlarge the domain"_s) == true);

    // After the repair, Apply commits both regions in one undoable World edit.
    Find<QDoubleSpinBox>(panel, u"worldEnvironmentRegionBounds:maximumX"_s).setValue(400.0);
    CHECK(status.text().isEmpty() == true);
    Find<QPushButton>(panel, u"worldEnvironmentApply"_s).click();
    REQUIRE(GetRegions(workspace).size() == 2);
    CHECK(workspace.Document().GetHistory().GetCount() == history + 1);
    CHECK(panel.HasPendingEdits() == false);
    CHECK(status.text().contains(u"Climate applied"_s) == true);
    const QJsonValue applied = workspace.Document().Draft().value(WORLD_CLIMATE);
    CHECK(applied.toObject().value("revision"_L1).toInteger() == 1);

    // Undo and Redo move the whole climate; the panel follows the World.
    workspace.Undo();
    CHECK(workspace.Document().Draft().contains(WORLD_CLIMATE) == false);
    CHECK(Find<QPushButton>(panel, u"worldEnvironmentAddClimate"_s).isVisible() == true);
    CHECK(overlay.GetClimateOutlineCount() == 0);
    workspace.Redo();
    CHECK(workspace.Document().Draft().value(WORLD_CLIMATE) == applied);
    CHECK(regions.count() == 2);

    // Another tab hides the overlay; returning shows it again.
    tabs.setCurrentIndex(0);
    CHECK(overlay.GetClimateOutlineCount() == 0);
    tabs.setCurrentWidget(&panel);
    CHECK(overlay.GetClimateOutlineCount() == 5);

    // Save and reopen keep the climate exactly.
    REQUIRE(workspace.Save() == true);
    {
        WorldWorkspace reopened;
        reopened.SetEntropySource(tests::MakeFixedEntropySource());
        reopened.SetProject(root, 1);
        REQUIRE(reopened.Open(path) == true);
        CHECK(reopened.Document().Draft().value(WORLD_CLIMATE) == applied);
        WorldEnvironmentPanel& reopenedPanel = Find<WorldEnvironmentPanel>(reopened, u"worldEnvironmentPanel"_s);
        CHECK(Find<QComboBox>(reopenedPanel, u"worldEnvironmentRegionChooser"_s).count() == 2);
        CHECK(reopenedPanel.HasPendingEdits() == false);
    }

    // Removal asks first: Cancel keeps the climate, the explicit button removes it in one undoable edit.
    ClickNextDialogButton(QString{}, QMessageBox::Cancel);
    Find<QPushButton>(panel, u"worldEnvironmentRemoveClimate"_s).click();
    CHECK(workspace.Document().Draft().value(WORLD_CLIMATE) == applied);
    ClickNextDialogButton(u"worldEnvironmentConfirmRemove"_s, QMessageBox::Cancel);
    Find<QPushButton>(panel, u"worldEnvironmentRemoveClimate"_s).click();
    CHECK(workspace.Document().Draft().contains(WORLD_CLIMATE) == false);
    CHECK(status.text().contains(u"Climate removed"_s) == true);
    workspace.Undo();
    CHECK(workspace.Document().Draft().value(WORLD_CLIMATE) == applied);
}

TEST_CASE("The Environment tool keeps unapplied edits when the World refreshes",
          "[editor][world][climate][environment]")
{
    WorldEnvironmentPanel panel;
    QJsonObject climate;
    {
        QTemporaryDir directory;
        WorldWorkspace workspace;
        workspace.SetEntropySource(tests::MakeFixedEntropySource());
        const QString root = QFileInfo(directory.path()).canonicalFilePath();
        workspace.SetProject(root, 1);
        REQUIRE(workspace.Create(QDir(root).filePath(u"seed.world.json"_s)) == true);
        WorldEnvironmentPanel& seeded = Find<WorldEnvironmentPanel>(workspace, u"worldEnvironmentPanel"_s);
        Find<QPushButton>(seeded, u"worldEnvironmentAddClimate"_s).click();
        climate = seeded.GetDraft().toObject();
    }
    panel.SetCommittedClimate(climate, true, true);
    CHECK(panel.HasPendingEdits() == false);
    Find<QPushButton>(panel, u"worldEnvironmentAddRegion"_s).click();
    REQUIRE(panel.HasPendingEdits() == true);
    // A refresh with the same committed climate, or a changed one, keeps the draft and says what Apply does.
    panel.SetCommittedClimate(climate, true, false);
    CHECK(panel.GetDraft().toObject().value("regions"_L1).toArray().size() == 1);
    panel.SetCommittedClimate(QJsonValue::Undefined, true, false);
    CHECK(panel.GetDraft().toObject().value("regions"_L1).toArray().size() == 1);
    CHECK(Find<QLabel>(panel, u"worldEnvironmentStatus"_s).text().contains(u"Apply replaces it"_s) == true);
    // Revert shows the World's climate again.
    Find<QPushButton>(panel, u"worldEnvironmentRevert"_s).click();
    CHECK(panel.GetDraft().isUndefined() == true);
    CHECK(panel.HasPendingEdits() == false);
}
} // namespace ludus::editor
