// The World workspace's Scatter tab (Generation P1): an override edited while a candidate is pending is refused as
// stale, named by placement and property, and repaired by keeping the authored value or rejecting the proposal
// (docs/development/editor-workspace.md#repair-a-stale-scatter-candidate).

#include "internal/world_demo_adapter.h"
#include "internal/world_scatter_panel.h"
#include "internal/world_workspace.h"

#include <ludus/content/procedural/environment/scatter_generation.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
namespace
{
namespace procedural = ludus::content::procedural;
using ludus::foundation::int32;
using ludus::foundation::uint16;
using ludus::foundation::usize;
using procedural::ScatterIdentity;
using procedural::ScatterManifest;
using procedural::ScatterPlacement;

/// Rotation edits: a quarter turn for the staged override, then an eighth turn while it is pending.
constexpr int32 QUARTER_TURN = 0x4000;
constexpr int32 EIGHTH_TURN = 0x2000;
constexpr int32 ROTATION_UNITS = 0x10000;
constexpr uint16 FIRST_ROOM = 1;
constexpr uint16 SECOND_ROOM = 2;

void Write(const QString& path, std::string_view bytes)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes.data(), static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size()));
}

/// Both built-in World-demo rooms in one folder; returns the first room's path.
QString WriteRooms(const QTemporaryDir& directory)
{
    REQUIRE(directory.isValid());
    const QDir folder(QFileInfo(directory.path()).canonicalFilePath());
    Write(folder.filePath(QStringLiteral("second-room.world.json")), ludus::world_demo::BuiltinLevel("second-room"));
    const QString first = folder.filePath(QStringLiteral("first-room.world.json"));
    Write(first, ludus::world_demo::BuiltinLevel("first-room"));
    return first;
}

template <typename Widget>
Widget* Find(const QWidget& parent, const QString& name)
{
    Widget* widgetOrNull = parent.findChild<Widget*>(name);
    REQUIRE(widgetOrNull != nullptr);
    return widgetOrNull;
}

void Click(const QWidget& parent, const QString& name)
{
    QPushButton* button = Find<QPushButton>(parent, name);
    REQUIRE(button->isEnabled() == true);
    QTest::mouseClick(button, Qt::LeftButton);
}

void SelectPlacement(const WorldScatterPanel& panel, ScatterIdentity identity)
{
    QTreeWidget* list = Find<QTreeWidget>(panel, QStringLiteral("scatterPlacements"));
    const qulonglong packed = procedural::PackScatterScope(identity);
    for (int32 row = 0; row < list->topLevelItemCount(); ++row)
    {
        if (list->topLevelItem(row)->data(0, Qt::UserRole).toULongLong() == packed)
        {
            list->setCurrentItem(list->topLevelItem(row));
            return;
        }
    }
    FAIL("Missing scatter placement");
}

bool AreEqual(const ScatterPlacement& a, const ScatterPlacement& b)
{
    return a.Identity == b.Identity && a.LatticeX == b.LatticeX && a.LatticeY == b.LatticeY && a.Variant == b.Variant &&
           a.Rotation == b.Rotation && a.Scale == b.Scale;
}

void RequireSameManifest(const ScatterManifest& expected, const ScatterManifest& actual)
{
    REQUIRE(actual.Digest == expected.Digest);
    REQUIRE(actual.Placements.GetSize() == expected.Placements.GetSize());
    for (usize i = 0; i < expected.Placements.GetSize(); ++i)
    {
        REQUIRE(AreEqual(actual.Placements[i], expected.Placements[i]) == true);
    }
}

/// The outcome the trace list shows for one room's layers: every row of the room, in order.
QStringList GetTraceOutcomes(const WorldScatterPanel& panel, const QString& room)
{
    QTreeWidget* trace = Find<QTreeWidget>(panel, QStringLiteral("scatterTrace"));
    QStringList outcomes;
    for (int32 row = 0; row < trace->topLevelItemCount(); ++row)
    {
        if (trace->topLevelItem(row)->text(0) == room)
        {
            outcomes.append(trace->topLevelItem(row)->text(1) + QStringLiteral(": ") +
                            trace->topLevelItem(row)->text(2));
        }
    }
    return outcomes;
}
} // namespace

