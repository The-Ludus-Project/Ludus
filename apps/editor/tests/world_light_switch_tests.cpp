#include "internal/level_workspace.h"
#include "internal/light_switches.h"
#include "internal/preview_lighting.h"
#include "internal/render_graph_preview.h"
#include "internal/world_lights_panel.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QScrollBar>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace renderer = ludus::graphics::renderer;

constexpr int32 SETTLE_TIMEOUT_MILLISECONDS = 10000;

void WriteFixtureFile(const QString& path, const QByteArray& bytes)
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()) == true);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}

/// Two emitter groups (one specified by power) with different radiance, one point light and a floor.
void WriteSwitchSource(const QString& path)
{
    WriteFixtureFile(path,
                     QByteArrayLiteral("LookAt 0 2 5  0 0 0  0 1 0\n"
                                       "Camera \"perspective\" \"float fov\" [45]\nWorldBegin\n"
                                       "LightSource \"point\" \"rgb I\" [3 1 2] \"point3 from\" [0 2 0]\n"
                                       "AttributeBegin\n"
                                       "AreaLightSource \"diffuse\" \"rgb L\" [4 4 4]\n"
                                       "Shape \"trianglemesh\" \"point3 P\" [-1 2 -1  -1 2 0  0 2 0  0 2 -1]\n"
                                       "\"integer indices\" [0 1 2  0 2 3]\n"
                                       "AttributeEnd\n"
                                       "AttributeBegin\n"
                                       "AreaLightSource \"diffuse\" \"rgb L\" [1 2 1] \"float power\" [20]\n"
                                       "Shape \"trianglemesh\" \"point3 P\" [1 2 1  1 2 2  2 2 2  2 2 1]\n"
                                       "\"integer indices\" [0 1 2  0 2 3]\n"
                                       "AttributeEnd\n"
                                       "Material \"diffuse\" \"rgb reflectance\" [.6 .5 .4]\n"
                                       "Shape \"trianglemesh\" \"point3 P\" [-3 0 -3  3 0 -3  3 0 3  -3 0 3]\n"
                                       "\"integer indices\" [0 1 2  0 2 3]\n"));
    WriteFixtureFile(QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral("LICENSE.txt")),
                     QByteArrayLiteral("CC0 synthetic fixture\n"));
}

void WaitForWorld(WorldWorkspace& inoutWorkspace)
{
    REQUIRE(QTest::qWaitFor([&inoutWorkspace]() { return inoutWorkspace.CanSave() == true; },
                            SETTLE_TIMEOUT_MILLISECONDS) == true);
}

ToolingPaths MakeSceneTools()
{
    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    return tools;
}

void CreateImportedWorld(const QString& root, const QString& path, const QString& source)
{
    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(path) == true);
    REQUIRE(workspace.ImportScene(source) == true);
    WaitForWorld(workspace);
    REQUIRE(workspace.Save() == true);
    WaitForWorld(workspace);
    REQUIRE(workspace.Dirty() == false);
    workspace.StopPreview();
}

/// Row index of the first row of `origin` in the panel, or -1.
qsizetype FindRow(const WorldLightsPanel& panel, WorldLightOrigin origin, qsizetype skip = 0)
{
    const QVector<WorldLightRow>& rows = panel.GetRows();
    for (qsizetype i = 0; i < rows.size(); ++i)
    {
        if (rows[i].Origin == origin && skip-- == 0)
        {
            return i;
        }
    }
    return -1;
}

QTreeWidgetItem* FindItem(QTreeWidget& inoutTree, qsizetype row)
{
    for (QTreeWidgetItemIterator it(&inoutTree); *it != nullptr; ++it)
    {
        if ((*it)->data(0, Qt::UserRole).isValid() == true && (*it)->data(0, Qt::UserRole).toLongLong() == row)
        {
            return *it;
        }
    }
    return nullptr;
}

void SetRowChecked(WorldLightsPanel& inoutPanel, qsizetype row, bool isChecked)
{
    QTreeWidget* treeOrNull = inoutPanel.findChild<QTreeWidget*>(QStringLiteral("worldLights"));
    REQUIRE(treeOrNull != nullptr);
    QTreeWidgetItem* itemOrNull = FindItem(*treeOrNull, row);
    REQUIRE(itemOrNull != nullptr);
    itemOrNull->setCheckState(0, isChecked == true ? Qt::Checked : Qt::Unchecked);
}

