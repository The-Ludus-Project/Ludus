#include "internal/controller.h"
#include "internal/inspector_fields.h"
#include "internal/workspace_style.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QString>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;

constexpr int32 SETTLE_TIMEOUT_MILLISECONDS = 10000;
/// The narrowest a World sidebar gets, a usual one, and a roomy one.
constexpr int32 NARROW_SIDEBAR_WIDTH = 220;
constexpr int32 USUAL_SIDEBAR_WIDTH = 300;
constexpr int32 WIDE_SIDEBAR_WIDTH = 460;
constexpr int32 SIDEBAR_HEIGHT = 520;
/// Properties dock widths: the narrowest the editor allows and a roomy one.
constexpr int32 NARROW_DOCK_WIDTH = 260;
constexpr int32 WIDE_DOCK_WIDTH = 480;
constexpr int32 DOCK_HEIGHT = 1100;
/// Characters the old fixed shortening showed: eight, an ellipsis and four.
constexpr int32 FORMER_SHORTENED_LENGTH = 13;

void WriteFixtureFile(const QString& path, const QByteArray& bytes)
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()) == true);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}

/// Two unnamed materials on two objects, a point light, and the licence the importer asks for.
void WriteLitSource(const QString& path)
{
    WriteFixtureFile(path,
                     QByteArrayLiteral("LookAt 0 2 5  0 0 0  0 1 0\n"
                                       "Camera \"perspective\" \"float fov\" [45]\nWorldBegin\n"
                                       "LightSource \"point\" \"rgb I\" [3 1 2] \"point3 from\" [0 2 0]\n"
                                       "Material \"diffuse\" \"rgb reflectance\" [.6 .5 .4]\n"
                                       "Shape \"trianglemesh\" \"point3 P\" [-3 0 -3  3 0 -3  3 0 3  -3 0 3]\n"
                                       "\"integer indices\" [0 1 2  0 2 3]\n"
                                       "Material \"diffuse\" \"rgb reflectance\" [.2 .5 .8]\n"
                                       "Shape \"trianglemesh\" \"point3 P\" [-1 0 -1  1 0 -1  1 1 -1  -1 1 -1]\n"
                                       "\"integer indices\" [0 1 2  0 2 3]\n"));
    WriteFixtureFile(QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral("LICENSE.txt")),
                     QByteArrayLiteral("CC0 synthetic fixture\n"));
}

