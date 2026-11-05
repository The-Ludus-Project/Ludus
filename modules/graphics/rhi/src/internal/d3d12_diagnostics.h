#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/graphics/rhi/gpu_scope.h>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace ludus::graphics::rhi::backend::d3d12
{
// Recording hooks borrow the frame list. Submission publishes the fence only after Signal.
void BeginTimingFrame(ID3D12GraphicsCommandList& inoutCommands) noexcept;
void DiscardRecording() noexcept;
void CloseTimingScopes(ID3D12GraphicsCommandList& inoutCommands) noexcept;
void EndTimingFrame(ID3D12GraphicsCommandList& inoutCommands) noexcept;
void MarkTimingsQueued() noexcept;
void SubmitTimings(foundation::uint64 fence) noexcept;
void ReleaseTimings(bool isQueueDrained) noexcept;
void BeginPhysicalPassTiming(ID3D12GraphicsCommandList& inoutCommands, GpuPassKind kind) noexcept;
void EndPhysicalPassTiming(ID3D12GraphicsCommandList& inoutCommands) noexcept;

void InitializePixMarkers() noexcept;
void ReleasePixMarkers() noexcept;
void BeginPixEvent(ID3D12GraphicsCommandList& inoutCommands, const char* name) noexcept;
void EndPixEvent(ID3D12GraphicsCommandList& inoutCommands) noexcept;
void ConfigureDred() noexcept;
void LogDeviceRemoval(ID3D12Device* deviceOrNull) noexcept;
[[nodiscard]] bool CheckDeviceRemoval() noexcept;
} // namespace ludus::graphics::rhi::backend::d3d12