void ClickCommand(WorldLightsPanel& inoutPanel, const QString& name, qsizetype selectedRow = -1)
{
    if (selectedRow >= 0)
    {
        QTreeWidget* treeOrNull = inoutPanel.findChild<QTreeWidget*>(QStringLiteral("worldLights"));
        REQUIRE(treeOrNull != nullptr);
        QTreeWidgetItem* itemOrNull = FindItem(*treeOrNull, selectedRow);
        REQUIRE(itemOrNull != nullptr);
        treeOrNull->clearSelection();
        itemOrNull->setSelected(true);
    }
    QPushButton* buttonOrNull = inoutPanel.findChild<QPushButton*>(name);
    REQUIRE(buttonOrNull != nullptr);
    REQUIRE(buttonOrNull->isEnabled() == true);
    buttonOrNull->click();
}

/// Whether every surface of the emitter group `sourceId` presents zero emission and no emission power.
bool IsEmitterSwitchedOff(const RenderScene& scene, uint64 sourceId)
{
    bool hasSurface = false;
    for (usize i = 0; i < scene.Definition->Materials.GetSize(); ++i)
    {
        if (scene.Definition->Materials[i].AreaSourceId != sourceId)
        {
            continue;
        }
        hasSurface = true;
        const PreviewSurface& surface = scene.Surfaces[static_cast<qsizetype>(i)];
        if (surface.Emission != math::Vector3{} || surface.HasEmissionPower == true)
        {
            return false;
        }
    }
    return hasSurface;
}

/// Whether the emitter group's surfaces present their cooked emission.
bool IsEmitterOn(const RenderScene& scene, uint64 sourceId)
{
    for (usize i = 0; i < scene.Definition->Materials.GetSize(); ++i)
    {
        const ludus::resources::SceneMaterial& material = scene.Definition->Materials[i];
        if (material.AreaSourceId == sourceId &&
            (scene.Surfaces[static_cast<qsizetype>(i)].Emission == material.Emission) == false)
        {
            return false;
        }
    }
    return true;
}

/// The render-graph extraction: no error material, and `sourceId` emits nothing in any resolved material.
void CheckExtraction(const RenderScene& scene, uint64 switchedOffSourceIdOrZero, usize expectedLightCount)
{
    RenderGraphExtraction extraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(scene, {}, 1, extraction, error) == true);
    CHECK(extraction.ShaderInputs.Lights.GetSize() == expectedLightCount);
    for (qsizetype i = 0; i < extraction.Scene.Objects.size(); ++i)
    {
        const PreviewObject& object = extraction.Scene.Objects[i];
        const renderer::SceneShaderMaterial& material = extraction.Materials[extraction.MaterialOverrides[i]];
        CHECK(material.HasError == false);
        if (switchedOffSourceIdOrZero != 0 && object.Surface >= 0 &&
            scene.Definition->Materials[static_cast<usize>(object.Surface)].AreaSourceId == switchedOffSourceIdOrZero)
        {
            CHECK(material.Emission == math::Vector3{});
        }
    }
}
} // namespace

TEST_CASE("Light source names round-trip through their World spelling", "[editor][world][lighting][light-switches]")
{
    uint64 parsed = 7;
    CHECK(FormatLightSourceId(0x0123456789abcdefULL) == QStringLiteral("0123456789abcdef"));
    CHECK(TryParseLightSourceId(QStringLiteral("0123456789abcdef"), parsed) == true);
    CHECK(parsed == 0x0123456789abcdefULL);
    for (const QString& invalid : {QStringLiteral("0123456789ABCDEF"),
                                   QStringLiteral("0123456789abcde"),
                                   QStringLiteral("0123456789abcdef0"),
                                   QStringLiteral("0123456789abcdeg"),
                                   QString()})
    {
        uint64 preserved = 7;
        CHECK(TryParseLightSourceId(invalid, preserved) == false);
        CHECK(preserved == 7);
    }
    LightSwitchSet set;
    set.Disabled = {{LightSourceKind::Light, 3},
                    {LightSourceKind::Emitter, 9},
                    {LightSourceKind::Emitter, 2},
                    {LightSourceKind::Emitter, 9},
                    {LightSourceKind::Emitter, 0}};
    NormalizeLightSwitches(set);
    const QVector<LightSourceKey> expected{{LightSourceKind::Emitter, 2},
                                           {LightSourceKind::Emitter, 9},
                                           {LightSourceKind::Light, 3}};
    CHECK(set.Disabled == expected);
}

