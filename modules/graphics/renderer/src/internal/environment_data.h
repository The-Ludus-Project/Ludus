#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/environment.hpp>

#include "../../shaders/renderer_environment_layout.h"

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace math = foundation::math;
inline constexpr usize ENVIRONMENT_SH_COUNT = LUDUS_ENVIRONMENT_SH_COEFFICIENTS;
struct EnvironmentSample final
{
    math::Vector3 DirectionWorld{};
    math::Vector3 Radiance{};
    float32 Pdf = 0;
};
struct EnvironmentData final
{
    rhi::DeviceHandle Device{};
    bool IsCpuOnly = false;
    rhi::TextureHandle Texture{};
    rhi::SamplerHandle Sampler{};
    Array<math::Vector4> Pixels;
    Array<float64> Distribution;
    math::Vector4 Irradiance[ENVIRONMENT_SH_COUNT]{};
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 MipLevels = 0;
    uint32 References = 1;
    float64 Weight = 0;
    float32 RadianceScale = 1;
    uint64 FingerprintLow = 0;
    uint64 FingerprintHigh = 0;
    usize CpuBytes = 0;
    ~EnvironmentData() noexcept;
};
[[nodiscard]] rhi::RasterStatus BuildEnvironmentDistribution(EnvironmentData& inoutData) noexcept;
void RetainEnvironment(EnvironmentData* dataOrNull) noexcept;
void ReleaseEnvironment(EnvironmentData* dataOrNull) noexcept;
void EvaluateEnvironmentBasis(math::Vector3 direction, float32* outBasis) noexcept;
math::Vector3 EvaluateEnvironment(const EnvironmentData&, math::Vector3 direction) noexcept;
/// Bilinear RGB reconstruction for the camera-visible atmosphere, wrapping longitude and clamping latitude.
[[nodiscard]] math::Vector3 EvaluateFilteredEnvironment(const EnvironmentData& data,
                                                        math::Vector3 directionWorld) noexcept;
float32 EvaluateEnvironmentPdf(const EnvironmentData&, math::Vector3 direction) noexcept;
[[nodiscard]] EnvironmentSample SampleEnvironment(const EnvironmentData&, float32 selector, float32 latitude) noexcept;
} // namespace ludus::graphics::renderer::internal
