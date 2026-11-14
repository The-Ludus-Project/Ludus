#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/profile.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::rhi
{
/// Opaque immutable compute pipeline in the shared RHI registry. Owner-thread
/// only; destruction invalidates public copies while retained graph/GPU uses live.
struct ComputePipelineHandle final
{
private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants shared registry access to this incarnation.
    friend struct internal::RasterAccess;
};
/// Immutable compute stage and exact group-zero layout; dependencies must be ready.
struct ComputePipelineDescription final
{
    /// Reflected Compute shader, including verified local workgroup dimensions.
    RasterShaderHandle Shader{};
    /// Immutable Compute-only buffer layout, unfiltered single 2D color/depth views,
    /// and optionally one ready native RayScene.
    /// Native scenes require enabled ray queries and stay retained through dispatch completion.
    BindingLayoutHandle Layout{};
    /// Queue native driver creation; also enabled by an asynchronous shader input.
    bool ShouldPrepareAsynchronously = false;
};
/// One logical compute pass contains exactly one dispatch. Dependent dispatches
/// need separate ordered graph passes. Shader-local races remain kernel obligations.
/// @note On native Metal, storage-buffer GetDimensions uses the bound range's byte
/// length, not the full allocation. Querying ranges beyond uint32 byte capacity
/// returns Unsupported before dispatch; shaders must still bound their own indices.
struct ComputeDispatch final
{
    /// Ready immutable pipeline.
    ComputePipelineHandle Pipeline{};
    /// Ready binding snapshot with exactly the pipeline layout.
    BindingSetHandle Bindings{};
    /// Number of workgroups, nonzero and bounded on every axis. With Indirect, these are the
    /// per-axis maxima that the GPU-written DispatchIndirectArguments must not exceed.
    ludus::foundation::uint32 Groups[3]{1, 1, 1};
    /// Additional byte offsets for dynamic uniform/storage entries, copied into the graph.
    RasterBindingOffsets BindingOffsets{};
    /// Optional ready StorageIndirect buffer (R12a). Non-null reads DispatchIndirectArguments
    /// at IndirectOffset on the GPU; requires IndirectCapabilities::HasIndirectDispatch and an
    /// ordered-graph GraphAccessMode::Indirect use of exactly that range in the compute pass.
    /// Native compute pipelines reject indirect dispatch.
    BufferHandle Indirect{};
    /// Byte offset of the arguments, a multiple of IndirectCapabilities::ArgumentAlignment.
    /// Must be zero without Indirect.
    ludus::foundation::usize IndirectOffset = 0;
};
/// Indexed indirect command ABI shared by Vulkan, Metal, WebGPU and Direct3D 12: twenty bytes.
/// The producing kernel must keep the command within the associated RasterDraw bounds:
/// FirstIndex + IndexCount <= RasterDraw::IndexCount, every index + BaseVertex below
/// RasterDraw::VertexCount, and FirstInstance + InstanceCount <= RasterDraw::InstanceCount.
/// IndexOffset in the packet selects the mesh; FirstIndex counts indices from it. The GPU
/// values are never read back, so violating this contract is a kernel correctness bug.
/// @note Offsets apply to vertex-stream fetches on every backend. Shader-visible vertex and
/// instance index built-ins include them on Vulkan, Metal and WebGPU but not for
/// FirstInstance on Direct3D 12; portable shaders must not rely on those built-ins.
struct IndexedIndirectArguments final
{
    /// Number of mesh indices, at most RasterDraw::IndexCount - FirstIndex; zero culls the draw.
    ludus::foundation::uint32 IndexCount = 0;
    /// Number of instances, at most RasterDraw::InstanceCount - FirstInstance; zero culls the draw.
    ludus::foundation::uint32 InstanceCount = 0;
    /// First index relative to RasterDraw::IndexOffset. Nonzero requires
    /// IndirectCapabilities::HasDrawOffsets; otherwise it must be zero.
    ludus::foundation::uint32 FirstIndex = 0;
    /// Signed value added to each index before vertex fetch. Nonzero requires
    /// IndirectCapabilities::HasDrawOffsets; otherwise it must be zero.
    ludus::foundation::int32 BaseVertex = 0;
    /// First per-instance stream record. Nonzero requires IndirectCapabilities::HasFirstInstance;
    /// otherwise it must be zero.
    ludus::foundation::uint32 FirstInstance = 0;
};
/// Non-indexed indirect command ABI shared by Vulkan, Metal, WebGPU and Direct3D 12: sixteen
/// bytes (R12a). Selected by a RasterDraw with Indirect and no Indices. The producing kernel
/// must keep FirstVertex + VertexCount <= RasterDraw::VertexCount and FirstInstance +
/// InstanceCount <= RasterDraw::InstanceCount; the values are never read back to the CPU.
struct DrawIndirectArguments final
{
    /// Number of vertices, a multiple of three for triangle lists; zero culls the draw.
    ludus::foundation::uint32 VertexCount = 0;
    /// Number of instances; zero culls the draw.
    ludus::foundation::uint32 InstanceCount = 0;
    /// First vertex record. Nonzero requires IndirectCapabilities::HasDrawOffsets.
    ludus::foundation::uint32 FirstVertex = 0;
    /// First per-instance stream record. Nonzero requires IndirectCapabilities::HasFirstInstance.
    ludus::foundation::uint32 FirstInstance = 0;
};
/// Indirect dispatch ABI shared by Vulkan, Metal, WebGPU and Direct3D 12: twelve bytes (R12a).
/// The producing kernel must keep each count at or below the ComputeDispatch::Groups maximum
/// for that axis. A zero count on any axis dispatches no workgroups.
struct DispatchIndirectArguments final
{
    /// Workgroups along x.
    ludus::foundation::uint32 GroupCountX = 0;
    /// Workgroups along y.
    ludus::foundation::uint32 GroupCountY = 0;
    /// Workgroups along z.
    ludus::foundation::uint32 GroupCountZ = 0;
};
/// Byte size of one IndexedIndirectArguments record (five 32-bit words) in argument buffers.
inline constexpr ludus::foundation::usize INDEXED_INDIRECT_ARGUMENT_BYTES = 20;
/// Byte size of one DrawIndirectArguments record (four 32-bit words) in argument buffers.
inline constexpr ludus::foundation::usize DRAW_INDIRECT_ARGUMENT_BYTES = 16;
/// Byte size of one DispatchIndirectArguments record (three 32-bit words) in argument buffers.
inline constexpr ludus::foundation::usize DISPATCH_INDIRECT_ARGUMENT_BYTES = 12;
static_assert(sizeof(IndexedIndirectArguments) == INDEXED_INDIRECT_ARGUMENT_BYTES);
static_assert(sizeof(DrawIndirectArguments) == DRAW_INDIRECT_ARGUMENT_BYTES);
static_assert(sizeof(DispatchIndirectArguments) == DISPATCH_INDIRECT_ARGUMENT_BYTES);
/// Query negotiated compute limits. Invalid owner preserves output; other failures
/// zero it. WebGL2 returns Unsupported. No operation waits for the GPU.
[[nodiscard]] RasterStatus GetComputeCapabilities(DeviceHandle, ComputeCapabilities&) noexcept;
/// Create a retained immutable compute pipeline between frames. Failure preserves
/// null output; Pending publishes ownership requiring polling and destruction.
[[nodiscard]] RasterStatus
CreateComputePipeline(DeviceHandle, const ComputePipelineDescription&, ComputePipelineHandle&) noexcept;
/// Poll creation; invalid/stale/foreign identities return InvalidHandle.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, ComputePipelineHandle) noexcept;
/// Logically destroy between frames; graph and GPU leases delay physical release.
/// Invalid handle preserves output. Success clears it.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, ComputePipelineHandle&) noexcept;
} // namespace ludus::graphics::rhi
