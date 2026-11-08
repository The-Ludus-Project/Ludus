#pragma once

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/static_lighting.hpp>

#include "../../shaders/probe_volume_layout.h"
#include "reference_data.h"

namespace ludus::graphics::renderer::internal
{
inline constexpr uint32 GI_PROBE_RADIANCE_SAMPLES = 256;
inline constexpr uint32 GI_PROBE_SAMPLES = LUDUS_GI_PROBE_VISIBILITY_TEXELS + GI_PROBE_RADIANCE_SAMPLES;

[[nodiscard]] uint32 CountGiProbes(const GiProbeGrid& grid) noexcept;
[[nodiscard]] uint32 ComputeGiProbeAtlasHeight(const GiProbeGrid& grid) noexcept;

// Work is disjoint by probe index. The owner encodes only after all workers complete.
class ProbeVolumeBake final
{
public:
    [[nodiscard]] rhi::RasterStatus Initialize(const GiProbeGrid& grid, usize byteBudget) noexcept;
    [[nodiscard]] rhi::RasterStatus Accumulate(const ReferenceJob& job,
                                               ReferenceCaptureInfo& inoutInfo,
                                               uint32 probeIndex,
                                               uint32 sampleIndex) noexcept;
    [[nodiscard]] rhi::RasterStatus Encode() noexcept;
    [[nodiscard]] rhi::RasterStatus GetAtlas(StaticGiProbeAtlas& outAtlas) const noexcept;

private:
    struct Probe final
    {
        StaticArray<ReferenceSum, LUDUS_GI_PROBE_FACES> Irradiance{};
        StaticArray<uint8, LUDUS_GI_PROBE_VISIBILITY_TEXELS> Depth{};
        bool IsValid = true;
    };
    GiProbeGrid mGrid{};
    Array<Probe> mProbes;
    Array<uint8> mPixels;
    float32 mScale = 0;
};
} // namespace ludus::graphics::renderer::internal
