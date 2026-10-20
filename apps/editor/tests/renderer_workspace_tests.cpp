// RG3 Renderer workspace: typed render-graph drafts, explicit save, mapped
// diagnostics and CLI/editor equivalence through the shared ludus_tools backend.
// Offscreen widgets prove document/job wiring, not GPU output or a node canvas.
#include "internal/controller.h"
#include "internal/main_window.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_job.h"
#include "internal/render_graph_shader_interface.h"
#include "internal/renderer_workspace.h"
#include "internal/workspace_style.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPoint>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTimer>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
constexpr int32 JOB_TIMEOUT_MILLISECONDS = 60000;
constexpr int32 CREATION_WORKSPACE_WIDTH = 1280;
constexpr int32 CREATION_WORKSPACE_HEIGHT = 800;
constexpr int32 MINIMUM_VISIBLE_CANVAS_WIDTH = 480;
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
const QString LIBRARY = QStringLiteral("rasterize.lurender.json");

QByteArray ReadFile(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

void WriteFile(const QString& path, const QByteArray& bytes)
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()) == true);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}

/// Copy the checked-in RG2 offscreen package into project/graphs.
QString MakePackage(const QTemporaryDir& project)
{
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    for (const QString& file : {ROOT_GRAPH,
                                LIBRARY,
                                QStringLiteral("main_shaders/color.slang"),
                                QStringLiteral("main_shaders/depth.slang"),
                                QStringLiteral("main_shaders/geometry.slang")})
    {
        WriteFile(QDir(package).filePath(file), ReadFile(source.filePath(file)));
    }
    QByteArray manifest = ReadFile(source.filePath(QStringLiteral("main.lushader.json.in")));
    manifest.replace("@RG2_TEST_PROFILES@", "[\"metal\", \"wgsl\"]");
    WriteFile(QDir(package).filePath(QStringLiteral("main.lushader.json")), manifest);
    return package;
}

// File, configuration and node are distinct named identities.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
QJsonObject
FindNode(RenderGraphDocument& inoutDocument, const QString& file, const QString& configuration, const QString& node)
{
    QJsonObject graph;
    REQUIRE(inoutDocument.GetGraph(file, graph) == true);
    const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    for (const QJsonValueConstRef entry : configurations)
    {
        if (entry.toObject().value(QStringLiteral("id")).toString() != configuration)
        {
            continue;
        }
        const QJsonArray nodes = entry.toObject().value(QStringLiteral("nodes")).toArray();
        for (const QJsonValueConstRef value : nodes)
        {
            if (value.toObject().value(QStringLiteral("id")).toString() == node)
            {
                return value.toObject();
            }
        }
    }
    return {};
}
// NOLINTEND(bugprone-easily-swappable-parameters)

QString FindPython()
{
    const QString managed = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/host-tools/venv/bin/python");
    return QFileInfo(managed).isExecutable() == true ? managed
                                                     : QStandardPaths::findExecutable(QStringLiteral("python3"));
}

/// Run the installed CLI exactly as a user would, for equivalence with the editor job.
QByteArray RunToolCli(const QString& python, const QStringList& arguments)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONPATH"), QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/scripts/python"));
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    process.start(python,
                  QStringList{QStringLiteral("-m"), QStringLiteral("ludus_tools"), QStringLiteral("--json")} +
                      arguments);
    REQUIRE(process.waitForFinished(JOB_TIMEOUT_MILLISECONDS) == true);
    return process.readAllStandardOutput();
}

QByteArray RunCli(const QString& python, const QStringList& arguments)
{
    return RunToolCli(python, QStringList{QStringLiteral("render-graph")} + arguments);
}

