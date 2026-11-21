#include "internal/adapter.h"
#include "internal/backend.h"
#include "internal/d3d12_diagnostics.h"
#include "internal/d3d12_raster.h"
#include "internal/dxil_container.h"
#include "internal/hosted_adapter.h"
#include "internal/hosted_d3d12.h"
#include "internal/lifecycle.h"
#include "internal/measurement.h"
#include "internal/native_interop.h"
#include "internal/owner.h"
#include "internal/resources.h"
#include "internal/surface.h"
#if defined(LUDUS_RHI_TEST_READBACK)
#    include "internal/readback.h"
#endif

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/platform/native_window.h>

#include <cstring>
#include <span>

#if !defined(LUDUS_PLATFORM_WINDOWS)
#    error "The Direct3D 12 backend requires Windows"
#endif

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

// Direct3D 12 backend (docs/architecture/windows-direct3d12.md, D1 and D2): device
// and adapter selection, a direct queue with two fenced frame slots, a flip-model
// swap chain for Win32 windows or an offscreen target for headless sessions,
// clear/present, and the fullscreen compatibility draw from DXIL shaders with one
// root constant buffer view. Raster resource and descriptor ownership lives in
// raster_d3d12.cpp; drawing, compute and transfers remain later slices.
//
// Thanks to Microsoft, "Creating a basic Direct3D 12 component" and "Synchronization
// and multi-engine", Direct3D 12 programming guide, for the queue, fence and
// per-frame allocator sequence:
// https://learn.microsoft.com/windows/win32/direct3d12/creating-a-basic-direct3d-12-component
// https://learn.microsoft.com/windows/win32/direct3d12/user-mode-heap-synchronization
// to Microsoft, "DXGI flip model", for the flip-discard swap chain and its resize rules:
// https://learn.microsoft.com/windows/win32/direct3ddxgi/dxgi-flip-model
// to Microsoft, "Windows Advanced Rasterization Platform (WARP) guide", for the
// software adapter used when no hardware adapter creates a device:
// https://learn.microsoft.com/windows/win32/direct3darticles/directx-warp
// and to Microsoft, "Using a root signature" and "Using descriptors directly in the
// root signature", for the root constant buffer view that binds the uniform:
// https://learn.microsoft.com/windows/win32/direct3d12/using-a-root-signature
// https://learn.microsoft.com/windows/win32/direct3d12/using-descriptors-directly-in-the-root-signature
namespace ludus::graphics::rhi::backend
{
static void ReleaseFrameResources() noexcept;
namespace
{
using namespace foundation;
constexpr logging::LogCategory LOG_RHI{"RHI"};

using namespace d3d12;

constexpr usize FRAME_COUNT = 2;
constexpr uint32 MAX_DIMENSION = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
/// One constant buffer view spans at most 4096 sixteen-byte elements.
constexpr uint64 MAX_UNIFORM_BYTES = uint64{D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT} * 16;
/// Swap chain buffers and the headless target share one presentable format.
constexpr DXGI_FORMAT SURFACE_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM;
/// Red, green, blue and alpha clear components.
constexpr usize CLEAR_COLOR_CHANNELS = 4;
/// Longest UTF-8 encoding of one UTF-16 code unit (a lone BMP code point).
constexpr usize MAX_UTF8_BYTES_PER_UTF16_UNIT = 3;
/// Descriptors of one surface: one per swap chain buffer, then the headless target.
constexpr uint32 HEADLESS_DESCRIPTOR = FRAME_COUNT;
constexpr uint32 SURFACE_DESCRIPTOR_COUNT = FRAME_COUNT + 1;
/// Every surface slot (R14b) owns a contiguous run of SURFACE_DESCRIPTOR_COUNT descriptors.
constexpr uint32 TARGET_DESCRIPTOR_COUNT = SURFACE_DESCRIPTOR_COUNT * MAX_DEVICE_SURFACES;
/// Bound on a CPU wait for GPU work. A healthy device finishes a clear far sooner;
/// a removed device signals every fence, so a timeout means the queue is hung.
constexpr DWORD FENCE_TIMEOUT_MILLISECONDS = 10000;
/// Further wait on the software adapter after FENCE_TIMEOUT_MILLISECONDS. WARP executes on
/// the CPU, and a cold clustered-deferred frame on a slow host runs past the hardware guard.
constexpr DWORD SOFTWARE_FENCE_EXTRA_TIMEOUT_MILLISECONDS = 110000;
constexpr D3D_FEATURE_LEVEL MINIMUM_FEATURE_LEVEL = D3D_FEATURE_LEVEL_11_0;
/// The fullscreen root signature's only parameter: the uniform as a root CBV at b0, space 0.
constexpr UINT UNIFORM_ROOT_PARAMETER = 0;
constexpr UINT UNIFORM_SHADER_REGISTER = 0;
constexpr UINT UNIFORM_REGISTER_SPACE = 0;
/// Root constant buffer view addresses are aligned to 256 bytes.
constexpr usize CONSTANT_BUFFER_ALIGNMENT = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
/// The fullscreen compatibility draw is one non-indexed triangle.
constexpr UINT FULLSCREEN_VERTEX_COUNT = 3;
/// Render target slots in a pipeline's blend state.
constexpr usize RENDER_TARGET_SLOTS = 8;
/// Writes every channel unblended; the fullscreen draw replaces the cleared target.
constexpr D3D12_RENDER_TARGET_BLEND_DESC OPAQUE_TARGET_BLEND =
{
    .BlendEnable = FALSE,
    .LogicOpEnable = FALSE,
    .SrcBlend = D3D12_BLEND_ONE,
    .DestBlend = D3D12_BLEND_ZERO,
    .BlendOp = D3D12_BLEND_OP_ADD,
    .SrcBlendAlpha = D3D12_BLEND_ONE,
    .DestBlendAlpha = D3D12_BLEND_ZERO,
    .BlendOpAlpha = D3D12_BLEND_OP_ADD,
    .LogicOp = D3D12_LOGIC_OP_NOOP,
    .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
};

/// DXIL bytecode copied at CreateShader; the description's bytes are only borrowed.
struct Shader final
{
    Array<uint8> Bytecode;
};
/// One upload buffer per frame slot, persistently mapped. Draw copies the latest bytes
/// into the open slot's buffer, which that slot's fence wait has released.
struct Uniform final
{
    ComOwner<ID3D12Resource> Buffers[FRAME_COUNT];
    uint8* MappedOrNull[FRAME_COUNT]{};
    usize Size = 0;
    uint8 Bytes[internal::UNIFORM_CAPACITY]{};
};
struct Pipeline final
{
    ComOwner<ID3D12PipelineState> State;
    usize UniformSlot = 0;
};

/// @brief Presentation record of one surface (R14b): its borrowed window, swap chain or
/// hosted producer, headless target, frame target and completion counters.
///
/// Slot 0 is the primary surface. Every surface of a session shares the primary's color
/// format, so pipelines created for the primary draw to all of them. The surface depth
/// buffer (raster_draw_d3d12.cpp) is shared and resized to the open frame's extent.
struct Direct3D12Surface final
{
    HWND WindowOrNull = nullptr;
    ComOwner<IDXGISwapChain3> SwapChain;
    StaticArray<ComOwner<ID3D12Resource>, FRAME_COUNT> BackBuffers{};
    uint32 SwapWidth = 0;
    uint32 SwapHeight = 0;
    ComOwner<ID3D12Resource> Headless;
    HostedProducer Hosted;
    FrameTarget Target{};
    /// Fence values of this surface's submitted frames not yet observed complete.
    StaticArray<uint64, FRAME_COUNT> PendingFences{};
    uint64 RenderedFrames = 0;
    uint64 PresentedFrames = 0;
    bool IsConnected = false;
};
/// @brief DXGI factory, device, direct queue, frame fence, surfaces and fullscreen resources
/// of one device owner (R14c).
struct Direct3D12State final
{
    ComOwner<IDXGIFactory6> Factory;
    ComOwner<IDXGIAdapter1> Adapter;
    ComOwner<ID3D12Device> Device;
    ComOwner<ID3D12CommandQueue> Queue;
    ComOwner<ID3D12Fence> Fence;
    HANDLE FenceEventOrNull = nullptr;
    uint64 FenceValue = 0;
    foundation::StaticArray<uint64, FRAME_COUNT> Submitted{};
    foundation::StaticArray<ComOwner<ID3D12CommandAllocator>, FRAME_COUNT> Allocators{};
    ComOwner<ID3D12GraphicsCommandList> Commands;
    ComOwner<ID3D12DescriptorHeap> TargetHeap;
    uint32 TargetDescriptorSize = 0;
    StaticArray<Direct3D12Surface, MAX_DEVICE_SURFACES> Surfaces{};
    /// Surface routed SetTarget/Begin/End; see backend::SelectSurface.
    usize ActiveSurface = 0;
    /// Shared by every fullscreen pipeline; created with the device.
    ComOwner<ID3D12RootSignature> RootSignature;
    foundation::StaticArray<Shader, internal::RESOURCE_CAPACITY> Shaders{};
    foundation::StaticArray<Uniform, internal::RESOURCE_CAPACITY> Uniforms{};
    foundation::StaticArray<Pipeline, internal::RESOURCE_CAPACITY> Pipelines{};
    /// Color target of the open frame: a swap chain buffer or the headless texture.
    ID3D12Resource* SurfaceOrNull = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE SurfaceDescriptor{};
    FrameInfo Frame{};
    usize Slot = 0;
    uint32 Token = 0;
    bool IsSoftwareAdapter = false;
    bool IsConnected = false;
    bool IsRendering = false;
    bool IsRecording = false;
};
/// One record per device owner slot (R14c); see internal/owner.h.
foundation::StaticArray<Direct3D12State, MAX_DEVICE_OWNERS> gDirect3D12States{};
/// Direct3D12State of the bound owner slot.
Direct3D12State& GetDirect3D12() noexcept
{
    return gDirect3D12States[internal::GetBoundOwnerSlot()];
}
Direct3D12Surface& GetActiveSurface() noexcept
{
    return GetDirect3D12().Surfaces[GetDirect3D12().ActiveSurface];
}

template <typename ObjectType>
[[nodiscard]] ComOwner<ObjectType> Adopt(ObjectType* objectOrNull) noexcept
{
    return ComOwner<ObjectType>(objectOrNull);
}

void LogFailure(const char* operation, HRESULT result) noexcept
{
    LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: {} failed (HRESULT {:x})", operation, static_cast<uint32>(result));
}

[[nodiscard]] bool IsValidChannel(float64 value) noexcept
{
    // Ordered comparisons reject NaN as well as values outside the clear range.
    return value >= 0 && value <= 1;
}

[[nodiscard]] bool IsOwnedWin32Window(HWND window) noexcept
{
    // Swap chains and their window messages belong to the window's creating thread.
    return IsWindow(window) != FALSE && GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId();
}

[[nodiscard]] bool IsValidWindow(const WindowInfo& window) noexcept
{
    if (window.System == platform::WindowSystem::Headless)
    {
        return true;
    }
    if (window.System == platform::WindowSystem::Hosted)
    {
        return window.HostedSurfaceOrNull != nullptr;
    }
    return window.System == platform::WindowSystem::Win32 && window.Win32WindowOrNull != nullptr &&
           IsOwnedWin32Window(static_cast<HWND>(window.Win32WindowOrNull)) == true;
}

[[nodiscard]] bool IsDeviceRemoved() noexcept
{
    return GetDirect3D12().Device.Get() != nullptr && GetDirect3D12().Device->GetDeviceRemovedReason() != S_OK;
}

[[nodiscard]] bool IsDeviceLossResult(HRESULT result) noexcept
{
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
           result == DXGI_ERROR_DEVICE_HUNG || IsDeviceRemoved() == true;
}

[[nodiscard]] FrameStatus FailFrame(StartupError error) noexcept
{
    LogDeviceRemoval(GetDirect3D12().Device.Get());
    DiscardRecording();
    GetDirect3D12().SurfaceOrNull = nullptr;
    GetDirect3D12().Frame = {};
    if (internal::Current(GetDirect3D12().Token) == true)
    {
        // The facade finalizes the session and calls Shutdown(); state is released there.
        internal::Fail(GetDirect3D12().Token, error);
    }
    else
    {
        ReleaseFrameResources();
    }
    return FrameStatus::Failed;
}

[[nodiscard]] FrameStatus FailFrame(const char* operation, HRESULT result) noexcept
{
    LogFailure(operation, result);
    return FailFrame(IsDeviceLossResult(result) == true ? StartupError::DeviceLost
                                                        : StartupError::RenderingUnavailable);
}

[[nodiscard]] bool WaitForFence(uint64 value) noexcept
{
    const uint64 completed = GetDirect3D12().Fence->GetCompletedValue();
    if (completed == ~uint64{0})
    {
        LogDeviceRemoval(GetDirect3D12().Device.Get());
        return false;
    }
    if (value == 0 || completed >= value)
    {
        return true;
    }
    const HRESULT armed = GetDirect3D12().Fence->SetEventOnCompletion(value, GetDirect3D12().FenceEventOrNull);
    if (FAILED(armed) == true)
    {
        LogFailure("Fence completion event", armed);
        return false;
    }
    DWORD waited = WaitForSingleObject(GetDirect3D12().FenceEventOrNull, FENCE_TIMEOUT_MILLISECONDS);
    if (waited == WAIT_TIMEOUT && GetDirect3D12().IsSoftwareAdapter == true)
    {
        LUDUS_LOG_WARN(LOG_RHI,
                       "Direct3D 12: the software adapter is still executing after {} ms; waiting up to {} ms more",
                       static_cast<uint32>(FENCE_TIMEOUT_MILLISECONDS),
                       static_cast<uint32>(SOFTWARE_FENCE_EXTRA_TIMEOUT_MILLISECONDS));
        waited = WaitForSingleObject(GetDirect3D12().FenceEventOrNull, SOFTWARE_FENCE_EXTRA_TIMEOUT_MILLISECONDS);
    }
    if (waited != WAIT_OBJECT_0)
    {
        LUDUS_LOG_ERROR(LOG_RHI,
                        "Direct3D 12: fence value {} did not complete (wait result {:x}, completed {}); "
                        "the queue is hung or the frame is too heavy for this adapter",
                        value,
                        static_cast<uint32>(waited),
                        GetDirect3D12().Fence->GetCompletedValue());
        return false;
    }
    const uint64 signaled = GetDirect3D12().Fence->GetCompletedValue();
    return signaled >= value && signaled != ~uint64{0};
}

[[nodiscard]] bool SignalFence(uint64& outValue) noexcept
{
    const uint64 value = GetDirect3D12().FenceValue + 1;
    if (FAILED(GetDirect3D12().Queue->Signal(GetDirect3D12().Fence.Get(), value)) == true)
    {
        return false;
    }
    GetDirect3D12().FenceValue = value;
    outValue = value;
    return true;
}

/// Waits for every submitted command list; false when the queue cannot be drained.
[[nodiscard]] bool WaitForIdle() noexcept
{
    if (GetDirect3D12().Queue.Get() == nullptr || GetDirect3D12().Fence.Get() == nullptr)
    {
        return true;
    }
    if (IsDeviceRemoved() == true)
    {
        LogDeviceRemoval(GetDirect3D12().Device.Get());
        return false;
    }
    uint64 value = 0;
    const bool isDrained = SignalFence(value) == true && WaitForFence(value) == true;
    if (isDrained == false)
    {
        LogDeviceRemoval(GetDirect3D12().Device.Get());
    }
    return isDrained;
}

/// Descriptor `index` (a back buffer or HEADLESS_DESCRIPTOR) of the active surface.
[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE GetTargetDescriptor(uint32 index) noexcept
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = GetDirect3D12().TargetHeap->GetCPUDescriptorHandleForHeapStart();
    const usize surfaceIndex = GetDirect3D12().ActiveSurface * SURFACE_DESCRIPTOR_COUNT + index;
    handle.ptr += surfaceIndex * GetDirect3D12().TargetDescriptorSize;
    return handle;
}

/// Resource states on either side of a transition barrier, named so they cannot be swapped.
struct StateTransition final
{
    D3D12_RESOURCE_STATES Before = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES After = D3D12_RESOURCE_STATE_COMMON;
};

[[nodiscard]] D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* resource, const StateTransition& states) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = states.Before;
    barrier.Transition.StateAfter = states.After;
    return barrier;
}

