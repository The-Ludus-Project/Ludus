#include "filesystem_fixture.h"

#include "internal/path_text.h"
#include "internal/project_explorer.h"
#include "internal/project_files.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMenu>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

using namespace ludus::editor;
namespace
{
void WriteExplorerFile(const QString& path, const QByteArray& bytes = "saved bytes")
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()) == true);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}
QByteArray ReadExplorerFile(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}
QTreeWidgetItem* FindExplorerItem(QTreeWidgetItem* parent, const QString& label)
{
    for (ludus::foundation::int32 i = 0; i < parent->childCount(); ++i)
    {
        QTreeWidgetItem* item = parent->child(i);
        if (item->text(0) == label)
        {
            return item;
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("Project files preserve destinations and reject escape and child symlinks", "[editor][project-explorer]")
{
    QTemporaryDir project;
    QTemporaryDir outside;
    const QString root = QFileInfo(project.path()).canonicalFilePath();
    const QString source = QDir(root).filePath(QStringLiteral("first.slang"));
    WriteExplorerFile(source);
    QString error;
    ProjectFileResult result{ .Path = QStringLiteral("unchanged"), .Error = {} };
    CHECK(ProjectFiles::Create(root, QStringLiteral("first.slang"), "replacement", error) == false);
    CHECK(ReadExplorerFile(source) == "saved bytes");
    CHECK(ProjectFiles::Create(root, QStringLiteral("../escape.slang"), "bad", error) == false);
    CHECK(ProjectFiles::Create(root, QDir(outside.path()).filePath(QStringLiteral("escape.slang")), "bad", error) ==
          false);
    REQUIRE(CreateTestSymbolicLink(outside.path(), QDir(root).filePath(QStringLiteral("linked"))) == true);
    CHECK(ProjectFiles::Create(root, QStringLiteral("linked/escape.slang"), "bad", error) == false);
    const QString external = QDir(outside.path()).filePath(QStringLiteral("external.slang"));
    WriteExplorerFile(external, "external shader");
    CHECK(ProjectFiles::Add(root,
                            QDir(root).filePath(QStringLiteral("linked/external.slang")),
                            QStringLiteral("escaped.slang"),
                            error) == false);
    REQUIRE(ProjectFiles::Add(root, external, QStringLiteral("imported.slang"), error) == true);
    CHECK(ReadExplorerFile(QDir(root).filePath(QStringLiteral("imported.slang"))) == "external shader");
    CHECK(ProjectFiles::Duplicate(root, source, QStringLiteral("../escape"), false, result) == false);
    CHECK(result.Path == QStringLiteral("unchanged"));
    REQUIRE(ProjectFiles::Duplicate(root, source, QStringLiteral("second.slang"), false, result) == true);
    CHECK(ReadExplorerFile(result.Path) == ReadExplorerFile(source));
    CHECK(ProjectFiles::Duplicate(root, source, QStringLiteral("second.slang"), false, result) == false);
    REQUIRE(CreateTestSymbolicLink(source, QDir(root).filePath(QStringLiteral("alias.slang"))) == true);
    CHECK(ProjectFiles::Remove(root, QDir(root).filePath(QStringLiteral("alias.slang")), result) == false);
    REQUIRE(ProjectFiles::Remove(root, source, result) == true);
    CHECK(QFileInfo(source).exists() == false);
    CHECK(ReadExplorerFile(result.Path) == "saved bytes");
    CHECK(result.Path.contains(QStringLiteral("/.ludus-trash/")) == true);
}

TEST_CASE("Render graph duplicates stage a complete package and roll back refused copies", "[editor][project-explorer]")
{
    QTemporaryDir project;
    QTemporaryDir outside;
    const QString root = QFileInfo(project.path()).canonicalFilePath();
    const QString graph = QDir(root).filePath(QStringLiteral("graphs/main/main.lurender.json"));
    WriteExplorerFile(graph, "{\"configurations\":{}}");
    WriteExplorerFile(QDir(root).filePath(QStringLiteral("graphs/main/shaders/color.slang")), "shader");
    ProjectFileResult result;
    REQUIRE(ProjectFiles::Duplicate(root, graph, QStringLiteral("main_copy"), true, result) == true);
    CHECK(result.Path == QDir(root).filePath(QStringLiteral("graphs/main_copy/main.lurender.json")));
    CHECK(ReadExplorerFile(result.Path) == ReadExplorerFile(graph));
    CHECK(ReadExplorerFile(QDir(root).filePath(QStringLiteral("graphs/main_copy/shaders/color.slang"))) == "shader");
    REQUIRE(CreateTestSymbolicLink(outside.path(), QDir(root).filePath(QStringLiteral("graphs/main/external"))) ==
            true);
    const QString previous = result.Path;
    CHECK(ProjectFiles::Duplicate(root, graph, QStringLiteral("refused"), true, result) == false);
    CHECK(result.Path == previous);
    CHECK(QFileInfo(QDir(root).filePath(QStringLiteral("graphs/refused"))).exists() == false);
    CHECK(QDir(root)
              .entryList({QStringLiteral(".ludus-copy-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
              .isEmpty() == true);
    QFile oversized(QDir(root).filePath(QStringLiteral("oversized.slang")));
    REQUIRE(oversized.open(QIODevice::WriteOnly) == true);
    REQUIRE(oversized.resize(ProjectFiles::MAX_COPY_BYTES + 1) == true);
    oversized.close();
    CHECK(ProjectFiles::Duplicate(root, oversized.fileName(), QStringLiteral("oversized_copy.slang"), false, result) ==
          false);
    CHECK(QFileInfo(QDir(root).filePath(QStringLiteral("oversized_copy.slang"))).exists() == false);
    // A recovery folder redirected outside the project cannot receive a removed file.
    REQUIRE(CreateTestSymbolicLink(outside.path(), QDir(root).filePath(QStringLiteral(".ludus-trash"))) == true);
    CHECK(ProjectFiles::Remove(root, graph, result) == false);
    CHECK(QFileInfo(graph).isFile() == true);
}

TEST_CASE("Explorer menus target the clicked file and expose each authoring category", "[editor][project-explorer]")
{
    QTemporaryDir project;
    const QString root = QFileInfo(project.path()).canonicalFilePath();
    WriteExplorerFile(QDir(root).filePath(QStringLiteral("first.world.json")));
    WriteExplorerFile(QDir(root).filePath(QStringLiteral("second.world.json")));
    WriteExplorerFile(QDir(root).filePath(QStringLiteral("scripts/test.luau")));
    WriteExplorerFile(QDir(root).filePath(QStringLiteral(".ludus-trash/old.world.json")));
    ProjectExplorer explorer;
    explorer.SetProject(root, QStringLiteral("Test"));
    explorer.resize(600, 600);
    explorer.show();
    QApplication::processEvents();
    QTreeWidget* tree = explorer.findChild<QTreeWidget*>(QStringLiteral("projectExplorerTree"));
    REQUIRE(tree != nullptr);
    CHECK(explorer.GetFiles(ProjectItemKind::World).size() == 2);
    CHECK(explorer.GetFiles(ProjectItemKind::Scripts).size() == 1);
    QTreeWidgetItem* worlds = FindExplorerItem(tree->topLevelItem(0), QStringLiteral("Worlds"));
    REQUIRE(worlds != nullptr);
    REQUIRE(worlds->childCount() == 2);
    tree->setCurrentItem(worlds->child(0));
    QSignalSpy requests(&explorer, &ProjectExplorer::ActionRequested);
    const QPoint position = tree->visualItemRect(worlds->child(1)).center();
    QContextMenuEvent contextMenu(QContextMenuEvent::Mouse, position, tree->viewport()->mapToGlobal(position));
    QApplication::sendEvent(tree->viewport(), &contextMenu);
    QMenu* menu = explorer.findChild<QMenu*>(QStringLiteral("projectExplorerContextMenu"));
    REQUIRE(menu != nullptr);
    QAction* duplicate = menu->findChild<QAction*>(QStringLiteral("projectExplorerDuplicate"));
    REQUIRE(duplicate != nullptr);
    REQUIRE(duplicate->isEnabled() == true);
    duplicate->trigger();
    REQUIRE(requests.size() == 1);
    CHECK(requests.first().at(2).toString() == QDir(root).filePath(QStringLiteral("second.world.json")));
    menu->close();
    QToolButton* add = explorer.findChild<QToolButton*>(QStringLiteral("projectExplorerAdd"));
    REQUIRE(add != nullptr);
    Q_EMIT add->menu()->aboutToShow();
    for (const QString& name : {QStringLiteral("projectExplorerNewWorld"),
                                QStringLiteral("projectExplorerNewRenderGraph"),
                                QStringLiteral("projectExplorerNewShader"),
                                QStringLiteral("projectExplorerNewScript"),
                                QStringLiteral("projectExplorerImportAudio"),
                                QStringLiteral("projectExplorerImportContent"),
                                QStringLiteral("projectExplorerNewSound"),
                                QStringLiteral("projectExplorerNewMusic")})
    {
        REQUIRE(add->menu()->findChild<QAction*>(name) != nullptr);
    }
    QAction* newWorld = add->menu()->findChild<QAction*>(QStringLiteral("projectExplorerNewWorld"));
    explorer.SetProject({}, {});
    requests.clear();
    newWorld->trigger();
    CHECK(requests.isEmpty() == true);
}

TEST_CASE("Without a project the explorer lists only its hint, not the Configuration item",
          "[editor][project-explorer]")
{
    ProjectExplorer explorer;
    explorer.SetProject({}, {});
    QTreeWidget* tree = explorer.findChild<QTreeWidget*>(QStringLiteral("projectExplorerTree"));
    REQUIRE(tree != nullptr);
    CHECK(tree->topLevelItemCount() == 0);

    QTemporaryDir project;
    explorer.SetProject(QFileInfo(project.path()).canonicalFilePath(), QStringLiteral("Test"));
    bool hasConfiguration = false;
    for (QTreeWidgetItemIterator iterator(tree); *iterator != nullptr; ++iterator)
    {
        hasConfiguration = hasConfiguration == true || (*iterator)->text(0) == QStringLiteral("Configuration");
    }
    CHECK(hasConfiguration == true);
}

TEST_CASE("Explorer rows show document names, tell shared names apart by folder and search whole file names",
          "[editor][project-explorer]")
{
    QTemporaryDir project;
    const QString root = QFileInfo(project.path()).canonicalFilePath();
    WriteExplorerFile(QDir(root).filePath(QStringLiteral("sponza.world.json")));
    WriteExplorerFile(QDir(root).filePath(QStringLiteral("assets/sponza.world.json")));
    WriteExplorerFile(QDir(root).filePath(QStringLiteral("assets/cornell-box.world.json")));
    WriteExplorerFile(QDir(root).filePath(QStringLiteral("graphs/deferred/main.lurender.json")));
    WriteExplorerFile(QDir(root).filePath(QStringLiteral("graphs/forward/main.lurender.json")));
    WriteExplorerFile(QDir(root).filePath(QStringLiteral("shaders/sky.lushader.json")));
    WriteExplorerFile(QDir(root).filePath(QStringLiteral("shaders/sky.slang")));
    ProjectExplorer explorer;
    explorer.SetProject(root, QStringLiteral("Test"));
    QTreeWidget* tree = explorer.findChild<QTreeWidget*>(QStringLiteral("projectExplorerTree"));
    REQUIRE(tree != nullptr);
    QTreeWidgetItem* worlds = FindExplorerItem(tree->topLevelItem(0), QStringLiteral("Worlds"));
    QTreeWidgetItem* graphs = FindExplorerItem(tree->topLevelItem(0), QStringLiteral("Render Graphs"));
    QTreeWidgetItem* shaders = FindExplorerItem(tree->topLevelItem(0), QStringLiteral("Shaders"));
    REQUIRE(worlds != nullptr);
    REQUIRE(graphs != nullptr);
    REQUIRE(shaders != nullptr);

    // The group already says ".world.json"; a name no other row shares needs no folder.
    QTreeWidgetItem* cornell = FindExplorerItem(worlds, QStringLiteral("cornell-box"));
    REQUIRE(cornell != nullptr);
    CHECK(cornell->data(0, DOCUMENT_QUALIFIER_ROLE).isValid() == true);
    CHECK(cornell->data(0, DOCUMENT_QUALIFIER_ROLE).toString().isEmpty() == true);
    CHECK(cornell->toolTip(0) == QDir::toNativeSeparators(QStringLiteral("assets/cornell-box.world.json")));
    // Two worlds named sponza: the folder tells them apart.
    QStringList sponzaQualifiers;
    QStringList graphQualifiers;
    for (ludus::foundation::int32 index = 0; index < worlds->childCount(); ++index)
    {
        if (worlds->child(index)->text(0) == QStringLiteral("sponza"))
        {
            sponzaQualifiers.append(worlds->child(index)->data(0, DOCUMENT_QUALIFIER_ROLE).toString());
        }
    }
    CHECK(sponzaQualifiers.size() == 2);
    // Rows are ordered by the shown name, so the two sponza rows sit together.
    REQUIRE(worlds->childCount() == 3);
    CHECK(worlds->child(0)->text(0) == QStringLiteral("cornell-box"));
    CHECK(worlds->child(1)->text(0) == QStringLiteral("sponza"));
    CHECK(worlds->child(2)->text(0) == QStringLiteral("sponza"));
    CHECK(sponzaQualifiers.contains(QStringLiteral("assets")) == true);
    CHECK(sponzaQualifiers.contains(QStringLiteral("project folder")) == true);
    // Graph packages are told apart by their own folder name, not the whole path.
    for (ludus::foundation::int32 index = 0; index < graphs->childCount(); ++index)
    {
        CHECK(graphs->child(index)->text(0) == QStringLiteral("main"));
        graphQualifiers.append(graphs->child(index)->data(0, DOCUMENT_QUALIFIER_ROLE).toString());
    }
    graphQualifiers.sort();
    CHECK(graphQualifiers == QStringList{QStringLiteral("deferred"), QStringLiteral("forward")});
    // Shader manifests drop their suffix; Slang sources keep their extension, so the two stay distinct.
    CHECK(FindExplorerItem(shaders, QStringLiteral("sky")) != nullptr);
    CHECK(FindExplorerItem(shaders, QStringLiteral("sky.slang")) != nullptr);
    CHECK(FindExplorerItem(shaders, QStringLiteral("sky"))->data(0, DOCUMENT_QUALIFIER_ROLE).toString().isEmpty() ==
          true);

    // The search still matches the whole file name and its folder.
    QLineEdit* filter = explorer.findChild<QLineEdit*>(QStringLiteral("projectExplorerFilter"));
    REQUIRE(filter != nullptr);
    CHECK(filter->placeholderText() == QStringLiteral("Search"));
    filter->setText(QStringLiteral(".world.json"));
    CHECK(cornell->isHidden() == false);
    CHECK(FindExplorerItem(shaders, QStringLiteral("sky"))->isHidden() == true);
    filter->setText(QStringLiteral("assets/"));
    for (ludus::foundation::int32 index = 0; index < worlds->childCount(); ++index)
    {
        QTreeWidgetItem* world = worlds->child(index);
        CHECK(world->isHidden() ==
              (world->data(0, DOCUMENT_QUALIFIER_ROLE).toString() == QStringLiteral("project folder")));
    }
    filter->clear();
    // Opening still uses the real file.
    QSignalSpy opens(&explorer, &ProjectExplorer::OpenRequested);
    REQUIRE(explorer.ActivateItem(ProjectItemKind::World,
                                  QDir(root).filePath(QStringLiteral("assets/cornell-box.world.json"))) == true);
    REQUIRE(opens.size() == 1);
    CHECK(opens.first().at(1).toString() == QDir(root).filePath(QStringLiteral("assets/cornell-box.world.json")));
}

TEST_CASE("Explorer Add and Refresh are named icon buttons beside the search field", "[editor][project-explorer]")
{
    ProjectExplorer explorer;
    for (const QString& name : {QStringLiteral("projectExplorerAdd"), QStringLiteral("projectExplorerRefresh")})
    {
        INFO(name.toStdString());
        QToolButton* button = explorer.findChild<QToolButton*>(name);
        REQUIRE(button != nullptr);
        CHECK(button->toolButtonStyle() == Qt::ToolButtonIconOnly);
        CHECK(button->icon().isNull() == false);
        CHECK(button->accessibleName().isEmpty() == false);
        CHECK(button->toolTip().isEmpty() == false);
    }
}
