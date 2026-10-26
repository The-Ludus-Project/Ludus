#include "internal/backend.h"
#include "internal/d3d12_device.h"
#include "internal/dxil_container.h"
#include "internal/lifecycle.h"
#include "internal/measurement.h"
#include "internal/native_interop.h"
#include "internal/resources.h"
#if defined(LUDUS_RHI_TEST_READBACK)
#    include "internal/readback.h"
#endif

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
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
/// Descriptors: one per swap chain buffer, then the headless target.
constexpr uint32 HEADLESS_DESCRIPTOR = FRAME_COUNT;
constexpr uint32 TARGET_DESCRIPTOR_COUNT = FRAME_COUNT + 1;
/// Bound on a CPU wait for GPU work. A healthy device finishes a clear far sooner;
/// a removed device signals every fence, so a timeout means the queue is hung.
constexpr DWORD FENCE_TIMEOUT_MILLISECONDS = 10000;
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
ComOwner<IDXGISwapChain3> gSwapChain;
ComOwner<ID3D12Resource> gBackBuffers[FRAME_COUNT];
uint32 gSwapWidth = 0;
uint32 gSwapHeight = 0;
ComOwner<ID3D12Resource> gHeadless;
/// Shared by every fullscreen pipeline; created with the device.
ComOwner<ID3D12RootSignature> gRootSignature;
Shader gShaders[internal::RESOURCE_CAPACITY];
Uniform gUniforms[internal::RESOURCE_CAPACITY];
Pipeline gPipelines[internal::RESOURCE_CAPACITY];
HWND gWindowOrNull = nullptr;
/// Color target of the open frame: a swap chain buffer or the headless texture.
ID3D12Resource* gSurfaceOrNull = nullptr;
D3D12_CPU_DESCRIPTOR_HANDLE gSurfaceDescriptor{};
FrameTarget gTarget{};
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
    // Hosted surfaces have no Windows transport yet.
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
    isRecording = false;
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
    if (value == 0 || gFence->GetCompletedValue() >= value)
    {
        return true;
    }
    if (FAILED(gFence->SetEventOnCompletion(value, gFenceEventOrNull)) == true)
    {
        return false;
    }
    return WaitForSingleObject(gFenceEventOrNull, FENCE_TIMEOUT_MILLISECONDS) == WAIT_OBJECT_0;
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
    uint64 value = 0;
    return SignalFence(value) == true && WaitForFence(value) == true;
}

[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE GetTargetDescriptor(uint32 index) noexcept
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = gTargetHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<usize>(index) * gTargetDescriptorSize;
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
    // The debug layer ships with the optional Graphics Tools feature; continue without it.
    ID3D12Debug* debugOrNull = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(GetInterfaceId(debugOrNull), GetInterfaceOutput(debugOrNull))) == true &&
        debugOrNull != nullptr)
    {
        debugOrNull->EnableDebugLayer();
        debugOrNull->Release();
    }
    else
    {
        LUDUS_LOG_INFO(LOG_RHI, "Direct3D 12: debug layer unavailable (install the Graphics Tools feature)");
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
    return true;
}

/// Prefers the highest-performance hardware adapter; falls back to WARP so headless
/// hosts without a GPU (CI runners) still run the same backend.
[[nodiscard]] bool SelectAdapter() noexcept
{
    for (UINT index = 0;; ++index)
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
    LUDUS_LOG_INFO(LOG_RHI, "Direct3D 12: no hardware adapter created a device; using WARP");
    return true;
}

[[nodiscard]] bool CreateQueueObjects() noexcept
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
    for (ComOwner<ID3D12Resource>& buffer : gBackBuffers)
    {
        buffer.Reset();
    }
}

