#pragma once

#include "d3d12_device.h"
#include "raster.h"

#include <ludus/foundation/containers/borrowed_view.hpp>

// Native transfers own staging, commands and COM references through exact fence
// completion. Resource accessors borrow from the raster registry on its owner thread.
namespace ludus::graphics::rhi::backend::d3d12
{
[[nodiscard]] RasterStatus CreateInitialBuffer(const internal::LifetimeUploadTarget& target,
                                               const BufferDescription& description,
                                               BorrowedView<const uint8> bytes) noexcept;
void PollInitialBuffers() noexcept;
void SetBuffer(usize slot, ComOwner<ID3D12Resource>&& resource) noexcept;
[[nodiscard]] ID3D12Resource* GetTexture(usize slot) noexcept;
[[nodiscard]] D3D12_RESOURCE_STATES GetTextureState(usize slot) noexcept;
void SetTextureState(usize slot, D3D12_RESOURCE_STATES state) noexcept;
} // namespace ludus::graphics::rhi::backend::d3d12