void EnableDebugLayer() noexcept
{
#if defined(LUDUS_BUILD_DEBUG) || defined(LUDUS_BUILD_DEVELOPMENT)
    // The debug layer comes from the app-local Agility SDK or Windows Graphics Tools.
    ID3D12Debug* debugOrNull = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(GetInterfaceId(debugOrNull), GetInterfaceOutput(debugOrNull))) == true &&
        debugOrNull != nullptr)
    {
        debugOrNull->EnableDebugLayer();
        debugOrNull->Release();
    }
    else
    {
        LUDUS_LOG_INFO(LOG_RHI,
                       "Direct3D 12: debug layer unavailable (restore the matching d3d12SDKLayers.dll in D3D12 "
                       "or install Windows Graphics Tools)");
    }
#endif
}

// Thanks to Microsoft, "D3D12CreateDevice function", Remarks: Direct3D 12 devices are
// singletons per adapter, and creating one on an adapter that already has a device in the
// process returns that device. R14c owners on one adapter therefore each hold a reference to
// one shared device (and share its removal); their queues, fences and registries stay separate.
// https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice
[[nodiscard]] bool CreateDeviceOn(IDXGIAdapter1* adapter) noexcept
{
    ID3D12Device* deviceOrNull = nullptr;
    const HRESULT result = D3D12CreateDevice(adapter,
                                             MINIMUM_FEATURE_LEVEL,
                                             GetInterfaceId(deviceOrNull),
                                             GetInterfaceOutput(deviceOrNull));
    if (FAILED(result) == true || deviceOrNull == nullptr)
    {
        return false;
    }
    GetDirect3D12().Device = Adopt(deviceOrNull);
    static_cast<void>(GetDirect3D12().Device->SetName(L"Ludus/Device"));
    return true;
}

// R14a adapter catalog. Thanks to Microsoft, DXGI API reference,
// "IDXGIFactory1::EnumAdapters1" and "DXGI_ADAPTER_DESC1", and Direct3D 12 API
// reference, "D3D12CreateDevice", whose null device output tests support without
// creating a device:
// https://learn.microsoft.com/windows/win32/api/dxgi/nf-dxgi-idxgifactory1-enumadapters1
// https://learn.microsoft.com/windows/win32/api/dxgi/ns-dxgi-dxgi_adapter_desc1
// https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice
static_assert(platform::HOSTED_ADAPTER_UUID_BYTES == ADAPTER_IDENTITY_BYTES);
[[nodiscard]] AdapterIdentity GetD3D12Identity(LUID adapter) noexcept
{
    AdapterIdentity identity{ .Api = Backend::Direct3D12 };
    const platform::HostedAdapterIdentity encoded = EncodeHostedAdapter(adapter);
    std::memcpy(identity.Bytes.GetData(), encoded.DeviceUuid.GetData(), ADAPTER_IDENTITY_BYTES);
    return identity;
}
[[nodiscard]] LUID GetD3D12Luid(const AdapterIdentity& identity) noexcept
{
    platform::HostedAdapterIdentity encoded;
    std::memcpy(encoded.DeviceUuid.GetData(), identity.Bytes.GetData(), ADAPTER_IDENTITY_BYTES);
    return DecodeHostedAdapter(encoded);
}
[[nodiscard]] bool SupportsMinimumFeatureLevel(IDXGIAdapter1* adapter) noexcept
{
    ID3D12Device* probeOrNull = nullptr;
    return SUCCEEDED(D3D12CreateDevice(adapter, MINIMUM_FEATURE_LEVEL, GetInterfaceId(probeOrNull), nullptr)) == true;
}
void DescribeD3D12Adapter(const DXGI_ADAPTER_DESC1& description,
                          bool hasBackendBaseline,
                          AdapterInfo& outAdapter) noexcept
{
    outAdapter = {};
    outAdapter.Identity = GetD3D12Identity(description.AdapterLuid);
    // DXGI names hold 128 UTF-16 units; UTF-8 needs at most three bytes for each.
    constexpr usize ADAPTER_NAME_UNITS = sizeof(description.Description) / sizeof(description.Description[0]);
    StaticArray<char, ADAPTER_NAME_UNITS * MAX_UTF8_BYTES_PER_UTF16_UNIT + 1> name{};
    const int32 bytes = WideCharToMultiByte(CP_UTF8,
                                            0,
                                            description.Description,
                                            -1,
                                            name.GetData(),
                                            static_cast<int32>(name.GetSize()),
                                            nullptr,
                                            nullptr);
    if (bytes > 1)
    {
        // The converted length counts the terminating zero.
        internal::AssignAdapterName({name.GetData(), static_cast<usize>(bytes - 1)}, outAdapter.Name);
    }
    outAdapter.Kind =
        (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ? AdapterKind::Software : AdapterKind::Unknown;
    outAdapter.VendorId = description.VendorId;
    outAdapter.DeviceId = description.DeviceId;
    outAdapter.LocalMemoryBytes = description.DedicatedVideoMemory;
    outAdapter.MaxTextureDimension2D = hasBackendBaseline == true ? D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION : 0;
    outAdapter.HasBackendBaseline = hasBackendBaseline;
}
/// Start on the explicitly requested catalog adapter, honouring the test adapter class.
[[nodiscard]] bool SelectRequestedAdapter(const AdapterIdentity& requested,
                                          const platform::HostedSurfaceChannel* channelOrNull,
                                          bool shouldUseWarp,
                                          bool isHardwareRequired) noexcept
{
    platform::HostedAdapterIdentity preferred;
    if (channelOrNull != nullptr && channelOrNull->TryGetConsumerAdapter(preferred) == true &&
        GetD3D12Identity(DecodeHostedAdapter(preferred)) != requested)
    {
        LUDUS_LOG_WARN(LOG_RHI, "Direct3D 12: the requested adapter differs from the hosting editor's");
    }
    IDXGIAdapter1* adapterOrNull = nullptr;
    const HRESULT found = GetDirect3D12().Factory->EnumAdapterByLuid(GetD3D12Luid(requested),
                                                                     GetInterfaceId(adapterOrNull),
                                                                     GetInterfaceOutput(adapterOrNull));
    ComOwner<IDXGIAdapter1> adapter = Adopt(adapterOrNull);
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(found) == true || adapter.Get() == nullptr || FAILED(adapter->GetDesc1(&description)) == true)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: the requested adapter is gone; list the adapters again");
        return false;
    }
    const bool isSoftware = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    if (shouldUseWarp == true && isSoftware == false)
    {
        LUDUS_LOG_ERROR(LOG_RHI,
                        "Direct3D 12: the requested adapter is hardware, but LUDUS_TEST_REQUIRE_WARP requires WARP; "
                        "request a software adapter or clear the variable");
        return false;
    }
    if (isHardwareRequired == true && isSoftware == true)
    {
        LUDUS_LOG_ERROR(LOG_RHI,
                        "Direct3D 12: the requested adapter is software, but LUDUS_TEST_REQUIRE_HARDWARE requires "
                        "hardware; request a hardware adapter or clear the variable");
        return false;
    }
    if (CreateDeviceOn(adapter.Get()) == false)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: the requested adapter has no feature level 11.0 device");
        return false;
    }
    GetDirect3D12().Adapter = Move(adapter);
    GetDirect3D12().IsSoftwareAdapter = isSoftware;
    return true;
}

