#include "internal/code_editor.h"
#include "internal/code_navigation.h"
#include "internal/renderer_workspace.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

using namespace ludus::editor;

namespace
{
QString MakeSourcePackage(const QTemporaryDir& project, const QString& name)
{
    const QString package = project.filePath(QStringLiteral("graphs/") + name);
    const QDir fixture(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    for (const QString& file : {QStringLiteral("main.lurender.json"),
                                QStringLiteral("rasterize.lurender.json"),
                                QStringLiteral("main.lushader.json.in"),
                                QStringLiteral("main_shaders/geometry.slang"),
                                QStringLiteral("main_shaders/color.slang"),
                                QStringLiteral("main_shaders/depth.slang")})
    {
        QFile input(fixture.filePath(file));
        REQUIRE(input.open(QIODevice::ReadOnly) == true);
        QByteArray bytes = input.readAll();
        bytes.replace("@RG2_TEST_PROFILES@", "[\"metal\"]");
        const QString destination =
            QDir(package).filePath(file.endsWith(QStringLiteral(".in")) == true ? file.chopped(3) : file);
        REQUIRE(QDir().mkpath(QFileInfo(destination).absolutePath()) == true);
        QFile output(destination);
        REQUIRE(output.open(QIODevice::WriteOnly) == true);
        REQUIRE(output.write(bytes) == bytes.size());
    }
    return QDir(package).filePath(QStringLiteral("main.lurender.json"));
}
} // namespace

TEST_CASE("Source view preferences restore the graph and shader without serializing draft text",
          "[editor][code-viewport]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString graph = MakeSourcePackage(project, QStringLiteral("A"));
    RendererWorkspace workspace(ToolingPaths{});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(graph) == true);
    REQUIRE(workspace.ShowSourceView() == true);
    workspace.show();
    QComboBox* filesOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
    CodeEditor* sourceOrNull = workspace.findChild<CodeEditor*>(QStringLiteral("renderGraphSource"));
    REQUIRE(filesOrNull != nullptr);
    REQUIRE(sourceOrNull != nullptr);
    const QString file = QStringLiteral("main_shaders/depth.slang");
    filesOrNull->setCurrentText(file);
    const QByteArray saved = workspace.GetDocument().GetDraft(file);
    QTest::keyClicks(sourceOrNull, QStringLiteral("// unsaved marker"));
    REQUIRE(workspace.IsDirty() == true);
    const QByteArray draft = workspace.GetDocument().GetDraft(file);
    REQUIRE(draft != saved);
    const QJsonObject state = workspace.GetSourceViewState();
    CHECK(QJsonDocument(state).toJson().contains("unsaved marker") == false);
    REQUIRE(workspace.RestoreSourceViewState(state) == true);
    CHECK(workspace.GetDocument().GetDraft(file) == draft);
    CHECK(filesOrNull->currentText() == file);
    RendererWorkspace reopened(ToolingPaths{});
    reopened.SetProject(project.path(), 1);
    REQUIRE(reopened.RestoreSourceViewState(state) == true);
    CHECK(reopened.GetSourceViewState() == state);
    CHECK(reopened.GetDocument().GetDraft(file) == saved);
    CHECK(reopened.IsDirty() == false);
    QFile disk(QDir(QFileInfo(graph).absolutePath()).filePath(file));
    REQUIRE(disk.open(QIODevice::ReadOnly) == true);
    CHECK(disk.readAll() == saved);
}

TEST_CASE("Invalid source view preferences and dirty graph replacement preserve the accepted draft",
          "[editor][code-viewport]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString graph = MakeSourcePackage(project, QStringLiteral("A"));
    static_cast<void>(MakeSourcePackage(project, QStringLiteral("B")));
    RendererWorkspace workspace(ToolingPaths{});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(graph) == true);
    REQUIRE(workspace.ShowSourceView() == true);
    workspace.show();
    CodeEditor* sourceOrNull = workspace.findChild<CodeEditor*>(QStringLiteral("renderGraphSource"));
    REQUIRE(sourceOrNull != nullptr);
    QTest::keyClicks(sourceOrNull, QStringLiteral("// retained marker"));
    REQUIRE(workspace.IsDirty() == true);
    const QJsonObject state = workspace.GetSourceViewState();
    const QString file = state.value(QStringLiteral("source")).toString();
    const QByteArray draft = workspace.GetDocument().GetDraft(file);
    for (const QString& path : {QStringLiteral("../outside/main.lurender.json"),
                                graph,
                                QStringLiteral("graphs/Missing/main.lurender.json"),
                                QStringLiteral("graphs/B/main.lurender.json")})
    {
        QJsonObject invalid = state;
        invalid.insert(QStringLiteral("graph"), path);
        CHECK(workspace.RestoreSourceViewState(invalid) == false);
        CHECK(workspace.GetSourceViewState() == state);
        CHECK(workspace.GetDocument().GetDraft(file) == draft);
    }
    QJsonObject invalid = state;
    invalid.insert(QStringLiteral("view"), 1000);
    CHECK(workspace.RestoreSourceViewState(invalid) == false);
    CHECK(workspace.RestoreSourceViewState({}) == false);
    CHECK(workspace.GetDocument().GetDraft(file) == draft);
}

