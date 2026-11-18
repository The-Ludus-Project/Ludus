#include "internal/d3d12_diagnostics.h"
#include "internal/d3d12_raster.h"
#include "internal/owner.h"
#include "internal/raster.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/compute.h>

namespace ludus::graphics::rhi::backend
{
namespace
{
using namespace foundation;
using namespace internal;
using namespace d3d12;

struct ComputePipeline final
{
    ComOwner<ID3D12PipelineState> State;
    usize Layout = RASTER_INVALID;
};
/// Compute pipelines and indirect command signatures of one device owner (R14c).
struct ComputeD3D12State final
{
    StorageTable<ComputePipeline> Pipelines;
    ComOwner<ID3D12CommandSignature> IndexedIndirectSignature;
    ComOwner<ID3D12CommandSignature> DrawIndirectSignature;
    ComOwner<ID3D12CommandSignature> DispatchIndirectSignature;
};
/// One record per device owner slot (R14c); see internal/owner.h.
foundation::StaticArray<ComputeD3D12State, MAX_DEVICE_OWNERS> gComputeD3D12States{};
/// ComputeD3D12State of the bound owner slot.
ComputeD3D12State& GetComputeD3D12() noexcept
{
    return gComputeD3D12States[internal::GetBoundOwnerSlot()];
}

// One-argument signatures change no root arguments, so they need no root signature.
ComOwner<ID3D12CommandSignature>
CreateIndirectSignature(ID3D12Device* device, D3D12_INDIRECT_ARGUMENT_TYPE type, UINT byteStride) noexcept
{
    const D3D12_INDIRECT_ARGUMENT_DESC argument{ .Type = type };
    const D3D12_COMMAND_SIGNATURE_DESC description
    {
        .ByteStride = byteStride,
        .NumArgumentDescs = 1,
        .pArgumentDescs = &argument,
    };
    ID3D12CommandSignature* signatureOrNull = nullptr;
    LUDUS_ASSERT(device != nullptr);
    const HRESULT result = device->CreateCommandSignature(&description,
                                                          nullptr,
                                                          GetInterfaceId(signatureOrNull),
                                                          GetInterfaceOutput(signatureOrNull));
    return SUCCEEDED(result) == true ? ComOwner<ID3D12CommandSignature>(signatureOrNull)
                                     : ComOwner<ID3D12CommandSignature>();
}
} // namespace

ComputeCapabilities ComputeLimits() noexcept
{
    if (GetComputeD3D12().IndexedIndirectSignature.Get() == nullptr)
    {
        return {};
    }
    constexpr usize STORAGE_OFFSET_ALIGNMENT_BYTES = 256;
    constexpr uint32 MAX_WORKGROUP_INVOCATIONS = 256;
    constexpr uint32 MAX_DISPATCH_GROUPS = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
    return
    {
        .MaxStorageRange = RasterLimits().MaxBufferSize,
        .StorageOffsetAlignment = STORAGE_OFFSET_ALIGNMENT_BYTES,
        .MaxWorkgroupSize = {MAX_WORKGROUP_INVOCATIONS, MAX_WORKGROUP_INVOCATIONS, D3D12_CS_THREAD_GROUP_MAX_Z},
        .MaxWorkgroupInvocations = MAX_WORKGROUP_INVOCATIONS,
        .MaxDispatch = {MAX_DISPATCH_GROUPS, MAX_DISPATCH_GROUPS, MAX_DISPATCH_GROUPS},
        .MaxStorageBindings = RASTER_BINDING_COUNT,
    };
}

// Thanks to Microsoft, "Resource Binding Tier" and "Hardware Feature Levels": feature
// level 11 devices expose UAVs to every stage, so pixel writes and typed UAV texture
// stores of RGBA8/RGBA16F need no optional feature here.
RasterStorageCapabilities RasterStorageLimits() noexcept
{
    const ComputeCapabilities compute = ComputeLimits();
    if (compute.MaxStorageRange == 0)
    {
        return {};
    }
    return
    {
        .MaxVertexStorageBuffers = compute.MaxStorageBindings,
        .MaxFragmentStorageBuffers = compute.MaxStorageBindings,
        .HasFragmentStorageWrites = true,
        .MaxFragmentStorageTextures = RASTER_BINDING_COUNT,
        .MaxComputeStorageTextures = RASTER_BINDING_COUNT,
    };
}

// Thanks to Microsoft, "Indirect drawing" (Direct3D 12 programming guide) and
// ID3D12GraphicsCommandList::ExecuteIndirect: D3D12_DRAW_ARGUMENTS, D3D12_DRAW_INDEXED_ARGUMENTS
// and D3D12_DISPATCH_ARGUMENTS match the engine ABI, offsets are four-byte aligned and every
// start location is core. SV_InstanceID excludes StartInstanceLocation, unlike other backends.
// https://learn.microsoft.com/windows/win32/direct3d12/indirect-drawing
IndirectCapabilities IndirectLimits() noexcept
{
    if (ComputeLimits().MaxStorageRange == 0)
    {
        return {};
    }
    return
    {
        .ArgumentAlignment = INDIRECT_ARGUMENT_ALIGNMENT_BYTES,
        .MaxCommandsPerDraw = MAX_INDIRECT_COMMANDS_PER_DRAW,
        .HasNonIndexedDraws = GetComputeD3D12().DrawIndirectSignature.Get() != nullptr,
        .HasDrawOffsets = true,
        .HasFirstInstance = true,
        .HasIndirectDispatch = GetComputeD3D12().DispatchIndirectSignature.Get() != nullptr,
    };
}

RasterStatus ComputeCreatePipeline(usize slot, const ComputePipelineInfo& info, uint32) noexcept
{
    ID3D12Device* device = GetDeviceOrNull();
    LUDUS_ASSERT(device != nullptr);
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = GetSignature(info.Layout);
    description.CS = GetShader(info.Shader);
    ID3D12PipelineState* stateOrNull = nullptr;
    const HRESULT result =
        device->CreateComputePipelineState(&description, GetInterfaceId(stateOrNull), GetInterfaceOutput(stateOrNull));
    if (FAILED(result) == true)
    {
        return result == E_OUTOFMEMORY                    ? RasterStatus::OutOfMemory
               : device->GetDeviceRemovedReason() != S_OK ? RasterStatus::DeviceLost
                                                          : RasterStatus::Failed;
    }
    GetComputeD3D12().Pipelines[slot] = { .State = ComOwner<ID3D12PipelineState>(stateOrNull), .Layout = info.Layout };
    return RasterStatus::Ready;
}

RasterStatus ComputeEncode(const ComputePacket& packet) noexcept
{
    ID3D12GraphicsCommandList* commands = GetCommandsOrNull();
    LUDUS_ASSERT(commands != nullptr);
    const ComputePipeline& pipeline = GetComputeD3D12().Pipelines[packet.Pipeline];
    BeginPhysicalPassTiming(*commands, GpuPassKind::Compute);
    commands->SetPipelineState(pipeline.State.Get());
    commands->SetComputeRootSignature(GetSignature(pipeline.Layout));
    BindSet(commands, packet.Set, ShaderStage::Compute);
    if (packet.Indirect != RASTER_INVALID)
    {
        ComputeBufferBarrier(packet.Indirect, GraphAccessMode::Indirect);
        commands->ExecuteIndirect(GetComputeD3D12().DispatchIndirectSignature.Get(),
                                  1,
                                  GetBuffer(packet.Indirect),
                                  packet.IndirectOffset,
                                  nullptr,
                                  0);
    }
    else
    {
        commands->Dispatch(packet.Groups[0], packet.Groups[1], packet.Groups[2]);
    }
    EndPhysicalPassTiming(*commands);
    return RasterStatus::Ready;
}

namespace d3d12
{
void InitializeCompute() noexcept
{
    ID3D12Device* device = GetDeviceOrNull();
    LUDUS_ASSERT(device != nullptr);
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_0};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) == true ||
        shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_0)
    {
        return;
    }

    static_assert(sizeof(IndexedIndirectArguments) == sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
    static_assert(sizeof(DrawIndirectArguments) == sizeof(D3D12_DRAW_ARGUMENTS));
    static_assert(sizeof(DispatchIndirectArguments) == sizeof(D3D12_DISPATCH_ARGUMENTS));
    GetComputeD3D12().IndexedIndirectSignature = CreateIndirectSignature(device,
                                                                         D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED,
                                                                         sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
    if (GetComputeD3D12().IndexedIndirectSignature.Get() == nullptr)
    {
        return;
    }
    GetComputeD3D12().DrawIndirectSignature =
        CreateIndirectSignature(device, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, sizeof(D3D12_DRAW_ARGUMENTS));
    GetComputeD3D12().DispatchIndirectSignature =
        CreateIndirectSignature(device, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, sizeof(D3D12_DISPATCH_ARGUMENTS));
}

void ReleaseCompute() noexcept
{
    GetComputeD3D12().DispatchIndirectSignature.Reset();
    GetComputeD3D12().DrawIndirectSignature.Reset();
    GetComputeD3D12().IndexedIndirectSignature.Reset();
}

ID3D12CommandSignature* GetIndexedIndirectSignature() noexcept
{
    ID3D12CommandSignature* signature = GetComputeD3D12().IndexedIndirectSignature.Get();
    LUDUS_ASSERT(signature != nullptr);
    return signature;
}

ID3D12CommandSignature* GetDrawIndirectSignature() noexcept
{
    ID3D12CommandSignature* signature = GetComputeD3D12().DrawIndirectSignature.Get();
    LUDUS_ASSERT(signature != nullptr);
    return signature;
}

RasterStatus GrowComputePipelines(usize count) noexcept
{
    return GetComputeD3D12().Pipelines.EnsureSlots(count) == true ? RasterStatus::Ready : StorageFailure();
}

void DestroyComputePipeline(usize slot) noexcept
{
    GetComputeD3D12().Pipelines[slot] = {};
}
} // namespace d3d12
} // namespace ludus::graphics::rhi::backend