/// Test builds can require one adapter class for reproducible qualification.
[[nodiscard]] bool IsTestAdapterRequired(const wchar_t* variable) noexcept
{
#if defined(LUDUS_RHI_TEST_READBACK)
    return ::GetEnvironmentVariableW(variable, nullptr, 0) != 0;
#else
    static_cast<void>(variable);
    return false;
#endif
}

/// Prefers the highest-performance hardware adapter with a WARP fallback.
[[nodiscard]] bool SelectAdapter(const platform::HostedSurfaceChannel* channelOrNull) noexcept
{
    const bool shouldUseWarp = IsTestAdapterRequired(L"LUDUS_TEST_REQUIRE_WARP");
    const bool isHardwareRequired = IsTestAdapterRequired(L"LUDUS_TEST_REQUIRE_HARDWARE");
    if (shouldUseWarp == true && isHardwareRequired == true)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: set only one test adapter requirement");
        return false;
    }
    const AdapterIdentity* requestedOrNull = internal::GetRequestedAdapterOrNull();
    if (requestedOrNull != nullptr)
    {
        return SelectRequestedAdapter(*requestedOrNull, channelOrNull, shouldUseWarp, isHardwareRequired);
    }
    platform::HostedAdapterIdentity preferred;
    if (channelOrNull != nullptr && channelOrNull->TryGetConsumerAdapter(preferred) == true)
    {
        IDXGIAdapter1* adapterOrNull = nullptr;
        const HRESULT found = GetDirect3D12().Factory->EnumAdapterByLuid(DecodeHostedAdapter(preferred),
                                                                         GetInterfaceId(adapterOrNull),
                                                                         GetInterfaceOutput(adapterOrNull));
        ComOwner<IDXGIAdapter1> adapter = Adopt(adapterOrNull);
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(found) == true || adapter.Get() == nullptr || FAILED(adapter->GetDesc1(&description)) == true)
        {
            return false;
        }
        const bool isSoftware = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if ((shouldUseWarp == true && isSoftware == false) || (isHardwareRequired == true && isSoftware == true) ||
            CreateDeviceOn(adapter.Get()) == false)
        {
            return false;
        }
        GetDirect3D12().Adapter = Move(adapter);
        GetDirect3D12().IsSoftwareAdapter = isSoftware;
        return true;
    }
    for (UINT index = 0; shouldUseWarp == false; ++index)
    {
        IDXGIAdapter1* adapterOrNull = nullptr;
        if (GetDirect3D12().Factory->EnumAdapterByGpuPreference(index,
                                                                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                GetInterfaceId(adapterOrNull),
                                                                GetInterfaceOutput(adapterOrNull)) ==
            DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (adapterOrNull == nullptr)
        {
            continue;
        }
        ComOwner<IDXGIAdapter1> adapter = Adopt(adapterOrNull);
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) == true || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }
        if (CreateDeviceOn(adapter.Get()) == true)
        {
            GetDirect3D12().Adapter = Move(adapter);
            GetDirect3D12().IsSoftwareAdapter = false;
            return true;
        }
    }
    if (isHardwareRequired == true)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: enable a compatible physical adapter for hardware qualification");
        return false;
    }
    IDXGIAdapter1* warpOrNull = nullptr;
    if (FAILED(GetDirect3D12().Factory->EnumWarpAdapter(GetInterfaceId(warpOrNull), GetInterfaceOutput(warpOrNull))) ==
            true ||
        warpOrNull == nullptr)
    {
        return false;
    }
    ComOwner<IDXGIAdapter1> warp = Adopt(warpOrNull);
    if (CreateDeviceOn(warp.Get()) == false)
    {
        return false;
    }
    GetDirect3D12().Adapter = Move(warp);
    GetDirect3D12().IsSoftwareAdapter = true;
    LUDUS_LOG_INFO(LOG_RHI, "Direct3D 12: using WARP");
    return true;
}

// Thanks to Microsoft, "IDXGIFactory4::EnumAdapterByLuid method", DXGI API
// reference, for resolving the adapter from the retained device's LUID.
// https://learn.microsoft.com/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgifactory4-enumadapterbyluid
[[nodiscard]] bool AdoptNativeDevice(const NativeDeviceIdentity& identity) noexcept
{
    ID3D12Device* device = static_cast<ID3D12Device*>(identity.Device.GetObject());
    ID3D12CommandQueue* queue = static_cast<ID3D12CommandQueue*>(identity.CommandQueue.GetObject());
    LUDUS_ASSERT(device != nullptr && queue != nullptr);
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || device->GetNodeCount() != 1 ||
        device->GetDeviceRemovedReason() != S_OK)
    {
        return false;
    }
    ID3D12Device* queueDeviceOrNull = nullptr;
    const HRESULT queueResult =
        queue->GetDevice(GetInterfaceId(queueDeviceOrNull), GetInterfaceOutput(queueDeviceOrNull));
    const ComOwner<ID3D12Device> queueDevice = Adopt(queueDeviceOrNull);
    if (FAILED(queueResult) == true || queueDevice.Get() != device)
    {
        return false;
    }
    IDXGIAdapter1* adapterOrNull = nullptr;
    const HRESULT adapterResult = GetDirect3D12().Factory->EnumAdapterByLuid(device->GetAdapterLuid(),
                                                                             GetInterfaceId(adapterOrNull),
                                                                             GetInterfaceOutput(adapterOrNull));
    if (FAILED(adapterResult) == true || adapterOrNull == nullptr)
    {
        return false;
    }
    GetDirect3D12().Adapter = Adopt(adapterOrNull);
    device->AddRef();
    queue->AddRef();
    GetDirect3D12().Device = Adopt(device);
    GetDirect3D12().Queue = Adopt(queue);
    return true;
}

[[nodiscard]] bool CreateDirectQueue() noexcept
{
    const D3D12_COMMAND_QUEUE_DESC queueDescription =
    {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    ID3D12CommandQueue* queueOrNull = nullptr;
    if (FAILED(GetDirect3D12().Device->CreateCommandQueue(&queueDescription,
                                                          GetInterfaceId(queueOrNull),
                                                          GetInterfaceOutput(queueOrNull))) == true)
    {
        return false;
    }
    GetDirect3D12().Queue = Adopt(queueOrNull);
    static_cast<void>(GetDirect3D12().Queue->SetName(L"Ludus/DirectQueue"));
    return true;
}

[[nodiscard]] bool CreateQueueObjects() noexcept
{
    if (GetDirect3D12().Queue.Get() == nullptr && CreateDirectQueue() == false)
    {
        return false;
    }
    ID3D12Fence* fenceOrNull = nullptr;
    if (FAILED(GetDirect3D12().Device->CreateFence(0,
                                                   D3D12_FENCE_FLAG_NONE,
                                                   GetInterfaceId(fenceOrNull),
                                                   GetInterfaceOutput(fenceOrNull))) == true)
    {
        return false;
    }
    GetDirect3D12().Fence = Adopt(fenceOrNull);
    GetDirect3D12().FenceEventOrNull = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (GetDirect3D12().FenceEventOrNull == nullptr)
    {
        return false;
    }
    for (ComOwner<ID3D12CommandAllocator>& allocator : GetDirect3D12().Allocators)
    {
        ID3D12CommandAllocator* allocatorOrNull = nullptr;
        if (FAILED(GetDirect3D12().Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                  GetInterfaceId(allocatorOrNull),
                                                                  GetInterfaceOutput(allocatorOrNull))) == true)
        {
            return false;
        }
        allocator = Adopt(allocatorOrNull);
    }
    ID3D12GraphicsCommandList* commandsOrNull = nullptr;
    if (FAILED(GetDirect3D12().Device->CreateCommandList(0,
                                                         D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         GetDirect3D12().Allocators[0].Get(),
                                                         nullptr,
                                                         GetInterfaceId(commandsOrNull),
                                                         GetInterfaceOutput(commandsOrNull))) == true)
    {
        return false;
    }
    GetDirect3D12().Commands = Adopt(commandsOrNull);
    static_cast<void>(GetDirect3D12().Commands->SetName(L"Ludus/FrameCommands"));
    // Command lists are created open; every frame resets it again.
    if (FAILED(GetDirect3D12().Commands->Close()) == true)
    {
        return false;
    }
    const D3D12_DESCRIPTOR_HEAP_DESC heapDescription =
    {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = TARGET_DESCRIPTOR_COUNT,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0,
    };
    ID3D12DescriptorHeap* heapOrNull = nullptr;
    if (FAILED(GetDirect3D12().Device->CreateDescriptorHeap(&heapDescription,
                                                            GetInterfaceId(heapOrNull),
                                                            GetInterfaceOutput(heapOrNull))) == true)
    {
        return false;
    }
    GetDirect3D12().TargetHeap = Adopt(heapOrNull);
    GetDirect3D12().TargetDescriptorSize =
        GetDirect3D12().Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

/// One root CBV visible to every stage; the fullscreen pipelines have no input layout.
[[nodiscard]] bool CreateRootSignature() noexcept
{
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister = UNIFORM_SHADER_REGISTER;
    parameter.Descriptor.RegisterSpace = UNIFORM_REGISTER_SPACE;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_ROOT_SIGNATURE_DESC description =
    {
        .NumParameters = 1,
        .pParameters = &parameter,
        .NumStaticSamplers = 0,
        .pStaticSamplers = nullptr,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS,
    };
    ID3DBlob* serializedOrNull = nullptr;
    ID3DBlob* errorsOrNull = nullptr;
    HRESULT result =
        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serializedOrNull, &errorsOrNull);
    const ComOwner<ID3DBlob> serialized = Adopt(serializedOrNull);
    const ComOwner<ID3DBlob> errors = Adopt(errorsOrNull);
    if (FAILED(result) == true || serialized.Get() == nullptr)
    {
        LogFailure("D3D12SerializeRootSignature", result);
        return false;
    }
    ID3D12RootSignature* signatureOrNull = nullptr;
    result = GetDirect3D12().Device->CreateRootSignature(0,
                                                         serialized->GetBufferPointer(),
                                                         serialized->GetBufferSize(),
                                                         GetInterfaceId(signatureOrNull),
                                                         GetInterfaceOutput(signatureOrNull));
    if (FAILED(result) == true)
    {
        LogFailure("CreateRootSignature", result);
        return false;
    }
    GetDirect3D12().RootSignature = Adopt(signatureOrNull);
    return true;
}

