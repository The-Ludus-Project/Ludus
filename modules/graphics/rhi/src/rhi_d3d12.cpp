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

ComOwner<IDXGIFactory6> gFactory;
ComOwner<IDXGIAdapter1> gAdapter;
ComOwner<ID3D12Device> gDevice;
ComOwner<ID3D12CommandQueue> gQueue;
ComOwner<ID3D12Fence> gFence;
HANDLE gFenceEventOrNull = nullptr;
uint64 gFenceValue = 0;
uint64 gSubmitted[FRAME_COUNT]{};
ComOwner<ID3D12CommandAllocator> gAllocators[FRAME_COUNT];
ComOwner<ID3D12GraphicsCommandList> gCommands;
ComOwner<ID3D12DescriptorHeap> gTargetHeap;
uint32 gTargetDescriptorSize = 0;
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
StaticArray<Direct3D12Surface, MAX_DEVICE_SURFACES> gSurfaces{};
/// Surface routed SetTarget/Begin/End; see backend::SelectSurface.
usize gActiveSurface = 0;
Direct3D12Surface& GetActiveSurface() noexcept
{
    return gSurfaces[gActiveSurface];
}
/// Shared by every fullscreen pipeline; created with the device.
ComOwner<ID3D12RootSignature> gRootSignature;
Shader gShaders[internal::RESOURCE_CAPACITY];
Uniform gUniforms[internal::RESOURCE_CAPACITY];
Pipeline gPipelines[internal::RESOURCE_CAPACITY];
/// Color target of the open frame: a swap chain buffer or the headless texture.
ID3D12Resource* gSurfaceOrNull = nullptr;
D3D12_CPU_DESCRIPTOR_HANDLE gSurfaceDescriptor{};
FrameInfo gFrame{};
usize gSlot = 0;
uint32 gToken = 0;
bool isSoftwareAdapter = false;
bool isConnected = false;
bool isRendering = false;
bool isRecording = false;

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
    return gDevice.Get() != nullptr && gDevice->GetDeviceRemovedReason() != S_OK;
}

[[nodiscard]] bool IsDeviceLossResult(HRESULT result) noexcept
{
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
           result == DXGI_ERROR_DEVICE_HUNG || IsDeviceRemoved() == true;
}

