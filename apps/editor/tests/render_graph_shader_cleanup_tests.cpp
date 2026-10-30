#include "internal/render_graph_document.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
const QString MANIFEST = QStringLiteral("main.lushader.json");

void WriteSource(const QTemporaryDir& directory, const QString& path, const QByteArray& bytes)
{
    const QString destination = QDir(directory.path()).filePath(path);
    REQUIRE(QDir().mkpath(QFileInfo(destination).absolutePath()) == true);
    QFile file(destination);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}

void MakePackage(const QTemporaryDir& directory)
{
    WriteSource(directory, ROOT_GRAPH, R"json({"version":1,"id":"Main","default":"Default","configurations":[
      {"id":"Default","nodes":[{"id":"child","kind":"Subgraph","asset":"nested/child.lurender.json"}]},
      {"id":"Inactive","nodes":[{"id":"pass","kind":"GraphicsPipeline","shaders":[{"program":"InactiveProgram"}]}]}
    ]})json");
    WriteSource(directory, QStringLiteral("nested/child.lurender.json"), R"json({"version":1,"configurations":[
      {"id":"UnusedConfiguration","nodes":[{"id":"pass","kind":"GraphicsPipeline","shaders":[{"program":"NestedProgram"}]}]}
    ]})json");
    WriteSource(directory, MANIFEST, R"json({"version":1,"programs":[
      {"id":"InactiveProgram","source":"inactive.slang"},
      {"id":"NestedProgram","source":"nested/used.slang"},
      {"id":"TechniqueVertex","source":"technique.slang"},
      {"id":"TechniqueHit","source":"technique.slang"},
      {"id":"ExclusiveUnused","source":"exclusive.slang"},
      {"id":"SharedUnused","source":"inactive.slang"},
      {"id":"SharedUnusedA","source":"shared.slang"},
      {"id":"SharedUnusedB","source":"shared.slang"},
      {"id":"IncludedUnused","source":"included.slang"},
      {"id":"TransitiveUnused","source":"transitive.slang"},
      {"id":"ImportedUnused","source":"imported.slang"}
    ],"techniques":[{"id":"Unselected","vertex":"TechniqueVertex","closest_hit":"TechniqueHit"}]})json");
    WriteSource(directory, QStringLiteral("nested/other.lushader.json"), R"json({"version":1,"programs":[
      {"id":"OtherUnused","source":"other.slang"},
      {"id":"OtherTechnique","source":"other_kept.slang"}
    ],"techniques":[{"id":"Other","miss":"OtherTechnique"}]})json");
    for (const QString& source : {QStringLiteral("inactive.slang"),
                                  QStringLiteral("nested/used.slang"),
                                  QStringLiteral("technique.slang"),
                                  QStringLiteral("exclusive.slang"),
                                  QStringLiteral("shared.slang"),
                                  QStringLiteral("transitive.slang"),
                                  QStringLiteral("imported.slang"),
                                  QStringLiteral("nested/other.slang"),
                                  QStringLiteral("nested/other_kept.slang")})
    {
        WriteSource(directory, source, QByteArrayLiteral("// authored shader\n"));
    }
    WriteSource(directory, QStringLiteral("included.slang"), QByteArrayLiteral("#include \"transitive.slang\"\n"));
    WriteSource(directory,
                QStringLiteral("loose.slang"),
                QByteArrayLiteral("#include \"included.slang\"\nimport imported;\n"));
}

QMap<QString, QByteArray> GetSources(const RenderGraphDocument& document)
{
    QMap<QString, QByteArray> files;
    for (const QString& file : document.GetFiles())
    {
        files.insert(file, document.GetDraft(file));
    }
    return files;
}
} // namespace