TEST_CASE("Imported emitters with a generated or missing object name read as numbered area lights",
          "[editor][world][lighting][light-switches]")
{
    CHECK(GetEmitterLabel(QStringLiteral("Imported object 3"), 2) == QStringLiteral("Area light 2"));
    CHECK(GetEmitterLabel(QString(), 1) == QStringLiteral("Area light 1"));
    CHECK(GetEmitterLabel(QStringLiteral("Ceiling panel"), 1) == QStringLiteral("Ceiling panel"));
    CHECK(GetEmitterLabel(QStringLiteral("Imported object 3 north"), 4) == QStringLiteral("Imported object 3 north"));
}

TEST_CASE("World light switches turn imported emitters and lights off without a re-cook",
          "[editor][world][lighting][light-switches]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString projectPath = QDir(directory.path()).filePath(QStringLiteral("project"));
    REQUIRE(QDir().mkpath(projectPath) == true);
    const QString root = QFileInfo(projectPath).canonicalFilePath();
    const QString source = directory.filePath(QStringLiteral("source/scene.pbrt"));
    WriteSwitchSource(source);
    const QString path = QDir(root).filePath(QStringLiteral("switches.world.json"));
    CreateImportedWorld(root, path, source);

    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    LevelWorkspace* levelOrNull = workspace.findChild<LevelWorkspace*>();
    WorldLightsPanel* panelOrNull = workspace.findChild<WorldLightsPanel*>();
    REQUIRE(levelOrNull != nullptr);
    REQUIRE(panelOrNull != nullptr);
    REQUIRE(workspace.Open(path) == true);
    WaitForWorld(workspace);
    INFO(levelOrNull->StatusText().toStdString());

    const RenderScene original = levelOrNull->PreviewScene();
    REQUIRE(original.Definition != nullptr);
    REQUIRE(original.AuthoredLights.Count == 1);
    const QByteArray originalKey = preview_lighting::MakeKey(original);
    const qsizetype ceiling = FindRow(*panelOrNull, WorldLightOrigin::ImportedEmitter, 0);
    const qsizetype powered = FindRow(*panelOrNull, WorldLightOrigin::ImportedEmitter, 1);
    const qsizetype point = FindRow(*panelOrNull, WorldLightOrigin::ImportedLight);
    REQUIRE(ceiling >= 0);
    REQUIRE(powered >= 0);
    REQUIRE(point >= 0);
    CHECK(FindRow(*panelOrNull, WorldLightOrigin::ImportedEmitter, 2) < 0);
    const LightSourceKey ceilingKey = panelOrNull->GetRows()[ceiling].Key;
    const LightSourceKey poweredKey = panelOrNull->GetRows()[powered].Key;
    const LightSourceKey pointKey = panelOrNull->GetRows()[point].Key;
    CHECK(original.UnswitchedIdentity.isEmpty() == true);
    CHECK(original.LightSwitchIdentity.isEmpty() == true);
    CheckExtraction(original, 0, 1);

    // One emitter off: presented at once from the same definition, without a cook.
    SetRowChecked(*panelOrNull, ceiling, false);
    CHECK(levelOrNull->Busy() == false);
    CHECK(levelOrNull->GetPlacementValidation() == PlacementValidation::Ready);
    const RenderScene& ceilingOff = levelOrNull->PreviewScene();
    CHECK(ceilingOff.Definition.Get() == original.Definition.Get());
    CHECK(IsEmitterSwitchedOff(ceilingOff, ceilingKey.SourceId) == true);
    CHECK(IsEmitterOn(ceilingOff, poweredKey.SourceId) == true);
    CHECK(ceilingOff.AuthoredLights.Count == 1);
    CHECK(ceilingOff.Identity != original.Identity);
    CHECK(ceilingOff.UnswitchedIdentity == original.Identity);
    CHECK(preview_lighting::MakeKey(ceilingOff) != originalKey);
    CheckExtraction(ceilingOff, ceilingKey.SourceId, 1);
    CHECK(workspace.Document().GetLightSwitches().Disabled == QVector<LightSourceKey>{ceilingKey});
    CHECK(workspace.Dirty() == true);
    CHECK(workspace.CanUndo() == true);

    // The power-specified emitter resolves no error material when its colour is switched off.
    SetRowChecked(*panelOrNull, powered, false);
    CHECK(IsEmitterSwitchedOff(levelOrNull->PreviewScene(), poweredKey.SourceId) == true);
    CheckExtraction(levelOrNull->PreviewScene(), poweredKey.SourceId, 1);

    // The analytic light leaves the presented light table but stays in the definition.
    SetRowChecked(*panelOrNull, point, false);
    CHECK(levelOrNull->PreviewScene().AuthoredLights.Count == 0);
    CHECK(levelOrNull->PreviewScene().Definition->Lights.GetSize() == 1);
    CheckExtraction(levelOrNull->PreviewScene(), poweredKey.SourceId, 0);
    preview_lighting::Inputs direct;
    const RenderScene& allOff = levelOrNull->PreviewScene();
    REQUIRE(preview_lighting::BuildInputs(allOff,
                                          preview_lighting::MakeKey(allOff),
                                          direct,
                                          preview_lighting::InputsTarget::Reference) == rhi::RasterStatus::Ready);
    CHECK(direct.Lights.Count == 0);

    // Solo keeps only the ceiling emitter on, as one undo step.
    ClickCommand(*panelOrNull, QStringLiteral("worldLightsSolo"), ceiling);
    const LightSwitchSet solo = workspace.Document().GetLightSwitches();
    CHECK(solo.Disabled == QVector<LightSourceKey>{poweredKey, pointKey});
    CHECK(IsEmitterOn(levelOrNull->PreviewScene(), ceilingKey.SourceId) == true);
    CHECK(IsEmitterSwitchedOff(levelOrNull->PreviewScene(), poweredKey.SourceId) == true);
    CHECK(levelOrNull->PreviewScene().AuthoredLights.Count == 0);
    const QByteArray soloIdentity = levelOrNull->PreviewScene().Identity;

    // Enable All restores the unswitched identity and lighting key exactly.
    ClickCommand(*panelOrNull, QStringLiteral("worldLightsEnableAll"));
    CHECK(workspace.Document().GetLightSwitches().Disabled.isEmpty() == true);
    CHECK(workspace.Document().Draft().contains(QStringLiteral("disabled_lights")) == false);
    CHECK(levelOrNull->PreviewScene().Identity == original.Identity);
    CHECK(levelOrNull->PreviewScene().UnswitchedIdentity.isEmpty() == true);
    CHECK(preview_lighting::MakeKey(levelOrNull->PreviewScene()) == originalKey);
    CHECK(levelOrNull->PreviewScene().AuthoredLights.Count == 1);

    // Undo returns to the Solo switches and their identity.
    workspace.Undo();
    CHECK(workspace.Document().GetLightSwitches() == solo);
    CHECK(levelOrNull->PreviewScene().Identity == soloIdentity);

    // Saved switches survive reopening, which assembles and decodes the scene again.
    REQUIRE(workspace.Save() == true);
    WaitForWorld(workspace);
    REQUIRE(workspace.Dirty() == false);
    REQUIRE(workspace.Open(path) == true);
    WaitForWorld(workspace);
    CHECK(workspace.Document().GetLightSwitches() == solo);
    const RenderScene& reopened = levelOrNull->PreviewScene();
    CHECK(IsEmitterOn(reopened, ceilingKey.SourceId) == true);
    CHECK(IsEmitterSwitchedOff(reopened, poweredKey.SourceId) == true);
    CHECK(reopened.AuthoredLights.Count == 0);
    CHECK(reopened.Identity == soloIdentity);
    workspace.StopPreview();
}