[[nodiscard]] FrameStatus FailFrame(StartupError error) noexcept
{
    LogDeviceRemoval(gDevice.Get());
    DiscardRecording();
    gSurfaceOrNull = nullptr;
    gFrame = {};
    if (internal::Current(gToken) == true)
    {
        // The facade finalizes the session and calls Shutdown(); state is released there.
        internal::Fail(gToken, error);
    }
    else
    {
        ShutdownRendering();
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
    const uint64 completed = gFence->GetCompletedValue();
    if (completed == ~uint64{0})
    {
        LogDeviceRemoval(gDevice.Get());
        return false;
    }
    if (value == 0 || completed >= value)
    {
        return true;
    }
    const HRESULT armed = gFence->SetEventOnCompletion(value, gFenceEventOrNull);
    if (FAILED(armed) == true)
    {
        LogFailure("Fence completion event", armed);
        return false;
    }
    DWORD waited = WaitForSingleObject(gFenceEventOrNull, FENCE_TIMEOUT_MILLISECONDS);
    if (waited == WAIT_TIMEOUT && isSoftwareAdapter == true)
    {
        LUDUS_LOG_WARN(LOG_RHI,
                       "Direct3D 12: the software adapter is still executing after {} ms; waiting up to {} ms more",
                       static_cast<uint32>(FENCE_TIMEOUT_MILLISECONDS),
                       static_cast<uint32>(SOFTWARE_FENCE_EXTRA_TIMEOUT_MILLISECONDS));
        waited = WaitForSingleObject(gFenceEventOrNull, SOFTWARE_FENCE_EXTRA_TIMEOUT_MILLISECONDS);
    }
    if (waited != WAIT_OBJECT_0)
    {
        LUDUS_LOG_ERROR(LOG_RHI,
                        "Direct3D 12: fence value {} did not complete (wait result {:x}, completed {}); "
                        "the queue is hung or the frame is too heavy for this adapter",
                        value,
                        static_cast<uint32>(waited),
                        gFence->GetCompletedValue());
        return false;
    }
    const uint64 signaled = gFence->GetCompletedValue();
    return signaled >= value && signaled != ~uint64{0};
}

[[nodiscard]] bool SignalFence(uint64& outValue) noexcept
{
    const uint64 value = gFenceValue + 1;
    if (FAILED(gQueue->Signal(gFence.Get(), value)) == true)
    {
        return false;
    }
    gFenceValue = value;
    outValue = value;
    return true;
}

/// Waits for every submitted command list; false when the queue cannot be drained.
[[nodiscard]] bool WaitForIdle() noexcept
{
    if (gQueue.Get() == nullptr || gFence.Get() == nullptr)
    {
        return true;
    }
    if (IsDeviceRemoved() == true)
    {
        LogDeviceRemoval(gDevice.Get());
        return false;
    }
    uint64 value = 0;
    const bool isDrained = SignalFence(value) == true && WaitForFence(value) == true;
    if (isDrained == false)
    {
        LogDeviceRemoval(gDevice.Get());
    }
    return isDrained;
}

/// Descriptor `index` (a back buffer or HEADLESS_DESCRIPTOR) of the active surface.
[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE GetTargetDescriptor(uint32 index) noexcept
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = gTargetHeap->GetCPUDescriptorHandleForHeapStart();
    const usize surfaceIndex = gActiveSurface * SURFACE_DESCRIPTOR_COUNT + index;
    handle.ptr += surfaceIndex * gTargetDescriptorSize;
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
    gDevice = Adopt(deviceOrNull);
    static_cast<void>(gDevice->SetName(L"Ludus/Device"));
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
    const HRESULT found = gFactory->EnumAdapterByLuid(GetD3D12Luid(requested),
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
    gAdapter = Move(adapter);
    isSoftwareAdapter = isSoftware;
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
        const HRESULT found = gFactory->EnumAdapterByLuid(DecodeHostedAdapter(preferred),
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
        gAdapter = Move(adapter);
        isSoftwareAdapter = isSoftware;
        return true;
    }
    for (UINT index = 0; shouldUseWarp == false; ++index)
    {
        IDXGIAdapter1* adapterOrNull = nullptr;
        if (gFactory->EnumAdapterByGpuPreference(index,
                                                 DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                 GetInterfaceId(adapterOrNull),
                                                 GetInterfaceOutput(adapterOrNull)) == DXGI_ERROR_NOT_FOUND)
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
            gAdapter = Move(adapter);
            isSoftwareAdapter = false;
            return true;
        }
    }
    if (isHardwareRequired == true)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: enable a compatible physical adapter for hardware qualification");
        return false;
    }
    IDXGIAdapter1* warpOrNull = nullptr;
    if (FAILED(gFactory->EnumWarpAdapter(GetInterfaceId(warpOrNull), GetInterfaceOutput(warpOrNull))) == true ||
        warpOrNull == nullptr)
    {
        return false;
    }
    ComOwner<IDXGIAdapter1> warp = Adopt(warpOrNull);
    if (CreateDeviceOn(warp.Get()) == false)
    {
        return false;
    }
    gAdapter = Move(warp);
    isSoftwareAdapter = true;
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
    const HRESULT adapterResult = gFactory->EnumAdapterByLuid(device->GetAdapterLuid(),
                                                              GetInterfaceId(adapterOrNull),
                                                              GetInterfaceOutput(adapterOrNull));
    if (FAILED(adapterResult) == true || adapterOrNull == nullptr)
    {
        return false;
    }
    gAdapter = Adopt(adapterOrNull);
    device->AddRef();
    queue->AddRef();
    gDevice = Adopt(device);
    gQueue = Adopt(queue);
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
    if (FAILED(gDevice->CreateCommandQueue(&queueDescription,
                                           GetInterfaceId(queueOrNull),
                                           GetInterfaceOutput(queueOrNull))) == true)
    {
        return false;
    }
    gQueue = Adopt(queueOrNull);
    static_cast<void>(gQueue->SetName(L"Ludus/DirectQueue"));
    return true;
}