TEST_CASE("shader cleanup scans every configuration subgraph and technique and preserves shared dependencies",
          "[editor][render-graph][shader-cleanup]")
{
    QTemporaryDir directory;
    MakePackage(directory);
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QMap<QString, QByteArray> original = GetSources(document);
    QStringList programs;
    QStringList sources;
    QString error;
    REQUIRE(document.RemoveUnusedShaders(programs, sources, error) == RenderGraphDocumentStatus::Ok);
    CHECK(programs.size() == 8);
    // Both programs of shared.slang are unused, so the source is free although two registrations named it.
    CHECK(sources == QStringList{QStringLiteral("exclusive.slang"),
                                 QStringLiteral("nested/other.slang"),
                                 QStringLiteral("shared.slang")});
    const QByteArray manifest = document.GetDraft(MANIFEST);
    for (const QByteArray& identity : {QByteArrayLiteral("InactiveProgram"),
                                       QByteArrayLiteral("NestedProgram"),
                                       QByteArrayLiteral("TechniqueVertex"),
                                       QByteArrayLiteral("TechniqueHit")})
    {
        CHECK(manifest.contains(identity) == true);
    }
    CHECK(manifest.contains("ExclusiveUnused") == false);
    CHECK(manifest.contains("IncludedUnused") == false);
    CHECK(document.GetDraft(QStringLiteral("nested/other.lushader.json")).contains("OtherTechnique") == true);
    for (const QString& file : original.keys())
    {
        if (file.endsWith(QStringLiteral(".lushader.json")) == false && sources.contains(file) == false)
        {
            CHECK(document.GetDraft(file) == original.value(file));
        }
    }
    // Draft pruning never deletes disk files, and all bytes restore in one step.
    CHECK(QFileInfo::exists(QDir(directory.path()).filePath(QStringLiteral("exclusive.slang"))) == true);
    const QMap<QString, QByteArray> cleaned = GetSources(document);
    REQUIRE(document.Undo() == true);
    CHECK(GetSources(document) == original);
    CHECK(document.IsDirty() == false);
    REQUIRE(document.Redo() == true);
    CHECK(GetSources(document) == cleaned);
    const uint64 revision = document.GetRevision();
    REQUIRE(document.RemoveUnusedShaders(programs, sources, error) == RenderGraphDocumentStatus::Ok);
    CHECK(programs.isEmpty() == true);
    CHECK(sources.isEmpty() == true);
    CHECK(document.GetRevision() == revision);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    CHECK(QFileInfo::exists(QDir(directory.path()).filePath(QStringLiteral("exclusive.slang"))) == false);
    CHECK(QFileInfo::exists(QDir(directory.path()).filePath(QStringLiteral("included.slang"))) == true);
    REQUIRE(document.Undo() == true);
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    CHECK(QFileInfo::exists(QDir(directory.path()).filePath(QStringLiteral("exclusive.slang"))) == true);
}

TEST_CASE("shader cleanup preserves an authored traversal source after removing its unused program",
          "[editor][render-graph][shader-cleanup]")
{
    QTemporaryDir directory;
    MakePackage(directory);
    WriteSource(directory, ROOT_GRAPH, R"json({"version":1,"id":"Main","default":"Default","configurations":[
      {"id":"Default","nodes":[{"id":"trace","kind":"RayTracingPipeline",
       "invocation":{"type":"Compute","traversal":"Custom","traversal_source":"exclusive.slang"}}]}
    ]})json");
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QStringList programs;
    QStringList sources;
    QString error;
    REQUIRE(document.RemoveUnusedShaders(programs, sources, error) == RenderGraphDocumentStatus::Ok);
    CHECK(programs.contains(MANIFEST + QStringLiteral("#ExclusiveUnused")) == true);
    CHECK(sources.contains(QStringLiteral("exclusive.slang")) == false);
    CHECK(document.GetDraft(QStringLiteral("exclusive.slang")).isEmpty() == false);
}

