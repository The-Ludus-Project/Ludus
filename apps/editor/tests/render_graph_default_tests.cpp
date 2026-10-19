// Default render-graph entry: a project without a graph shows and previews the engine default,
// the first edit becomes an undoable project draft that Save creates under renderer/, the
// explicit copy never replaces project files, and shader tools default to the ones this
// editor build used. Offscreen widgets prove document, file and job wiring; GPU pixels of
// the prebuilt default are covered by render_graph_preview_tests.cpp.
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_job.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
constexpr int32 JOB_TIMEOUT_MILLISECONDS = 120000;
const QString CONFIGURATION = QStringLiteral("Default");
const QString LIBRARY = QStringLiteral("rasterize.lurender.json");
const QString COLOR_SHADER = QStringLiteral("main_shaders/color.slang");

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

/// Every entry of a project folder, including hidden files: proves nothing was written.
QStringList ListProject(const QTemporaryDir& project)
{
    return QDir(project.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
}

QString GetPackage(const QTemporaryDir& project)
{
    return QDir(project.path()).filePath(PROJECT_RENDER_GRAPH_DIRECTORY);
}

/// An authored-field edit of the default's camera node, as the inspector applies it.
RenderGraphDocumentStatus DeactivateCamera(RenderGraphDocument& inoutDocument)
{
    QJsonObject graph;
    REQUIRE(inoutDocument.GetGraph(DEFAULT_RENDER_GRAPH_ROOT, graph) == true);
    const QJsonArray nodes = graph.value(QStringLiteral("configurations"))
                                 .toArray()
                                 .at(0)
                                 .toObject()
                                 .value(QStringLiteral("nodes"))
                                 .toArray();
    for (const QJsonValueConstRef value : nodes)
    {
        QJsonObject node = value.toObject();
        if (node.value(QStringLiteral("id")).toString() == QStringLiteral("camera"))
        {
            node.insert(QStringLiteral("active"), false);
            return inoutDocument.ReplaceNode(DEFAULT_RENDER_GRAPH_ROOT, CONFIGURATION, node);
        }
    }
    FAIL("The default graph has no camera node");
    return RenderGraphDocumentStatus::NotFound;
}

bool HasVisibleDialog()
{
    const QWidgetList windows = QApplication::topLevelWidgets();
    for (const QWidget* window : windows)
    {
        if (qobject_cast<const QDialog*>(window) != nullptr && window->isVisible() == true)
        {
            return true;
        }
    }
    return false;
}

QString FindPython()
{
    const QString managed = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/host-tools/venv/bin/python");
    return QFileInfo(managed).isExecutable() == true ? managed
                                                     : QStandardPaths::findExecutable(QStringLiteral("python3"));
}
} // namespace

