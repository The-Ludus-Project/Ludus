#include "internal/project_rendering_settings.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_resolution_settings.h"
#include "internal/renderer_workspace.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
namespace
{
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
QString MakeResolutionPackage(const QTemporaryDir& directory)
{
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    for (const QString& file : {ROOT_GRAPH, QStringLiteral("rasterize.lurender.json")})
    {
        REQUIRE(QFile::copy(source.filePath(file), QDir(directory.path()).filePath(file)) == true);
    }
    return QDir(directory.path()).filePath(ROOT_GRAPH);
}
QJsonObject ReadRoot(const RenderGraphDocument& document)
{
    QJsonObject graph;
    REQUIRE(document.GetGraph(ROOT_GRAPH, graph) == true);
    return graph;
}
void ReplaceRoot(RenderGraphDocument& inoutDocument, const QJsonObject& graph)
{
    REQUIRE(inoutDocument.SetText(ROOT_GRAPH, EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);
}
void CheckSettings(const RenderGraphDocument& document,
                   const QJsonValue& expectedExtent,
                   const QJsonArray& expectedScale)
{
    const QVector<RenderGraphResolutionSelection> selections = FindRenderGraphResolutionSettings(document);
    REQUIRE(selections.size() == 1);
    QJsonValue extent;
    QJsonArray scale;
    REQUIRE(ReadRenderGraphResolutionSettings(document, selections.at(0), extent, scale) == true);
    CHECK(extent == expectedExtent);
    CHECK(scale == expectedScale);
}
} // namespace

TEST_CASE("project resolution setup preserves dimensions and has one graph undo step", "[editor][resolution]")
{
    QTemporaryDir directory;
    static_cast<void>(MakeResolutionPackage(directory));
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QByteArray original = document.GetSaved(ROOT_GRAPH);
    CHECK(FindRenderGraphResolutionSettings(document).isEmpty() == true);
    CHECK(document.IsDirty() == false);
    QString error;
    REQUIRE(EnableRenderGraphResolutionSettings(document, error) == true);
    CheckSettings(document, QJsonArray{96, 64}, QJsonArray{1, 1});
    const QJsonObject configuration =
        ReadRoot(document).value(QStringLiteral("configurations")).toArray().at(0).toObject();
    for (const QJsonValueConstRef value : configuration.value(QStringLiteral("nodes")).toArray())
    {
        const QJsonObject node = value.toObject();
        if (node.value(QStringLiteral("kind")) == QStringLiteral("Target"))
        {
            CHECK(node.value(QStringLiteral("extent")) == QStringLiteral("RenderSize"));
        }
    }
    REQUIRE(document.Undo() == true);
    CHECK(document.IsDirty() == false);
    CHECK(document.GetSaved(ROOT_GRAPH) == original);
    CHECK(FindRenderGraphResolutionSettings(document).isEmpty() == true);
    REQUIRE(document.Redo() == true);
    const RenderGraphResolutionSelection selection = FindRenderGraphResolutionSettings(document).at(0);
    REQUIRE(WriteRenderGraphResolutionSettings(document, selection, QJsonArray{1920, 1080}, QJsonArray{2, 3}, error) ==
            true);
    CheckSettings(document, QJsonArray{1920, 1080}, QJsonArray{2, 3});
    REQUIRE(document.Undo() == true);
    CheckSettings(document, QJsonArray{96, 64}, QJsonArray{1, 1});
    REQUIRE(document.Redo() == true);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CheckSettings(reopened, QJsonArray{1920, 1080}, QJsonArray{2, 3});
}

TEST_CASE("project resolution rejects invalid and stale providers without changing history", "[editor][resolution]")
{
    QTemporaryDir directory;
    static_cast<void>(MakeResolutionPackage(directory));
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QString error;
    REQUIRE(EnableRenderGraphResolutionSettings(document, error) == true);
    RenderGraphResolutionSelection selection = FindRenderGraphResolutionSettings(document).at(0);
    const ludus::foundation::uint64 revision = document.GetRevision();
    CHECK(WriteRenderGraphResolutionSettings(document, selection, QJsonArray{0, 1080}, QJsonArray{1, 2}, error) ==
          false);
    CHECK(
        WriteRenderGraphResolutionSettings(document, selection, QStringLiteral("Viewport"), QJsonArray{1, 0}, error) ==
        false);
    CHECK(WriteRenderGraphResolutionSettings(document, selection, QJsonArray{1920.5, 1080}, QJsonArray{1, 2}, error) ==
          false);
    selection.RenderNode = QStringLiteral("missing");
    CHECK(
        WriteRenderGraphResolutionSettings(document, selection, QStringLiteral("Viewport"), QJsonArray{1, 2}, error) ==
        false);
    CHECK(error.isEmpty() == false);
    CHECK(document.GetRevision() == revision);
    CheckSettings(document, QJsonArray{96, 64}, QJsonArray{1, 1});
}

TEST_CASE("project resolution setup preserves legacy view scaling and rejects custom graphs", "[editor][resolution]")
{
    QTemporaryDir directory;
    static_cast<void>(MakeResolutionPackage(directory));
    RenderGraphDocument document;
    REQUIRE(document.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject graph = ReadRoot(document);
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    QJsonObject configuration = configurations.at(0).toObject();
    QJsonArray nodes = configuration.value(QStringLiteral("nodes")).toArray();
    QJsonArray edges = configuration.value(QStringLiteral("edges")).toArray();
    SECTION("legacy view policy")
    {
        nodes.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("existingSize")},
                                 {QStringLiteral("kind"), QStringLiteral("RenderSize")},
                                 {QStringLiteral("extent"),
                                  QJsonObject{{QStringLiteral("reference"), QStringLiteral("View")},
                                              {QStringLiteral("scale"), QJsonArray{2, 3}}}}});
        for (qsizetype index = 0; index < nodes.size(); ++index)
        {
            QJsonObject node = nodes.at(index).toObject();
            if (node.value(QStringLiteral("kind")) == QStringLiteral("Target"))
            {
                node.insert(QStringLiteral("extent"), QStringLiteral("RenderSize"));
                nodes.replace(index, node);
                edges.append(QJsonObject{
                    {QStringLiteral("from"), QStringLiteral("existingSize.size")},
                    {QStringLiteral("to"), node.value(QStringLiteral("id")).toString() + QStringLiteral(".size")}});
            }
        }
        configuration.insert(QStringLiteral("nodes"), nodes);
        configuration.insert(QStringLiteral("edges"), edges);
        configurations.replace(0, configuration);
        graph.insert(QStringLiteral("configurations"), configurations);
        ReplaceRoot(document, graph);
        QString error;
        REQUIRE(EnableRenderGraphResolutionSettings(document, error) == true);
        CheckSettings(document, QStringLiteral("Viewport"), QJsonArray{2, 3});
        CHECK(FindRenderGraphResolutionSettings(document).at(0).RenderNode == QStringLiteral("existingSize"));
    }
    SECTION("ambiguous multiple color targets")
    {
        nodes.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("other")},
                                 {QStringLiteral("kind"), QStringLiteral("Target")},
                                 {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                 {QStringLiteral("extent"), QJsonArray{96, 64}}});
        configuration.insert(QStringLiteral("nodes"), nodes);
        configurations.replace(0, configuration);
        graph.insert(QStringLiteral("configurations"), configurations);
        ReplaceRoot(document, graph);
        const ludus::foundation::uint64 revision = document.GetRevision();
        QString error;
        CHECK(EnableRenderGraphResolutionSettings(document, error) == false);
        CHECK(document.GetRevision() == revision);
        CHECK(FindRenderGraphResolutionSettings(document).isEmpty() == true);
    }
}

