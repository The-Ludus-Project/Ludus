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
    CHECK(sources == QStringList{QStringLiteral("exclusive.slang"), QStringLiteral("nested/other.slang")});
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
