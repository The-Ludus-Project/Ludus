#pragma once

#include <ludus/graphics/renderer/light_transformer_encoder.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>

namespace ludus::graphics::renderer::internal
{
class LightTransformerGraph final
{
public:
    [[nodiscard]] rhi::RasterStatus Initialize(const LightTransformerModelDescription& description) noexcept;
    [[nodiscard]] foundation::usize GetRequiredPacketBytes() const noexcept;
    [[nodiscard]] rhi::RasterStatus Prepare(const RenderGraphSceneDescription& scene,
                                            rhi::DeviceHandle device,
                                            LightTransformerEncoding& outEncoding,
                                            LightTransformerPreparationReport& outReport) const noexcept;

private:
    foundation::Array<foundation::uint8> mEncoder;
    foundation::Array<foundation::float32> mDecoder;
    rhi::RasterShaderDescription mShader;
    foundation::float32 mWorldUnitsPerModelUnit = 0;
    foundation::uint32 mCount = 0;
    foundation::uint64 mSeed = 0;
    foundation::usize mMaximumBufferBytes = 0;
    foundation::usize mMaximumSurfacePreparationBytes = 0;
    foundation::uint64 mMaximumSamplingAttempts = 0;
};
} // namespace ludus::graphics::renderer::internal