void ReleaseBackBuffers() noexcept
{
    for (ComOwner<ID3D12Resource>& buffer : GetActiveSurface().BackBuffers)
    {
        buffer.Reset();
    }
}

[[nodiscard]] HRESULT AcquireBackBuffers() noexcept
{
    for (uint32 index = 0; index < FRAME_COUNT; ++index)
    {
        ID3D12Resource* bufferOrNull = nullptr;
        const HRESULT result = GetActiveSurface().SwapChain->GetBuffer(index,
                                                                       GetInterfaceId(bufferOrNull),
                                                                       GetInterfaceOutput(bufferOrNull));
        if (FAILED(result) == true)
        {
            ReleaseBackBuffers();
            return result;
        }
        GetActiveSurface().BackBuffers[index] = Adopt(bufferOrNull);
        GetDirect3D12().Device->CreateRenderTargetView(bufferOrNull, nullptr, GetTargetDescriptor(index));
    }
    DXGI_SWAP_CHAIN_DESC1 description{};
    const HRESULT result = GetActiveSurface().SwapChain->GetDesc1(&description);
    if (FAILED(result) == true)
    {
        ReleaseBackBuffers();
        return result;
    }
    GetActiveSurface().SwapWidth = description.Width;
    GetActiveSurface().SwapHeight = description.Height;
    return S_OK;
}

[[nodiscard]] bool CreateSwapChain(HWND window, uint32 width, uint32 height) noexcept
{
    const DXGI_SWAP_CHAIN_DESC1 description =
    {
        .Width = width,
        .Height = height,
        .Format = SURFACE_FORMAT,
        .Stereo = FALSE,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = static_cast<UINT>(FRAME_COUNT),
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
        .Flags = 0,
    };
    IDXGISwapChain1* swapChainOrNull = nullptr;
    HRESULT result = GetDirect3D12().Factory->CreateSwapChainForHwnd(GetDirect3D12().Queue.Get(),
                                                                     window,
                                                                     &description,
                                                                     nullptr,
                                                                     nullptr,
                                                                     &swapChainOrNull);
    if (FAILED(result) == true || swapChainOrNull == nullptr)
    {
        LogFailure("CreateSwapChainForHwnd", result);
        return false;
    }
    ComOwner<IDXGISwapChain1> swapChain = Adopt(swapChainOrNull);
    IDXGISwapChain3* swapChain3OrNull = nullptr;
    result = swapChain->QueryInterface(GetInterfaceId(swapChain3OrNull), GetInterfaceOutput(swapChain3OrNull));
    if (FAILED(result) == true || swapChain3OrNull == nullptr)
    {
        LogFailure("IDXGISwapChain3 query", result);
        return false;
    }
    GetActiveSurface().SwapChain = Adopt(swapChain3OrNull);
    // Ludus owns fullscreen policy; DXGI must not toggle it on Alt+Enter.
    static_cast<void>(GetDirect3D12().Factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER));
    result = AcquireBackBuffers();
    if (FAILED(result) == true)
    {
        LogFailure("swap chain buffer acquisition", result);
        GetActiveSurface().SwapChain.Reset();
        return false;
    }
    return true;
}

/// Matches the swap chain to the frame target; DXGI stretches it to the client area.
[[nodiscard]] HRESULT ResizeSwapChain(uint32 width, uint32 height) noexcept
{
    if (width == GetActiveSurface().SwapWidth && height == GetActiveSurface().SwapHeight)
    {
        return S_OK;
    }
    // ResizeBuffers requires every reference to the old buffers to be released.
    if (WaitForIdle() == false)
    {
        return DXGI_ERROR_DEVICE_HUNG;
    }
    ReleaseBackBuffers();
    const HRESULT result =
        GetActiveSurface().SwapChain->ResizeBuffers(static_cast<UINT>(FRAME_COUNT), width, height, SURFACE_FORMAT, 0);
    if (FAILED(result) == true)
    {
        return result;
    }
    return AcquireBackBuffers();
}

[[nodiscard]] HRESULT PrepareHeadless(uint32 width, uint32 height) noexcept
{
    if (GetActiveSurface().Headless.Get() != nullptr)
    {
        const D3D12_RESOURCE_DESC current = GetActiveSurface().Headless->GetDesc();
        if (current.Width == width && current.Height == height)
        {
            return S_OK;
        }
        // The previous target may still be in use by an in-flight frame.
        if (WaitForIdle() == false)
        {
            return DXGI_ERROR_DEVICE_HUNG;
        }
        GetActiveSurface().Headless.Reset();
    }
    const D3D12_HEAP_PROPERTIES heap =
    {
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 0,
        .VisibleNodeMask = 0,
    };
    const D3D12_RESOURCE_DESC description =
    {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = GetSurfaceColorFormat(),
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };
    ID3D12Resource* textureOrNull = nullptr;
    // Between frames the headless target rests in COMMON, like a presented swap chain buffer.
    const HRESULT result = GetDirect3D12().Device->CreateCommittedResource(&heap,
                                                                           D3D12_HEAP_FLAG_NONE,
                                                                           &description,
                                                                           D3D12_RESOURCE_STATE_COMMON,
                                                                           nullptr,
                                                                           GetInterfaceId(textureOrNull),
                                                                           GetInterfaceOutput(textureOrNull));
    if (FAILED(result) == true)
    {
        return result;
    }
    GetActiveSurface().Headless = Adopt(textureOrNull);
    GetDirect3D12().Device->CreateRenderTargetView(textureOrNull, nullptr, GetTargetDescriptor(HEADLESS_DESCRIPTOR));
    return S_OK;
}

/// Count this surface's completed frames without waiting.
void RefreshSurface(Direct3D12Surface& inoutSurface) noexcept
{
    const uint64 completed = GetDirect3D12().Fence.Get() != nullptr ? GetDirect3D12().Fence->GetCompletedValue() : 0;
    if (completed == ~uint64{0})
    {
        // A removed device signals nothing reliably; device loss is reported elsewhere.
        return;
    }
    for (uint64& fence : inoutSurface.PendingFences)
    {
        if (fence != 0 && fence <= completed)
        {
            ++inoutSurface.RenderedFrames;
            fence = 0;
        }
    }
}
/// Record a submitted frame of this surface for render-completion reporting.
void TrackSurfaceFrame(Direct3D12Surface& inoutSurface, uint64 fence) noexcept
{
    RefreshSurface(inoutSurface);
    for (uint64& pending : inoutSurface.PendingFences)
    {
        if (pending == 0)
        {
            pending = fence;
            return;
        }
    }
    // Device frame slots bound the frames in flight; wait for the oldest only if all are pending.
    static_cast<void>(WaitForFence(inoutSurface.PendingFences[0]));
    RefreshSurface(inoutSurface);
    inoutSurface.PendingFences[0] = fence;
}
/// Borrow a validated window into the active surface record, which keeps its extent as the target.
[[nodiscard]] bool ConnectSurface(const WindowInfo& window) noexcept
{
    Direct3D12Surface& surface = GetActiveSurface();
    if (window.System == platform::WindowSystem::Win32)
    {
        const HWND handle = static_cast<HWND>(window.Win32WindowOrNull);
        if (CreateSwapChain(handle, window.Width, window.Height) == false)
        {
            return false;
        }
        surface.WindowOrNull = handle;
    }
    if (window.System == platform::WindowSystem::Hosted)
    {
        platform::HostedSurfaceChannel* channelOrNull = window.HostedSurfaceOrNull;
        LUDUS_ASSERT(channelOrNull != nullptr);
        platform::HostedAdapterIdentity preferred;
        if (channelOrNull->TryGetConsumerAdapter(preferred) == true &&
            internal::AreAdaptersEqual(preferred, EncodeHostedAdapter(GetDirect3D12().Device->GetAdapterLuid())) ==
                false)
        {
            LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: the hosted viewport requires the editor's GPU adapter");
            return false;
        }
        surface.Hosted.Connect(*channelOrNull);
    }
    surface.Target = { .Width = window.Width, .Height = window.Height };
    surface.RenderedFrames = 0;
    surface.PresentedFrames = 0;
    surface.IsConnected = true;
    return true;
}
/// @brief Retire the active surface's presentation resources (R14b).
///
/// Drains the queue, then releases this surface's swap chain buffers and swap chain,
/// headless target and hosted producer. Completion counters are kept, so a lost surface
/// still reports what it rendered.
void ReleaseActiveSurface() noexcept
{
    Direct3D12Surface& surface = GetActiveSurface();
    static_cast<void>(WaitForIdle());
    RefreshSurface(surface);
    surface.Hosted.Disconnect();
    ReleaseBackBuffers();
    if (surface.SwapChain.Get() != nullptr)
    {
        // Flip-model swap chains must not be released while fullscreen.
        static_cast<void>(surface.SwapChain->SetFullscreenState(FALSE, nullptr));
    }
    surface.SwapChain.Reset();
    surface.Headless.Reset();
    surface.SwapWidth = 0;
    surface.SwapHeight = 0;
    surface.WindowOrNull = nullptr;
    surface.Target = {};
    surface.PendingFences.Fill(0);
    surface.IsConnected = false;
}
/// Retire every created surface (slots above the primary) and route frames to the primary.
void ReleaseCreatedSurfaces() noexcept
{
    for (usize slot = 1; slot < GetDirect3D12().Surfaces.GetSize(); ++slot)
    {
        if (GetDirect3D12().Surfaces[slot].IsConnected == true)
        {
            GetDirect3D12().ActiveSurface = slot;
            ReleaseActiveSurface();
        }
    }
    GetDirect3D12().ActiveSurface = 0;
}
/// @brief A created surface stopped accepting frames: retire only its presentation
/// resources and report it lost. The device and other surfaces keep working; no frame opens.
[[nodiscard]] FrameStatus LoseActiveSurface() noexcept
{
    const usize slot = GetDirect3D12().ActiveSurface;
    ReleaseActiveSurface();
    internal::LoseSurface(slot);
    return FrameStatus::Skipped;
}