TEST_CASE("A World keeps opening when it switches off lights its scene no longer has",
          "[editor][world][lighting][light-switches]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString projectPath = QDir(directory.path()).filePath(QStringLiteral("project"));
    REQUIRE(QDir().mkpath(projectPath) == true);
    const QString root = QFileInfo(projectPath).canonicalFilePath();
    const QString source = directory.filePath(QStringLiteral("source/scene.pbrt"));
    WriteSwitchSource(source);
    const QString path = QDir(root).filePath(QStringLiteral("missing.world.json"));
    CreateImportedWorld(root, path, source);
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    QJsonObject world = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    world.insert(QStringLiteral("disabled_lights"),
                 QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("emitter")},
                                        {QStringLiteral("source"), QStringLiteral("00000000000000ff")}}});
    WriteFixtureFile(path, QJsonDocument(world).toJson(QJsonDocument::Compact) + '\n');

    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    LevelWorkspace* levelOrNull = workspace.findChild<LevelWorkspace*>();
    WorldLightsPanel* panelOrNull = workspace.findChild<WorldLightsPanel*>();
    REQUIRE(levelOrNull != nullptr);
    REQUIRE(panelOrNull != nullptr);
    REQUIRE(workspace.Open(path) == true);
    WaitForWorld(workspace);
    INFO(levelOrNull->StatusText().toStdString());
    REQUIRE(levelOrNull->PreviewScene().Definition != nullptr);
    // The stale switch changes nothing the scene presents.
    CHECK(levelOrNull->PreviewScene().LightSwitchIdentity.isEmpty() == true);
    const qsizetype missing = FindRow(*panelOrNull, WorldLightOrigin::Missing);
    REQUIRE(missing >= 0);
    CHECK(panelOrNull->GetRows()[missing].Key.SourceId == 0xffU);
    ClickCommand(*panelOrNull, QStringLiteral("worldLightsRemoveMissing"));
    CHECK(workspace.Document().GetLightSwitches().Disabled.isEmpty() == true);
    CHECK(FindRow(*panelOrNull, WorldLightOrigin::Missing) < 0);
    workspace.StopPreview();
}