TEST_CASE("shader cleanup rejects incomplete reference data without changing history or outputs",
          "[editor][render-graph][shader-cleanup]")
{
    QTemporaryDir directory;
    MakePackage(directory);
    QString damaged = QStringLiteral("nested/child.lurender.json");
    QByteArray replacement = QByteArrayLiteral("{");
    SECTION("malformed nested graph") {}
    SECTION("malformed secondary manifest")
    {
        damaged = QStringLiteral("nested/other.lushader.json");
    }
    SECTION("unsupported version")
    {
        replacement = QByteArrayLiteral("{\"version\":3,\"configurations\":[]}");
    }
    SECTION("malformed configurations")
    {
        replacement = QByteArrayLiteral("{\"configurations\":{}}");
    }
    SECTION("malformed shader table")
    {
        replacement = QByteArrayLiteral(R"({"version":1,"configurations":[{"nodes":[{"shaders":{}}]}]})");
    }
    SECTION("missing subgraph")
    {
        replacement = QByteArrayLiteral(
            R"({"version":1,"configurations":[{"nodes":[{"kind":"Subgraph","asset":"missing.lurender.json"}]}]})");
    }
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.SetText(damaged, replacement) == RenderGraphDocumentStatus::Ok);
    const QMap<QString, QByteArray> original = GetSources(document);
    const uint64 revision = document.GetRevision();
    QStringList programs{QStringLiteral("sentinel")};
    QStringList sources = programs;
    QString error;
    CHECK(document.RemoveUnusedShaders(programs, sources, error) == RenderGraphDocumentStatus::Malformed);
    CHECK(error.contains(damaged) == true);
    CHECK(GetSources(document) == original);
    CHECK(document.GetRevision() == revision);
    CHECK(programs == QStringList{QStringLiteral("sentinel")});
    CHECK(sources == programs);
}

TEST_CASE("shader cleanup save refuses externally changed sources before deleting anything",
          "[editor][render-graph][shader-cleanup]")
{
    QTemporaryDir directory;
    MakePackage(directory);
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QStringList programs;
    QStringList sources;
    QString error;
    REQUIRE(document.RemoveUnusedShaders(programs, sources, error) == RenderGraphDocumentStatus::Ok);
    WriteSource(directory, QStringLiteral("exclusive.slang"), QByteArrayLiteral("// changed externally\n"));
    QStringList saved;
    CHECK(document.Save(saved) == RenderGraphDocumentStatus::Conflict);
    CHECK(saved.isEmpty() == true);
    CHECK(QFileInfo::exists(QDir(directory.path()).filePath(QStringLiteral("nested/other.slang"))) == true);
    CHECK(document.IsDirty() == true);
}

TEST_CASE("shader cleanup retains source files when includes leave the tracked snapshot",
          "[editor][render-graph][shader-cleanup]")
{
    QTemporaryDir directory;
    MakePackage(directory);
    QByteArray include = QByteArrayLiteral("#include \"untracked.slangh\"\n");
    SECTION("untracked header") {}
    SECTION("computed include")
    {
        include = QByteArrayLiteral("#include SHARED_HEADER\n");
    }
    WriteSource(directory, QStringLiteral("loose.slang"), include);
    WriteSource(directory, QStringLiteral("untracked.slangh"), QByteArrayLiteral("#include \"exclusive.slang\"\n"));
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QStringList programs;
    QStringList sources;
    QString error;
    REQUIRE(document.RemoveUnusedShaders(programs, sources, error) == RenderGraphDocumentStatus::Ok);
    CHECK(programs.size() == 8);
    CHECK(sources.isEmpty() == true);
    CHECK(document.GetFiles().contains(QStringLiteral("exclusive.slang")) == true);
}

