// RG3 Renderer workspace: typed render-graph drafts, explicit save, mapped
// diagnostics and CLI/editor equivalence through the shared ludus_tools backend.
// Offscreen widgets prove document/job wiring, not GPU output or a node canvas.
#include "internal/controller.h"
#include "internal/main_window.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_job.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
constexpr int32 JOB_TIMEOUT_MILLISECONDS = 60000;
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
QByteArray RunCli(const QString& python, const QStringList& arguments)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONPATH"), QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/scripts/python"));
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    process.start(python,
                  QStringList{QStringLiteral("-m"),
                              QStringLiteral("ludus_tools"),
                              QStringLiteral("--json"),
                              QStringLiteral("render-graph")} +
                      arguments);
    REQUIRE(process.waitForFinished(JOB_TIMEOUT_MILLISECONDS) == true);
    return process.readAllStandardOutput();
}
} // namespace

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
    workspace.SetBuildTools({ .Helper = helper.path(), .Compiler = compiler, .Validator = {}, .SpirvCross = {} });
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