TEST_CASE("World light switch lists are validated before they are admitted",
          "[editor][world][lighting][light-switches]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString projectPath = QDir(directory.path()).filePath(QStringLiteral("project"));
    REQUIRE(QDir().mkpath(projectPath) == true);
    const QString root = QFileInfo(projectPath).canonicalFilePath();
    const QString source = directory.filePath(QStringLiteral("source/scene.pbrt"));
    WriteSwitchSource(source);
    const QString path = QDir(root).filePath(QStringLiteral("validation.world.json"));
    CreateImportedWorld(root, path, source);
    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Open(path) == true);
    WaitForWorld(workspace);
    WorldDocument document = workspace.Document();
    const QJsonObject saved = document.Draft();

    const auto makeEntry = [](const QJsonValue& kind, const QJsonValue& sourceId) {
        return QJsonObject{{QStringLiteral("kind"), kind}, {QStringLiteral("source"), sourceId}};
    };
    const auto load = [&document, &saved](const QJsonValue& list, bool shouldKeepScene) {
        QJsonObject candidate = saved;
        candidate.insert(QStringLiteral("disabled_lights"), list);
        if (shouldKeepScene == false)
        {
            candidate.remove(QStringLiteral("scene"));
            QJsonArray entities;
            candidate.insert(QStringLiteral("entities"), entities);
        }
        QString error;
        return document.Load(QJsonDocument(candidate).toJson(QJsonDocument::Compact), error);
    };
    const QJsonObject valid = makeEntry(QStringLiteral("emitter"), QStringLiteral("00000000000000ff"));
    CHECK(load(QJsonArray{valid, makeEntry(QStringLiteral("light"), QStringLiteral("00000000000000ff"))}, true) ==
          true);
    CHECK(document.GetLightSwitches().Disabled.size() == 2);
    CHECK(load(QJsonArray{valid}, false) == false);
    CHECK(load(QJsonArray{valid, valid}, true) == false);
    CHECK(load(QJsonArray{makeEntry(QStringLiteral("emitter"), QStringLiteral("0000000000000000"))}, true) == false);
    CHECK(load(QJsonArray{makeEntry(QStringLiteral("emitter"), QStringLiteral("00000000000000FF"))}, true) == false);
    CHECK(load(QJsonArray{makeEntry(QStringLiteral("sun"), QStringLiteral("00000000000000ff"))}, true) == false);
    CHECK(load(QJsonArray{makeEntry(QStringLiteral("emitter"), 255)}, true) == false);
    QJsonObject extra = valid;
    extra.insert(QStringLiteral("enabled"), false);
    CHECK(load(QJsonArray{extra}, true) == false);
    CHECK(load(QJsonObject{}, true) == false);
    QJsonArray tooMany;
    for (usize i = 0; i <= GetMaxDisabledLightSources(); ++i)
    {
        tooMany.append(makeEntry(QStringLiteral("emitter"), FormatLightSourceId(i + 1)));
    }
    CHECK(load(tooMany, true) == false);

    // An empty set removes the list, an unchanged one adds no history, and a scene-less World refuses switches.
    REQUIRE(load(QJsonArray{valid}, true) == true);
    QString error;
    REQUIRE(document.SetLightSwitches({}, {}, {}, error) == true);
    CHECK(document.Draft() == saved);
    CHECK(document.CanUndo() == true);
    document.AcknowledgeSave();
    REQUIRE(document.Undo() == true);
    CHECK(document.Draft().contains(QStringLiteral("disabled_lights")) == true);
    LightSwitchSet same = document.GetLightSwitches();
    REQUIRE(document.Redo() == true);
    REQUIRE(document.Undo() == true);
    const bool canRedoBeforeEdit = document.CanRedo();
    REQUIRE(document.SetLightSwitches(same, {}, {}, error) == true);
    CHECK(document.CanRedo() == canRedoBeforeEdit);
    WorldDocument sceneless = workspace.Document();
    REQUIRE(sceneless.New(error) == true);
    CHECK(sceneless.SetLightSwitches(same, {}, {}, error) == false);
    CHECK(sceneless.Draft().contains(QStringLiteral("disabled_lights")) == false);
    workspace.StopPreview();
}