[[nodiscard]] bool CreateQueueObjects() noexcept
{
    if (gQueue.Get() == nullptr && CreateDirectQueue() == false)
    {
        return false;
    }
    ID3D12Fence* fenceOrNull = nullptr;
    if (FAILED(gDevice->CreateFence(0,
                                    D3D12_FENCE_FLAG_NONE,
                                    GetInterfaceId(fenceOrNull),
                                    GetInterfaceOutput(fenceOrNull))) == true)
    {
        return false;
    }
    gFence = Adopt(fenceOrNull);
    gFenceEventOrNull = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (gFenceEventOrNull == nullptr)
    {
        return false;
    }
    for (ComOwner<ID3D12CommandAllocator>& allocator : gAllocators)
    {
        ID3D12CommandAllocator* allocatorOrNull = nullptr;
        if (FAILED(gDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   GetInterfaceId(allocatorOrNull),
                                                   GetInterfaceOutput(allocatorOrNull))) == true)
        {
            return false;
        }
        allocator = Adopt(allocatorOrNull);
    }
    ID3D12GraphicsCommandList* commandsOrNull = nullptr;
    if (FAILED(gDevice->CreateCommandList(0,
                                          D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          gAllocators[0].Get(),
                                          nullptr,
                                          GetInterfaceId(commandsOrNull),
                                          GetInterfaceOutput(commandsOrNull))) == true)
    {
        return false;
    }
    gCommands = Adopt(commandsOrNull);
    static_cast<void>(gCommands->SetName(L"Ludus/FrameCommands"));
    // Command lists are created open; every frame resets it again.
    if (FAILED(gCommands->Close()) == true)
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
    if (FAILED(gDevice->CreateDescriptorHeap(&heapDescription,
                                             GetInterfaceId(heapOrNull),
                                             GetInterfaceOutput(heapOrNull))) == true)
    {
        return false;
    }
    gTargetHeap = Adopt(heapOrNull);
    gTargetDescriptorSize = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
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
    result = gDevice->CreateRootSignature(0,
                                          serialized->GetBufferPointer(),
                                          serialized->GetBufferSize(),
                                          GetInterfaceId(signatureOrNull),
                                          GetInterfaceOutput(signatureOrNull));
    if (FAILED(result) == true)
    {
        LogFailure("CreateRootSignature", result);
        return false;
    }
    gRootSignature = Adopt(signatureOrNull);
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
        gDevice->CreateRenderTargetView(bufferOrNull, nullptr, GetTargetDescriptor(index));
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
    HRESULT result =
        gFactory->CreateSwapChainForHwnd(gQueue.Get(), window, &description, nullptr, nullptr, &swapChainOrNull);
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
    static_cast<void>(gFactory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER));
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
    const HRESULT result = gDevice->CreateCommittedResource(&heap,
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
    gDevice->CreateRenderTargetView(textureOrNull, nullptr, GetTargetDescriptor(HEADLESS_DESCRIPTOR));
    return S_OK;
}

/// Count this surface's completed frames without waiting.
void RefreshSurface(Direct3D12Surface& inoutSurface) noexcept
{
    const uint64 completed = gFence.Get() != nullptr ? gFence->GetCompletedValue() : 0;
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
            internal::AreAdaptersEqual(preferred, EncodeHostedAdapter(gDevice->GetAdapterLuid())) == false)
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
    for (usize slot = 1; slot < gSurfaces.GetSize(); ++slot)
    {
        if (gSurfaces[slot].IsConnected == true)
        {
            gActiveSurface = slot;
            ReleaseActiveSurface();
        }
    }
    gActiveSurface = 0;
}
/// @brief A created surface stopped accepting frames: retire only its presentation
/// resources and report it lost. The device and other surfaces keep working; no frame opens.
[[nodiscard]] FrameStatus LoseActiveSurface() noexcept
{
    const usize slot = gActiveSurface;
    ReleaseActiveSurface();
    internal::LoseSurface(slot);
    return FrameStatus::Skipped;
}

