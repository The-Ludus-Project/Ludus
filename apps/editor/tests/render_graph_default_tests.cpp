// Default render-graph entry: a project without a graph shows and previews the engine default,
// the first edit becomes an undoable project draft that Save creates under graphs/Main/, the
// explicit copy never replaces project files, and an edited default builds like any graph.
// Offscreen widgets prove document, file and job wiring; GPU pixels of the prebuilt default
// are covered by render_graph_preview_tests.cpp.
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_job.h"
#include "internal/render_graph_live_build.h"
#include "internal/renderer_workspace.h"
#include "render_graph_helper_fixture.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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
constexpr QLatin1String CONFIGURATION("Default");
constexpr QLatin1String LIBRARY("rasterize.lurender.json");
constexpr QLatin1String COLOR_SHADER("main_shaders/color.slang");

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

/// The workspace binds the canonical project root (a temporary directory may be a symlink).
QString GetProjectRoot(const QTemporaryDir& project)
{
    return QFileInfo(project.path()).canonicalFilePath();
}

QString GetPackage(const QTemporaryDir& project)
{
    return QDir(GetProjectRoot(project)).filePath(PROJECT_RENDER_GRAPH_DIRECTORY);
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
                                              QStringLiteral("main_shaders/lighting.slang"),
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
          QDir(GetProjectRoot(project)).filePath(QStringLiteral("out/render-graph/graphs/Main/main/main.lurgrt")));
    CHECK(workspace.GetPreviewPackagePath() == workspace.GetRuntimePackagePath());
    CHECK(ListProject(project).isEmpty() == true);

    // Undo returns to the clean engine default.
    REQUIRE(workspace.CanUndo() == true);
    workspace.Undo();
    CHECK(workspace.IsDirty() == false);
    CHECK(workspace.IsBuiltIn() == true);
    CHECK(document.GetDraft(DEFAULT_RENDER_GRAPH_ROOT) == original);
    CHECK(workspace.GetStatusText().contains(QStringLiteral("Engine default renderer")) == true);

    // Redo, then Save: the whole package is created under graphs/Main/ and says so.
    workspace.Redo();
    CHECK(workspace.GetStatusText().contains(QStringLiteral("Save creates graphs/Main/")) == true);
    REQUIRE(workspace.Save() == true);
    CHECK(workspace.IsBuiltIn() == false);
    CHECK(workspace.IsDirty() == false);
    CHECK(workspace.GetStatusText().contains(QStringLiteral("Created graphs/Main in the project")) == true);
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

TEST_CASE("A project copy of the default has its own build identity", "[editor][render-graph]")
{
    // New Graph's starter is byte-identical to the default; the live build must still cook it.
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    RenderGraphDocument& document = workspace.GetDocument();
    REQUIRE(document.IsBuiltIn() == true);
    const QString configuration = QStringLiteral("Default");
    QHash<QString, QByteArray> builtInSources;
    for (const QString& file : document.GetFiles(RenderGraphSource::Saved))
    {
        builtInSources.insert(file, document.GetSaved(file));
    }
    const QByteArray builtIn = ComputeRenderGraphBuildIdentity(document, configuration, RenderGraphSource::Saved);
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    REQUIRE(document.IsBuiltIn() == false);
    for (const QString& file : document.GetFiles(RenderGraphSource::Saved))
    {
        if (builtInSources.contains(file) == true)
        {
            CHECK(document.GetSaved(file) == builtInSources.value(file));
        }
    }
    const QByteArray copy = ComputeRenderGraphBuildIdentity(document, configuration, RenderGraphSource::Saved);
    CHECK(copy != builtIn);
    // The identity stays stable for unchanged sources at the same location.
    CHECK(ComputeRenderGraphBuildIdentity(document, configuration, RenderGraphSource::Saved) == copy);
}

TEST_CASE("An unreadable project graph reports failure without substituting the default", "[editor][render-graph]")
{
    QTemporaryDir project;
    const QString graph = QDir(GetPackage(project)).filePath(DEFAULT_RENDER_GRAPH_ROOT);
    const QByteArray oversized(RenderGraphDocument::MAX_FILE_BYTES + 1, ' ');
    WriteFile(graph, oversized);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    CHECK(workspace.IsBuiltIn() == false);
    CHECK(workspace.GetDocument().IsLoaded() == false);
    CHECK(workspace.GetStatusText().isEmpty() == false);
    CHECK(workspace.CustomizeBuiltIn() == false);
    CHECK(ReadFile(graph) == oversized);
}

TEST_CASE("Build & Preview builds an edited default like any project graph", "[editor][render-graph]")
{
    const QString python = FindPython();
    const QString compiler = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/shader-tools/slang/bin/slangc");
    if (python.isEmpty() == true || QFileInfo(compiler).isExecutable() == false)
    {
        SKIP("Needs python3 and the prepared pinned Slang compiler");
    }
    // Explicit tools stand in for project SDK detection, which this bare folder has no setup for.
    QTemporaryDir tools;
    const QDir helper(QDir(tools.path()).filePath(QStringLiteral("shaders")));
    tests::CopyRenderGraphBuildHelpers(helper.path());
    QTemporaryDir project;
    RendererWorkspace workspace({python, {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetWorkerSocket({});
    workspace.SetProject(project.path(), 1);
    workspace.SetBuildTools(helper.path(), compiler);
    RenderGraphDocument& document = workspace.GetDocument();
    // The default targets SPIR-V on Linux, which also needs the validator as an override.
    const QJsonArray profiles = QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json")))
                                    .object()
                                    .value(QStringLiteral("profiles"))
                                    .toArray();
    if (profiles.contains(QStringLiteral("spirv")) == true)
    {
        const QString validator = QStringLiteral(LUDUS_EDITOR_TEST_SPIRV_VALIDATOR);
        if (QFileInfo(validator).isExecutable() == false)
        {
            SKIP("The default's SPIR-V profile needs the configured spirv-val");
        }
        RenderGraphBuildSettings settings = workspace.GetBuildSettings();
        settings.Tools.Validator = validator;
        workspace.SetBuildSettings(settings);
    }
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

TEST_CASE("Project graph resolution selects named packages and keeps libraries under their execution root",
          "[editor][render-graph]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    const QDir graphs(QDir(project.path()).filePath(QStringLiteral("graphs")));
    REQUIRE(QDir().rename(graphs.filePath(QStringLiteral("Main")), graphs.filePath(QStringLiteral("Other"))) == true);
    RendererWorkspace reopened({});
    reopened.SetProject(project.path(), 2);
    REQUIRE(reopened.IsBuiltIn() == false);
    CHECK(reopened.GetDocument().GetPackageRoot().endsWith(QStringLiteral("/graphs/Other")) == true);
    REQUIRE(reopened.OpenGraph(
                QDir(reopened.GetDocument().GetPackageRoot()).filePath(QStringLiteral("rasterize.lurender.json"))) ==
            true);
    CHECK(reopened.GetDocument().GetRootGraph() == DEFAULT_RENDER_GRAPH_ROOT);
}