TEST_CASE("A project without a render graph shows the engine default and writes nothing", "[editor][render-graph]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.IsBuiltIn() == true);
    const RenderGraphDocument& document = workspace.GetDocument();
    CHECK(document.GetRootGraph() == DEFAULT_RENDER_GRAPH_ROOT);
    CHECK(document.GetPackageRoot() == GetPackage(project));
    CHECK(document.GetFiles() == QStringList({QStringLiteral("main.lurender.json"),
                                              QStringLiteral("main.lushader.json"),
                                              QStringLiteral("main_shaders/color.slang"),
                                              QStringLiteral("main_shaders/depth.slang"),
                                              QStringLiteral("main_shaders/geometry.slang"),
                                              LIBRARY}));
    // The embedded sources are the engine-owned package (its manifest is configured per host).
    const QDir engine(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/modules/graphics/renderer/render_graphs/default"));
    for (const QString& file : document.GetFiles())
    {
        if (file.endsWith(QStringLiteral(".lushader.json")) == false)
        {
            CHECK(document.GetDraft(file) == ReadFile(engine.filePath(file)));
        }
    }
    CHECK(QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json")))
              .object()
              .value(QStringLiteral("id"))
              .toString() == QStringLiteral("RendererStaticBuild"));

    // Nothing asks for a graph, nothing is dirty, and neither Save nor discard prompts write.
    CHECK(workspace.IsDirty() == false);
    CHECK(workspace.CanSave() == false);
    CHECK(workspace.ConfirmDiscard() == true);
    CHECK(workspace.Save() == true);
    CHECK(HasVisibleDialog() == false);
    CHECK(workspace.GetStatusText().contains(QStringLiteral("Engine default renderer")) == true);
    CHECK(workspace.GetStatusText().contains(QStringLiteral("required")) == false);
    CHECK(ListProject(project).isEmpty() == true);

    // Preview needs no project files: an unedited default loads the editor's prebuilt generation.
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    REQUIRE(GetDefaultRenderGraphRuntime().isEmpty() == false);
    CHECK(workspace.GetPreviewPackagePath() == GetDefaultRenderGraphRuntime());
    CHECK(workspace.BuildAndPreview() == true);
    CHECK(workspace.IsJobActive() == false);
#else
    CHECK(GetDefaultRenderGraphRuntime().isEmpty() == true);
    CHECK(workspace.GetPreviewPackagePath() == workspace.GetRuntimePackagePath());
#endif
    CHECK(ListProject(project).isEmpty() == true);

    // Closing the project closes the default without a question.
    workspace.SetProject({}, 2);
    CHECK(workspace.GetDocument().IsLoaded() == false);
    CHECK(workspace.IsBuiltIn() == false);
}

TEST_CASE("The first edit of the default is an undoable project draft that Save creates", "[editor][render-graph]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray original = document.GetDraft(DEFAULT_RENDER_GRAPH_ROOT);

    // First edit: the draft belongs to the project and names where Save puts it; disk is untouched.
    REQUIRE(DeactivateCamera(document) == RenderGraphDocumentStatus::Ok);
    CHECK(workspace.IsDirty() == true);
    CHECK(workspace.IsBuiltIn() == true);
    CHECK(document.IsFileDirty(DEFAULT_RENDER_GRAPH_ROOT) == true);
    CHECK(document.IsFileDirty(LIBRARY) == false);
    CHECK(workspace.GetRuntimePackagePath() ==
          QDir(project.path()).filePath(QStringLiteral("out/render-graph/renderer/main/main.lurgrt")));
    CHECK(workspace.GetPreviewPackagePath() == workspace.GetRuntimePackagePath());
    CHECK(ListProject(project).isEmpty() == true);

    // Undo returns to the clean engine default.
    REQUIRE(workspace.CanUndo() == true);
    workspace.Undo();
    CHECK(workspace.IsDirty() == false);
    CHECK(workspace.IsBuiltIn() == true);
    CHECK(document.GetDraft(DEFAULT_RENDER_GRAPH_ROOT) == original);
    CHECK(workspace.GetStatusText().contains(QStringLiteral("Engine default renderer")) == true);

    // Redo, then Save: the whole package is created under renderer/ and says so.
    workspace.Redo();
    CHECK(workspace.GetStatusText().contains(QStringLiteral("Save creates renderer/")) == true);
    REQUIRE(workspace.Save() == true);
    CHECK(workspace.IsBuiltIn() == false);
    CHECK(workspace.IsDirty() == false);
    CHECK(workspace.GetStatusText().contains(QStringLiteral("Created renderer in the project")) == true);
    const QDir package(GetPackage(project));
    for (const QString& file : document.GetFiles())
    {
        CHECK(ReadFile(package.filePath(file)) == document.GetDraft(file));
    }
    CHECK(ReadFile(package.filePath(DEFAULT_RENDER_GRAPH_ROOT)) != original);
    CHECK(QDir(package).exists(QStringLiteral(".ludus-editor-render-graph.lock")) == false);

    // Undo after the copy is an ordinary edit of the project's package.
    workspace.Undo();
    CHECK(workspace.IsDirty() == true);
    CHECK(workspace.IsBuiltIn() == false);
    workspace.Redo();

    // Reopening the project uses its own graph instead of the default.
    RendererWorkspace reopened({});
    reopened.SetProject(project.path(), 2);
    CHECK(reopened.IsBuiltIn() == false);
    CHECK(reopened.GetDocument().GetDraft(DEFAULT_RENDER_GRAPH_ROOT) ==
          ReadFile(package.filePath(DEFAULT_RENDER_GRAPH_ROOT)));
}

TEST_CASE("Customize copies the default once and never replaces project files", "[editor][render-graph]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    CHECK(workspace.IsBuiltIn() == false);
    CHECK(workspace.IsDirty() == false);
    CHECK(QFileInfo::exists(QDir(GetPackage(project)).filePath(DEFAULT_RENDER_GRAPH_ROOT)) == true);
    CHECK(workspace.CustomizeBuiltIn() == false);

    // A stray file at the destination is a conflict: nothing is written or replaced.
    QTemporaryDir occupied;
    const QString stray = QDir(GetPackage(occupied)).filePath(COLOR_SHADER);
    WriteFile(stray, QByteArray("// user file\n"));
    RendererWorkspace other({});
    other.SetProject(occupied.path(), 1);
    REQUIRE(other.IsBuiltIn() == true);
    CHECK(other.CustomizeBuiltIn() == false);
    CHECK(other.GetStatusText().contains(QStringLiteral("nothing was written")) == true);
    CHECK(other.IsBuiltIn() == true);
    CHECK(ReadFile(stray) == QByteArray("// user file\n"));
    CHECK(QDir(GetPackage(occupied)).entryList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty() == true);

    // Without a project there is nowhere to copy to and no default is shown.
    RendererWorkspace closed({});
    CHECK(closed.IsBuiltIn() == false);
    CHECK(closed.CustomizeBuiltIn() == false);
}

TEST_CASE("Render-graph build tools default to the editor build and accept overrides", "[editor][render-graph]")
{
    const RenderGraphBuildTools defaults
    {
        .Helper = QStringLiteral("/sdk/shaders"),
        .Compiler = QStringLiteral("/sdk/slangc"),
        .Validator = QStringLiteral("/sdk/spirv-val"),
        .SpirvCross = {},
    };
    const RenderGraphBuildTools chosen = ResolveBuildTools(
        {
            .Helper = {},
            .Compiler = QStringLiteral(" /mine/slangc "),
            .Validator = QStringLiteral("  "),
            .SpirvCross = {},
        },
        defaults);
    CHECK(chosen.Helper == defaults.Helper);
    CHECK(chosen.Compiler == QStringLiteral("/mine/slangc"));
    CHECK(chosen.Validator == defaults.Validator);
    CHECK(chosen.SpirvCross.isEmpty() == true);

    // Empty override fields use the tools this editor was built with.
    RendererWorkspace workspace({});
    workspace.SetBuildTools({});
    const RenderGraphBuildTools editor = GetEditorBuildTools();
    CHECK(workspace.GetBuildTools().Helper == editor.Helper);
    CHECK(workspace.GetBuildTools().Compiler == editor.Compiler);
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    REQUIRE(editor.Helper.isEmpty() == false);
    REQUIRE(editor.Compiler.isEmpty() == false);
    CHECK(QFileInfo::exists(QDir(editor.Helper).filePath(QStringLiteral("compile_shader.py"))) == true);
    CHECK(QFileInfo::exists(QDir(editor.Helper).filePath(QStringLiteral("shader_toolchain.json"))) == true);
#endif

    // A build with neither defaults nor overrides explains itself instead of starting a job.
    QTemporaryDir project;
    workspace.SetProject(project.path(), 1);
    workspace.SetDefaultBuildTools({});
    REQUIRE(DeactivateCamera(workspace.GetDocument()) == RenderGraphDocumentStatus::Ok);
    CHECK(workspace.Build() == false);
    CHECK(workspace.IsJobActive() == false);
    CHECK(workspace.GetStatusText().contains(QStringLiteral("Tool overrides")) == true);
    CHECK(ListProject(project).isEmpty() == true);
}

TEST_CASE("Build & Preview builds an edited default with the editor's tools", "[editor][render-graph]")
{
    const QString python = FindPython();
    const RenderGraphBuildTools tools = GetEditorBuildTools();
    if (python.isEmpty() == true || tools.Helper.isEmpty() == true || tools.Compiler.isEmpty() == true)
    {
        SKIP("Needs python3 and an editor built with the native preview's shader tools");
    }
    QTemporaryDir project;
    RendererWorkspace workspace({python, {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetBuildTools({});
    workspace.SetWorkerSocket({});
    workspace.SetProject(project.path(), 1);
    RenderGraphDocument& document = workspace.GetDocument();
    REQUIRE(document.SetText(COLOR_SHADER, QByteArray("// customized\n") + document.GetDraft(COLOR_SHADER)) ==
            RenderGraphDocumentStatus::Ok);
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    REQUIRE(workspace.BuildAndPreview() == true);
    REQUIRE(finished.wait(JOB_TIMEOUT_MILLISECONDS) == true);
    const RenderGraphJobResult& result = workspace.GetLastResult();
    INFO(qPrintable(result.Code + QStringLiteral(": ") + result.Message));
    REQUIRE(result.IsSuccessful == true);
    CHECK(QFileInfo(workspace.GetRuntimePackagePath()).isFile() == true);
    CHECK(workspace.GetPreviewPackagePath() == workspace.GetRuntimePackagePath());
    // Building the draft wrote only build products; the sources still wait for Save.
    CHECK(ListProject(project) == QStringList{QStringLiteral("out")});
    CHECK(workspace.IsBuiltIn() == true);
}