void ReleaseDevice() noexcept
{
    ReleasePixMarkers();
    ReleaseCompute();
    gRootSignature.Reset();
    gCommands.Reset();
    for (ComOwner<ID3D12CommandAllocator>& allocator : gAllocators)
    {
        allocator.Reset();
    }
    gTargetHeap.Reset();
    gFence.Reset();
    if (gFenceEventOrNull != nullptr)
    {
        CloseHandle(gFenceEventOrNull);
        gFenceEventOrNull = nullptr;
    }
    gQueue.Reset();
    gDevice.Reset();
    gAdapter.Reset();
    gFactory.Reset();
    gFenceValue = 0;
    gTargetDescriptorSize = 0;
    isSoftwareAdapter = false;
    for (uint64& submitted : gSubmitted)
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
    if (isRecording == true)
    {
        CloseTimingScopes(*gCommands);
        static_cast<void>(gCommands->Close());
        isRecording = false;
    }
}

ID3D12GraphicsCommandList* GetCommandsOrNull() noexcept
{
    return isRecording == true ? gCommands.Get() : nullptr;
}
DXGI_FORMAT GetSurfaceColorFormat() noexcept
{
    return GetActiveSurface().Hosted.IsConnected() == true ? HOSTED_SURFACE_FORMAT : SURFACE_FORMAT;
}
Attachment GetSurfaceAttachment() noexcept
{
    return { .ColorOrNull = gSurfaceOrNull, .Color = gSurfaceDescriptor };
}
ID3D12Device* GetDeviceOrNull() noexcept
{
    return gDevice.Get();
}
ID3D12CommandQueue* GetQueueOrNull() noexcept
{
    return gQueue.Get();
}
ID3D12Fence* GetFenceOrNull() noexcept
{
    return gFence.Get();
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
    if (gFence.Get() == nullptr)
    {
        return 0;
    }
    const uint64 completed = gFence->GetCompletedValue();
    if (completed == ~uint64{0})
    {
        static_cast<void>(FailFrame(StartupError::DeviceLost));
        return 0;
    }
    return completed;
}
uint64 GetLastFence() noexcept
{
    return gFenceValue;
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
    gQueue->ExecuteCommandLists(1, lists);
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
    IDXGIFactory6* factory = gFactory.Get();
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
    if (gAdapter.Get() == nullptr || FAILED(gAdapter->GetDesc1(&description)) == true)
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
static bool InitializeDevice(const platform::HostedSurfaceChannel* channelOrNull) noexcept
{
    if (gDevice.Get() != nullptr)
    {
        return gCommands.Get() != nullptr;
    }
    const NativeDeviceDescription* importedOrNull = internal::NativeDeviceRequest();
    InitializePixMarkers();
    if (importedOrNull == nullptr)
    {
        ConfigureDred();
        EnableDebugLayer();
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
    gFactory = Adopt(factoryOrNull);
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
StartupError Start(const ApplicationInfo&, const WindowInfo& window, uint32 token, BackendSelection) noexcept
{
    if (IsValidWindow(window) == false)
    {
        return StartupError::InvalidWindow;
    }
    gToken = token;
    if (InitializeDevice(window.HostedSurfaceOrNull) == false)
    {
        return gFactory.Get() == nullptr  ? StartupError::InstanceUnavailable
               : gDevice.Get() == nullptr ? StartupError::AdapterUnavailable
                                          : StartupError::DeviceUnavailable;
    }
    if (ConnectWindow(window) == false)
    {
        return StartupError::SurfaceUnavailable;
    }
    if (InitializeRendering() == false)
    {
        return StartupError::RenderingUnavailable;
    }
    internal::Complete(token,
                       StartupError::None,
                       { .MaxFrameDimension2D = MAX_DIMENSION, .MaxUniformBufferSize = MAX_UNIFORM_BYTES });
    return StartupError::None;
}
bool Initialize(const ApplicationInfo&) noexcept
{
    return InitializeDevice(nullptr);
}
bool ConnectWindow(const WindowInfo& window) noexcept
{
    if (gDevice.Get() == nullptr || isConnected == true || IsValidWindow(window) == false ||
        window.Width > MAX_DIMENSION || window.Height > MAX_DIMENSION)
    {
        return false;
    }
    // The legacy and owned startup paths connect only the primary surface.
    gActiveSurface = 0;
    if (ConnectSurface(window) == false)
    {
        return false;
    }
    isConnected = true;
    return true;
}
bool InitializeRendering() noexcept
{
    isRendering = gCommands.Get() != nullptr && isConnected == true;
    return isRendering;
}
void ShutdownRendering() noexcept
{
    const bool isDrained = WaitForIdle();
    DiscardRecording();
    ReleaseTimings(isDrained);
    for (Direct3D12Surface& surface : gSurfaces)
    {
        RefreshSurface(surface);
        surface.Hosted.Disconnect();
        surface.Headless.Reset();
    }
    gSurfaceOrNull = nullptr;
    gFrame = {};
    gSlot = 0;
    isRendering = false;
}
void NativeInteropDetachSurface() noexcept
{
    ShutdownRendering();
    ReleaseCreatedSurfaces();
    ReleaseActiveSurface();
    isConnected = false;
}
void Shutdown() noexcept
{
    NativeInteropDetachSurface();
    ReleaseDevice();
    gSurfaces[0].RenderedFrames = 0;
    gSurfaces[0].PresentedFrames = 0;
    gToken = 0;
}
// R14c: this backend keeps one process session, so the facade admits a single device owner.
usize GetDeviceOwnerLimit() noexcept
{
    return 1;
}
usize AdditionalSurfaceLimit() noexcept
{
    return isRendering == true ? MAX_DEVICE_SURFACES - 1 : 0;
}
StartupError CreateSurface(usize slot, const WindowInfo& window) noexcept
{
    if (isRendering == false || isRecording == true || slot == 0 || slot >= gSurfaces.GetSize() ||
        gSurfaces[slot].IsConnected == true || IsValidWindow(window) == false || window.Width > MAX_DIMENSION ||
        window.Height > MAX_DIMENSION)
    {
        return StartupError::InvalidWindow;
    }
    // Pipelines target the primary's color format, so every surface of a session shares it.
    const bool isHosted = window.System == platform::WindowSystem::Hosted;
    if ((isHosted == true ? HOSTED_SURFACE_FORMAT : SURFACE_FORMAT) != GetSurfaceColorFormat())
    {
        return StartupError::BackendUnavailable;
    }
    const usize selected = gActiveSurface;
    gActiveSurface = slot;
    const bool isCreated = ConnectSurface(window);
    if (isCreated == false)
    {
        ReleaseActiveSurface();
    }
    gActiveSurface = selected;
    if (isCreated == false && IsDeviceRemoved() == true)
    {
        static_cast<void>(FailFrame(StartupError::DeviceLost));
    }
    return isCreated == true ? StartupError::None : StartupError::SurfaceUnavailable;
}
void DestroySurface(usize slot) noexcept
{
    if (slot == 0 || slot >= gSurfaces.GetSize() || gSurfaces[slot].IsConnected == false)
    {
        return;
    }
    const usize selected = gActiveSurface;
    gActiveSurface = slot;
    ReleaseActiveSurface();
    gActiveSurface = selected == slot ? 0 : selected;
}
void SelectSurface(usize slot) noexcept
{
    gActiveSurface = slot < gSurfaces.GetSize() ? slot : 0;
}
SurfaceCompletion GetSurfaceCompletion(usize slot) noexcept
{
    if (slot >= gSurfaces.GetSize())
    {
        return {};
    }
    RefreshSurface(gSurfaces[slot]);
    return { .RenderedFrames = gSurfaces[slot].RenderedFrames, .PresentedFrames = gSurfaces[slot].PresentedFrames };
}
FrameStatus SetTarget(const FrameTarget& target) noexcept
{
    if (isRecording == true)
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
    if (isRendering == false)
    {
        return FrameStatus::NotReady;
    }
    if (isRecording == true)
    {
        return FrameStatus::InvalidState;
    }
    gFrame = {};
    if (IsDeviceRemoved() == true)
    {
        return FailFrame("device", gDevice->GetDeviceRemovedReason());
    }
    if (gActiveSurface != 0 && GetActiveSurface().IsConnected == false)
    {
        // Lost earlier; the facade reports SurfaceLost.
        return FrameStatus::Skipped;
    }
    // A created surface is lost once its window is destroyed or leaves this thread.
    if (gActiveSurface != 0 && GetActiveSurface().WindowOrNull != nullptr &&
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
    if (WaitForFence(gSubmitted[gSlot]) == false)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: frame fence wait failed or timed out");
        return FailFrame(StartupError::DeviceLost);
    }
    gSubmitted[gSlot] = 0;
    HRESULT result = S_OK;
    if (GetActiveSurface().SwapChain.Get() != nullptr && GetActiveSurface().Target.ShouldPresentToSurface == true)
    {
        result = ResizeSwapChain(GetActiveSurface().Target.Width, GetActiveSurface().Target.Height);
        if (FAILED(result) == true && gActiveSurface != 0 && IsDeviceLossResult(result) == false)
        {
            LogFailure("ResizeBuffers", result);
            return LoseActiveSurface();
        }
        if (FAILED(result) == true)
        {
            return FailFrame("ResizeBuffers", result);
        }
        const uint32 index = GetActiveSurface().SwapChain->GetCurrentBackBufferIndex();
        gSurfaceOrNull = GetActiveSurface().BackBuffers[index].Get();
        gSurfaceDescriptor = GetTargetDescriptor(index);
    }
    else if (GetActiveSurface().Hosted.IsConnected() == true &&
             GetActiveSurface().Target.ShouldPresentToSurface == true)
    {
        const HostedAcquire acquired = GetActiveSurface().Hosted.Acquire(GetActiveSurface().Target.Width,
                                                                         GetActiveSurface().Target.Height,
                                                                         gSurfaceOrNull,
                                                                         gSurfaceDescriptor);
        if (acquired == HostedAcquire::Pending)
        {
            return FrameStatus::Skipped;
        }
        if (acquired == HostedAcquire::Failed && gActiveSurface != 0 && IsDeviceRemoved() == false)
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
        gSurfaceOrNull = GetActiveSurface().Headless.Get();
        gSurfaceDescriptor = GetTargetDescriptor(HEADLESS_DESCRIPTOR);
    }
    ID3D12CommandAllocator* allocator = gAllocators[gSlot].Get();
    result = allocator->Reset();
    if (FAILED(result) == true)
    {
        return FailFrame("ID3D12CommandAllocator::Reset", result);
    }
    result = gCommands->Reset(allocator, nullptr);
    if (FAILED(result) == true)
    {
        return FailFrame("ID3D12GraphicsCommandList::Reset", result);
    }
    isRecording = true;
    ResetBufferStates();
    RasterEndPass();
    BeginTimingFrame(*gCommands);
    const D3D12_RESOURCE_BARRIER toTarget =
        MakeTransition(gSurfaceOrNull,
                       { .Before = D3D12_RESOURCE_STATE_PRESENT, .After = D3D12_RESOURCE_STATE_RENDER_TARGET });
    gCommands->ResourceBarrier(1, &toTarget);
    const float32 color[CLEAR_COLOR_CHANNELS] = {
        static_cast<float32>(GetActiveSurface().Target.Red),
        static_cast<float32>(GetActiveSurface().Target.Green),
        static_cast<float32>(GetActiveSurface().Target.Blue),
        static_cast<float32>(GetActiveSurface().Target.Alpha),
    };
    gCommands->ClearRenderTargetView(gSurfaceDescriptor, color, 0, nullptr);
    gCommands->OMSetRenderTargets(1, &gSurfaceDescriptor, FALSE, nullptr);
    const D3D12_RESOURCE_DESC surface = gSurfaceOrNull->GetDesc();
    // Extent comes from the acquired target, bounded by the frame target above.
    gFrame = {static_cast<uint32>(surface.Width), surface.Height, SurfaceEncoding::Unorm};
    return FrameStatus::Ready;
}
FrameStatus End() noexcept
{
    if (isRecording == false)
    {
        return FrameStatus::InvalidState;
    }
    RasterEndPass();
    const D3D12_RESOURCE_BARRIER toPresent =
        MakeTransition(gSurfaceOrNull,
                       { .Before = D3D12_RESOURCE_STATE_RENDER_TARGET, .After = D3D12_RESOURCE_STATE_PRESENT });
    gCommands->ResourceBarrier(1, &toPresent);
    EndTimingFrame(*gCommands);
    HRESULT result = gCommands->Close();
    isRecording = false;
    gSurfaceOrNull = nullptr;
    gFrame = {};
    if (FAILED(result) == true)
    {
        return FailFrame("ID3D12GraphicsCommandList::Close", result);
    }
    ID3D12CommandList* const lists[] = {gCommands.Get()};
    MarkTimingsQueued();
    gQueue->ExecuteCommandLists(1, lists);
    bool isPresented = false;
    bool isSurfaceLost = false;
    if (GetActiveSurface().SwapChain.Get() != nullptr && GetActiveSurface().Target.ShouldPresentToSurface == true)
    {
        // Synchronize to vertical blank; DXGI_STATUS_OCCLUDED is a success code.
        result = GetActiveSurface().SwapChain->Present(1, 0);
        isSurfaceLost = FAILED(result) == true && gActiveSurface != 0 && IsDeviceLossResult(result) == false;
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
    gSubmitted[gSlot] = value;
    gSlot = (gSlot + 1) % FRAME_COUNT;
    if (isSurfaceLost == true)
    {
        // The frame was submitted; only its created surface stops presenting.
        LogFailure("IDXGISwapChain::Present", result);
        static_cast<void>(LoseActiveSurface());
    }
    return FrameStatus::Ready;
}
bool BeginFrame() noexcept
{
    return Begin() == FrameStatus::Ready;
}
bool EndFrame() noexcept
{
    return End() == FrameStatus::Ready;
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
    return gShaders[slot].Bytecode.TryAddRange(description.Dxil) == true ? ResourceStatus::Ready
                                                                         : ResourceStatus::Failed;
}
ResourceStatus CreateUniform(usize slot, const UniformDescription& description, uint32) noexcept
{
    Uniform& uniform = gUniforms[slot];
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
        HRESULT result = gDevice->CreateCommittedResource(&heap,
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
    const Array<uint8>& vertex = gShaders[resources.Vertex].Bytecode;
    const Array<uint8>& fragment = gShaders[resources.Fragment].Bytecode;
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
        .pRootSignature = gRootSignature.Get(),
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
    const HRESULT result = gDevice->CreateGraphicsPipelineState(&description,
                                                                GetInterfaceId(stateOrNull),
                                                                GetInterfaceOutput(stateOrNull));
    if (FAILED(result) == true)
    {
        LogFailure("CreateGraphicsPipelineState", result);
        return ResourceStatus::Failed;
    }
    gPipelines[slot] = { .State = Adopt(stateOrNull), .UniformSlot = resources.Uniform };
    return ResourceStatus::Ready;
}
void DestroyShader(usize slot) noexcept
{
    // Pipeline state objects keep their own compiled copy of the bytecode.
    Array<uint8> released;
    gShaders[slot].Bytecode.Swap(released);
}
void DestroyUniform(usize slot) noexcept
{
    Uniform& uniform = gUniforms[slot];
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
    Pipeline& pipeline = gPipelines[slot];
    if (pipeline.State.Get() != nullptr)
    {
        static_cast<void>(WaitForIdle());
    }
    pipeline.State.Reset();
    pipeline.UniformSlot = 0;
}
void UpdateUniform(usize slot, ludus::foundation::BorrowedView<const uint8> bytes) noexcept
{
    std::memcpy(gUniforms[slot].Bytes, bytes.GetDataOrNull(), bytes.GetSize());
}
FrameInfo GetFrameInfo() noexcept
{
    return gFrame;
}
ResourceStatus Draw(usize slot) noexcept
{
    const Pipeline& pipeline = gPipelines[slot];
    const Uniform& uniform = gUniforms[pipeline.UniformSlot];
    // Begin waited for this slot's fence, so the GPU no longer reads its buffer.
    std::memcpy(uniform.MappedOrNull[gSlot], uniform.Bytes, uniform.Size);
    const D3D12_VIEWPORT viewport =
    {
        .TopLeftX = 0,
        .TopLeftY = 0,
        .Width = static_cast<float32>(gFrame.Width),
        .Height = static_cast<float32>(gFrame.Height),
        .MinDepth = 0,
        .MaxDepth = 1,
    };
    const D3D12_RECT scissor =
    {
        .left = 0,
        .top = 0,
        .right = static_cast<LONG>(gFrame.Width),
        .bottom = static_cast<LONG>(gFrame.Height),
    };
    gCommands->SetGraphicsRootSignature(gRootSignature.Get());
    gCommands->SetPipelineState(pipeline.State.Get());
    gCommands->SetGraphicsRootConstantBufferView(UNIFORM_ROOT_PARAMETER,
                                                 uniform.Buffers[gSlot]->GetGPUVirtualAddress());
    gCommands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gCommands->RSSetViewports(1, &viewport);
    gCommands->RSSetScissorRects(1, &scissor);
    gCommands->DrawInstanced(FULLSCREEN_VERTEX_COUNT, 1, 0, 0);
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
    if (gAdapter.Get() == nullptr || FAILED(gAdapter->GetDesc1(&description)) == true)
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
    output.Software = isSoftwareAdapter == true || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
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
    if (isRendering == false || GetActiveSurface().SwapChain.Get() != nullptr || isRecording == true ||
        GetActiveSurface().Headless.Get() == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_RHI,
                        "Direct3D 12: headless readback needs an idle headless frame (rendering {}, swap chain {}, "
                        "recording {}, headless target {})",
                        isRendering,
                        GetActiveSurface().SwapChain.Get() != nullptr,
                        isRecording,
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
    gDevice->GetCopyableFootprints(&source, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBytes);
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
    const HRESULT created = gDevice->CreateCommittedResource(&heap,
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
    ID3D12CommandAllocator* allocator = gAllocators[gSlot].Get();
    const HRESULT allocatorReset = allocator->Reset();
    if (FAILED(allocatorReset) == true)
    {
        LogFailure("Headless readback command allocator reset", allocatorReset);
        return false;
    }
    const HRESULT commandsReset = gCommands->Reset(allocator, nullptr);
    if (FAILED(commandsReset) == true)
    {
        LogFailure("Headless readback command list reset", commandsReset);
        return false;
    }
    const D3D12_RESOURCE_BARRIER toCopy =
        MakeTransition(GetActiveSurface().Headless.Get(),
                       { .Before = D3D12_RESOURCE_STATE_COMMON, .After = D3D12_RESOURCE_STATE_COPY_SOURCE });
    gCommands->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION copySource{};
    copySource.pResource = GetActiveSurface().Headless.Get();
    copySource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copySource.SubresourceIndex = 0;
    gCommands->CopyTextureRegion(&destination, 0, 0, 0, &copySource, nullptr);
    const D3D12_RESOURCE_BARRIER toCommon =
        MakeTransition(GetActiveSurface().Headless.Get(),
                       { .Before = D3D12_RESOURCE_STATE_COPY_SOURCE, .After = D3D12_RESOURCE_STATE_COMMON });
    gCommands->ResourceBarrier(1, &toCommon);
    const HRESULT closed = gCommands->Close();
    if (FAILED(closed) == true)
    {
        LogFailure("Headless readback command list close", closed);
        return false;
    }
    ID3D12CommandList* const lists[] = {gCommands.Get()};
    gQueue->ExecuteCommandLists(1, lists);
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
