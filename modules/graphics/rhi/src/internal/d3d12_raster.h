#pragma once

#include "d3d12_device.h"
#include "raster.h"

// Borrowed native resource access for the pipeline/pass owner. The common registry
// retains every referenced slot through its accepted submission's queue fence.
namespace ludus::graphics::rhi::backend::d3d12
{
inline constexpr usize MAX_COLOR_ATTACHMENTS = 4;
inline constexpr uint32 MAX_VERTEX_STRIDE_BYTES = 256;
struct Attachment final
{
    ID3D12Resource* ColorOrNull = nullptr;
    ID3D12Resource* DepthOrNull = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE Color{};
    D3D12_CPU_DESCRIPTOR_HANDLE Depth{};
};
[[nodiscard]] DXGI_FORMAT GetRasterFormat(RasterFormat format) noexcept;
[[nodiscard]] ID3D12Resource* GetBuffer(usize slot) noexcept;
[[nodiscard]] D3D12_SHADER_BYTECODE GetShader(usize slot) noexcept;
[[nodiscard]] ID3D12RootSignature* GetSignature(usize slot) noexcept;
void BindSet(ID3D12GraphicsCommandList* commands, usize slot) noexcept;
[[nodiscard]] Attachment GetAttachment(usize slot, uint32 mip, uint32 layer) noexcept;
[[nodiscard]] ID3D12GraphicsCommandList* GetCommandsOrNull() noexcept;
[[nodiscard]] Attachment GetSurfaceAttachment() noexcept;
[[nodiscard]] RasterStatus GrowPipelines(usize count) noexcept;
void DestroyPipeline(usize slot) noexcept;
void ResetRasterPasses() noexcept;
} // namespace ludus::graphics::rhi::backend::d3d12