void ReleaseDevice() noexcept
{
    ReleasePixMarkers();
    ReleaseCompute();
    GetDirect3D12().RootSignature.Reset();
    GetDirect3D12().Commands.Reset();
    for (ComOwner<ID3D12CommandAllocator>& allocator : GetDirect3D12().Allocators)
    {
        allocator.Reset();
    }
    GetDirect3D12().TargetHeap.Reset();
    GetDirect3D12().Fence.Reset();
    if (GetDirect3D12().FenceEventOrNull != nullptr)
    {
        CloseHandle(GetDirect3D12().FenceEventOrNull);
        GetDirect3D12().FenceEventOrNull = nullptr;
    }
    GetDirect3D12().Queue.Reset();
    GetDirect3D12().Device.Reset();
    GetDirect3D12().Adapter.Reset();
    GetDirect3D12().Factory.Reset();
    GetDirect3D12().FenceValue = 0;
    GetDirect3D12().TargetDescriptorSize = 0;
    GetDirect3D12().IsSoftwareAdapter = false;
    for (uint64& submitted : GetDirect3D12().Submitted)
    {
        submitted = 0;
    }
}
} // namespace

namespace d3d12
{
void DiscardRecording() noexcept
{
    GetActiveSurface().Hosted.Cancel();
    if (GetDirect3D12().IsRecording == true)
    {
        CloseTimingScopes(*GetDirect3D12().Commands);
        static_cast<void>(GetDirect3D12().Commands->Close());
        GetDirect3D12().IsRecording = false;
    }
}

ID3D12GraphicsCommandList* GetCommandsOrNull() noexcept
{
    return GetDirect3D12().IsRecording == true ? GetDirect3D12().Commands.Get() : nullptr;
}
DXGI_FORMAT GetSurfaceColorFormat() noexcept
{
    return GetActiveSurface().Hosted.IsConnected() == true ? HOSTED_SURFACE_FORMAT : SURFACE_FORMAT;
}
Attachment GetSurfaceAttachment() noexcept
{
    return { .ColorOrNull = GetDirect3D12().SurfaceOrNull, .Color = GetDirect3D12().SurfaceDescriptor };
}
ID3D12Device* GetDeviceOrNull() noexcept
{
    return GetDirect3D12().Device.Get();
}
ID3D12CommandQueue* GetQueueOrNull() noexcept
{
    return GetDirect3D12().Queue.Get();
}
ID3D12Fence* GetFenceOrNull() noexcept
{
    return GetDirect3D12().Fence.Get();
}
bool CheckDeviceRemoval() noexcept
{
    if (IsDeviceRemoved() == false)
    {
        return false;
    }
    static_cast<void>(FailFrame(StartupError::DeviceLost));
    return true;
}
uint64 GetCompletedFence() noexcept
{
    if (GetDirect3D12().Fence.Get() == nullptr)
    {
        return 0;
    }
    const uint64 completed = GetDirect3D12().Fence->GetCompletedValue();
    if (completed == ~uint64{0})
    {
        static_cast<void>(FailFrame(StartupError::DeviceLost));
        return 0;
    }
    return completed;
}
uint64 GetLastFence() noexcept
{
    return GetDirect3D12().FenceValue;
}
bool WaitForQueue() noexcept
{
    return WaitForIdle();
}
bool SignalQueueFence(uint64& outValue) noexcept
{
    return SignalFence(outValue);
}
bool WaitForQueueFence(uint64 value) noexcept
{
    return WaitForFence(value);
}
bool SubmitCreation(ID3D12CommandList* commands, uint64& outFence) noexcept
{
    LUDUS_ASSERT(commands != nullptr);
    ID3D12CommandList* const lists[] = {commands};
    GetDirect3D12().Queue->ExecuteCommandLists(1, lists);
    // A failed signal cannot prove completion. Retain the creation backing until
    // shutdown instead of reclaiming it through the failed publication path.
    outFence = ~uint64{0};
    if (SignalFence(outFence) == false)
    {
        // Work may have executed without a completion identity. Stop the session
        // before another operation can reuse a possibly modified resource.
        static_cast<void>(FailFrame(StartupError::DeviceLost));
        return false;
    }
    return true;
}
} // namespace d3d12

Backend Kind() noexcept
{
    return Backend::Direct3D12;
}
AdapterStatus EnumerateAdapters(AdapterCatalog& outCatalog) noexcept
{
    // Without a session, list through a temporary factory released on return.
    ComOwner<IDXGIFactory6> temporary;
    IDXGIFactory6* factory = GetDirect3D12().Factory.Get();
    if (factory == nullptr)
    {
        IDXGIFactory6* createdOrNull = nullptr;
        if (FAILED(CreateDXGIFactory2(0, GetInterfaceId(createdOrNull), GetInterfaceOutput(createdOrNull))) == true ||
            createdOrNull == nullptr)
        {
            return AdapterStatus::Failed;
        }
        temporary = Adopt(createdOrNull);
        factory = temporary.Get();
    }
    AdapterCatalog catalog{ .Api = Backend::Direct3D12 };
    for (UINT index = 0;; ++index)
    {
        IDXGIAdapter1* adapterOrNull = nullptr;
        const HRESULT found = factory->EnumAdapters1(index, &adapterOrNull);
        ComOwner<IDXGIAdapter1> adapter = Adopt(adapterOrNull);
        if (found == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(found) == true || adapter.Get() == nullptr || FAILED(adapter->GetDesc1(&description)) == true)
        {
            return AdapterStatus::Failed;
        }
        AdapterInfo* entryOrNull = internal::ReserveCatalogEntryOrNull(catalog);
        if (entryOrNull != nullptr)
        {
            DescribeD3D12Adapter(description, SupportsMinimumFeatureLevel(adapter.Get()), *entryOrNull);
        }
    }
    outCatalog = catalog;
    return AdapterStatus::Ready;
}
bool GetSelectedAdapter(AdapterInfo& outAdapter) noexcept
{
    DXGI_ADAPTER_DESC1 description{};
    if (GetDirect3D12().Adapter.Get() == nullptr || FAILED(GetDirect3D12().Adapter->GetDesc1(&description)) == true)
    {
        return false;
    }
    // The live device already proves the minimum feature level.
    DescribeD3D12Adapter(description, true, outAdapter);
    return true;
}
bool Supports(BackendSelection selection) noexcept
{
    return selection == BackendSelection::Auto;
}
/// @brief True when another owner slot holds a live device (R14c).
///
/// Thanks to Microsoft, "ID3D12Debug::EnableDebugLayer method": enabling the debug layer
/// after a device exists removes that device. A later owner therefore leaves the layer as the
/// first owner's start set it; the layer stays enabled for the process once enabled.
/// https://learn.microsoft.com/windows/win32/api/d3d12sdklayers/nf-d3d12sdklayers-id3d12debug-enabledebuglayer
[[nodiscard]] static bool HasOtherOwnerDevice() noexcept
{
    const usize bound = internal::GetBoundOwnerSlot();
    for (usize slot = 0; slot < gDirect3D12States.GetSize(); ++slot)
    {
        if (slot != bound && gDirect3D12States[slot].Device.Get() != nullptr)
        {
            return true;
        }
    }
    return false;
}
static bool InitializeDevice(const platform::HostedSurfaceChannel* channelOrNull) noexcept
{
    if (GetDirect3D12().Device.Get() != nullptr)
    {
        return GetDirect3D12().Commands.Get() != nullptr;
    }
    const NativeDeviceDescription* importedOrNull = internal::NativeDeviceRequest();
    InitializePixMarkers();
    if (importedOrNull == nullptr)
    {
        ConfigureDred();
        if (HasOtherOwnerDevice() == false)
        {
            EnableDebugLayer();
        }
    }
    UINT factoryFlags = 0;
#if defined(LUDUS_BUILD_DEBUG) || defined(LUDUS_BUILD_DEVELOPMENT)
    factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
    IDXGIFactory6* factoryOrNull = nullptr;
    HRESULT result = CreateDXGIFactory2(factoryFlags, GetInterfaceId(factoryOrNull), GetInterfaceOutput(factoryOrNull));
    if (FAILED(result) == true && factoryFlags != 0)
    {
        // The DXGI debug factory also requires Graphics Tools.
        result = CreateDXGIFactory2(0, GetInterfaceId(factoryOrNull), GetInterfaceOutput(factoryOrNull));
    }
    if (FAILED(result) == true || factoryOrNull == nullptr)
    {
        LogFailure("CreateDXGIFactory2", result);
        return false;
    }
    GetDirect3D12().Factory = Adopt(factoryOrNull);
    const bool hasDevice =
        importedOrNull != nullptr ? AdoptNativeDevice(importedOrNull->Identity) : SelectAdapter(channelOrNull);
    if (hasDevice == false)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: no compatible device and direct queue");
        return false;
    }
    if (CreateQueueObjects() == false || CreateRootSignature() == false)
    {
        // The device stays until Shutdown() so Start reports DeviceUnavailable.
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: creating the queue, fence or command objects failed");
        return false;
    }
    InitializeCompute();
    return true;
}
static bool ConnectPrimarySurface(const WindowInfo& window) noexcept;
static bool InitializeFrameResources() noexcept;