TEST_CASE("Scatter tab refuses a stale override, names the conflict and repairs it",
          "[editor][world][world-demo][scatter]")
{
    QTemporaryDir directory;
    const QString path = WriteRooms(directory);
    WorldWorkspace view;
    view.SetProject(directory.path(), 1);
    REQUIRE(view.Open(path));
    REQUIRE(IsWorldDemo(view.Document()));
    WorldScatterPanel* panelOrNull = view.findChild<WorldScatterPanel*>();
    REQUIRE(panelOrNull != nullptr);
    WorldScatterPanel& panel = *panelOrNull;
    QTabWidget* lists = Find<QTabWidget>(view, QStringLiteral("worldSidebarLists"));
    CHECK(lists->isTabVisible(lists->indexOf(&panel)) == true);
    // The open room comes from the draft and the sibling room from its file, in ascending room order.
    const ScatterGenerationSession& session = panel.GetSession();
    REQUIRE(session.GetRoomCount() == 2);
    REQUIRE(session.GetRecipe(0).Room == FIRST_ROOM);
    REQUIRE(session.GetRecipe(1).Room == SECOND_ROOM);

    // Stage and publish the generated placements as the accepted baseline.
    CHECK(Find<QPushButton>(panel, QStringLiteral("scatterPublish"))->isEnabled() == false);
    Click(panel, QStringLiteral("scatterStage"));
    REQUIRE(session.HasPendingCandidate() == true);
    Click(panel, QStringLiteral("scatterPublish"));
    REQUIRE(session.GetAcceptedRevision() == 1);
    const ScatterManifest accepted = session.GetAccepted();
    REQUIRE(accepted.Placements.IsEmpty() == false);
    QTreeWidget* placements = Find<QTreeWidget>(panel, QStringLiteral("scatterPlacements"));
    REQUIRE(placements->topLevelItemCount() == static_cast<int32>(accepted.Placements.GetSize()));

    // Override the rotation of one second-room placement and stage a candidate with it.
    const ScatterPlacement target = accepted.Placements.GetLast();
    REQUIRE(target.Identity.Room == SECOND_ROOM);
    SelectPlacement(panel, target.Identity);
    QSpinBox* rotation = Find<QSpinBox>(panel, QStringLiteral("scatterOverride:rotation"));
    REQUIRE(rotation->isEnabled() == true);
    REQUIRE(rotation->value() == target.Rotation);
    const uint16 staged = static_cast<uint16>((target.Rotation + QUARTER_TURN) % ROTATION_UNITS);
    rotation->setValue(staged);
    Click(panel, QStringLiteral("scatterApplyOverride"));
    REQUIRE(session.FindOverrideOrNull(target.Identity) != nullptr);
    Click(panel, QStringLiteral("scatterStage"));
    REQUIRE(session.GetCandidate().Status == procedural::GenerationStatus::Success);
    REQUIRE(session.GetCandidate().Manifest.FindPlacement(target.Identity)->Rotation == staged);

    // Edit the override while the candidate is pending: publishing is refused and the accepted World is unchanged.
    const uint16 authored = static_cast<uint16>((staged + EIGHTH_TURN) % ROTATION_UNITS);
    SelectPlacement(panel, target.Identity);
    rotation->setValue(authored);
    Click(panel, QStringLiteral("scatterApplyOverride"));
    Click(panel, QStringLiteral("scatterPublish"));
    REQUIRE(session.GetAcceptedRevision() == 1);
    RequireSameManifest(accepted, session.GetAccepted());
    REQUIRE(session.HasPendingCandidate() == true);
    CHECK(Find<QLabel>(panel, QStringLiteral("scatterStatus"))->text().contains(QStringLiteral("Publication refused")));

    // The conflict names the placement and its rotation, with the authored and the candidate values.
    QWidget* conflicts = Find<QWidget>(panel, QStringLiteral("scatterConflicts"));
    REQUIRE(conflicts->isHidden() == false);
    QTreeWidget* conflictList = Find<QTreeWidget>(panel, QStringLiteral("scatterConflictList"));
    REQUIRE(conflictList->topLevelItemCount() == 1);
    const QTreeWidgetItem* conflict = conflictList->topLevelItem(0);
    CHECK(conflict->text(0) == panel.DescribePlacement(target.Identity));
    CHECK(conflict->text(0).startsWith(QStringLiteral("second-room · ")));
    CHECK(conflict->text(1) == QStringLiteral("Rotation"));
    ScatterPlacement authoredPlacement = target;
    authoredPlacement.Rotation = authored;
    ScatterPlacement stagedPlacement = target;
    stagedPlacement.Rotation = staged;
    CHECK(conflict->text(2) ==
          DescribeScatterValue(procedural::ScatterOverrideProperty::Rotation, true, authoredPlacement));
    CHECK(conflict->text(3) ==
          DescribeScatterValue(procedural::ScatterOverrideProperty::Rotation, true, stagedPlacement));
    CHECK(conflict->text(2).startsWith(QString::number(authored) + QStringLiteral(" (")));
    CHECK(Find<QPushButton>(panel, QStringLiteral("scatterRetry"))->isHidden() == false);

    SECTION("keeping the authored value restages only the edited room's layer and publishes it")
    {
        Click(panel, QStringLiteral("scatterRetry"));
        REQUIRE(session.GetAcceptedRevision() == 2);
        REQUIRE(session.GetAccepted().FindPlacement(target.Identity)->Rotation == authored);
        REQUIRE(session.HasPendingCandidate() == false);
        CHECK(conflicts->isHidden() == true);
        // The first room is all cache hits; in the second room only the overridden layer ran.
        const QStringList first = GetTraceOutcomes(panel, QStringLiteral("first-room"));
        REQUIRE(first.isEmpty() == false);
        for (const QString& outcome : first)
        {
            CHECK(outcome.endsWith(QStringLiteral(": Cache hit")));
        }
        const QStringList second = GetTraceOutcomes(panel, QStringLiteral("second-room"));
        REQUIRE(second.size() == 2);
        CHECK(second.filter(QStringLiteral(": Executed")).size() == 1);
        CHECK(second.filter(QStringLiteral(": Cache hit")).size() == 1);
        for (const ScatterPlacement& placement : accepted.Placements)
        {
            const ScatterPlacement* publishedOrNull = session.GetAccepted().FindPlacement(placement.Identity);
            if (placement.Identity.Room == FIRST_ROOM)
            {
                REQUIRE(publishedOrNull != nullptr);
                CHECK(AreEqual(*publishedOrNull, placement) == true);
            }
        }
        CHECK(Find<QLabel>(panel, QStringLiteral("scatterStatus"))->text().contains(QStringLiteral("revision 2")));
    }
    SECTION("rejecting the proposal keeps the accepted state and the override until a valid publication")
    {
        Click(panel, QStringLiteral("scatterRejectProposal"));
        REQUIRE(session.HasPendingCandidate() == false);
        REQUIRE(session.GetAcceptedRevision() == 1);
        RequireSameManifest(accepted, session.GetAccepted());
        CHECK(conflicts->isHidden() == true);
        CHECK(Find<QPushButton>(panel, QStringLiteral("scatterPublish"))->isEnabled() == false);
        REQUIRE(session.FindOverrideOrNull(target.Identity) != nullptr);
        REQUIRE(session.FindOverrideOrNull(target.Identity)->Rotation == authored);
        // A fresh candidate of the current input publishes the authored value.
        Click(panel, QStringLiteral("scatterStage"));
        Click(panel, QStringLiteral("scatterPublish"));
        REQUIRE(session.GetAcceptedRevision() == 2);
        REQUIRE(session.GetAccepted().FindPlacement(target.Identity)->Rotation == authored);
    }
}

