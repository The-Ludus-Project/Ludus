#include "internal/d3d12_diagnostics.h"
#include "internal/d3d12_raster.h"
#include "internal/raster.h"

#include <ludus/foundation/base/core.h>
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
StorageTable<ComputePipeline> gComputePipelines;
ComOwner<ID3D12CommandSignature> gIndexedIndirectSignature;
} // namespace

ComputeCapabilities ComputeLimits() noexcept
{
    if (gIndexedIndirectSignature.Get() == nullptr)
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
    gComputePipelines[slot] = { .State = ComOwner<ID3D12PipelineState>(stateOrNull), .Layout = info.Layout };
    return RasterStatus::Ready;
}

RasterStatus ComputeEncode(const ComputePacket& packet) noexcept
{
    ID3D12GraphicsCommandList* commands = GetCommandsOrNull();
    LUDUS_ASSERT(commands != nullptr);
    const ComputePipeline& pipeline = gComputePipelines[packet.Pipeline];
    BeginPhysicalPassTiming(*commands, GpuPassKind::Compute);
    commands->SetPipelineState(pipeline.State.Get());
    commands->SetComputeRootSignature(GetSignature(pipeline.Layout));
    BindSet(commands, packet.Set, ShaderStage::Compute);
    commands->Dispatch(packet.Groups[0], packet.Groups[1], packet.Groups[2]);
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
    const D3D12_INDIRECT_ARGUMENT_DESC argument{ .Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED };
    const D3D12_COMMAND_SIGNATURE_DESC description
    {
        .ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
        .NumArgumentDescs = 1,
        .pArgumentDescs = &argument,
    };
    ID3D12CommandSignature* signatureOrNull = nullptr;
    const HRESULT result = device->CreateCommandSignature(&description,
                                                          nullptr,
                                                          GetInterfaceId(signatureOrNull),
                                                          GetInterfaceOutput(signatureOrNull));
    if (SUCCEEDED(result) == true)
    {
        gIndexedIndirectSignature = ComOwner<ID3D12CommandSignature>(signatureOrNull);
    }
}

void ReleaseCompute() noexcept
{
    gIndexedIndirectSignature.Reset();
}

ID3D12CommandSignature* GetIndexedIndirectSignature() noexcept
{
    ID3D12CommandSignature* signature = gIndexedIndirectSignature.Get();
    LUDUS_ASSERT(signature != nullptr);
    return signature;
}

RasterStatus GrowComputePipelines(usize count) noexcept
{
    return gComputePipelines.EnsureSlots(count) == true ? RasterStatus::Ready : StorageFailure();
}

void DestroyComputePipeline(usize slot) noexcept
{
    gComputePipelines[slot] = {};
}
} // namespace d3d12
} // namespace ludus::graphics::rhi::backend