TEST_CASE("Project Settings edits the renderer providers and tracks graph undo", "[editor][resolution]")
{
    QTemporaryDir directory;
    const QString graph = MakeResolutionPackage(directory);
    RendererWorkspace renderer({});
    renderer.SetProject(directory.path(), 1);
    REQUIRE(renderer.OpenGraph(graph) == true);
    renderer.SetLivePreview(false);
    ProjectRenderingSettings settings(renderer);
    settings.SetProjectEditable(true);
    CHECK(renderer.IsDirty() == false);
    QPushButton* enable = settings.findChild<QPushButton*>(QStringLiteral("projectEnableResolution"));
    QComboBox* mode = settings.findChild<QComboBox*>(QStringLiteral("projectOutputMode"));
    QComboBox* scale = settings.findChild<QComboBox*>(QStringLiteral("projectRenderScale"));
    QSpinBox* width = settings.findChild<QSpinBox*>(QStringLiteral("projectOutputWidth"));
    QSpinBox* numerator = settings.findChild<QSpinBox*>(QStringLiteral("projectRenderNumerator"));
    REQUIRE(enable != nullptr);
    REQUIRE(mode != nullptr);
    REQUIRE(scale != nullptr);
    REQUIRE(width != nullptr);
    REQUIRE(numerator != nullptr);
    QSignalSpy changes(&renderer, &RendererWorkspace::DocumentChanged);
    enable->click();
    REQUIRE(changes.count() > 0);
    CheckSettings(renderer.GetDocument(), QJsonArray{96, 64}, QJsonArray{1, 1});
    width->setValue(1920);
    CheckSettings(renderer.GetDocument(), QJsonArray{1920, 64}, QJsonArray{1, 1});
    mode->setCurrentIndex(0);
    CHECK(width->isEnabled() == false);
    scale->setCurrentIndex(3);
    CheckSettings(renderer.GetDocument(), QStringLiteral("Viewport"), QJsonArray{1, 2});
    renderer.Undo();
    CHECK(scale->currentIndex() == 0);
    renderer.Redo();
    CHECK(scale->currentIndex() == 3);
    scale->setCurrentIndex(5);
    numerator->setValue(7);
    CheckSettings(renderer.GetDocument(), QStringLiteral("Viewport"), QJsonArray{7, 2});
    settings.SetProjectEditable(false);
    CHECK(scale->isEnabled() == false);
    CHECK(mode->isEnabled() == false);
    REQUIRE(renderer.Save() == true);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(directory.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CheckSettings(reopened, QStringLiteral("Viewport"), QJsonArray{7, 2});
}

TEST_CASE("Project Settings keeps distinct upscaling provider groups independently selectable", "[editor][resolution]")
{
    QTemporaryDir directory;
    const QString graph = MakeResolutionPackage(directory);
    RendererWorkspace renderer({});
    renderer.SetProject(directory.path(), 1);
    REQUIRE(renderer.OpenGraph(graph) == true);
    renderer.SetLivePreview(false);
    RenderGraphDocument& document = renderer.GetDocument();
    REQUIRE(document.AddUpscalePreset(ROOT_GRAPH,
                                      QStringLiteral("Default"),
                                      QStringLiteral("dlss-sr"),
                                      QStringLiteral("dlss")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.AddUpscalePreset(ROOT_GRAPH,
                                      QStringLiteral("Default"),
                                      QStringLiteral("fsr-2"),
                                      QStringLiteral("fsr")) == RenderGraphDocumentStatus::Ok);
    ProjectRenderingSettings settings(renderer);
    settings.SetProjectEditable(true);
    QComboBox* providers = settings.findChild<QComboBox*>(QStringLiteral("projectResolutionProvider"));
    QComboBox* scale = settings.findChild<QComboBox*>(QStringLiteral("projectRenderScale"));
    REQUIRE(providers != nullptr);
    REQUIRE(scale != nullptr);
    REQUIRE(providers->count() == 2);
    providers->setCurrentIndex(1);
    scale->setCurrentIndex(0);
    const QVector<RenderGraphResolutionSelection> selections = FindRenderGraphResolutionSettings(document);
    QJsonValue extent;
    QJsonArray firstScale;
    QJsonArray secondScale;
    REQUIRE(ReadRenderGraphResolutionSettings(document, selections.at(0), extent, firstScale) == true);
    REQUIRE(ReadRenderGraphResolutionSettings(document, selections.at(1), extent, secondScale) == true);
    CHECK(firstScale == QJsonArray{1, 2});
    CHECK(secondScale == QJsonArray{1, 1});
    renderer.Undo();
    CHECK(providers->currentIndex() == 1);
    CHECK(scale->currentIndex() == 3);
}

TEST_CASE("Project Settings leaves the built-in renderer untouched until explicit setup and Save",
          "[editor][resolution]")
{
    QTemporaryDir directory;
    RendererWorkspace renderer({});
    renderer.SetProject(directory.path(), 1);
    renderer.SetLivePreview(false);
    REQUIRE(renderer.IsBuiltIn() == true);
    ProjectRenderingSettings settings(renderer);
    settings.SetProjectEditable(true);
    CHECK(renderer.IsDirty() == false);
    const QString destination = renderer.GetDocument().GetPackageRoot();
    CHECK(QFile::exists(destination) == false);
    QString error;
    REQUIRE(renderer.EnableResolutionSettings(error) == true);
    CHECK(QFile::exists(destination) == false);
    REQUIRE(renderer.Save() == true);
    CHECK(renderer.IsBuiltIn() == false);
    CHECK(QFile::exists(QDir(destination).filePath(renderer.GetDocument().GetRootGraph())) == true);
    CHECK(FindRenderGraphResolutionSettings(renderer.GetDocument()).size() == 1);
}
