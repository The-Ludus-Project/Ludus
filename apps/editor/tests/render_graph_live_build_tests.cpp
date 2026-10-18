#include "internal/render_graph_document.h"
#include "internal/render_graph_live_build.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace ludus::editor;
namespace
{
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
const QString CONFIGURATION = QStringLiteral("Default");
void Write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}
} // namespace
TEST_CASE("Live shader build identity excludes layout and includes source and configuration", "[editor][shader]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QByteArray source = QByteArrayLiteral(
        R"({"version":1,"id":"Main","default":"Default","configurations":[{"id":"Default","inputs":[],"outputs":[],"nodes":[],"edges":[]}]})");
    Write(QDir(directory.path()).filePath(ROOT_GRAPH), source);
    Write(QDir(directory.path()).filePath(QStringLiteral("color.slang")), QByteArrayLiteral("float4 Color();"));
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QByteArray original = ComputeRenderGraphBuildIdentity(document, CONFIGURATION);
    QJsonObject graph;
    REQUIRE(document.GetGraph(ROOT_GRAPH, graph) == true);
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    QJsonObject selected = configurations[0].toObject();
    selected.insert(QStringLiteral("layout"), QJsonObject{{QStringLiteral("positions"), QJsonArray()}});
    configurations[0] = selected;
    graph.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(document.SetText(ROOT_GRAPH, QJsonDocument(graph).toJson()) == RenderGraphDocumentStatus::Ok);
    CHECK(ComputeRenderGraphBuildIdentity(document, CONFIGURATION) == original);
    CHECK(ComputeRenderGraphBuildIdentity(document, QStringLiteral("Other")) != original);
    REQUIRE(document.SetText(QStringLiteral("color.slang"), QByteArrayLiteral("float4 Changed();")) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(ComputeRenderGraphBuildIdentity(document, CONFIGURATION) != original);
}
TEST_CASE("Live shader builds debounce, retain only the newest edit and stop on cancellation", "[editor][shader]")
{
    RenderGraphLiveBuild live;
    QSignalSpy requested(&live, &RenderGraphLiveBuild::BuildRequested);
    live.SetEnabled(true);
    live.SetBusy(true);
    live.Schedule(QByteArrayLiteral("old"));
    live.Schedule(QByteArrayLiteral("newest"));
    CHECK(requested.wait(600) == false);
    live.SetBusy(false);
    REQUIRE(requested.wait(1000) == true);
    CHECK(requested.count() == 1);
    live.MarkRequested(QByteArrayLiteral("newest"));
    live.Schedule(QByteArrayLiteral("newest"));
    CHECK(requested.wait(600) == false);
    live.Schedule(QByteArrayLiteral("cancelled"));
    live.SetEnabled(false);
    CHECK(requested.wait(600) == false);
    CHECK(requested.count() == 1);
}
TEST_CASE("Live shader build reset discards work from the previous document", "[editor][shader]")
{
    RenderGraphLiveBuild live;
    QSignalSpy requested(&live, &RenderGraphLiveBuild::BuildRequested);
    live.SetEnabled(true);
    live.Schedule(QByteArrayLiteral("closed document"));
    live.Reset();
    CHECK(requested.wait(600) == false);
    live.Schedule(QByteArrayLiteral("new document"));
    REQUIRE(requested.wait(1000) == true);
}
