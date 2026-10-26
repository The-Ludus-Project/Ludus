#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/base/types.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>

// Private COM ownership and borrowed access to the existing device/queue lifetime.
// No second device or submission queue is created by the raster resource owner.
namespace ludus::graphics::rhi::backend::d3d12
{
using namespace foundation;
/// Releases one COM reference; the D3D12/DXGI owner protocol for UniquePtr.
struct ComRelease final
{
    LUDUS_INLINE void operator()(IUnknown* objectOrNull) const noexcept
    {
        objectOrNull->Release();
    }
};
template <typename ObjectType>
using ComOwner = UniquePtr<ObjectType, ComRelease>;

/// SDK GUID of each COM interface this backend requests. IID_PPV_ARGS spells it with
/// __uuidof, a Microsoft extension that the project's -Wpedantic build rejects.
template <typename InterfaceType>
struct InterfaceId;
template <>
struct InterfaceId<ID3D12Debug> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_ID3D12Debug;
    }
};
template <>
struct InterfaceId<ID3D12Device> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_ID3D12Device;
    }
};
template <>
struct InterfaceId<ID3D12CommandQueue> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_ID3D12CommandQueue;
    }
};
template <>
struct InterfaceId<ID3D12Fence> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_ID3D12Fence;
    }
};
template <>
struct InterfaceId<ID3D12CommandAllocator> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_ID3D12CommandAllocator;
    }
};
template <>
struct InterfaceId<ID3D12GraphicsCommandList> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_ID3D12GraphicsCommandList;
    }
};
template <>
struct InterfaceId<ID3D12DescriptorHeap> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_ID3D12DescriptorHeap;
    }
};
template <>
struct InterfaceId<ID3D12RootSignature> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_ID3D12RootSignature;
    }
};
template <>
struct InterfaceId<ID3D12PipelineState> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_ID3D12PipelineState;
    }
};
template <>
struct InterfaceId<ID3D12Resource> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_ID3D12Resource;
    }
};
template <>
struct InterfaceId<IDXGIFactory6> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_IDXGIFactory6;
    }
};
template <>
struct InterfaceId<IDXGIAdapter1> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_IDXGIAdapter1;
    }
};
template <>
struct InterfaceId<IDXGISwapChain3> final
{
    [[nodiscard]] static LUDUS_INLINE const IID& Get() noexcept
    {
        return IID_IDXGISwapChain3;
    }
};
/// Interface ID for an output pointer; pass the same pointer to GetInterfaceOutput.
template <typename InterfaceType>
[[nodiscard]] const IID& GetInterfaceId(InterfaceType* const&) noexcept
{
    return InterfaceId<InterfaceType>::Get();
}
template <typename InterfaceType>
[[nodiscard]] void** GetInterfaceOutput(InterfaceType*& outObjectOrNull) noexcept
{
    return reinterpret_cast<void**>(&outObjectOrNull);
}

[[nodiscard]] ID3D12Device* GetDeviceOrNull() noexcept;
[[nodiscard]] ID3D12CommandQueue* GetQueueOrNull() noexcept;
[[nodiscard]] ID3D12Fence* GetFenceOrNull() noexcept;
[[nodiscard]] uint64 GetCompletedFence() noexcept;
[[nodiscard]] uint64 GetLastFence() noexcept;
[[nodiscard]] bool WaitForQueue() noexcept;
// Executes a closed creation list on the device's direct queue. Failure may have
// submitted work: the resource owner must retain its backing until reset/drain.
[[nodiscard]] bool SubmitCreation(ID3D12CommandList* commands, uint64& outFence) noexcept;
} // namespace ludus::graphics::rhi::backend::d3d12