StartupError Start(const ApplicationInfo&, const WindowInfo& window, uint32 token, BackendSelection) noexcept
{
    if (IsValidWindow(window) == false)
    {
        return StartupError::InvalidWindow;
    }
    GetDirect3D12().Token = token;
    if (InitializeDevice(window.HostedSurfaceOrNull) == false)
    {
        return GetDirect3D12().Factory.Get() == nullptr  ? StartupError::InstanceUnavailable
               : GetDirect3D12().Device.Get() == nullptr ? StartupError::AdapterUnavailable
                                                         : StartupError::DeviceUnavailable;
    }
    if (ConnectPrimarySurface(window) == false)
    {
        return StartupError::SurfaceUnavailable;
    }
    if (InitializeFrameResources() == false)
    {
        return StartupError::RenderingUnavailable;
    }
    internal::Complete(token,
                       StartupError::None,
                       { .MaxFrameDimension2D = MAX_DIMENSION, .MaxUniformBufferSize = MAX_UNIFORM_BYTES });
    return StartupError::None;
}
static bool ConnectPrimarySurface(const WindowInfo& window) noexcept
{
    if (GetDirect3D12().Device.Get() == nullptr || GetDirect3D12().IsConnected == true ||
        IsValidWindow(window) == false || window.Width > MAX_DIMENSION || window.Height > MAX_DIMENSION)
    {
        return false;
    }
    // Startup connects only the primary surface.
    GetDirect3D12().ActiveSurface = 0;
    if (ConnectSurface(window) == false)
    {
        return false;
    }
    GetDirect3D12().IsConnected = true;
    return true;
}
static bool InitializeFrameResources() noexcept
{
    GetDirect3D12().IsRendering = GetDirect3D12().Commands.Get() != nullptr && GetDirect3D12().IsConnected == true;
    return GetDirect3D12().IsRendering;
}
static void ReleaseFrameResources() noexcept
{
    const bool isDrained = WaitForIdle();
    DiscardRecording();
    ReleaseTimings(isDrained);
    for (Direct3D12Surface& surface : GetDirect3D12().Surfaces)
    {
        RefreshSurface(surface);
        surface.Hosted.Disconnect();
        surface.Headless.Reset();
    }
    GetDirect3D12().SurfaceOrNull = nullptr;
    GetDirect3D12().Frame = {};
    GetDirect3D12().Slot = 0;
    GetDirect3D12().IsRendering = false;
}
void NativeInteropDetachSurface() noexcept
{
    ReleaseFrameResources();
    ReleaseCreatedSurfaces();
    ReleaseActiveSurface();
    GetDirect3D12().IsConnected = false;
}
void Shutdown() noexcept
{
    NativeInteropDetachSurface();
    ReleaseDevice();
    GetDirect3D12().Surfaces[0].RenderedFrames = 0;
    GetDirect3D12().Surfaces[0].PresentedFrames = 0;
    GetDirect3D12().Token = 0;
}
// R14c: every owner slot keeps its own factory, device, direct queue, fence and
// Direct3D12State; a borrowed device needs no process-wide state, so it may join others.
usize GetDeviceOwnerLimit(bool) noexcept
{
    return MAX_DEVICE_OWNERS;
}
usize AdditionalSurfaceLimit() noexcept
{
    return GetDirect3D12().IsRendering == true ? MAX_DEVICE_SURFACES - 1 : 0;
}
StartupError CreateSurface(usize slot, const WindowInfo& window) noexcept
{
    if (GetDirect3D12().IsRendering == false || GetDirect3D12().IsRecording == true || slot == 0 ||
        slot >= GetDirect3D12().Surfaces.GetSize() || GetDirect3D12().Surfaces[slot].IsConnected == true ||
        IsValidWindow(window) == false || window.Width > MAX_DIMENSION || window.Height > MAX_DIMENSION)
    {
        return StartupError::InvalidWindow;
    }
    // Pipelines target the primary's color format, so every surface of a session shares it.
    const bool isHosted = window.System == platform::WindowSystem::Hosted;
    if ((isHosted == true ? HOSTED_SURFACE_FORMAT : SURFACE_FORMAT) != GetSurfaceColorFormat())
    {
        return StartupError::BackendUnavailable;
    }
    const usize selected = GetDirect3D12().ActiveSurface;
    GetDirect3D12().ActiveSurface = slot;
    const bool isCreated = ConnectSurface(window);
    if (isCreated == false)
    {
        ReleaseActiveSurface();
    }
    GetDirect3D12().ActiveSurface = selected;
    if (isCreated == false && IsDeviceRemoved() == true)
    {
        static_cast<void>(FailFrame(StartupError::DeviceLost));
    }
    return isCreated == true ? StartupError::None : StartupError::SurfaceUnavailable;
}
void DestroySurface(usize slot) noexcept
{
    if (slot == 0 || slot >= GetDirect3D12().Surfaces.GetSize() || GetDirect3D12().Surfaces[slot].IsConnected == false)
    {
        return;
    }
    const usize selected = GetDirect3D12().ActiveSurface;
    GetDirect3D12().ActiveSurface = slot;
    ReleaseActiveSurface();
    GetDirect3D12().ActiveSurface = selected == slot ? 0 : selected;
}
void SelectSurface(usize slot) noexcept
{
    GetDirect3D12().ActiveSurface = slot < GetDirect3D12().Surfaces.GetSize() ? slot : 0;
}
SurfaceCompletion GetSurfaceCompletion(usize slot) noexcept
{
    if (slot >= GetDirect3D12().Surfaces.GetSize())
    {
        return {};
    }
    RefreshSurface(GetDirect3D12().Surfaces[slot]);
    return
    {
        .RenderedFrames = GetDirect3D12().Surfaces[slot].RenderedFrames,
        .PresentedFrames = GetDirect3D12().Surfaces[slot].PresentedFrames,
    };
}
FrameStatus SetTarget(const FrameTarget& target) noexcept
{
    if (GetDirect3D12().IsRecording == true)
    {
        return FrameStatus::InvalidState;
    }
    if (target.Width > MAX_DIMENSION || target.Height > MAX_DIMENSION || IsValidChannel(target.Red) == false ||
        IsValidChannel(target.Green) == false || IsValidChannel(target.Blue) == false ||
        IsValidChannel(target.Alpha) == false)
    {
        return FrameStatus::Failed;
    }
    GetActiveSurface().Target = target;
    return FrameStatus::Ready;
}
FrameStatus Begin() noexcept
{
    if (GetDirect3D12().IsRendering == false)
    {
        return FrameStatus::NotReady;
    }
    if (GetDirect3D12().IsRecording == true)
    {
        return FrameStatus::InvalidState;
    }
    GetDirect3D12().Frame = {};
    if (IsDeviceRemoved() == true)
    {
        return FailFrame("device", GetDirect3D12().Device->GetDeviceRemovedReason());
    }
    if (GetDirect3D12().ActiveSurface != 0 && GetActiveSurface().IsConnected == false)
    {
        // Lost earlier; the facade reports SurfaceLost.
        return FrameStatus::Skipped;
    }
    // A created surface is lost once its window is destroyed or leaves this thread.
    if (GetDirect3D12().ActiveSurface != 0 && GetActiveSurface().WindowOrNull != nullptr &&
        IsOwnedWin32Window(GetActiveSurface().WindowOrNull) == false)
    {
        return LoseActiveSurface();
    }
    if (GetActiveSurface().Target.Width == 0 || GetActiveSurface().Target.Height == 0 ||
        (GetActiveSurface().Target.ShouldPresentToSurface == true && GetActiveSurface().WindowOrNull != nullptr &&
         (IsIconic(GetActiveSurface().WindowOrNull) != FALSE ||
          IsWindowVisible(GetActiveSurface().WindowOrNull) == FALSE)))
    {
        return FrameStatus::Skipped;
    }
    if (WaitForFence(GetDirect3D12().Submitted[GetDirect3D12().Slot]) == false)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: frame fence wait failed or timed out");
        return FailFrame(StartupError::DeviceLost);
    }
    GetDirect3D12().Submitted[GetDirect3D12().Slot] = 0;
    HRESULT result = S_OK;
    if (GetActiveSurface().SwapChain.Get() != nullptr && GetActiveSurface().Target.ShouldPresentToSurface == true)
    {
        result = ResizeSwapChain(GetActiveSurface().Target.Width, GetActiveSurface().Target.Height);
        if (FAILED(result) == true && GetDirect3D12().ActiveSurface != 0 && IsDeviceLossResult(result) == false)
        {
            LogFailure("ResizeBuffers", result);
            return LoseActiveSurface();
        }
        if (FAILED(result) == true)
        {
            return FailFrame("ResizeBuffers", result);
        }
        const uint32 index = GetActiveSurface().SwapChain->GetCurrentBackBufferIndex();
        GetDirect3D12().SurfaceOrNull = GetActiveSurface().BackBuffers[index].Get();
        GetDirect3D12().SurfaceDescriptor = GetTargetDescriptor(index);
    }
    else if (GetActiveSurface().Hosted.IsConnected() == true &&
             GetActiveSurface().Target.ShouldPresentToSurface == true)
    {
        const HostedAcquire acquired = GetActiveSurface().Hosted.Acquire(GetActiveSurface().Target.Width,
                                                                         GetActiveSurface().Target.Height,
                                                                         GetDirect3D12().SurfaceOrNull,
                                                                         GetDirect3D12().SurfaceDescriptor);
        if (acquired == HostedAcquire::Pending)
        {
            return FrameStatus::Skipped;
        }
        if (acquired == HostedAcquire::Failed && GetDirect3D12().ActiveSurface != 0 && IsDeviceRemoved() == false)
        {
            return LoseActiveSurface();
        }
        if (acquired == HostedAcquire::Failed)
        {
            return FailFrame(IsDeviceRemoved() == true ? StartupError::DeviceLost : StartupError::SurfaceUnavailable);
        }
    }
    else
    {
        result = PrepareHeadless(GetActiveSurface().Target.Width, GetActiveSurface().Target.Height);
        if (FAILED(result) == true)
        {
            return FailFrame("headless target creation", result);
        }
        GetDirect3D12().SurfaceOrNull = GetActiveSurface().Headless.Get();
        GetDirect3D12().SurfaceDescriptor = GetTargetDescriptor(HEADLESS_DESCRIPTOR);
    }
    ID3D12CommandAllocator* allocator = GetDirect3D12().Allocators[GetDirect3D12().Slot].Get();
    result = allocator->Reset();
    if (FAILED(result) == true)
    {
        return FailFrame("ID3D12CommandAllocator::Reset", result);
    }
    result = GetDirect3D12().Commands->Reset(allocator, nullptr);
    if (FAILED(result) == true)
    {
        return FailFrame("ID3D12GraphicsCommandList::Reset", result);
    }
    GetDirect3D12().IsRecording = true;
    ResetBufferStates();
    RasterEndPass();
    BeginTimingFrame(*GetDirect3D12().Commands);
    const D3D12_RESOURCE_BARRIER toTarget =
        MakeTransition(GetDirect3D12().SurfaceOrNull,
                       { .Before = D3D12_RESOURCE_STATE_PRESENT, .After = D3D12_RESOURCE_STATE_RENDER_TARGET });
    GetDirect3D12().Commands->ResourceBarrier(1, &toTarget);
    const float32 color[CLEAR_COLOR_CHANNELS] = {
        static_cast<float32>(GetActiveSurface().Target.Red),
        static_cast<float32>(GetActiveSurface().Target.Green),
        static_cast<float32>(GetActiveSurface().Target.Blue),
        static_cast<float32>(GetActiveSurface().Target.Alpha),
    };
    GetDirect3D12().Commands->ClearRenderTargetView(GetDirect3D12().SurfaceDescriptor, color, 0, nullptr);
    GetDirect3D12().Commands->OMSetRenderTargets(1, &GetDirect3D12().SurfaceDescriptor, FALSE, nullptr);
    const D3D12_RESOURCE_DESC surface = GetDirect3D12().SurfaceOrNull->GetDesc();
    // Extent comes from the acquired target, bounded by the frame target above.
    GetDirect3D12().Frame = {static_cast<uint32>(surface.Width), surface.Height, SurfaceEncoding::Unorm};
    return FrameStatus::Ready;
}
FrameStatus End() noexcept
{
    if (GetDirect3D12().IsRecording == false)
    {
        return FrameStatus::InvalidState;
    }
    RasterEndPass();
    const D3D12_RESOURCE_BARRIER toPresent =
        MakeTransition(GetDirect3D12().SurfaceOrNull,
                       { .Before = D3D12_RESOURCE_STATE_RENDER_TARGET, .After = D3D12_RESOURCE_STATE_PRESENT });
    GetDirect3D12().Commands->ResourceBarrier(1, &toPresent);
    EndTimingFrame(*GetDirect3D12().Commands);
    HRESULT result = GetDirect3D12().Commands->Close();
    GetDirect3D12().IsRecording = false;
    GetDirect3D12().SurfaceOrNull = nullptr;
    GetDirect3D12().Frame = {};
    if (FAILED(result) == true)
    {
        return FailFrame("ID3D12GraphicsCommandList::Close", result);
    }
    ID3D12CommandList* const lists[] = {GetDirect3D12().Commands.Get()};
    MarkTimingsQueued();
    GetDirect3D12().Queue->ExecuteCommandLists(1, lists);
    bool isPresented = false;
    bool isSurfaceLost = false;
    if (GetActiveSurface().SwapChain.Get() != nullptr && GetActiveSurface().Target.ShouldPresentToSurface == true)
    {
        // Synchronize to vertical blank; DXGI_STATUS_OCCLUDED is a success code.
        result = GetActiveSurface().SwapChain->Present(1, 0);
        isSurfaceLost =
            FAILED(result) == true && GetDirect3D12().ActiveSurface != 0 && IsDeviceLossResult(result) == false;
        if (FAILED(result) == true && isSurfaceLost == false)
        {
            return FailFrame("IDXGISwapChain::Present", result);
        }
        isPresented = FAILED(result) == false;
    }
    uint64 value = 0;
    if (SignalFence(value) == false)
    {
        return FailFrame(StartupError::DeviceLost);
    }
    SubmitTimings(value);
    if (GetActiveSurface().Hosted.IsConnected() == true)
    {
        GetActiveSurface().Hosted.Submit(value);
        isPresented = GetActiveSurface().Target.ShouldPresentToSurface == true;
    }
    TrackSurfaceFrame(GetActiveSurface(), value);
    GetActiveSurface().PresentedFrames += isPresented == true ? 1 : 0;
    GetDirect3D12().Submitted[GetDirect3D12().Slot] = value;
    GetDirect3D12().Slot = (GetDirect3D12().Slot + 1) % FRAME_COUNT;
    if (isSurfaceLost == true)
    {
        // The frame was submitted; only its created surface stops presenting.
        LogFailure("IDXGISwapChain::Present", result);
        static_cast<void>(LoseActiveSurface());
    }
    return FrameStatus::Ready;
}
ResourceStatus CreateShader(usize slot, const ShaderDescription& description, uint32) noexcept
{
    // The facade checked the container header; Direct3D 12 itself never sees the entry name.
    if (internal::HasDxilEntry({description.Dxil.GetDataOrNull(), description.Dxil.GetSize()},
                               description.Stage,
                               description.DxilEntry) == false)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: DXIL container does not hold the requested stage and entry");
        return ResourceStatus::Failed;
    }
    return GetDirect3D12().Shaders[slot].Bytecode.TryAddRange(description.Dxil) == true ? ResourceStatus::Ready
                                                                                        : ResourceStatus::Failed;
}
ResourceStatus CreateUniform(usize slot, const UniformDescription& description, uint32) noexcept
{
    Uniform& uniform = GetDirect3D12().Uniforms[slot];
    uniform.Size = description.Size;
    const D3D12_HEAP_PROPERTIES heap =
    {
        .Type = D3D12_HEAP_TYPE_UPLOAD,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 0,
        .VisibleNodeMask = 0,
    };
    const D3D12_RESOURCE_DESC bufferDescription =
    {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width =
            (description.Size + CONSTANT_BUFFER_ALIGNMENT - 1) / CONSTANT_BUFFER_ALIGNMENT * CONSTANT_BUFFER_ALIGNMENT,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    for (usize index = 0; index < FRAME_COUNT; ++index)
    {
        ID3D12Resource* bufferOrNull = nullptr;
        HRESULT result = GetDirect3D12().Device->CreateCommittedResource(&heap,
                                                                         D3D12_HEAP_FLAG_NONE,
                                                                         &bufferDescription,
                                                                         D3D12_RESOURCE_STATE_GENERIC_READ,
                                                                         nullptr,
                                                                         GetInterfaceId(bufferOrNull),
                                                                         GetInterfaceOutput(bufferOrNull));
        if (FAILED(result) == true)
        {
            LogFailure("uniform buffer creation", result);
            return ResourceStatus::Failed;
        }
        uniform.Buffers[index] = Adopt(bufferOrNull);
        // Upload heaps stay mapped for the resource's lifetime; the CPU never reads them.
        const D3D12_RANGE noRead = { .Begin = 0, .End = 0 };
        void* mappedOrNull = nullptr;
        result = bufferOrNull->Map(0, &noRead, &mappedOrNull);
        if (FAILED(result) == true || mappedOrNull == nullptr)
        {
            LogFailure("uniform buffer mapping", result);
            return ResourceStatus::Failed;
        }
        uniform.MappedOrNull[index] = static_cast<uint8*>(mappedOrNull);
    }
    return ResourceStatus::Ready;
}
ResourceStatus CreatePipeline(usize slot, const PipelineResources& resources, uint32) noexcept
{
    const Array<uint8>& vertex = GetDirect3D12().Shaders[resources.Vertex].Bytecode;
    const Array<uint8>& fragment = GetDirect3D12().Shaders[resources.Fragment].Bytecode;
    // Every slot holds valid enums; only slot 0 is bound.
    static_assert(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT == RENDER_TARGET_SLOTS);
    const D3D12_BLEND_DESC blend =
    {
        .AlphaToCoverageEnable = FALSE,
        .IndependentBlendEnable = FALSE,
        .RenderTarget =
            {
                OPAQUE_TARGET_BLEND,
                OPAQUE_TARGET_BLEND,
                OPAQUE_TARGET_BLEND,
                OPAQUE_TARGET_BLEND,
                OPAQUE_TARGET_BLEND,
                OPAQUE_TARGET_BLEND,
                OPAQUE_TARGET_BLEND,
                OPAQUE_TARGET_BLEND,
            },
    };
    // Neither depth nor stencil is tested; the operations only need valid values.
    const D3D12_DEPTH_STENCILOP_DESC keepStencil =
    {
        .StencilFailOp = D3D12_STENCIL_OP_KEEP,
        .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
        .StencilPassOp = D3D12_STENCIL_OP_KEEP,
        .StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS,
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description =
    {
        .pRootSignature = GetDirect3D12().RootSignature.Get(),
        .VS = { .pShaderBytecode = vertex.GetData(), .BytecodeLength = vertex.GetSize() },
        .PS = { .pShaderBytecode = fragment.GetData(), .BytecodeLength = fragment.GetSize() },
        .DS = { .pShaderBytecode = nullptr, .BytecodeLength = 0 },
        .HS = { .pShaderBytecode = nullptr, .BytecodeLength = 0 },
        .GS = { .pShaderBytecode = nullptr, .BytecodeLength = 0 },
        .StreamOutput =
        {
            .pSODeclaration = nullptr,
            .NumEntries = 0,
            .pBufferStrides = nullptr,
            .NumStrides = 0,
            .RasterizedStream = 0,
        },
        .BlendState = blend,
        .SampleMask = D3D12_DEFAULT_SAMPLE_MASK,
        // The triangle covers the target in either winding; no depth target is bound.
        .RasterizerState =
        {
            .FillMode = D3D12_FILL_MODE_SOLID,
            .CullMode = D3D12_CULL_MODE_NONE,
            .FrontCounterClockwise = FALSE,
            .DepthBias = D3D12_DEFAULT_DEPTH_BIAS,
            .DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
            .SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
            .DepthClipEnable = TRUE,
            .MultisampleEnable = FALSE,
            .AntialiasedLineEnable = FALSE,
            .ForcedSampleCount = 0,
            .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
        },
        .DepthStencilState =
        {
            .DepthEnable = FALSE,
            .DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO,
            .DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS,
            .StencilEnable = FALSE,
            .StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK,
            .StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK,
            .FrontFace = keepStencil,
            .BackFace = keepStencil,
        },
        .InputLayout = { .pInputElementDescs = nullptr, .NumElements = 0 },
        .IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED,
        .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
        .NumRenderTargets = 1,
        .RTVFormats = {GetSurfaceColorFormat()},
        .DSVFormat = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .NodeMask = 0,
        .CachedPSO = { .pCachedBlob = nullptr, .CachedBlobSizeInBytes = 0 },
        .Flags = D3D12_PIPELINE_STATE_FLAG_NONE,
    };
    ID3D12PipelineState* stateOrNull = nullptr;
    const HRESULT result = GetDirect3D12().Device->CreateGraphicsPipelineState(&description,
                                                                               GetInterfaceId(stateOrNull),
                                                                               GetInterfaceOutput(stateOrNull));
    if (FAILED(result) == true)
    {
        LogFailure("CreateGraphicsPipelineState", result);
        return ResourceStatus::Failed;
    }
    GetDirect3D12().Pipelines[slot] = { .State = Adopt(stateOrNull), .UniformSlot = resources.Uniform };
    return ResourceStatus::Ready;
}
void DestroyShader(usize slot) noexcept
{
    // Pipeline state objects keep their own compiled copy of the bytecode.
    Array<uint8> released;
    GetDirect3D12().Shaders[slot].Bytecode.Swap(released);
}
void DestroyUniform(usize slot) noexcept
{
    Uniform& uniform = GetDirect3D12().Uniforms[slot];
    // A frame in flight may still read the buffers.
    if (uniform.Size != 0)
    {
        static_cast<void>(WaitForIdle());
    }
    for (usize index = 0; index < FRAME_COUNT; ++index)
    {
        uniform.MappedOrNull[index] = nullptr;
        uniform.Buffers[index].Reset();
    }
    uniform.Size = 0;
}
void DestroyPipeline(usize slot) noexcept
{
    Pipeline& pipeline = GetDirect3D12().Pipelines[slot];
    if (pipeline.State.Get() != nullptr)
    {
        static_cast<void>(WaitForIdle());
    }
    pipeline.State.Reset();
    pipeline.UniformSlot = 0;
}
void UpdateUniform(usize slot, ludus::foundation::BorrowedView<const uint8> bytes) noexcept
{
    std::memcpy(GetDirect3D12().Uniforms[slot].Bytes, bytes.GetDataOrNull(), bytes.GetSize());
}
FrameInfo GetFrameInfo() noexcept
{
    return GetDirect3D12().Frame;
}
ResourceStatus Draw(usize slot) noexcept
{
    const Pipeline& pipeline = GetDirect3D12().Pipelines[slot];
    const Uniform& uniform = GetDirect3D12().Uniforms[pipeline.UniformSlot];
    // Begin waited for this slot's fence, so the GPU no longer reads its buffer.
    std::memcpy(uniform.MappedOrNull[GetDirect3D12().Slot], uniform.Bytes, uniform.Size);
    const D3D12_VIEWPORT viewport =
    {
        .TopLeftX = 0,
        .TopLeftY = 0,
        .Width = static_cast<float32>(GetDirect3D12().Frame.Width),
        .Height = static_cast<float32>(GetDirect3D12().Frame.Height),
        .MinDepth = 0,
        .MaxDepth = 1,
    };
    const D3D12_RECT scissor =
    {
        .left = 0,
        .top = 0,
        .right = static_cast<LONG>(GetDirect3D12().Frame.Width),
        .bottom = static_cast<LONG>(GetDirect3D12().Frame.Height),
    };
    GetDirect3D12().Commands->SetGraphicsRootSignature(GetDirect3D12().RootSignature.Get());
    GetDirect3D12().Commands->SetPipelineState(pipeline.State.Get());
    GetDirect3D12().Commands->SetGraphicsRootConstantBufferView(
        UNIFORM_ROOT_PARAMETER,
        uniform.Buffers[GetDirect3D12().Slot]->GetGPUVirtualAddress());
    GetDirect3D12().Commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    GetDirect3D12().Commands->RSSetViewports(1, &viewport);
    GetDirect3D12().Commands->RSSetScissorRects(1, &scissor);
    GetDirect3D12().Commands->DrawInstanced(FULLSCREEN_VERTEX_COUNT, 1, 0, 0);
    return ResourceStatus::Ready;
}
// Adapter identity is independent of optional timing and allocation telemetry.
bool AllocatorBlockBytes(uint64&) noexcept
{
    return false;
}
MeasurementAdapter TimingAdapter() noexcept
{
    MeasurementAdapter output;
    DXGI_ADAPTER_DESC1 description{};
    if (GetDirect3D12().Adapter.Get() == nullptr || FAILED(GetDirect3D12().Adapter->GetDesc1(&description)) == true)
    {
        return output;
    }
    // DXGI names hold 128 UTF-16 units; UTF-8 needs at most three bytes for each.
    constexpr usize ADAPTER_NAME_UNITS = sizeof(description.Description) / sizeof(description.Description[0]);
    char name[ADAPTER_NAME_UNITS * MAX_UTF8_BYTES_PER_UTF16_UNIT + 1]{};
    const int32 bytes = WideCharToMultiByte(CP_UTF8,
                                            0,
                                            description.Description,
                                            -1,
                                            name,
                                            static_cast<int32>(sizeof(name)),
                                            nullptr,
                                            nullptr);
    if (bytes > 0)
    {
        internal::CopyMeasurementName(name, static_cast<usize>(bytes), output.Name);
    }
    output.VendorId = description.VendorId;
    output.DeviceId = description.DeviceId;
    // DXGI flags the software rasterizer (WARP) explicitly; every other adapter is hardware.
    output.SoftwareKnown = true;
    output.Software =
        GetDirect3D12().IsSoftwareAdapter == true || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    return output;
}
#if defined(LUDUS_RHI_TEST_READBACK)
bool ReadHeadlessPixels(std::span<uint8> pixels) noexcept
{
    constexpr usize SURFACE_BYTES_PER_PIXEL = 4;
    // Byte offsets of the BGRA8 surface channels; readback returns them in RGBA order.
    constexpr usize SURFACE_BLUE = 0;
    constexpr usize SURFACE_GREEN = 1;
    constexpr usize SURFACE_RED = 2;
    constexpr usize SURFACE_ALPHA = 3;
    // Each refusal names its cause: a bare false left deferred WARP failures undiagnosable.
    if (GetDirect3D12().IsRendering == false || GetActiveSurface().SwapChain.Get() != nullptr ||
        GetDirect3D12().IsRecording == true || GetActiveSurface().Headless.Get() == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_RHI,
                        "Direct3D 12: headless readback needs an idle headless frame (rendering {}, swap chain {}, "
                        "recording {}, headless target {})",
                        GetDirect3D12().IsRendering,
                        GetActiveSurface().SwapChain.Get() != nullptr,
                        GetDirect3D12().IsRecording,
                        GetActiveSurface().Headless.Get() != nullptr);
        return false;
    }
    const D3D12_RESOURCE_DESC source = GetActiveSurface().Headless->GetDesc();
    const usize width = static_cast<usize>(source.Width);
    const usize height = source.Height;
    if (pixels.size() != width * height * SURFACE_BYTES_PER_PIXEL)
    {
        LUDUS_LOG_ERROR(LOG_RHI,
                        "Direct3D 12: headless readback received {} bytes for a {} x {} target",
                        pixels.size(),
                        width,
                        height);
        return false;
    }
    if (WaitForIdle() == false)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: headless readback could not drain the queue before copying");
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readbackBytes = 0;
    GetDirect3D12().Device->GetCopyableFootprints(&source, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBytes);
    const D3D12_HEAP_PROPERTIES heap =
    {
        .Type = D3D12_HEAP_TYPE_READBACK,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 0,
        .VisibleNodeMask = 0,
    };
    const D3D12_RESOURCE_DESC bufferDescription =
    {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = readbackBytes,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    ID3D12Resource* bufferOrNull = nullptr;
    const HRESULT created = GetDirect3D12().Device->CreateCommittedResource(&heap,
                                                                            D3D12_HEAP_FLAG_NONE,
                                                                            &bufferDescription,
                                                                            D3D12_RESOURCE_STATE_COPY_DEST,
                                                                            nullptr,
                                                                            GetInterfaceId(bufferOrNull),
                                                                            GetInterfaceOutput(bufferOrNull));
    if (FAILED(created) == true)
    {
        LogFailure("Headless readback buffer creation", created);
        return false;
    }
    ComOwner<ID3D12Resource> buffer = Adopt(bufferOrNull);
    ID3D12CommandAllocator* allocator = GetDirect3D12().Allocators[GetDirect3D12().Slot].Get();
    const HRESULT allocatorReset = allocator->Reset();
    if (FAILED(allocatorReset) == true)
    {
        LogFailure("Headless readback command allocator reset", allocatorReset);
        return false;
    }
    const HRESULT commandsReset = GetDirect3D12().Commands->Reset(allocator, nullptr);
    if (FAILED(commandsReset) == true)
    {
        LogFailure("Headless readback command list reset", commandsReset);
        return false;
    }
    const D3D12_RESOURCE_BARRIER toCopy =
        MakeTransition(GetActiveSurface().Headless.Get(),
                       { .Before = D3D12_RESOURCE_STATE_COMMON, .After = D3D12_RESOURCE_STATE_COPY_SOURCE });
    GetDirect3D12().Commands->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION copySource{};
    copySource.pResource = GetActiveSurface().Headless.Get();
    copySource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copySource.SubresourceIndex = 0;
    GetDirect3D12().Commands->CopyTextureRegion(&destination, 0, 0, 0, &copySource, nullptr);
    const D3D12_RESOURCE_BARRIER toCommon =
        MakeTransition(GetActiveSurface().Headless.Get(),
                       { .Before = D3D12_RESOURCE_STATE_COPY_SOURCE, .After = D3D12_RESOURCE_STATE_COMMON });
    GetDirect3D12().Commands->ResourceBarrier(1, &toCommon);
    const HRESULT closed = GetDirect3D12().Commands->Close();
    if (FAILED(closed) == true)
    {
        LogFailure("Headless readback command list close", closed);
        return false;
    }
    ID3D12CommandList* const lists[] = {GetDirect3D12().Commands.Get()};
    GetDirect3D12().Queue->ExecuteCommandLists(1, lists);
    if (WaitForIdle() == false)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: headless readback copy did not complete");
        return false;
    }
    void* mappedOrNull = nullptr;
    const D3D12_RANGE readRange = { .Begin = 0, .End = static_cast<SIZE_T>(readbackBytes) };
    const HRESULT mapped = buffer->Map(0, &readRange, &mappedOrNull);
    if (FAILED(mapped) == true || mappedOrNull == nullptr)
    {
        LogFailure("Headless readback buffer map", mapped);
        return false;
    }
    const uint8* bytes = static_cast<const uint8*>(mappedOrNull);
    const usize pitch = footprint.Footprint.RowPitch;
    for (usize y = 0; y < height; ++y)
    {
        for (usize x = 0; x < width; ++x)
        {
            const usize sourceIndex = footprint.Offset + y * pitch + x * SURFACE_BYTES_PER_PIXEL;
            const usize targetIndex = (y * width + x) * SURFACE_BYTES_PER_PIXEL;
            // BGRA8 storage, RGBA8 readback order (matches the Metal test readback).
            pixels[targetIndex] = bytes[sourceIndex + SURFACE_RED];
            pixels[targetIndex + 1] = bytes[sourceIndex + SURFACE_GREEN];
            pixels[targetIndex + 2] = bytes[sourceIndex + SURFACE_BLUE];
            pixels[targetIndex + SURFACE_ALPHA] = bytes[sourceIndex + SURFACE_ALPHA];
        }
    }
    const D3D12_RANGE writtenRange = { .Begin = 0, .End = 0 };
    buffer->Unmap(0, &writtenRange);
    return true;
}
#endif
} // namespace ludus::graphics::rhi::backend