namespace
{
const QString TEMPLATE_GRAPH = QStringLiteral("template.lurender.json");
const QString INNER_GRAPH = QStringLiteral("template/inner.lurender.json");

/// A root graph that imports an applied template's library (which imports an inner library) and
/// another library whose sources reuse the template's file names in their own folder.
void MakeTemplatePackage(const QTemporaryDir& directory)
{
    WriteSource(directory, ROOT_GRAPH, R"json({"version":2,"id":"Main","default":"Default","configurations":[
      {"id":"Default","nodes":[
        {"id":"root","kind":"GraphicsPipeline","shaders":[{"program":"RootProgram"}]},
        {"id":"Template","kind":"Subgraph","asset":"template.lurender.json","configuration":"Template"},
        {"id":"Other","kind":"Subgraph","asset":"other.lurender.json","configuration":"Other"}],
       "edges":[{"from":"Template.color","to":"root.color"}],
       "outputs":[{"id":"color","source":"Template.color"}]}
    ]})json");
    WriteSource(directory, TEMPLATE_GRAPH, R"json({"version":2,"id":"Template","default":"Template",
      "configurations":[{"id":"Template","nodes":[
        {"id":"primary","kind":"ComputePipeline","shaders":[{"program":"TemplatePrimary"}]},
        {"id":"shade","kind":"ComputePipeline","shaders":[
          {"program":"TemplateShadeA"},{"program":"TemplateShadeB"},{"program":"TemplateShadeC"}]},
        {"id":"inner","kind":"Subgraph","asset":"template/inner.lurender.json","configuration":"Inner"}]}]})json");
    WriteSource(directory, INNER_GRAPH, R"json({"version":2,"configurations":[
      {"id":"Inner","nodes":[{"id":"pass","kind":"ComputePipeline","shaders":[{"program":"InnerProgram"}]}]}
    ]})json");
    WriteSource(directory, QStringLiteral("other.lurender.json"), R"json({"version":2,"configurations":[
      {"id":"Other","nodes":[{"id":"shade","kind":"ComputePipeline","shaders":[{"program":"OtherShade"}]}]}
    ]})json");
    WriteSource(directory, MANIFEST, R"json({"version":1,"programs":[
      {"id":"RootProgram","source":"root.slang"},
      {"id":"TemplatePrimary","source":"template/primary.slang"},
      {"id":"TemplateShadeA","source":"template/shade.slang"},
      {"id":"TemplateShadeB","source":"template/shade.slang"},
      {"id":"TemplateShadeC","source":"template/shade.slang"},
      {"id":"InnerProgram","source":"template/inner.slang"},
      {"id":"OtherShade","source":"other/shade.slang"},
      {"id":"StaleProgram","source":"stale.slang"}
    ],"techniques":[]})json");
    // The surviving root shader includes a template-folder helper, which includes another.
    WriteSource(directory, QStringLiteral("root.slang"), QByteArrayLiteral("#include \"template/common.slang\"\n"));
    WriteSource(directory, QStringLiteral("template/common.slang"), QByteArrayLiteral("#include \"math.slang\"\n"));
    WriteSource(directory, QStringLiteral("template/math.slang"), QByteArrayLiteral("// Common math.\n"));
    // The template's sources include and mention each other.
    WriteSource(directory,
                QStringLiteral("template/primary.slang"),
                QByteArrayLiteral("#include \"hit_record.slang\"\n"));
    WriteSource(directory,
                QStringLiteral("template/shade.slang"),
                QByteArrayLiteral("#include \"hit_record.slang\"\n#include \"mis.slang\"\n#include \"common.slang\"\n"
                                  "// Weights from mis.slang.\n"));
    WriteSource(directory,
                QStringLiteral("template/hit_record.slang"),
                QByteArrayLiteral("// Hit record of the template's passes.\n"));
    WriteSource(directory, QStringLiteral("template/mis.slang"), QByteArrayLiteral("// MIS weights.\n"));
    WriteSource(directory,
                QStringLiteral("template/inner.slang"),
                QByteArrayLiteral("#include \"hit_record.slang\"\n"));
    // A file the user wrote in the template folder that nothing includes.
    WriteSource(directory,
                QStringLiteral("template/user_helper.slang"),
                QByteArrayLiteral("// The user's notes; nothing includes this file.\n"));
    WriteSource(directory,
                QStringLiteral("other/shade.slang"),
                QByteArrayLiteral("#include \"hit_record.slang\"\n// Same file names as the template's.\n"));
    WriteSource(directory, QStringLiteral("other/hit_record.slang"), QByteArrayLiteral("// Other hit record.\n"));
    WriteSource(directory, QStringLiteral("stale.slang"), QByteArrayLiteral("// Stale.\n"));
}
} // namespace