TEST_CASE("Switching a light keeps the light list's scroll position and selection",
          "[editor][world][lighting][light-switches]")
{
    constexpr qsizetype ROW_COUNT = 40;
    constexpr int32 LIST_WIDTH = 240;
    constexpr int32 LIST_HEIGHT = 200;
    QVector<WorldLightRow> rows;
    for (qsizetype i = 0; i < ROW_COUNT; ++i)
    {
        WorldLightRow row;
        row.Origin = WorldLightOrigin::ImportedEmitter;
        row.Key = {LightSourceKind::Emitter, static_cast<uint64>(i + 1)};
        row.Label = QStringLiteral("Imported object %1").arg(i);
        rows.append(row);
    }
    // The panel hides itself while it has no rows, so it lives in a window of its own.
    QWidget window;
    QVBoxLayout* layout = new QVBoxLayout(&window);
    WorldLightsPanel* panelOrNull = new WorldLightsPanel(&window);
    layout->addWidget(panelOrNull);
    WorldLightsPanel& panel = *panelOrNull;
    window.resize(LIST_WIDTH, LIST_HEIGHT);
    panel.SetRows(rows, {});
    window.show();
    QTreeWidget* treeOrNull = panel.findChild<QTreeWidget*>(QStringLiteral("worldLights"));
    REQUIRE(treeOrNull != nullptr);
    REQUIRE(QTest::qWaitForWindowExposed(&window) == true);
    QScrollBar* scrollOrNull = treeOrNull->verticalScrollBar();
    REQUIRE(scrollOrNull->maximum() > 0);
    const qsizetype selectedRow = ROW_COUNT / 2;
    QTreeWidgetItem* selectedOrNull = FindItem(*treeOrNull, selectedRow);
    REQUIRE(selectedOrNull != nullptr);
    treeOrNull->scrollToItem(selectedOrNull, QAbstractItemView::PositionAtTop);
    selectedOrNull->setSelected(true);
    const int32 scrolled = scrollOrNull->value();
    REQUIRE(scrolled > 0);

    // The workspace answers a switch by presenting the same lights with one state changed.
    rows[selectedRow].IsEnabled = false;
    panel.SetRows(rows, {{{LightSourceKind::Emitter, rows[selectedRow].Key.SourceId}}});
    CHECK(scrollOrNull->value() == scrolled);
    CHECK(FindItem(*treeOrNull, selectedRow) == selectedOrNull);
    CHECK(selectedOrNull->isSelected() == true);
    CHECK(selectedOrNull->checkState(0) == Qt::Unchecked);

    // A changed list is recreated with the same light at the top, even across an empty list.
    panel.SetRows({}, {});
    WorldLightRow added;
    added.Origin = WorldLightOrigin::ImportedEmitter;
    added.Key = {LightSourceKind::Emitter, static_cast<uint64>(ROW_COUNT + 1)};
    added.Label = QStringLiteral("Added object");
    rows.prepend(added);
    panel.SetRows(rows, {});
    QTreeWidgetItem* topOrNull = treeOrNull->itemAt(0, 0);
    REQUIRE(topOrNull != nullptr);
    CHECK(topOrNull->text(0) == QStringLiteral("Imported object %1").arg(selectedRow));
}