void MakeShaderHelper(const QString& directory)
{
    const QDir helper(directory);
    for (const QString& file : {QStringLiteral("cmake/shaders/compile_shader.py"),
                                QStringLiteral("cmake/shaders/compile_raster.py"),
                                QStringLiteral("cmake/shaders/compile_graph_stage.py"),
                                QStringLiteral("cmake/shaders/native_worker.py"),
                                QStringLiteral("config/shader_toolchain.json"),
                                QStringLiteral("config/spirv_cross_toolchain.json")})
    {
        WriteFile(helper.filePath(QFileInfo(file).fileName()),
                  ReadFile(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/") + file));
    }
}
} // namespace

TEST_CASE("Renderer creates a complete project-owned graph with New Graph", "[editor][render-graph][creation]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString root = QFileInfo(project.path()).canonicalFilePath();
    RendererWorkspace workspace({FindPython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    QPushButton* createFirstOrNull = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphCreateFirst"));
    REQUIRE(createFirstOrNull != nullptr);
    // New Graph and Build & Preview are shell commands; their gates are the workspace's.
    CHECK(workspace.CanCreateGraph() == false);
    CHECK(workspace.CanBuild() == false);
    // Without a project, the empty page offers nothing to create into.
    CHECK(createFirstOrNull->isEnabled() == false);
    workspace.SetProject(root, 1);
    ApplyWorkspaceStyle(&workspace);
    workspace.resize(CREATION_WORKSPACE_WIDTH, CREATION_WORKSPACE_HEIGHT);
    workspace.show();
    QApplication::processEvents();
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_RENDER_GRAPH_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(workspace.grab().save(capture + QStringLiteral("-empty.png")) == true);
    }
    // A project without graphs shows the engine default instead of an empty page; New Graph
    // stays available as a command.
    CHECK(workspace.CanCreateGraph() == true);
    CHECK(workspace.IsBuiltIn() == true);
    CHECK(createFirstOrNull->isVisible() == false);
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    QTimer::singleShot(0, &workspace, [&root]() {
        QInputDialog* dialogOrNull = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        CHECK(dialogOrNull->labelText().contains(root + QStringLiteral("/graphs")) == true);
        dialogOrNull->setTextValue(QStringLiteral("MyGraph"));
        dialogOrNull->accept();
    });
    CHECK(workspace.PromptNewGraph() == true);
    CHECK(workspace.CanCreateGraph() == false);
    // Repeated state publications for this project must not cancel creation.
    workspace.SetProject(project.path(), 1);
    REQUIRE(finished.wait(JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetStatusText().toStdString());
    CHECK(workspace.GetDocument().IsLoaded() == true);
    CHECK(workspace.IsBuiltIn() == false);
    CHECK(workspace.GetDocument().IsDirty() == false);
    CHECK(workspace.GetDocument().GetFiles().size() == 6);
    CHECK(workspace.GetRuntimePackagePath().startsWith(root + QStringLiteral("/out/render-graph/")) == true);
    CHECK(QFileInfo(root + QStringLiteral("/graphs/MyGraph/main.lushader.json")).isFile() == true);
    CHECK(workspace.GetCanvas().GetNodeCount() > 0);
    CHECK(createFirstOrNull->isVisible() == false);
    CHECK(workspace.CanBuild() == true);
    CHECK(workspace.GetStatusText().contains(QStringLiteral("Build & Preview")) == true);
    workspace.SetBuildTools({}, {});
    CHECK(workspace.IsJobActive() == false);
    QApplication::processEvents();
    CHECK(workspace.GetCanvas().width() >= MINIMUM_VISIBLE_CANVAS_WIDTH);
    const QPoint sceneNodePositionPixel =
        workspace.GetCanvas().mapFromScene(workspace.GetCanvas().GetNodePosition(QStringLiteral("scene")));
    CHECK(workspace.GetCanvas().viewport()->rect().contains(sceneNodePositionPixel) == true);
    if (capture.isEmpty() == false)
    {
        REQUIRE(workspace.grab().save(capture + QStringLiteral("-created.png")) == true);
    }
    const QByteArray original = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    CHECK(workspace.CreateGraph(QStringLiteral("MyGraph")) == false);
    CHECK(workspace.GetStatusText().contains(QStringLiteral("already exists")) == true);
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == original);
    CHECK(workspace.CreateGraph(QStringLiteral("../escape")) == false);
    CHECK(QFileInfo(root + QStringLiteral("/escape")).exists() == false);
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == original);
}

TEST_CASE("Renderer creation refuses linked graph directories and ignores cancelled callbacks",
          "[editor][render-graph][creation]")
{
    QTemporaryDir project;
    QTemporaryDir engine;
    REQUIRE(QFile::link(engine.path(), QDir(project.path()).filePath(QStringLiteral("graphs"))) == true);
    RenderGraphCreation creation;
    const ToolingPaths tooling{FindPython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)};
    QString error;
    CHECK(creation.Start(tooling, project.path(), QStringLiteral("Main"), 1, error) == false);
    CHECK(QDir(engine.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty() == true);
    REQUIRE(QFile::remove(QDir(project.path()).filePath(QStringLiteral("graphs"))) == true);
    QSignalSpy finished(&creation, &RenderGraphCreation::Finished);
    QSignalSpy created(&creation, &RenderGraphCreation::Created);
    QSignalSpy failed(&creation, &RenderGraphCreation::Failed);
    REQUIRE(creation.Start(tooling, project.path(), QStringLiteral("Main"), 1, error) == true);
    creation.Cancel();
    REQUIRE(finished.wait(JOB_TIMEOUT_MILLISECONDS) == true);
    CHECK(creation.IsActive() == false);
    CHECK(created.count() == 0);
    CHECK(failed.count() == 0);
}

TEST_CASE("New Graph builds and cooks with the project's SDK without manual shader paths",
          "[editor][render-graph][automatic-tools]")
{
    const QString python = FindPython();
    const QString compiler = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/shader-tools/slang/bin/slangc");
    if (python.isEmpty() == true || QFileInfo(compiler).isExecutable() == false)
    {
        SKIP("Prepared pinned Slang is required for the automatic build integration test");
    }
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString sdk = QDir(root).filePath(QStringLiteral("selected-sdk"));
    const QString manifest = QDir(sdk).filePath(QStringLiteral("share/Ludus/LudusSdkManifest.json"));
    const QByteArray sdkIdentity = ReadFile(QStringLiteral(LUDUS_EDITOR_SDK_MANIFEST));
    WriteFile(manifest, sdkIdentity);
    MakeShaderHelper(QDir(sdk).filePath(QStringLiteral("lib/cmake/Ludus/shaders")));
    const QString project = QDir(root).filePath(QStringLiteral("Game"));
    const QByteArray created = RunToolCli(python,
                                          {QStringLiteral("project"),
                                           QStringLiteral("create"),
                                           project,
                                           QStringLiteral("--name"),
                                           QStringLiteral("Game"),
                                           QStringLiteral("--sdk"),
                                           sdk});
    INFO(created.toStdString());
    REQUIRE(QJsonDocument::fromJson(created).object().contains(QStringLiteral("error")) == false);

    QSettings preferences;
    preferences.setValue(QStringLiteral("renderGraph/helper"), QStringLiteral("/stale/other-project/helper"));
    preferences.setValue(QStringLiteral("renderGraph/compiler"), QStringLiteral("/stale/other-project/slangc"));
    RendererWorkspace workspace({python, {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    workspace.EnableWorldRendering();
#endif
    workspace.SetProject(project, 1);
    workspace.SetWorkerSocket({});
    // Stale preferences from another project are never overrides.
    CHECK(workspace.GetBuildSettings().ShouldOverrideTools == false);
    CHECK(workspace.GetBuildSettings().Tools.Compiler.isEmpty() == true);
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    REQUIRE(workspace.CreateGraph(QStringLiteral("Automatic")) == true);
    REQUIRE(finished.wait(JOB_TIMEOUT_MILLISECONDS) == true);
#if !defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    REQUIRE(workspace.BuildAndPreview() == true);
#endif
    // Native World rendering builds the newly created graph and starts its own worker.
    REQUIRE(finished.wait(JOB_TIMEOUT_MILLISECONDS) == true);
    const RenderGraphJobResult result = workspace.GetLastResult();
    INFO(result.Code.toStdString() + ": " + result.Message.toStdString());
    REQUIRE(result.IsSuccessful == true);
    CHECK(result.Payload.value(QStringLiteral("toolchain")).toObject().value(QStringLiteral("sdk")).toString() == sdk);
    CHECK(workspace.GetResolvedBuildTools().Compiler == compiler);
    CHECK(workspace.GetResolvedSdk() == sdk);
    CHECK(workspace.GetBuildSettings().ShouldOverrideTools == false);
    CHECK(QFileInfo(workspace.GetRuntimePackagePath()).isFile() == true);
    CHECK(ReadFile(manifest) == sdkIdentity);
    workspace.SetBuildTools(QStringLiteral("/advanced/helper"), QStringLiteral("/advanced/slangc"));
    CHECK(workspace.GetBuildSettings().ShouldOverrideTools == true);
    workspace.SetProject(root, 2);
    CHECK(workspace.GetBuildSettings().ShouldOverrideTools == false);
    CHECK(workspace.GetBuildSettings().Tools.Compiler.isEmpty() == true);
    CHECK(workspace.GetResolvedBuildTools().Compiler.isEmpty() == true);
}

TEST_CASE("RG3 editor canonical JSON matches the CLI's saved graph bytes", "[editor][render-graph]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString package = MakePackage(project);
    for (const QString& file : {ROOT_GRAPH, LIBRARY})
    {
        const QByteArray bytes = ReadFile(QDir(package).filePath(file));
        CHECK(EncodeCanonicalJson(QJsonDocument::fromJson(bytes).object()) == bytes);
    }
    // Python json.dumps(sort_keys=True, indent=2, ensure_ascii=True) spelling of edge cases.
    const QJsonObject edge{{QStringLiteral("b"), QJsonArray{}},
                           {QStringLiteral("a"), QStringLiteral("\u00e9\"\n")},
                           {QStringLiteral("c"), QJsonObject{}},
                           {QStringLiteral("d"), QJsonArray{-3, true, QJsonValue::Null}}};
    CHECK(EncodeCanonicalJson(edge) == QByteArray("{\n  \"a\": \"\\u00e9\\\"\\n\",\n  \"b\": [],\n  \"c\": {},\n"
                                                  "  \"d\": [\n    -3,\n    true,\n    null\n  ]\n}\n"));
}

TEST_CASE("RG3 render-graph edits stay drafts until an explicit save", "[editor][render-graph]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    const QByteArray saved = ReadFile(QDir(package).filePath(ROOT_GRAPH));
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetFiles().size() == 6);
    QJsonObject camera = FindNode(document, ROOT_GRAPH, QStringLiteral("Default"), QStringLiteral("camera"));
    camera.insert(QStringLiteral("active"), false);
    REQUIRE(document.ReplaceNode(ROOT_GRAPH, QStringLiteral("Default"), camera) == RenderGraphDocumentStatus::Ok);
    CHECK(document.IsDirty() == true);
    CHECK(document.IsFileDirty(LIBRARY) == false);
    CHECK(ReadFile(QDir(package).filePath(ROOT_GRAPH)) == saved);
    const uint64 edited = document.GetRevision();
    REQUIRE(document.Undo() == true);
    // Undo back to saved content is clean at a newer revision.
    CHECK(document.IsDirty() == false);
    CHECK(document.GetRevision() > edited);
    REQUIRE(document.Redo() == true);
    QStringList written;
    REQUIRE(document.Save(written) == RenderGraphDocumentStatus::Ok);
    CHECK(written == QStringList{ROOT_GRAPH});
    CHECK(document.IsDirty() == false);
    const QByteArray disk = ReadFile(QDir(package).filePath(ROOT_GRAPH));
    CHECK(disk == document.GetDraft(ROOT_GRAPH));
    CHECK(QJsonDocument::fromJson(disk).object() != QJsonDocument::fromJson(saved).object());
    // Kind changes and unknown identities are not edits of an existing node.
    camera.insert(QStringLiteral("kind"), QStringLiteral("Scene"));
    CHECK(document.ReplaceNode(ROOT_GRAPH, QStringLiteral("Default"), camera) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.RemoveNode(ROOT_GRAPH, QStringLiteral("Default"), QStringLiteral("missing")) ==
          RenderGraphDocumentStatus::InvalidEdit);
}

TEST_CASE("RG3 layout moves coalesce and node removal drops its connections", "[editor][render-graph]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QByteArray original = document.GetDraft(ROOT_GRAPH);
    for (int32 step = 1; step <= 3; ++step)
    {
        REQUIRE(document.MoveNode(ROOT_GRAPH, QStringLiteral("Default"), QStringLiteral("camera"), step * 10, -step) ==
                RenderGraphDocumentStatus::Ok);
    }
    QJsonObject graph;
    REQUIRE(document.GetGraph(ROOT_GRAPH, graph) == true);
    const QJsonObject configuration = graph.value(QStringLiteral("configurations")).toArray().at(0).toObject();
    const QJsonArray positions =
        configuration.value(QStringLiteral("layout")).toObject().value(QStringLiteral("positions")).toArray();
    REQUIRE(positions.size() == 1);
    CHECK(positions.at(0).toObject().value(QStringLiteral("x")).toInt() == 30);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    CHECK(document.CanUndo() == false);
    REQUIRE(document.RemoveNode(ROOT_GRAPH, QStringLiteral("Default"), QStringLiteral("camera")) ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(document.GetGraph(ROOT_GRAPH, graph) == true);
    const QJsonArray edges = graph.value(QStringLiteral("configurations"))
                                 .toArray()
                                 .at(0)
                                 .toObject()
                                 .value(QStringLiteral("edges"))
                                 .toArray();
    for (const QJsonValueConstRef edge : edges)
    {
        CHECK(edge.toObject().value(QStringLiteral("from")).toString().startsWith(QStringLiteral("camera.")) == false);
        CHECK(edge.toObject().value(QStringLiteral("to")).toString().startsWith(QStringLiteral("camera.")) == false);
    }
}

TEST_CASE("RG3 save refuses external changes and snapshots never touch saved sources", "[editor][render-graph]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString shader = QStringLiteral("main_shaders/color.slang");
    REQUIRE(document.SetText(shader, QByteArray("// draft\n") + document.GetDraft(shader)) ==
            RenderGraphDocumentStatus::Ok);
    const QString snapshot = QDir(project.path()).filePath(QStringLiteral("snapshot"));
    REQUIRE(QDir().mkpath(snapshot) == true);
    REQUIRE(document.WriteSnapshot(snapshot) == RenderGraphDocumentStatus::Ok);
    CHECK(ReadFile(QDir(snapshot).filePath(shader)).startsWith("// draft") == true);
    CHECK(ReadFile(QDir(package).filePath(shader)).startsWith("// draft") == false);
    CHECK(document.WriteSnapshot(snapshot) == RenderGraphDocumentStatus::InvalidEdit);
    WriteFile(QDir(package).filePath(shader), QByteArray("// external writer\n"));
    QStringList written;
    CHECK(document.Save(written) == RenderGraphDocumentStatus::Conflict);
    CHECK(written.isEmpty() == true);
    CHECK(ReadFile(QDir(package).filePath(shader)) == QByteArray("// external writer\n"));
    CHECK(document.IsDirty() == true);
    CHECK(document.SetText(QStringLiteral("unknown.slang"), {}) == RenderGraphDocumentStatus::NotFound);
}

TEST_CASE("RG3 build outputs stay inside a project opened through a symlink", "[editor][render-graph]")
{
    QTemporaryDir project;
    QTemporaryDir links;
    QTemporaryDir outside;
    REQUIRE(project.isValid() == true);
    REQUIRE(links.isValid() == true);
    REQUIRE(outside.isValid() == true);
    static_cast<void>(MakePackage(project));
    const QString link = QDir(links.path()).filePath(QStringLiteral("project"));
    if (QFile::link(project.path(), link) == false)
    {
        SKIP("this file system cannot create a directory symlink");
    }
    const QString root = QFileInfo(project.path()).canonicalFilePath();
    REQUIRE(root.isEmpty() == false);
    REQUIRE(QFileInfo(link).canonicalFilePath() == root);
    REQUIRE(QDir::cleanPath(link) != root);

    RendererWorkspace workspace({FindPython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(link, 5);
    REQUIRE(workspace.OpenGraph(QDir(link).filePath(QStringLiteral("graphs/") + ROOT_GRAPH)) == true);
    const QString runtime = workspace.GetRuntimePackagePath();
    INFO(runtime.toStdString());
    CHECK(runtime == QDir(root).filePath(QStringLiteral("out/render-graph/graphs/main/main.lurgrt")));

    // A package outside the project has no build tree there, so Build refuses to start.
    const QString foreign = MakePackage(outside);
    REQUIRE(workspace.GetDocument().Load(QFileInfo(foreign).canonicalFilePath(), ROOT_GRAPH) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(workspace.GetRuntimePackagePath().isEmpty() == true);
    CHECK(workspace.Build() == false);
    CHECK(QFileInfo::exists(QDir(root).filePath(QStringLiteral("out"))) == false);
}

TEST_CASE("RG3 Renderer workspace maps CLI errors and matches CLI plans", "[editor][render-graph]")
{
    const QString python = FindPython();
    if (python.isEmpty() == true)
    {
        SKIP("python3 is required to run the shared render-graph backend");
    }
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({python, {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 7);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);

    // CLI/editor equivalence: the editor's draft-snapshot plan equals the CLI plan for saved sources.
    REQUIRE(workspace.Validate() == true);
    REQUIRE(finished.wait(JOB_TIMEOUT_MILLISECONDS) == true);
    REQUIRE(workspace.GetLastResult().IsSuccessful == true);
    const RenderGraphJobResult cli = RenderGraphJob::ParseOutput(
        {},
        RunCli(python, {QStringLiteral("dump"), QDir(package).filePath(ROOT_GRAPH), QStringLiteral("--root"), package}),
        0);
    REQUIRE(cli.IsSuccessful == true);
    CHECK(workspace.GetLastResult().Payload == cli.Payload);
    CHECK(QDir(QDir(project.path()).filePath(QStringLiteral("out/render-graph/drafts"))).isEmpty() == true);

    // A draft error in an imported library maps back to its authored node; disk is unchanged.
    const QByteArray library = ReadFile(QDir(package).filePath(LIBRARY));
    RenderGraphDocument& document = workspace.GetDocument();
    QJsonObject draw = FindNode(document, LIBRARY, QStringLiteral("ColorAndDepth"), QStringLiteral("draw"));
    draw.insert(QStringLiteral("depth_load"), QStringLiteral("Load"));
    REQUIRE(document.ReplaceNode(LIBRARY, QStringLiteral("ColorAndDepth"), draw) == RenderGraphDocumentStatus::Ok);
    REQUIRE(workspace.Validate() == true);
    REQUIRE(finished.wait(JOB_TIMEOUT_MILLISECONDS) == true);
    const RenderGraphJobResult failure = workspace.GetLastResult();
    CHECK(failure.IsSuccessful == false);
    CHECK(failure.Code == QStringLiteral("RenderGraphInitialization"));
    CHECK(failure.Location.value(QStringLiteral("instance")).toString() == QStringLiteral("Main/raster/draw"));
    CHECK(ReadFile(QDir(package).filePath(LIBRARY)) == library);
    REQUIRE(workspace.GetDiagnosticCount() == 1);
    REQUIRE(workspace.ActivateDiagnostic(0) == true);
    CHECK(workspace.GetSelection() == QStringLiteral("rasterize.lurender.json#ColorAndDepth/draw"));

    // Shader diagnostics open the authored function line; generated wrappers open their manifest owner.
    RenderGraphJobResult shader = failure;
    shader.Code = QStringLiteral("RenderGraphBuildShader");
    shader.Location = QJsonObject{{QStringLiteral("source"), QStringLiteral("main_shaders/geometry.slang")},
                                  {QStringLiteral("line"), 3}};
    shader.Diagnostics =
        QJsonArray{QJsonObject{{QStringLiteral("severity"), QStringLiteral("error")},
                               {QStringLiteral("message"), QStringLiteral("undefined identifier")},
                               {QStringLiteral("program"), QStringLiteral("Geometry")},
                               {QStringLiteral("target"), QStringLiteral("metal")},
                               {QStringLiteral("location"),
                                QJsonObject{{QStringLiteral("generated"), QStringLiteral("wrapper.slang")},
                                            {QStringLiteral("program"), QStringLiteral("Geometry")},
                                            {QStringLiteral("line"), 9}}}}};
    workspace.AcceptResult(shader);
    REQUIRE(workspace.GetDiagnosticCount() == 2);
    REQUIRE(workspace.ActivateDiagnostic(0) == true);
    CHECK(workspace.GetSourceFile() == QStringLiteral("main_shaders/geometry.slang"));
    CHECK(workspace.GetSourceLine() == 3);
    REQUIRE(workspace.ActivateDiagnostic(1) == true);
    CHECK(workspace.GetSourceFile() == QStringLiteral("main.lushader.json"));
    CHECK(QString::fromUtf8(document.GetDraft(QStringLiteral("main.lushader.json")))
              .split(QLatin1Char('\n'))
              .at(workspace.GetSourceLine() - 1)
              .contains(QStringLiteral("\"Geometry\"")) == true);

    // A stale request never replaces current diagnostics.
    RenderGraphJobResult stale = shader;
    stale.Request.Request += 100;
    stale.Code = QStringLiteral("RenderGraphStale");
    workspace.AcceptResult(stale);
    CHECK(workspace.GetLastResult().Code == QStringLiteral("RenderGraphBuildShader"));

    // Saving publishes exactly the draft; the CLI then reports the same mapped failure.
    REQUIRE(workspace.Save() == true);
    const RenderGraphJobResult saved = RenderGraphJob::ParseOutput(
        {},
        RunCli(python,
               {QStringLiteral("validate"), QDir(package).filePath(ROOT_GRAPH), QStringLiteral("--root"), package}),
        1);
    CHECK(saved.Code == failure.Code);
    CHECK(saved.Location == failure.Location);
    CHECK(saved.Message == failure.Message);
}

TEST_CASE("RG3 a real compiler error opens the authored shader line", "[editor][render-graph]")
{
    const QString python = FindPython();
    const QString compiler = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/shader-tools/slang/bin/slangc");
    if (python.isEmpty() == true || QFileInfo(compiler).isExecutable() == false)
    {
        SKIP("python3 and the pinned slangc (./scripts/shader-probe bootstrap) are required");
    }
    QTemporaryDir project;
    const QString package = MakePackage(project);
    // The helper directory has the installed SDK layout: shader drivers beside their locks.
    const QDir helper(QDir(project.path()).filePath(QStringLiteral("sdk")));
    MakeShaderHelper(helper.path());
    // An undefined identifier on line 3 of the authored color function.
    const QString color = QDir(package).filePath(QStringLiteral("main_shaders/color.slang"));
    QByteArray broken = ReadFile(color);
    REQUIRE(broken.split('\n').at(2).contains("value.Color") == true);
    broken.replace("value.Color", "undefinedColor");
    WriteFile(color, broken);

    // The build tree is derived relative to the project root; use the canonical temporary path.
    const QString root = QFileInfo(project.path()).canonicalFilePath();
    RendererWorkspace workspace({python, {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(root, 3);
    workspace.SetBuildTools(helper.path(), compiler);
    REQUIRE(workspace.OpenGraph(QDir(root).filePath(QStringLiteral("graphs/") + ROOT_GRAPH)) == true);
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    REQUIRE(workspace.Build() == true);
    REQUIRE(finished.wait(JOB_TIMEOUT_MILLISECONDS) == true);
    // The pinned compiler's report arrives through compile_shader.py's prefixed stderr line.
    const RenderGraphJobResult failure = workspace.GetLastResult();
    INFO(failure.Code.toStdString() + ": " + failure.Message.toStdString());
    CHECK(failure.Code == QStringLiteral("RenderGraphBuildShader"));
    CHECK(failure.Message.contains(QStringLiteral("Ludus shader build: main_shaders/color.slang(3)")) == true);
    CHECK(failure.Location == QJsonObject{{QStringLiteral("source"), QStringLiteral("main_shaders/color.slang")},
                                          {QStringLiteral("line"), 3}});
    REQUIRE(workspace.GetDiagnosticCount() >= 1);
    REQUIRE(workspace.ActivateDiagnostic(0) == true);
    CHECK(workspace.GetSourceFile() == QStringLiteral("main_shaders/color.slang"));
    CHECK(workspace.GetSourceLine() == 3);
}

TEST_CASE("RG3 Renderer tab joins the main window document actions", "[editor][render-graph]")
{
    EditorController controller({});
    QTemporaryDir settings;
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("layout.ini")));
    RendererWorkspace* workspace = window.findChild<RendererWorkspace*>(QStringLiteral("rendererWorkspace"));
    REQUIRE(workspace != nullptr);
    CHECK(workspace->CanSave() == false);
    CHECK(workspace->ConfirmDiscard() == true);
    CHECK(workspace->Validate() == false);
}

TEST_CASE("Renderer graph ownership follows the project descriptor rather than its CMake source folder",
          "[editor][render-graph][creation]")
{
    QTemporaryDir project;
    const QString descriptor = QDir(project.path()).filePath(QStringLiteral("ludus.project.json"));
    WriteFile(descriptor, QByteArrayLiteral(R"json({"version":1,"name":"Game","provider":"cmake",
"source_dir":"code","preset":"linux-clang-development","target":"app","run":{"cwd":".","args":[]}})json"));
    REQUIRE(QDir().mkpath(QDir(project.path()).filePath(QStringLiteral("code"))) == true);
    EditorController controller({FindPython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    MainWindow window(&controller, nullptr, QDir(project.path()).filePath(QStringLiteral("layout.ini")));
    controller.OpenProject(descriptor);
    RendererWorkspace* workspaceOrNull = window.findChild<RendererWorkspace*>(QStringLiteral("rendererWorkspace"));
    REQUIRE(workspaceOrNull != nullptr);
    QSignalSpy finished(workspaceOrNull, &RendererWorkspace::JobFinished);
    REQUIRE(workspaceOrNull->CreateGraph(QStringLiteral("Main")) == true);
    REQUIRE(finished.wait(JOB_TIMEOUT_MILLISECONDS) == true);
    CHECK(workspaceOrNull->GetDocument().IsLoaded() == true);
    CHECK(QFileInfo(QDir(project.path()).filePath(QStringLiteral("graphs/Main/main.lurender.json"))).isFile() == true);
    CHECK(QDir(QDir(project.path()).filePath(QStringLiteral("code")))
              .entryList(QDir::NoDotAndDotDot | QDir::AllEntries)
              .isEmpty() == true);
}

TEST_CASE("External graph saves refresh a clean draft as one undoable revision",
          "[editor][render-graph][source-editor]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString shader = QStringLiteral("main_shaders/color.slang");
    const QByteArray original = document.GetDraft(shader);
    bool hasChanged = true;
    REQUIRE(document.RefreshFromDisk(hasChanged) == RenderGraphDocumentStatus::Ok);
    CHECK(hasChanged == false);
    CHECK(document.GetRevision() == 0);
    const QByteArray external = QByteArray("// external save\n") + original;
    WriteFile(QDir(package).filePath(shader), external);
    REQUIRE(document.RefreshFromDisk(hasChanged) == RenderGraphDocumentStatus::Ok);
    CHECK(hasChanged == true);
    CHECK(document.GetDraft(shader) == external);
    CHECK(document.GetSaved(shader) == external);
    CHECK(document.IsDirty() == false);
    CHECK(document.GetRevision() == 1);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(shader) == original);
    CHECK(document.IsDirty() == true);
    const uint64 revision = document.GetRevision();
    WriteFile(QDir(package).filePath(shader), QByteArray("// another external save\n") + external);
    CHECK(document.RefreshFromDisk(hasChanged) == RenderGraphDocumentStatus::Conflict);
    CHECK(hasChanged == false);
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetDraft(shader) == original);
    CHECK(document.GetSaved(shader) == external);
}

TEST_CASE("External source refresh preserves a draft when files exceed admission limits",
          "[editor][render-graph][source-editor]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString shader = QStringLiteral("main_shaders/color.slang");
    const QByteArray original = document.GetDraft(shader);
    WriteFile(QDir(package).filePath(shader), QByteArray(RenderGraphDocument::MAX_FILE_BYTES + 1, 'x'));
    bool hasChanged = true;
    CHECK(document.RefreshFromDisk(hasChanged) == RenderGraphDocumentStatus::Capacity);
    CHECK(hasChanged == false);
    CHECK(document.GetDraft(shader) == original);
    CHECK(document.GetRevision() == 0);
}

TEST_CASE("Renderer validation automatically captures external shader saves", "[editor][render-graph][source-editor]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({FindPython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    const QString shader = QStringLiteral("main_shaders/color.slang");
    const QByteArray external = QByteArray("// external save\n") + workspace.GetDocument().GetDraft(shader);
    WriteFile(QDir(package).filePath(shader), external);
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    REQUIRE(workspace.Validate() == true);
    CHECK(workspace.GetDocument().GetDraft(shader) == external);
    CHECK(workspace.GetDocument().IsDirty() == false);
    REQUIRE(finished.wait(JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetLastResult().Code.toStdString() + ": " + workspace.GetLastResult().Message.toStdString());
    CHECK(workspace.GetLastResult().IsSuccessful == true);
    CHECK(workspace.GetLastResult().Request.Revision == workspace.GetDocument().GetRevision());
}

TEST_CASE("Code editor handoff cancellation retains unapplied source text", "[editor][render-graph][source-editor]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    QComboBox* selectorOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
    QPlainTextEdit* sourceOrNull = workspace.findChild<QPlainTextEdit*>(QStringLiteral("renderGraphSource"));
    REQUIRE(selectorOrNull != nullptr);
    REQUIRE(sourceOrNull != nullptr);
    const QString shader = QStringLiteral("main_shaders/color.slang");
    selectorOrNull->setCurrentText(shader);
    const QByteArray original = workspace.GetDocument().GetDraft(shader);
    const QString pending = QStringLiteral("// unapplied\n") + sourceOrNull->toPlainText();
    sourceOrNull->moveCursor(QTextCursor::Start);
    sourceOrNull->insertPlainText(QStringLiteral("// unapplied\n"));
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_SOURCE_EDITOR_CAPTURE");
    if (capture.isEmpty() == false)
    {
        ApplyWorkspaceStyle(&workspace);
        workspace.resize(1500, 900);
        QTabWidget* tabsOrNull = workspace.findChild<QTabWidget*>();
        REQUIRE(tabsOrNull != nullptr);
        tabsOrNull->setCurrentIndex(1);
        workspace.show();
        QApplication::processEvents();
        REQUIRE(workspace.grab().save(capture) == true);
    }
    QTimer::singleShot(0, &workspace, []() {
        QMessageBox* questionOrNull = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (questionOrNull != nullptr)
        {
            questionOrNull->done(QMessageBox::Cancel);
        }
    });
    CHECK(workspace.OpenSourceInCodeEditor() == false);
    CHECK(sourceOrNull->toPlainText() == pending);
    CHECK(workspace.GetDocument().GetDraft(shader) == original);
    CHECK(ReadFile(QDir(package).filePath(shader)) == original);
    CHECK(workspace.ReloadSourcesFromDisk() == false);
    CHECK(sourceOrNull->toPlainText() == pending);
}

TEST_CASE("Shader source exposes draft input fields and named outputs", "[editor][render-graph][source-editor]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    QComboBox* selectorOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
    QPlainTextEdit* interfaceOrNull =
        workspace.findChild<QPlainTextEdit*>(QStringLiteral("renderGraphSourceInterface"));
    QPlainTextEdit* sourceOrNull = workspace.findChild<QPlainTextEdit*>(QStringLiteral("renderGraphSource"));
    REQUIRE(selectorOrNull != nullptr);
    REQUIRE(interfaceOrNull != nullptr);
    REQUIRE(sourceOrNull != nullptr);
    selectorOrNull->setCurrentText(QStringLiteral("main_shaders/color.slang"));
    CHECK(interfaceOrNull->isReadOnly() == true);
    CHECK(interfaceOrNull->toPlainText().contains(QStringLiteral("PSMain (fragment)")) == true);
    CHECK(interfaceOrNull->toPlainText().contains(QStringLiteral("PositionClip : Float4  [SV_Position, built-in]")) ==
          true);
    CHECK(interfaceOrNull->toPlainText().contains(QStringLiteral("Color : Float4  [COLOR0, location 0]")) == true);
    CHECK(interfaceOrNull->toPlainText().contains(QStringLiteral("Color : float4 → SV_Target0")) == true);
    CHECK(sourceOrNull->toPlainText().toUtf8() == workspace.GetDocument().GetDraft(selectorOrNull->currentText()));
    CHECK(workspace.IsDirty() == false);
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_SHADER_INTERFACE_CAPTURE");
    if (capture.isEmpty() == false)
    {
        ApplyWorkspaceStyle(&workspace);
        workspace.resize(CREATION_WORKSPACE_WIDTH, CREATION_WORKSPACE_HEIGHT);
        QTabWidget* tabsOrNull = workspace.findChild<QTabWidget*>(QStringLiteral("renderGraphViews"));
        REQUIRE(tabsOrNull != nullptr);
        tabsOrNull->setCurrentIndex(1);
        workspace.show();
        QApplication::processEvents();
        REQUIRE(workspace.grab().save(capture) == true);
    }

    selectorOrNull->setCurrentText(QStringLiteral("main_shaders/geometry.slang"));
    CHECK(interfaceOrNull->toPlainText().contains(QStringLiteral("PositionMesh : Float3")) == true);
    CHECK(interfaceOrNull->toPlainText().contains(QStringLiteral("Outputs\nRasterVarying")) == true);
    selectorOrNull->setCurrentText(QStringLiteral("main_shaders/depth.slang"));
    CHECK(interfaceOrNull->toPlainText().contains(QStringLiteral("fixed-function depth")) == true);
    CHECK(interfaceOrNull->toPlainText().contains(QStringLiteral("returns void")) == true);
    CHECK(interfaceOrNull->toPlainText().contains(QStringLiteral("SV_Target0")) == false);

    const QString manifestFile = QStringLiteral("main.lushader.json");
    selectorOrNull->setCurrentText(manifestFile);
    CHECK(interfaceOrNull->parentWidget()->isHidden() == true);
    sourceOrNull->setPlainText(QStringLiteral("{ broken manifest"));
    QPushButton* applyOrNull = nullptr;
    const QList<QPushButton*> buttons = workspace.findChildren<QPushButton*>();
    for (QPushButton* button : buttons)
    {
        if (button->text() == QStringLiteral("Apply Text to Draft"))
        {
            applyOrNull = button;
        }
    }
    REQUIRE(applyOrNull != nullptr);
    applyOrNull->click();
    selectorOrNull->setCurrentText(QStringLiteral("main_shaders/color.slang"));
    CHECK(interfaceOrNull->toPlainText().contains(QStringLiteral("missing or invalid")) == true);
    workspace.Undo();
    CHECK(interfaceOrNull->toPlainText().contains(QStringLiteral("Color : float4 → SV_Target0")) == true);
}

TEST_CASE("Shader interface lists shared-source programs and uniform fields", "[editor][render-graph][source-editor]")
{
    const QByteArray manifest = R"json({
      "schemas":[{"id":"Varying","fields":[{"id":"Color","type":"Float4","semantic":"COLOR0","location":0}]},
                 {"id":"Material","role":"Uniform","fields":[{"id":"Tint","type":"Float4"}]}],
      "programs":[{"id":"Tinted","function":"Shade","stage":"fragment","source":"shaders/color.slang",
                   "input":"Varying","output":"Color","bindings":[{"id":"material","schema":"Material","slot":2}]},
                  {"id":"Coverage","function":"Coverage","stage":"fragment","source":"shaders/color.slang",
                   "input":"Varying","output":"Depth","bindings":[]}]
    })json";
    const QString description = DescribeShaderInterface(manifest, QStringLiteral("shaders/./color.slang"));
    CHECK(description.contains(QStringLiteral("Tinted — Shade (fragment)")) == true);
    CHECK(description.contains(QStringLiteral("Coverage — Coverage (fragment)")) == true);
    CHECK(description.contains(QStringLiteral("material — slot 2, set 0")) == true);
    CHECK(description.contains(QStringLiteral("Tint : Float4")) == true);
    CHECK(DescribeShaderInterface(manifest, QStringLiteral("other.slang"))
              .contains(QStringLiteral("No shader program")) == true);
    CHECK(DescribeShaderInterface(QByteArrayLiteral("[]"), QStringLiteral("color.slang"))
              .contains(QStringLiteral("unavailable")) == true);
}
