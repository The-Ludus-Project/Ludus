#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer::internal
{
/// Optional native denoiser resources for one immutable ray job. Owner thread,
/// between frames; RHI owns submitted lifetimes after Reset. No readback/fallback.
class GpuDenoiser final
{
public:
    GpuDenoiser() noexcept = default;
    ~GpuDenoiser() noexcept;
    GpuDenoiser(const GpuDenoiser&) = delete;
    GpuDenoiser& operator=(const GpuDenoiser&) = delete;
    [[nodiscard]] rhi::RasterStatus Prepare(rhi::DeviceHandle device,
                                            rhi::BufferHandle colors,
                                            foundation::uint32 width,
                                            foundation::uint32 height,
                                            foundation::uint32 quality) noexcept;
    void Reset() noexcept;
    [[nodiscard]] rhi::ComputeDispatch GetDispatch() const noexcept;

private:
    rhi::DeviceHandle mDevice{};
    rhi::BindingLayoutHandle mLayout{};
    rhi::BindingSetHandle mBindings{};
    rhi::ComputePipelineHandle mPipeline{};
    bool mHasPipeline = false;
};
[[nodiscard]] bool IsGpuDenoisingAvailable() noexcept;
} // namespace ludus::graphics::renderer::internal