TEST_CASE("Source preferences reject missing and escaping graphs before replacing a clean document",
          "[editor][code-viewport]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString graph = MakeSourcePackage(project, QStringLiteral("A"));
    QStringList invalidPaths{QStringLiteral("graphs/Missing/main.lurender.json")};
#if defined(Q_OS_UNIX)
    QTemporaryDir outside;
    REQUIRE(outside.isValid() == true);
    const QString outsideGraph = MakeSourcePackage(outside, QStringLiteral("Outside"));
    REQUIRE(QFile::link(QFileInfo(outsideGraph).absolutePath(), project.filePath(QStringLiteral("escaped"))) == true);
    invalidPaths.append(QStringLiteral("escaped/main.lurender.json"));
#endif
    RendererWorkspace workspace(ToolingPaths{});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(graph) == true);
    REQUIRE(workspace.ShowSourceView() == true);
    const QJsonObject state = workspace.GetSourceViewState();
    const QString file = state.value(QStringLiteral("source")).toString();
    const QByteArray source = workspace.GetDocument().GetDraft(file);
    for (const QString& path : invalidPaths)
    {
        QJsonObject invalid = state;
        invalid.insert(QStringLiteral("graph"), path);
        CHECK(workspace.RestoreSourceViewState(invalid) == false);
        CHECK(workspace.GetSourceViewState() == state);
        CHECK(workspace.GetDocument().GetDraft(file) == source);
        CHECK(workspace.IsDirty() == false);
    }
}

TEST_CASE("Workspace source navigation crosses package drafts and resets on document replacement",
          "[editor][code-navigation][source-navigation]")
{
    QTemporaryDir project;
    const QString graph = MakeSourcePackage(project, QStringLiteral("A"));
    RendererWorkspace workspace(ToolingPaths{});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(graph) == true);
    REQUIRE(workspace.ShowSourceView() == true);
    QComboBox* filesOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
    CodeEditor* sourceOrNull = workspace.findChild<CodeEditor*>(QStringLiteral("renderGraphSource"));
    REQUIRE(filesOrNull != nullptr);
    REQUIRE(sourceOrNull != nullptr);
    const QString first = QStringLiteral("main_shaders/color.slang");
    const QString second = QStringLiteral("main_shaders/depth.slang");
    filesOrNull->setCurrentText(first);
    QTest::keyClicks(sourceOrNull, QStringLiteral("// unsaved navigation marker"));
    QTest::keyClick(sourceOrNull, Qt::Key_Return);
    sourceOrNull->GoToLine(3);
    const QByteArray accepted = workspace.GetDocument().GetDraft(first);
    const ludus::foundation::uint64 revision = workspace.GetDocument().GetRevision();
    REQUIRE(sourceOrNull->GetNavigation().Navigate({second, second, {}, 0, 0, false}) == true);
    CHECK(filesOrNull->currentText() == second);
    sourceOrNull->GetNavigation().Back();
    CHECK(filesOrNull->currentText() == first);
    CHECK(sourceOrNull->textCursor().blockNumber() == 2);
    CHECK(workspace.GetDocument().GetDraft(first) == accepted);
    CHECK(workspace.GetDocument().GetRevision() == revision);
    workspace.SetProject(project.path(), 2);
    const QString current = filesOrNull->currentText();
    sourceOrNull->GetNavigation().Back();
    CHECK(filesOrNull->currentText() == current);
    CHECK(workspace.GetDocument().GetDraft(first) == accepted);
}