/// The same scene with no light at all.
void WriteUnlitSource(const QString& path)
{
    WriteFixtureFile(path,
                     QByteArrayLiteral("LookAt 0 2 5  0 0 0  0 1 0\n"
                                       "Camera \"perspective\" \"float fov\" [45]\nWorldBegin\n"
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
    workspace.StopPreview();
}

/// An imported World, either lit or unlit, ready for a test to open.
struct ImportedFixture final
{
    QTemporaryDir Directory;
    QString Root;
    QString Path;
};

void MakeFixture(ImportedFixture& outFixture, bool isLit)
{
    REQUIRE(outFixture.Directory.isValid() == true);
    const QString projectPath = QDir(outFixture.Directory.path()).filePath(QStringLiteral("project"));
    REQUIRE(QDir().mkpath(projectPath) == true);
    outFixture.Root = QFileInfo(projectPath).canonicalFilePath();
    const QString source = outFixture.Directory.filePath(QStringLiteral("source/scene.pbrt"));
    if (isLit == true)
    {
        WriteLitSource(source);
    }
    else
    {
        WriteUnlitSource(source);
    }
    outFixture.Path = QDir(outFixture.Root).filePath(QStringLiteral("scene.world.json"));
    CreateImportedWorld(outFixture.Root, outFixture.Path, source);
}

void Capture(const QString& captures, const QString& name, QWidget& outWidget)
{
    if (captures.isEmpty() == false)
    {
        REQUIRE(outWidget.grab().save(captures + name + QStringLiteral(".png")) == true);
    }
}
/// What each sidebar tab must say about the World it shows.
void CheckSidebarTab(const QTabWidget& tabs, const QString& tab, bool isLit)
{
    const QWidget* pageOrNull = tabs.currentWidget();
    REQUIRE(pageOrNull != nullptr);
    CHECK(pageOrNull->isVisible() == true);
    if (tab == QStringLiteral("Materials"))
    {
        const QTreeWidget* treeOrNull = pageOrNull->findChild<QTreeWidget*>(QStringLiteral("worldMaterials"));
        REQUIRE(treeOrNull != nullptr);
        REQUIRE(treeOrNull->topLevelItemCount() > 0);
        for (int32 index = 0; index < treeOrNull->topLevelItemCount(); ++index)
        {
            const QTreeWidgetItem& item = *treeOrNull->topLevelItem(index);
            INFO(item.text(0).toStdString());
            // An unnamed material is called what Properties calls it, not a head and tail cut from its ID.
            CHECK(item.text(0).startsWith(QStringLiteral("Material ")) == true);
            CHECK(item.text(0).contains(QStringLiteral("…")) == false);
            CHECK(item.toolTip(0).contains(QStringLiteral("material/")) == true);
        }
        CHECK(treeOrNull->textElideMode() == Qt::ElideMiddle);
    }
    else if (tab == QStringLiteral("Lights"))
    {
        const QLabel* statusOrNull = pageOrNull->findChild<QLabel*>(QStringLiteral("worldLightsStatus"));
        REQUIRE(statusOrNull != nullptr);
        CHECK(statusOrNull->isVisible() == (isLit == false));
        if (isLit == false)
        {
            CHECK(statusOrNull->text().contains(QStringLiteral("No lights")) == true);
            CHECK(statusOrNull->text().contains(QStringLiteral("dark")) == true);
            CHECK(statusOrNull->height() >= statusOrNull->heightForWidth(statusOrNull->width()));
        }
    }
    else
    {
        const QLabel* emptyOrNull = pageOrNull->findChild<QLabel*>(QStringLiteral("worldEnvironmentEmptyText"));
        REQUIRE(emptyOrNull != nullptr);
        CHECK(emptyOrNull->isVisible() == true);
        CHECK(emptyOrNull->height() >= emptyOrNull->heightForWidth(emptyOrNull->width()));
    }
}
} // namespace

TEST_CASE("The World sidebar lists read as names at every sidebar width", "[editor][world][layout][names]")
{
    const QString captures = qEnvironmentVariable("LUDUS_EDITOR_LAYOUT_CAPTURE");
    for (const bool isLit : {true, false})
    {
        ImportedFixture fixture;
        MakeFixture(fixture, isLit);
        QWidget host;
        QVBoxLayout* hostLayout = new QVBoxLayout(&host);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        WorldWorkspace* workspaceOrNull = new WorldWorkspace(MakeSceneTools(), &host);
        hostLayout->addWidget(workspaceOrNull);
        workspaceOrNull->SetProject(fixture.Root, 1);
        REQUIRE(workspaceOrNull->Open(fixture.Path) == true);
        WaitForWorld(*workspaceOrNull);
        QTabWidget* listsOrNull = workspaceOrNull->findChild<QTabWidget*>(QStringLiteral("worldSidebarLists"));
        REQUIRE(listsOrNull != nullptr);
        for (const int32 width : {NARROW_SIDEBAR_WIDTH, USUAL_SIDEBAR_WIDTH, WIDE_SIDEBAR_WIDTH})
        {
            INFO("Sidebar width " << width << (isLit == true ? " lit" : " unlit"));
            listsOrNull->setFixedSize(width, SIDEBAR_HEIGHT);
            host.show();
            REQUIRE(QTest::qWaitForWindowExposed(&host) == true);
            for (const char* tab : {"Materials", "Lights", "Environment"})
            {
                for (int32 index = 0; index < listsOrNull->count(); ++index)
                {
                    if (listsOrNull->tabText(index) == QString::fromLatin1(tab))
                    {
                        listsOrNull->setCurrentIndex(index);
                    }
                }
                QApplication::processEvents();
                CheckSidebarTab(*listsOrNull, QString::fromLatin1(tab), isLit);
                Capture(captures,
                        QStringLiteral("-sidebar-%1-%2-%3")
                            .arg(QString::fromLatin1(tab).toLower(),
                                 isLit == true ? QStringLiteral("lit") : QStringLiteral("unlit"),
                                 QString::number(width)),
                        *listsOrNull);
            }
        }
        workspaceOrNull->StopPreview();
    }
}

TEST_CASE("Properties shows as much of an identifier as fits", "[editor][world][layout][names]")
{
    const QString captures = qEnvironmentVariable("LUDUS_EDITOR_LAYOUT_CAPTURE");
    ImportedFixture fixture;
    MakeFixture(fixture, true);
    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(fixture.Root, 1);
    QWidget dock;
    QVBoxLayout* dockLayout = new QVBoxLayout(&dock);
    dockLayout->setContentsMargins(0, 0, 0, 0);
    QWidget* panelOrNull = workspace.TakePropertiesPanel(&dock);
    REQUIRE(panelOrNull != nullptr);
    dockLayout->addWidget(panelOrNull);
    ApplyWorkspaceStyle(&dock);
    REQUIRE(workspace.Open(fixture.Path) == true);
    WaitForWorld(workspace);
    QString renderable;
    for (const QJsonValue value : workspace.Document().Draft().value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject entity = value.toObject();
        if (entity.value(QStringLiteral("scene")).isObject() == true && renderable.isEmpty() == true)
        {
            renderable = workspace.Document().GetEntityKey(entity);
        }
    }
    REQUIRE(renderable.isEmpty() == false);
    REQUIRE(workspace.SelectEntity(renderable) == true);
    for (const int32 width : {NARROW_DOCK_WIDTH, WIDE_DOCK_WIDTH})
    {
        INFO("Dock width " << width);
        dock.resize(width, DOCK_HEIGHT);
        dock.show();
        REQUIRE(QTest::qWaitForWindowExposed(&dock) == true);
        QTest::qWait(100);
        const QList<IdentifierRow*> rows = dock.findChildren<IdentifierRow*>();
        REQUIRE(rows.size() >= 3);
        for (const IdentifierRow* row : rows)
        {
            const ElidedLabel* textOrNull = row->findChild<ElidedLabel*>(QStringLiteral("inspectorIdentifierText"));
            REQUIRE(textOrNull != nullptr);
            INFO(row->GetIdentifier().toStdString());
            // Every identifier is held whole, with its full text a hover away and the copy button beside it.
            CHECK(textOrNull->text() == row->GetIdentifier());
            CHECK(textOrNull->toolTip().contains(row->GetIdentifier()) == true);
            CHECK(row->findChild<QToolButton*>(QStringLiteral("inspectorIdentifierCopy"))->isVisible() == true);
            if (textOrNull->IsElided() == true)
            {
                // The start and the end of the identifier both stay visible around the gap.
                CHECK(textOrNull->GetShownText().startsWith(row->GetIdentifier().left(1)) == true);
                CHECK(textOrNull->GetShownText().endsWith(row->GetIdentifier().right(1)) == true);
            }
            else
            {
                CHECK(textOrNull->GetShownText() == row->GetIdentifier());
            }
        }
        const IdentifierRow* materialRowOrNull = nullptr;
        for (const IdentifierRow* row : rows)
        {
            if (row->GetIdentifier().startsWith(QStringLiteral("material/")) == true)
            {
                materialRowOrNull = row;
            }
        }
        REQUIRE(materialRowOrNull != nullptr);
        if (width == WIDE_DOCK_WIDTH)
        {
            // A roomy dock shows more of the resource ID than the first eight characters and last four it used to.
            const ElidedLabel* resourceOrNull =
                materialRowOrNull->findChild<ElidedLabel*>(QStringLiteral("inspectorIdentifierText"));
            REQUIRE(resourceOrNull != nullptr);
            CHECK(resourceOrNull->GetShownText().size() > FORMER_SHORTENED_LENGTH);
        }
        Capture(captures, QStringLiteral("-properties-%1").arg(width), dock);
    }
    workspace.StopPreview();
}