[[nodiscard]] HRESULT AcquireBackBuffers() noexcept
{
    for (uint32 index = 0; index < FRAME_COUNT; ++index)
    {
        ID3D12Resource* bufferOrNull = nullptr;
        const HRESULT result =
            gSwapChain->GetBuffer(index, GetInterfaceId(bufferOrNull), GetInterfaceOutput(bufferOrNull));
        if (FAILED(result) == true)
        {
            ReleaseBackBuffers();
            return result;
        }
        gBackBuffers[index] = Adopt(bufferOrNull);
        gDevice->CreateRenderTargetView(bufferOrNull, nullptr, GetTargetDescriptor(index));
    }
    DXGI_SWAP_CHAIN_DESC1 description{};
    const HRESULT result = gSwapChain->GetDesc1(&description);
    if (FAILED(result) == true)
    {
        ReleaseBackBuffers();
        return result;
    }
    gSwapWidth = description.Width;
    gSwapHeight = description.Height;
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
    gSwapChain = Adopt(swapChain3OrNull);
    // Ludus owns fullscreen policy; DXGI must not toggle it on Alt+Enter.
    static_cast<void>(gFactory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER));
    result = AcquireBackBuffers();
    if (FAILED(result) == true)
    {
        LogFailure("swap chain buffer acquisition", result);
        gSwapChain.Reset();
        return false;
    }
    return true;
}

/// Matches the swap chain to the frame target; DXGI stretches it to the client area.
[[nodiscard]] HRESULT ResizeSwapChain(uint32 width, uint32 height) noexcept
{
    if (width == gSwapWidth && height == gSwapHeight)
    {
        return S_OK;
    }
    // ResizeBuffers requires every reference to the old buffers to be released.
    if (WaitForIdle() == false)
    {
        return DXGI_ERROR_DEVICE_HUNG;
    }
    ReleaseBackBuffers();
    const HRESULT result = gSwapChain->ResizeBuffers(static_cast<UINT>(FRAME_COUNT), width, height, SURFACE_FORMAT, 0);
    if (FAILED(result) == true)
    {
        return result;
    }
    return AcquireBackBuffers();
}