TEST_CASE("deleting a template's Subgraph and library frees all its files and keeps what survivors include",
          "[editor][render-graph][shader-cleanup][library-removal]")
{
    QTemporaryDir directory;
    MakeTemplatePackage(directory);
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QMap<QString, QByteArray> original = GetSources(document);
    RenderGraphLibraryRemoval removal;
    QString error;
    REQUIRE(document.RemoveSubgraphLibrary(ROOT_GRAPH,
                                           QStringLiteral("Default"),
                                           QStringLiteral("Template"),
                                           removal,
                                           error) == RenderGraphDocumentStatus::Ok);
    INFO(error.toStdString());
    CHECK(removal.Graphs == QStringList{TEMPLATE_GRAPH, INNER_GRAPH});
    CHECK(removal.Programs == QStringList{MANIFEST + QStringLiteral("#TemplatePrimary"),
                                          MANIFEST + QStringLiteral("#TemplateShadeA"),
                                          MANIFEST + QStringLiteral("#TemplateShadeB"),
                                          MANIFEST + QStringLiteral("#TemplateShadeC"),
                                          MANIFEST + QStringLiteral("#InnerProgram")});
    // Every file of the template goes: three programs share shade.slang, its sources include and
    // mention each other, and hit_record.slang and mis.slang are helpers that no program registers.
    const QStringList freed{QStringLiteral("template/hit_record.slang"),
                            QStringLiteral("template/inner.slang"),
                            QStringLiteral("template/mis.slang"),
                            QStringLiteral("template/primary.slang"),
                            QStringLiteral("template/shade.slang")};
    CHECK(removal.Sources == freed);
    CHECK(removal.RetainedBy.isEmpty() == true);
    QStringList remaining = original.keys();
    for (const QString& file : freed + removal.Graphs)
    {
        remaining.removeOne(file);
    }
    CHECK(document.GetFiles() == remaining);
    // The surviving root shader's include and its transitive include survive although only the
    // template's shade.slang also included them; so does the user's helper and the other library's
    // same-named files, and an unused program that the template never named.
    for (const QString& kept : {QStringLiteral("template/common.slang"),
                                QStringLiteral("template/math.slang"),
                                QStringLiteral("template/user_helper.slang"),
                                QStringLiteral("other/hit_record.slang"),
                                QStringLiteral("stale.slang")})
    {
        CHECK(document.GetDraft(kept) == original.value(kept));
    }
    const QByteArray manifest = document.GetDraft(MANIFEST);
    CHECK(manifest.contains("StaleProgram") == true);
    CHECK(manifest.contains("OtherShade") == true);
    CHECK(manifest.contains("Template") == false);
    const QByteArray root = document.GetDraft(ROOT_GRAPH);
    CHECK(root.contains("template.lurender.json") == false);
    CHECK(root.contains("Template.color") == false);
    CHECK(root.contains("other.lurender.json") == true);

    // One undo step restores every file; Save deletes them on disk, and saving the undo restores them.
    const QMap<QString, QByteArray> cleaned = GetSources(document);
    REQUIRE(document.Undo() == true);
    CHECK(GetSources(document) == original);
    CHECK(document.IsDirty() == false);
    REQUIRE(document.Redo() == true);
    CHECK(GetSources(document) == cleaned);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    const QDir package(directory.path());
    CHECK(QFileInfo::exists(package.filePath(TEMPLATE_GRAPH)) == false);
    CHECK(QFileInfo::exists(package.filePath(QStringLiteral("template/shade.slang"))) == false);
    CHECK(QFileInfo::exists(package.filePath(QStringLiteral("template/common.slang"))) == true);
    REQUIRE(document.Undo() == true);
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(GetSources(reopened) == original);
}

TEST_CASE("deleting a Subgraph keeps a library that another Subgraph still uses",
          "[editor][render-graph][shader-cleanup][library-removal]")
{
    QTemporaryDir directory;
    MakeTemplatePackage(directory);
    WriteSource(directory, ROOT_GRAPH, R"json({"version":2,"id":"Main","default":"Default","configurations":[
      {"id":"Default","nodes":[
        {"id":"root","kind":"GraphicsPipeline","shaders":[{"program":"RootProgram"}]},
        {"id":"Template","kind":"Subgraph","asset":"template.lurender.json","configuration":"Template"},
        {"id":"Other","kind":"Subgraph","asset":"other.lurender.json","configuration":"Other"}]},
      {"id":"Alternative","nodes":[
        {"id":"Again","kind":"Subgraph","asset":"template.lurender.json","configuration":"Template"}]}
    ]})json");
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QStringList files = document.GetFiles();
    RenderGraphLibraryRemoval removal;
    QString error;
    REQUIRE(document.RemoveSubgraphLibrary(ROOT_GRAPH,
                                           QStringLiteral("Default"),
                                           QStringLiteral("Template"),
                                           removal,
                                           error) == RenderGraphDocumentStatus::Ok);
    CHECK(removal.Graphs.isEmpty() == true);
    CHECK(removal.Programs.isEmpty() == true);
    CHECK(removal.Sources.isEmpty() == true);
    CHECK(removal.RetainedBy == ROOT_GRAPH);
    CHECK(document.GetFiles() == files);
    // Deleting the last user then frees the library.
    REQUIRE(document.RemoveSubgraphLibrary(ROOT_GRAPH,
                                           QStringLiteral("Alternative"),
                                           QStringLiteral("Again"),
                                           removal,
                                           error) == RenderGraphDocumentStatus::Ok);
    CHECK(removal.Graphs == QStringList{TEMPLATE_GRAPH, INNER_GRAPH});
    CHECK(removal.Sources.size() == 5);
}

