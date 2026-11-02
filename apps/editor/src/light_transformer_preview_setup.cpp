#include "internal/light_transformer_preview_setup.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace ludus::editor
{
using namespace foundation;
namespace renderer = graphics::renderer;
namespace
{
constexpr int64 MAXIMUM_CONFIGURATION_BYTES = 65536;
constexpr uint32 CONFIGURATION_VERSION = 1;
constexpr uint32 MAXIMUM_SEED = ~uint32{0};
constexpr uint64 MAXIMUM_MODEL_BUDGET_BYTES = uint64{2} * 1024 * 1024 * 1024;
constexpr uint64 DECODER_ASSET_BYTES =
    (LUDUS_LIGHT_TRANSFORMER_FILE_HEADER_WORDS + LUDUS_LIGHT_TRANSFORMER_WEIGHT_COUNT) * sizeof(uint32);
bool ReadAsset(const QString& path, int64 requiredBytes, QByteArray& outBytes, QString& outError)
{
    QFile file(path);
    if (QFileInfo(path).size() != requiredBytes || file.open(QIODevice::ReadOnly) == false)
    {
        outError = QStringLiteral("Light Transformer needs a readable %1-byte %2. Export the complete trained "
                                  "checkpoint with export-light-transformer --model-directory %3 "
                                  "--world-units-per-model-unit <fixed training scale>, then preview again.")
                       .arg(requiredBytes)
                       .arg(path, QFileInfo(path).absolutePath());
        return false;
    }
    QByteArray bytes = file.read(requiredBytes + 1);
    if (bytes.size() != requiredBytes)
    {
        outError =
            QStringLiteral("The model changed while reading %1. Finish exporting it, then preview again.").arg(path);
        return false;
    }
    outBytes = Move(bytes);
    return true;
}
bool ReadInteger(const QJsonObject& document, const QString& name, uint64 minimum, uint64 maximum, uint64& outValue)
{
    const QJsonValue value = document.value(name);
    const float64 number = value.toDouble(-1);
    if (value.isDouble() == false || math::IsFinite(number) == false || number < static_cast<float64>(minimum) ||
        number > static_cast<float64>(maximum))
    {
        return false;
    }
    const uint64 integer = static_cast<uint64>(number);
    if (number != static_cast<float64>(integer))
    {
        return false;
    }
    outValue = integer;
    return true;
}
} // namespace
void LightTransformerPreviewSetup::BindAssets() noexcept
{
    Description.EncoderAsset = {reinterpret_cast<const uint8*>(Encoder.constData()),
                                static_cast<usize>(Encoder.size())};
    Description.DecoderAsset = {reinterpret_cast<const uint8*>(Decoder.constData()),
                                static_cast<usize>(Decoder.size())};
}
bool ReadLightTransformerPreviewSetup(const QString& directory,
                                      LightTransformerPreviewSetup& outSetup,
                                      QString& outError)
{
    const QDir modelDirectory(QDir(directory).filePath(QStringLiteral("light_transformer")));
    const QString configurationPath = modelDirectory.filePath(QStringLiteral("model.json"));
    QFile file(configurationPath);
    if (file.size() > MAXIMUM_CONFIGURATION_BYTES || file.open(QIODevice::ReadOnly) == false)
    {
        outError = QStringLiteral("Light Transformer model setup is missing at %1. Export trained encoder/decoder "
                                  "assets into that directory and set their fixed training scale, then preview again.")
                       .arg(configurationPath);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.read(MAXIMUM_CONFIGURATION_BYTES + 1), &parseError);
    const QJsonObject document = json.object();
    const float64 scale = document.value(QStringLiteral("world_units_per_model_unit")).toDouble(0);
    float32 scaleForGpu = 0;
    uint64 version = 0;
    uint64 points = 0;
    uint64 seed = 0;
    uint64 budget = 0;
    uint64 surfaceBudget = 0;
    uint64 attempts = renderer::LightTransformerModelDescription{}.MaximumSamplingAttempts;
    if (parseError.error != QJsonParseError::NoError || json.isObject() == false ||
        ReadInteger(document, QStringLiteral("version"), CONFIGURATION_VERSION, CONFIGURATION_VERSION, version) ==
            false ||
        math::TryConvertToFloat32(scale, scaleForGpu) == false || scaleForGpu <= 0 ||
        math::IsFinite(1 / scaleForGpu) == false ||
        ReadInteger(document,
                    QStringLiteral("scene_points"),
                    LUDUS_LIGHT_ENCODER_MIN_POINTS,
                    LUDUS_LIGHT_ENCODER_MAX_POINTS,
                    points) == false ||
        ReadInteger(document, QStringLiteral("seed"), 0, MAXIMUM_SEED, seed) == false ||
        ReadInteger(document, QStringLiteral("maximum_buffer_bytes"), 1, MAXIMUM_MODEL_BUDGET_BYTES, budget) == false ||
        ReadInteger(document,
                    QStringLiteral("maximum_surface_preparation_bytes"),
                    1,
                    MAXIMUM_MODEL_BUDGET_BYTES,
                    surfaceBudget) == false ||
        (document.contains(QStringLiteral("maximum_sampling_attempts")) == true &&
         ReadInteger(document, QStringLiteral("maximum_sampling_attempts"), points, MAXIMUM_SEED, attempts) == false))
    {
        outError =
            QStringLiteral("Repair %1: set version 1, a positive world_units_per_model_unit matching the "
                           "checkpoint's fixed dataset scale, scene_points in [%2,%3], an unsigned 32-bit seed "
                           "and positive maximum_buffer_bytes/maximum_surface_preparation_bytes up to %4. Optional "
                           "maximum_sampling_attempts must be at least scene_points. Scenes are translated by their "
                           "minimum; they are never individually fitted to a unit cube.")
                .arg(configurationPath)
                .arg(LUDUS_LIGHT_ENCODER_MIN_POINTS)
                .arg(LUDUS_LIGHT_ENCODER_MAX_POINTS)
                .arg(MAXIMUM_MODEL_BUDGET_BYTES);
        return false;
    }
    LightTransformerPreviewSetup candidate;
    if (ReadAsset(modelDirectory.filePath(QStringLiteral("encoder.llte")),
                  LUDUS_LIGHT_ENCODER_ASSET_BYTES,
                  candidate.Encoder,
                  outError) == false ||
        ReadAsset(modelDirectory.filePath(QStringLiteral("decoder.lltd")),
                  static_cast<int64>(DECODER_ASSET_BYTES),
                  candidate.Decoder,
                  outError) == false)
    {
        return false;
    }
    const QByteArray encoderIdentity = QCryptographicHash::hash(candidate.Encoder, QCryptographicHash::Sha256).toHex();
    const QByteArray decoderIdentity = QCryptographicHash::hash(candidate.Decoder, QCryptographicHash::Sha256).toHex();
    if (document.value(QStringLiteral("encoder_sha256")).toString().toLatin1() != encoderIdentity ||
        document.value(QStringLiteral("decoder_sha256")).toString().toLatin1() != decoderIdentity)
    {
        outError = QStringLiteral("The trained assets do not match %1. Finish exporting both assets and their "
                                  "model.json together, then preview again; the shown frame is kept.")
                       .arg(configurationPath);
        return false;
    }
    candidate.Description.WorldUnitsPerModelUnit = scaleForGpu;
    candidate.Description.ScenePoints = static_cast<uint32>(points);
    candidate.Description.Seed = seed;
    candidate.Description.MaximumBufferBytes = static_cast<usize>(budget);
    candidate.Description.MaximumSurfacePreparationBytes = static_cast<usize>(surfaceBudget);
    candidate.Description.MaximumSamplingAttempts = attempts;
    outSetup = Move(candidate);
    outSetup.BindAssets();
    outError.clear();
    return true;
}
QString DescribeLightTransformerPreparationFailure(const renderer::LightTransformerPreparationReport& report)
{
    using Issue = renderer::LightTransformerPreparationIssue;
    switch (report.Issue)
    {
        case Issue::None:
            return {};
        case Issue::MaterialInterface:
            return QStringLiteral("Light Transformer needs a complete material-aware World extraction. Repair its "
                                  "material bindings, then preview again.");
        case Issue::AnalyticLights:
            return QStringLiteral("The World has %1 analytic lights. This trained model encodes emission from meshes; "
                                  "disable analytic lights or select another renderer.")
                .arg(report.AnalyticLights);
        case Issue::Environment:
            return QStringLiteral("This trained model does not encode environment illumination. Disable the "
                                  "environment or select another renderer.");
        case Issue::Materials:
            return QStringLiteral("Light Transformer found %1 non-diffuse materials and %2 sampled surfaces with "
                                  "unassigned bindings. Assign typed diffuse materials or select another renderer.")
                .arg(report.UnsupportedMaterials)
                .arg(report.UnassignedSamples);
        case Issue::Sampling:
            return QStringLiteral(
                       "Preparing %1 surface points within %2 attempts and %3 logical CPU bytes failed. Repair "
                       "degenerate/fully masked geometry, or raise "
                       "maximum_sampling_attempts/maximum_surface_preparation_bytes in light_transformer/model.json.")
                .arg(report.RequestedPoints)
                .arg(report.MaximumSamplingAttempts)
                .arg(report.MaximumSurfacePreparationBytes);
        case Issue::FixedScale:
            return QStringLiteral(
                       "The World spans (%1, %2, %3) units; this model's fixed training extent is %4 units. Partition "
                       "the World or use a model trained for that extent; do not normalize scenes independently.")
                .arg(report.ExtentWorld.X)
                .arg(report.ExtentWorld.Y)
                .arg(report.ExtentWorld.Z)
                .arg(report.WorldUnitsPerModelUnit);
        case Issue::ModelAsset:
            return QStringLiteral("The encoder asset is incompatible or contains nonfinite parameters. Re-export the "
                                  "complete checkpoint into light_transformer, then preview again.");
        case Issue::EmbeddingCapacity:
            return QStringLiteral("The embedding Buffer admits %1 bytes; this model needs %2 bytes. Increase that "
                                  "Buffer capacity from its canvas Details, then preview again.")
                .arg(report.EmbeddingBufferBytes)
                .arg(report.RequiredPacketBytes);
        case Issue::DeviceCapacity:
            return QStringLiteral(
                       "The %1-point model exceeds its %2-byte budget or an enabled device limit. Raise "
                       "maximum_buffer_bytes, reduce scene_points, or restart with larger device storage limits.")
                .arg(report.RequestedPoints)
                .arg(report.MaximumBufferBytes);
    }
    return {};
}
} // namespace ludus::editor