[[nodiscard]] HRESULT PrepareHeadless(uint32 width, uint32 height) noexcept
{
    if (gHeadless.Get() != nullptr)
    {
        const D3D12_RESOURCE_DESC current = gHeadless->GetDesc();
        if (current.Width == width && current.Height == height)
        {
            return S_OK;
        }
        // The previous target may still be in use by an in-flight frame.
        if (WaitForIdle() == false)
        {
            return DXGI_ERROR_DEVICE_HUNG;
        }
        gHeadless.Reset();
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
        .Format = SURFACE_FORMAT,
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
    gHeadless = Adopt(textureOrNull);
    gDevice->CreateRenderTargetView(textureOrNull, nullptr, GetTargetDescriptor(HEADLESS_DESCRIPTOR));
    return S_OK;
}

void ReleaseDevice() noexcept
{
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
ID3D12Device* GetDeviceOrNull() noexcept
{
    return gDevice.Get();
}
ID3D12Fence* GetFenceOrNull() noexcept
{
    return gFence.Get();
}
uint64 GetCompletedFence() noexcept
{
    return gFence.Get() == nullptr ? 0 : gFence->GetCompletedValue();
}
uint64 GetLastFence() noexcept
{
    return gFenceValue;
}
bool WaitForQueue() noexcept
{
    return WaitForIdle();
}
bool SubmitCreation(ID3D12CommandList* commands, uint64& outFence) noexcept
{
    LUDUS_ASSERT(commands != nullptr);
    ID3D12CommandList* const lists[] = {commands};
    gQueue->ExecuteCommandLists(1, lists);
    // A failed signal cannot prove completion. Retain the creation backing until
    // shutdown instead of reclaiming it through the failed publication path.
    outFence = ~uint64{0};
    return SignalFence(outFence);
}
} // namespace d3d12

Backend Kind() noexcept
{
    return Backend::Direct3D12;
}
bool Supports(BackendSelection selection) noexcept
{
    return selection == BackendSelection::Auto;
}
StartupError Start(const ApplicationInfo& app, const WindowInfo& window, uint32 token, BackendSelection) noexcept
{
    if (IsValidWindow(window) == false)
    {
        return StartupError::InvalidWindow;
    }
    gToken = token;
    if (backend::Initialize(app) == false)
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
    if (gDevice.Get() != nullptr)
    {
        return gCommands.Get() != nullptr;
    }
    // Native device import is a Metal/Vulkan interop contract.
    if (internal::NativeDeviceRequest() != nullptr)
    {
        return false;
    }
    EnableDebugLayer();
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
    if (SelectAdapter() == false)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: no adapter supports feature level 11_0");
        return false;
    }
    if (CreateQueueObjects() == false || CreateRootSignature() == false)
    {
        // The device stays until Shutdown() so Start reports DeviceUnavailable.
        LUDUS_LOG_ERROR(LOG_RHI, "Direct3D 12: creating the queue, fence or command objects failed");
        return false;
    }
    return true;
}
bool ConnectWindow(const WindowInfo& window) noexcept
{
    if (gDevice.Get() == nullptr || isConnected == true || IsValidWindow(window) == false ||
        window.Width > MAX_DIMENSION || window.Height > MAX_DIMENSION)
    {
        return false;
    }
    if (window.System == platform::WindowSystem::Win32)
    {
        const HWND handle = static_cast<HWND>(window.Win32WindowOrNull);
        if (CreateSwapChain(handle, window.Width, window.Height) == false)
        {
            return false;
        }
        gWindowOrNull = handle;
    }
    gTarget = { .Width = window.Width, .Height = window.Height };
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
    static_cast<void>(WaitForIdle());
    if (isRecording == true)
    {
        static_cast<void>(gCommands->Close());
        isRecording = false;
    }
    gHeadless.Reset();
    gSurfaceOrNull = nullptr;
    gFrame = {};
    gSlot = 0;
    isRendering = false;
}
void Shutdown() noexcept
{
    ShutdownRendering();
    ReleaseBackBuffers();
    if (gSwapChain.Get() != nullptr)
    {
        // Flip-model swap chains must not be released while fullscreen.
        static_cast<void>(gSwapChain->SetFullscreenState(FALSE, nullptr));
    }
    gSwapChain.Reset();
    gSwapWidth = 0;
    gSwapHeight = 0;
    gWindowOrNull = nullptr;
    ReleaseDevice();
    gTarget = {};
    isConnected = false;
    gToken = 0;
}
FrameStatus SetTarget(const FrameTarget& target) noexcept
{
    if (target.ShouldPresentToSurface == false)
    {
        return FrameStatus::Unsupported;
    }
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
    gTarget = target;
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
    if (gTarget.Width == 0 || gTarget.Height == 0 ||
        (gWindowOrNull != nullptr && (IsIconic(gWindowOrNull) != FALSE || IsWindowVisible(gWindowOrNull) == FALSE)))
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
    if (gSwapChain.Get() != nullptr)
    {
        result = ResizeSwapChain(gTarget.Width, gTarget.Height);
        if (FAILED(result) == true)
        {
            return FailFrame("ResizeBuffers", result);
        }
        const uint32 index = gSwapChain->GetCurrentBackBufferIndex();
        gSurfaceOrNull = gBackBuffers[index].Get();
        gSurfaceDescriptor = GetTargetDescriptor(index);
    }
    else
    {
        result = PrepareHeadless(gTarget.Width, gTarget.Height);
        if (FAILED(result) == true)
        {
            return FailFrame("headless target creation", result);
        }
        gSurfaceOrNull = gHeadless.Get();
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
    const D3D12_RESOURCE_BARRIER toTarget =
        MakeTransition(gSurfaceOrNull,
                       { .Before = D3D12_RESOURCE_STATE_PRESENT, .After = D3D12_RESOURCE_STATE_RENDER_TARGET });
    gCommands->ResourceBarrier(1, &toTarget);
    const float32 color[CLEAR_COLOR_CHANNELS] = {
        static_cast<float32>(gTarget.Red),
        static_cast<float32>(gTarget.Green),
        static_cast<float32>(gTarget.Blue),
        static_cast<float32>(gTarget.Alpha),
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
    const D3D12_RESOURCE_BARRIER toPresent =
        MakeTransition(gSurfaceOrNull,
                       { .Before = D3D12_RESOURCE_STATE_RENDER_TARGET, .After = D3D12_RESOURCE_STATE_PRESENT });
    gCommands->ResourceBarrier(1, &toPresent);
    HRESULT result = gCommands->Close();
    isRecording = false;
    gSurfaceOrNull = nullptr;
    gFrame = {};
    if (FAILED(result) == true)
    {
        return FailFrame("ID3D12GraphicsCommandList::Close", result);
    }
    ID3D12CommandList* const lists[] = {gCommands.Get()};
    gQueue->ExecuteCommandLists(1, lists);
    if (gSwapChain.Get() != nullptr)
    {
        // Synchronize to vertical blank; DXGI_STATUS_OCCLUDED is a success code.
        result = gSwapChain->Present(1, 0);
        if (FAILED(result) == true)
        {
            return FailFrame("IDXGISwapChain::Present", result);
        }
    }
    uint64 value = 0;
    if (SignalFence(value) == false)
    {
        return FailFrame(StartupError::DeviceLost);
    }
    gSubmitted[gSlot] = value;
    gSlot = (gSlot + 1) % FRAME_COUNT;
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
    if (internal::HasDxilEntry(description.Dxil, description.Stage, description.DxilEntry) == false)
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
        .RTVFormats = {SURFACE_FORMAT},
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
void UpdateUniform(usize slot, std::span<const uint8> bytes) noexcept
{
    std::memcpy(gUniforms[slot].Bytes, bytes.data(), bytes.size());
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
// GPU timing and allocation telemetry are not implemented yet; adapter identity is.
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
RasterStatus TimingArm() noexcept
{
    return RasterStatus::Unsupported;
}
GpuTiming TimingPoll() noexcept
{
    return { .State = GpuTimingState::Unavailable };
}
void TimingReset() noexcept {}
bool RasterAllocationBytes(internal::RasterKind, usize, uint64&) noexcept
{
    return false;
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
    if (isRendering == false || gSwapChain.Get() != nullptr || isRecording == true || gHeadless.Get() == nullptr)
    {
        return false;
    }
    const D3D12_RESOURCE_DESC source = gHeadless->GetDesc();
    const usize width = static_cast<usize>(source.Width);
    const usize height = source.Height;
    if (pixels.size() != width * height * SURFACE_BYTES_PER_PIXEL || WaitForIdle() == false)
    {
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
    if (FAILED(gDevice->CreateCommittedResource(&heap,
                                                D3D12_HEAP_FLAG_NONE,
                                                &bufferDescription,
                                                D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr,
                                                GetInterfaceId(bufferOrNull),
                                                GetInterfaceOutput(bufferOrNull))) == true)
    {
        return false;
    }
    ComOwner<ID3D12Resource> buffer = Adopt(bufferOrNull);
    ID3D12CommandAllocator* allocator = gAllocators[gSlot].Get();
    if (FAILED(allocator->Reset()) == true || FAILED(gCommands->Reset(allocator, nullptr)) == true)
    {
        return false;
    }
    const D3D12_RESOURCE_BARRIER toCopy =
        MakeTransition(gHeadless.Get(),
                       { .Before = D3D12_RESOURCE_STATE_COMMON, .After = D3D12_RESOURCE_STATE_COPY_SOURCE });
    gCommands->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION copySource{};
    copySource.pResource = gHeadless.Get();
    copySource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copySource.SubresourceIndex = 0;
    gCommands->CopyTextureRegion(&destination, 0, 0, 0, &copySource, nullptr);
    const D3D12_RESOURCE_BARRIER toCommon =
        MakeTransition(gHeadless.Get(),
                       { .Before = D3D12_RESOURCE_STATE_COPY_SOURCE, .After = D3D12_RESOURCE_STATE_COMMON });
    gCommands->ResourceBarrier(1, &toCommon);
    if (FAILED(gCommands->Close()) == true)
    {
        return false;
    }
    ID3D12CommandList* const lists[] = {gCommands.Get()};
    gQueue->ExecuteCommandLists(1, lists);
    if (WaitForIdle() == false)
    {
        return false;
    }
    void* mappedOrNull = nullptr;
    const D3D12_RANGE readRange = { .Begin = 0, .End = static_cast<SIZE_T>(readbackBytes) };
    if (FAILED(buffer->Map(0, &readRange, &mappedOrNull)) == true || mappedOrNull == nullptr)
    {
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
