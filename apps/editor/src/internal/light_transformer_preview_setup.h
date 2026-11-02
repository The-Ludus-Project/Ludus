#pragma once

#include <ludus/graphics/renderer/light_transformer_encoder.hpp>

#include <QByteArray>
#include <QString>

namespace ludus::editor
{
struct LightTransformerPreviewSetup final
{
    QByteArray Encoder;
    QByteArray Decoder;
    graphics::renderer::LightTransformerModelDescription Description;
    void BindAssets() noexcept;
};
/// Read one immutable model setup from directory/light_transformer. No download,
/// Python execution or source modification. Failure preserves outSetup and gives a repair.
[[nodiscard]] bool
ReadLightTransformerPreviewSetup(const QString& directory, LightTransformerPreviewSetup& outSetup, QString& outError);
[[nodiscard]] QString
DescribeLightTransformerPreparationFailure(const graphics::renderer::LightTransformerPreparationReport& report);
} // namespace ludus::editor
