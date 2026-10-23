#include "internal/render_graph_shader_interface.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCursor>
#include <QWidget>

using namespace ludus::editor;

namespace
{
QByteArray ReadFixture(const QString& profile)
{
    QFile file(
        QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/%1/main.lushader.json.in").arg(profile));
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll().replace("@RG2_TEST_PROFILES@", "[\"metal\",\"wgsl\"]");
}
} // namespace

TEST_CASE("Ray callbacks expose their actual fields without scene-only APIs", "[editor][shader-api]")
{
    const QByteArray manifest = ReadFixture(QStringLiteral("raytracing"));
    const QString closest = DescribeShaderInterface(manifest, QStringLiteral("main_shaders/closest.slang"));
    CHECK(closest.contains(QStringLiteral("float3 ShadeClosest(RayHit input);")) == true);
    CHECK(closest.contains(QStringLiteral("struct RayHit\n{")) == true);
    CHECK(closest.contains(QStringLiteral("Ray IncomingRay;")) == true);
    CHECK(closest.contains(QStringLiteral("float3 NormalWorld;")) == true);
    CHECK(closest.contains(QStringLiteral("float2 Barycentric;")) == true);
    CHECK(closest.contains(QStringLiteral("float4 Color;")) == true);
    CHECK(closest.contains(QStringLiteral("float MinDistanceWorld;")) == true);
    CHECK(closest.contains(QStringLiteral("SceneContext")) == false);
    CHECK(closest.contains(QStringLiteral("EvaluateMaterial")) == false);
    const QString generate = DescribeShaderInterface(manifest, QStringLiteral("main_shaders/generate.slang"));
    CHECK(generate.contains(QStringLiteral("Ray GenerateRay(RayLaunch input);")) == true);
    CHECK(generate.contains(QStringLiteral("uint2 ExtentPixel;")) == true);
    CHECK(generate.contains(QStringLiteral("SampleIndex")) == false);
    const QString miss = DescribeShaderInterface(manifest, QStringLiteral("main_shaders/miss.slang"));
    CHECK(miss.contains(QStringLiteral("float3 ShadeMiss(Ray input);")) == true);
    CHECK(miss.contains(QStringLiteral("RayHit")) == false);
}

TEST_CASE("Scene callback references include query functions and the current payload draft", "[editor][shader-api]")
{
    const QByteArray manifest = ReadFixture(QStringLiteral("scene_ray"));
    QMap<QString, QByteArray> sources;
    sources.insert(QStringLiteral("main_shaders/payload.slang"),
                   QByteArrayLiteral("struct ScenePayload\n{\n    float3 Radiance;\n    uint BounceCount;\n};"));
    const QString closest = DescribeShaderInterface(manifest, QStringLiteral("main_shaders/closest.slang"), sources);
    CHECK(closest.contains(QStringLiteral("void ShadeClosest(")) == true);
    CHECK(closest.contains(QStringLiteral("SurfaceHit hit,")) == true);
    CHECK(closest.contains(QStringLiteral("inout RayQueryContext queries,")) == true);
    CHECK(closest.contains(QStringLiteral("inout ScenePayload payload);")) == true);
    CHECK(closest.contains(QStringLiteral("float3 GeometricNormalWorld;")) == true);
    CHECK(closest.contains(QStringLiteral("float2 TexcoordUv;")) == true);
    CHECK(closest.contains(QStringLiteral("uint BounceCount;")) == true);
    CHECK(closest.contains(QStringLiteral("MaterialSample EvaluateMaterial(SceneContext scene, SurfaceHit hit);")) ==
          true);
    CHECK(
        closest.contains(QStringLiteral(
            "uint TraceClosest(SceneContext scene, inout RayQueryContext queries, Ray ray, out SurfaceHit outHit);")) ==
        true);
    CHECK(closest.contains(QStringLiteral("uint TestVisibility(SceneContext scene, inout RayQueryContext queries, "
                                          "SurfaceHit receiver, LightSample light);")) == true);
    CHECK(closest.contains(QStringLiteral("static const uint SCENE_QUERY_SURFACE = 1;")) == true);
    CHECK(closest.contains(QStringLiteral("return result")) == false);
    CHECK(closest.contains(QStringLiteral("struct RayHit")) == false);
    sources[QStringLiteral("main_shaders/payload.slang")] =
        QByteArrayLiteral("struct ScenePayload { float Throughput; };");
    const QString updated = DescribeShaderInterface(manifest, QStringLiteral("main_shaders/closest.slang"), sources);
    CHECK(updated.contains(QStringLiteral("float Throughput;")) == true);
    CHECK(updated.contains(QStringLiteral("BounceCount")) == false);
    CHECK(DescribeShaderInterface(manifest, QStringLiteral("main_shaders/closest.slang"))
              .contains(QStringLiteral("ScenePayload declaration unavailable: main_shaders/payload.slang")) == true);
    const QString generate = DescribeShaderInterface(manifest, QStringLiteral("main_shaders/generate.slang"), sources);
    CHECK(generate.contains(QStringLiteral("float3 GenerateRay(")) == true);
    CHECK(generate.contains(QStringLiteral("uint SampleIndex;")) == true);
    CHECK(generate.contains(QStringLiteral("inout RayQueryContext queries);")) == true);
    const QString miss = DescribeShaderInterface(manifest, QStringLiteral("main_shaders/miss.slang"), sources);
    CHECK(miss.contains(QStringLiteral("void ShadeMiss(")) == true);
    CHECK(miss.contains(QStringLiteral("Ray ray,")) == true);
}

TEST_CASE("Scene callback references display the pipeline payload without a project helper", "[editor][shader-api]")
{
    QJsonObject manifest = QJsonDocument::fromJson(ReadFixture(QStringLiteral("scene_ray"))).object();
    QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    for (qsizetype index = 0; index < techniques.size(); ++index)
    {
        QJsonObject technique = techniques.at(index).toObject();
        if (technique.contains(QStringLiteral("scene_interface")) == true)
        {
            QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
            interface.remove(QStringLiteral("payload"));
            technique.insert(QStringLiteral("scene_interface"), interface);
            techniques.replace(index, technique);
        }
    }
    manifest.insert(QStringLiteral("techniques"), techniques);
    const QString reference =
        DescribeShaderInterface(QJsonDocument(manifest).toJson(), QStringLiteral("main_shaders/closest.slang"));
    CHECK(reference.contains(QStringLiteral("Pipeline default payload")) == true);
    CHECK(reference.contains(QStringLiteral("struct ScenePayload")) == true);
    CHECK(reference.contains(QStringLiteral("float3 Radiance;")) == true);
    CHECK(reference.contains(QStringLiteral("declaration unavailable")) == false);
}

TEST_CASE("Shader API lookup searches immediately and keeps position across refreshes", "[editor][shader-api]")
{
    QWidget parent;
    QPlainTextEdit* text = new QPlainTextEdit(&parent);
    QWidget* page = CreateShaderInterfaceView(text, &parent);
    page->resize(500, 350);
    page->show();
    const QString declarations =
        DescribeShaderInterface(ReadFixture(QStringLiteral("scene_ray")), QStringLiteral("main_shaders/closest.slang"));
    UpdateShaderInterfaceView(*text, QStringLiteral("closest.slang"), declarations);
    QLineEdit* searchOrNull = page->findChild<QLineEdit*>(QStringLiteral("shaderApiSearch"));
    REQUIRE(searchOrNull != nullptr);
    searchOrNull->setText(QStringLiteral("EvaluateMaterial"));
    CHECK(text->textCursor().selectedText() == QStringLiteral("EvaluateMaterial"));
    const ludus::foundation::int32 position = text->textCursor().position();
    const ludus::foundation::int32 scrollPosition = text->verticalScrollBar()->value();
    UpdateShaderInterfaceView(*text, QStringLiteral("closest.slang"), declarations);
    CHECK(text->textCursor().position() == position);
    CHECK(text->verticalScrollBar()->value() == scrollPosition);
    UpdateShaderInterfaceView(*text,
                              QStringLiteral("closest.slang"),
                              declarations + QStringLiteral("\n// changed payload"));
    CHECK(text->verticalScrollBar()->value() == scrollPosition);
    searchOrNull->setText(QStringLiteral("noSuchField"));
    CHECK(searchOrNull->toolTip() == QStringLiteral("No matching declaration."));
    searchOrNull->setText(QStringLiteral("PositionWorld"));
    QPushButton* nextOrNull = page->findChild<QPushButton*>(QStringLiteral("shaderApiFindNext"));
    REQUIRE(nextOrNull != nullptr);
    const ludus::foundation::int32 first = text->textCursor().position();
    nextOrNull->click();
    CHECK(text->textCursor().selectedText() == QStringLiteral("PositionWorld"));
    CHECK(text->textCursor().position() != first);
    CHECK(text->lineWrapMode() == QPlainTextEdit::NoWrap);
    UpdateShaderInterfaceView(*text, QStringLiteral("miss.slang"), QStringLiteral("float3 ShadeMiss(Ray input);"));
    CHECK(text->verticalScrollBar()->value() == 0);
    const QString capture = qEnvironmentVariable("LUDUS_SHADER_API_CAPTURE");
    if (capture.isEmpty() == false)
    {
        searchOrNull->clear();
        page->resize(520, 650);
        UpdateShaderInterfaceView(*text,
                                  QStringLiteral("closest.slang"),
                                  DescribeShaderInterface(ReadFixture(QStringLiteral("raytracing")),
                                                          QStringLiteral("main_shaders/closest.slang")));
        QApplication::processEvents();
        REQUIRE(page->grab().save(capture) == true);
    }
}

TEST_CASE("Visibility ray references expose depth reconstruction and the codec", "[editor][shader-api]")
{
    const QByteArray manifest = ReadFixture(QStringLiteral("visibility"));
    const QString generate = DescribeShaderInterface(manifest, QStringLiteral("main_shaders/generate.slang"));
    CHECK(generate.contains(QStringLiteral("uint LoadVisibilitySurface(")) == true);
    CHECK(generate.contains(QStringLiteral("float4 EncodeVisibility(")) == true);
    CHECK(generate.contains(QStringLiteral("float4x4 CameraClipToWorld;")) == true);
    CHECK(generate.contains(QStringLiteral("Ray SpawnSurfaceRay(")) == true);
}