TEST_CASE("Scatter tab names an override it cannot honour and offers the edit", "[editor][world][world-demo][scatter]")
{
    QTemporaryDir directory;
    const QString path = WriteRooms(directory);
    WorldWorkspace view;
    view.SetProject(directory.path(), 1);
    REQUIRE(view.Open(path));
    WorldScatterPanel* panelOrNull = view.findChild<WorldScatterPanel*>();
    REQUIRE(panelOrNull != nullptr);
    WorldScatterPanel& panel = *panelOrNull;
    const ScatterGenerationSession& session = panel.GetSession();
    Click(panel, QStringLiteral("scatterStage"));
    Click(panel, QStringLiteral("scatterPublish"));
    const ScatterManifest accepted = session.GetAccepted();

    // Lock a first-room placement inside the keep-out around the room's first collision box, which lies in the
    // placement's cell: the candidate cannot honour it and cannot publish.
    const ludus::world_demo::Level& level = session.GetLevel(0);
    const procedural::ScatterRecipe& recipe = session.GetRecipe(0);
    REQUIRE(recipe.KeepOuts.IsEmpty() == false);
    const ScatterPlacement* targetOrNull = nullptr;
    procedural::ScatterRectangle keepOut;
    for (const ScatterPlacement& placement : accepted.Placements)
    {
        const procedural::ScatterLayer* layerOrNull =
            session.FindLayerOrNull(placement.Identity.Room, placement.Identity.Layer);
        REQUIRE(layerOrNull != nullptr);
        const int32 cellX = static_cast<int32>(placement.Identity.CellX) * layerOrNull->CellSize;
        const int32 cellY = static_cast<int32>(placement.Identity.CellY) * layerOrNull->CellSize;
        for (const procedural::ScatterRectangle& rectangle : recipe.KeepOuts)
        {
            if (placement.Identity.Room == FIRST_ROOM && rectangle.MinimumX >= recipe.Region.MinimumX &&
                rectangle.MinimumY >= recipe.Region.MinimumY && rectangle.MinimumX < recipe.Region.MaximumX &&
                rectangle.MinimumY < recipe.Region.MaximumY && rectangle.MinimumX >= cellX &&
                rectangle.MinimumX < cellX + layerOrNull->CellSize && rectangle.MinimumY >= cellY &&
                rectangle.MinimumY < cellY + layerOrNull->CellSize)
            {
                targetOrNull = &placement;
                keepOut = rectangle;
                break;
            }
        }
        if (targetOrNull != nullptr)
        {
            break;
        }
    }
    REQUIRE(level.HasScatter == true);
    REQUIRE(targetOrNull != nullptr);
    SelectPlacement(panel, targetOrNull->Identity);
    Find<QSpinBox>(panel, QStringLiteral("scatterOverride:x"))->setValue(keepOut.MinimumX);
    Find<QSpinBox>(panel, QStringLiteral("scatterOverride:y"))->setValue(keepOut.MinimumY);
    Click(panel, QStringLiteral("scatterApplyOverride"));
    Click(panel, QStringLiteral("scatterStage"));
    REQUIRE(session.GetCandidate().Status == procedural::GenerationStatus::ConstraintFailed);
    CHECK(Find<QPushButton>(panel, QStringLiteral("scatterPublish"))->isEnabled() == false);
    QWidget* conflicts = Find<QWidget>(panel, QStringLiteral("scatterConflicts"));
    REQUIRE(conflicts->isHidden() == false);
    CHECK(Find<QPushButton>(panel, QStringLiteral("scatterRetry"))->isHidden() == true);
    QTreeWidget* conflictList = Find<QTreeWidget>(panel, QStringLiteral("scatterConflictList"));
    REQUIRE(conflictList->topLevelItemCount() == 1);
    CHECK(conflictList->topLevelItem(0)->text(0) == panel.DescribePlacement(targetOrNull->Identity));
    CHECK(conflictList->topLevelItem(0)->text(3).startsWith(QStringLiteral("Inside keep-out")));
    REQUIRE(session.GetAcceptedRevision() == 1);
    RequireSameManifest(accepted, session.GetAccepted());

    // Removing the override is the repair: the next candidate equals the accepted placements and publishes.
    SelectPlacement(panel, targetOrNull->Identity);
    Click(panel, QStringLiteral("scatterRemoveOverride"));
    Click(panel, QStringLiteral("scatterStage"));
    REQUIRE(session.GetCandidate().Status == procedural::GenerationStatus::Success);
    CHECK(conflicts->isHidden() == true);
    Click(panel, QStringLiteral("scatterPublish"));
    REQUIRE(session.GetAcceptedRevision() == 2);
    RequireSameManifest(accepted, session.GetAccepted());
}
