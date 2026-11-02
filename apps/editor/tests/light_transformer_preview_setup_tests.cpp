#include "internal/light_transformer_preview_setup.h"

#include <ludus/foundation/base/types.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
namespace editor = ludus::editor;
namespace renderer = ludus::graphics::renderer;
QByteArray WriteAsset(const QString& path, int64 bytes)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadWrite) == true);
    REQUIRE(file.resize(bytes) == true);
    REQUIRE(file.seek(0) == true);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    REQUIRE(hash.addData(&file) == true);
    return hash.result().toHex();
}
void WriteConfiguration(const QString& path, const QJsonObject& document)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    const QByteArray data = QJsonDocument(document).toJson();
    REQUIRE(file.write(data) == data.size());
}
} // namespace
TEST_CASE("Light Transformer preview validates a complete paired setup and preserves accepted outputs",
          "[editor][light-transformer]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString modelDirectory = QDir(directory.path()).filePath(QStringLiteral("light_transformer"));
    REQUIRE(QDir().mkpath(modelDirectory) == true);
    const QString configurationPath = QDir(modelDirectory).filePath(QStringLiteral("model.json"));
    editor::LightTransformerPreviewSetup setup;
    setup.Description.ScenePoints = 1;
    QString error;
    REQUIRE(editor::ReadLightTransformerPreviewSetup(directory.path(), setup, error) == false);
    CHECK(setup.Description.ScenePoints == 1);
    CHECK(error.contains(QStringLiteral("Export"), Qt::CaseInsensitive) == true);
    const QByteArray encoder =
        WriteAsset(QDir(modelDirectory).filePath(QStringLiteral("encoder.llte")), LUDUS_LIGHT_ENCODER_ASSET_BYTES);
    constexpr int64 DECODER_BYTES =
        (LUDUS_LIGHT_TRANSFORMER_FILE_HEADER_WORDS + LUDUS_LIGHT_TRANSFORMER_WEIGHT_COUNT) * sizeof(uint32);
    const QByteArray decoder = WriteAsset(QDir(modelDirectory).filePath(QStringLiteral("decoder.lltd")), DECODER_BYTES);
    QJsonObject document{{QStringLiteral("version"), 1},
                         {QStringLiteral("world_units_per_model_unit"), 2},
                         {QStringLiteral("scene_points"), LUDUS_LIGHT_ENCODER_MIN_POINTS},
                         {QStringLiteral("seed"), 1},
                         {QStringLiteral("maximum_buffer_bytes"), 536870912},
                         {QStringLiteral("maximum_surface_preparation_bytes"), 536870912},
                         {QStringLiteral("encoder_sha256"), QString::fromLatin1(encoder)},
                         {QStringLiteral("decoder_sha256"), QString::fromLatin1(decoder)}};
    WriteConfiguration(configurationPath, document);
    REQUIRE(editor::ReadLightTransformerPreviewSetup(directory.path(), setup, error) == true);
    CHECK(error.isEmpty() == true);
    CHECK(setup.Description.ScenePoints == LUDUS_LIGHT_ENCODER_MIN_POINTS);
    CHECK(setup.Description.EncoderAsset.GetSize() == LUDUS_LIGHT_ENCODER_ASSET_BYTES);
    CHECK(setup.Description.EncoderAsset.GetDataOrNull() == reinterpret_cast<const uint8*>(setup.Encoder.constData()));
    document.insert(QStringLiteral("decoder_sha256"), QStringLiteral("stale"));
    WriteConfiguration(configurationPath, document);
    CHECK(editor::ReadLightTransformerPreviewSetup(directory.path(), setup, error) == false);
    CHECK(error.contains(QStringLiteral("together")) == true);
    CHECK(setup.Description.ScenePoints == LUDUS_LIGHT_ENCODER_MIN_POINTS);
    document.insert(QStringLiteral("world_units_per_model_unit"), 1e308);
    WriteConfiguration(configurationPath, document);
    CHECK(editor::ReadLightTransformerPreviewSetup(directory.path(), setup, error) == false);
    CHECK(error.contains(QStringLiteral("fixed dataset scale")) == true);
    CHECK(setup.Description.ScenePoints == LUDUS_LIGHT_ENCODER_MIN_POINTS);
}
TEST_CASE("Light Transformer model diagnostics name repair inputs", "[editor][light-transformer]")
{
    renderer::LightTransformerPreparationReport report;
    CHECK(editor::DescribeLightTransformerPreparationFailure(report).isEmpty() == true);
    report.Issue = renderer::LightTransformerPreparationIssue::EmbeddingCapacity;
    report.RequiredPacketBytes = 123;
    report.EmbeddingBufferBytes = 12;
    const QString message = editor::DescribeLightTransformerPreparationFailure(report);
    CHECK(message.contains(QStringLiteral("123")) == true);
    CHECK(message.contains(QStringLiteral("12")) == true);
    CHECK(message.contains(QStringLiteral("Buffer")) == true);
}