TEST_CASE("deleting a Subgraph and library rejects other nodes and unsafe packages without changing the draft",
          "[editor][render-graph][shader-cleanup][library-removal]")
{
    QTemporaryDir directory;
    MakeTemplatePackage(directory);
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QString node = QStringLiteral("root");
    RenderGraphDocumentStatus expected = RenderGraphDocumentStatus::InvalidEdit;
    QString named = QStringLiteral("not a Subgraph");
    SECTION("pipeline node") {}
    SECTION("unknown node")
    {
        node = QStringLiteral("missing");
    }
    SECTION("malformed graph elsewhere in the package")
    {
        REQUIRE(document.SetText(QStringLiteral("other.lurender.json"), QByteArrayLiteral("{")) ==
                RenderGraphDocumentStatus::Ok);
        node = QStringLiteral("Template");
        expected = RenderGraphDocumentStatus::Malformed;
        named = QStringLiteral("other.lurender.json");
    }
    const QMap<QString, QByteArray> original = GetSources(document);
    const uint64 revision = document.GetRevision();
    RenderGraphLibraryRemoval removal;
    removal.RetainedBy = QStringLiteral("sentinel");
    QString error;
    CHECK(document.RemoveSubgraphLibrary(ROOT_GRAPH, QStringLiteral("Default"), node, removal, error) == expected);
    CHECK(error.contains(named) == true);
    CHECK(error.contains(QStringLiteral("The draft was preserved.")) == true);
    CHECK(GetSources(document) == original);
    CHECK(document.GetRevision() == revision);
    CHECK(removal.RetainedBy == QStringLiteral("sentinel"));
    CHECK(removal.Graphs.isEmpty() == true);
}

TEST_CASE("shader cleanup frees sources that only removed sources include or mention",
          "[editor][render-graph][shader-cleanup]")
{
    // Before the fixed point, a template's own sources kept each other and helpers stayed forever.
    QTemporaryDir directory;
    MakeTemplatePackage(directory);
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    // Drop the library's programs from its graph, as a user would by deleting its passes.
    REQUIRE(document.SetText(TEMPLATE_GRAPH, QByteArrayLiteral(R"json({"version":2,"id":"Template",
      "default":"Template","configurations":[{"id":"Template","nodes":[]}]})json")) == RenderGraphDocumentStatus::Ok);
    QStringList programs;
    QStringList sources;
    QString error;
    REQUIRE(document.RemoveUnusedShaders(programs, sources, error) == RenderGraphDocumentStatus::Ok);
    // The inner library is now a loose graph that still names InnerProgram, so inner.slang and the
    // hit_record.slang it includes survive; the other sources only removed sources reach go. (Had the
    // surviving hit_record.slang mentioned shade.slang, even in a comment, shade.slang would stay.)
    CHECK(sources == QStringList{QStringLiteral("stale.slang"),
                                 QStringLiteral("template/mis.slang"),
                                 QStringLiteral("template/primary.slang"),
                                 QStringLiteral("template/shade.slang")});
    CHECK(document.GetFiles().contains(QStringLiteral("template/hit_record.slang")) == true);
    CHECK(document.GetFiles().contains(QStringLiteral("template/common.slang")) == true);
    CHECK(document.GetFiles().contains(QStringLiteral("template/math.slang")) == true);
    CHECK(document.GetFiles().contains(QStringLiteral("template/user_helper.slang")) == true);
    CHECK(document.GetFiles().contains(INNER_GRAPH) == true);
}
