#include "internal/gpu_preview_graph.h"

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace math = foundation::math;
using Status = rhi::RasterStatus;
namespace
{
// One ledger for all invocations: shared storage imports once, then each writer
// advances its version so the RHI compiler sees the actual inter-pass hazards.
constexpr usize GRAPH_SEQUENCE_RESOURCE_COUNT = rhi::DEFAULT_GRAPH_RESOURCE_COUNT;
constexpr usize JOB_BUFFER_COUNT = 8;
constexpr usize QUAD_BUFFER_INDEX = 4;
constexpr usize INDEX_BUFFER_INDEX = 5;
constexpr usize IMAGE_PARAMETERS_INDEX = 6;
constexpr usize TEXEL_BUFFER_INDEX = 7;
constexpr usize BASE_COMPUTE_USE_COUNT = 5;
constexpr usize IMAGE_USE_COUNT = 5;
constexpr usize IMAGE_VERTEX_SLICE_COUNT = 1;
constexpr usize QUAD_VERTEX_COUNT = 4;
constexpr usize QUAD_INDEX_COUNT = 6;
constexpr usize QUAD_BYTES = sizeof(math::Vector2) * QUAD_VERTEX_COUNT;
constexpr usize INDEX_BYTES = sizeof(uint16) * QUAD_INDEX_COUNT;
constexpr usize IMAGE_PARAMETERS_BYTES = LUDUS_GPU_PREVIEW_VECTOR_BYTES;
struct GpuGraphResources final
{
    struct BufferEntry final
    {
        rhi::BufferHandle Handle{};
        rhi::GraphVersion Version{};
        bool IsHistory = false;
    };
    struct TextureEntry final
    {
        rhi::TextureHandle Handle{};
        rhi::GraphVersion Version{};
    };
    StaticArray<BufferEntry, GRAPH_SEQUENCE_RESOURCE_COUNT> Buffers{};
    StaticArray<TextureEntry, content::render_graph_format::MAX_RAY_PASSES> Textures{};
    usize BufferCount = 0;
    usize TextureCount = 0;
    Status ImportBuffer(rhi::DeviceHandle device,
                        rhi::OrderedGraph graph,
                        rhi::BufferHandle buffer,
                        rhi::GraphVersion& outVersion) noexcept
    {
        for (usize index = 0; index < BufferCount; ++index)
        {
            if (Buffers[index].Handle == buffer)
            {
                outVersion = Buffers[index].Version;
                return Status::Ready;
            }
        }
        if (BufferCount == Buffers.GetSize())
        {
            return Status::CapacityExceeded;
        }
        const Status status = rhi::ImportGraphBuffer(device, graph, buffer, outVersion);
        if (status == Status::Ready)
        {
            Buffers[BufferCount++] = {buffer, outVersion};
        }
        return status;
    }
    void SetBufferVersion(rhi::BufferHandle buffer, rhi::GraphVersion version) noexcept
    {
        for (usize index = 0; index < BufferCount; ++index)
        {
            if (Buffers[index].Handle == buffer)
            {
                Buffers[index].Version = version;
                Buffers[index].IsHistory = true;
                return;
            }
        }
    }
    Status ImportTexture(rhi::DeviceHandle device,
                         rhi::OrderedGraph graph,
                         rhi::TextureHandle texture,
                         rhi::GraphVersion& outVersion) noexcept
    {
        for (usize index = 0; index < TextureCount; ++index)
        {
            if (Textures[index].Handle == texture)
            {
                outVersion = Textures[index].Version;
                return Status::Ready;
            }
        }
        if (TextureCount == Textures.GetSize())
        {
            return Status::CapacityExceeded;
        }
        rhi::RasterTextureState state;
        Status status = rhi::GetTextureState(device, texture, state);
        if (status == Status::Ready)
        {
            status = rhi::ImportGraphTexture(device,
                                             graph,
                                             texture,
                                             state,
                                             rhi::RasterTextureUse::SampledFragment,
                                             outVersion);
        }
        if (status == Status::Ready)
        {
            Textures[TextureCount++] = {texture, outVersion};
        }
        return status;
    }
    [[nodiscard]] Status AddRoots(rhi::DeviceHandle device, rhi::OrderedGraph graph) const noexcept
    {
        for (usize index = 0; index < BufferCount; ++index)
        {
            if (Buffers[index].IsHistory == true)
            {
                const Status status = rhi::AddGraphRoot(device, graph, Buffers[index].Version, rhi::GraphRoot::History);
                if (status != Status::Ready)
                {
                    return status;
                }
            }
        }
        for (usize index = 0; index < TextureCount; ++index)
        {
            const Status status = rhi::AddGraphRoot(device, graph, Textures[index].Version, rhi::GraphRoot::Export);
            if (status != Status::Ready)
            {
                return status;
            }
        }
        return Status::Ready;
    }
    void SetTextureVersion(rhi::TextureHandle texture, rhi::GraphVersion version) noexcept
    {
        for (usize index = 0; index < TextureCount; ++index)
        {
            if (Textures[index].Handle == texture)
            {
                Textures[index].Version = version;
                return;
            }
        }
    }
};
Status AppendGpuGraph(rhi::DeviceHandle device,
                      const GpuPreviewState& state,
                      const GpuPreviewRecord& job,
                      uint32 image,
                      rhi::OrderedGraph graph,
                      GpuGraphResources& inoutResources,
                      const rhi::TextureHandle* targetOrNull = nullptr,
                      const VisibilityRasterPass* visibilityOrNull = nullptr) noexcept
{
    constexpr usize COLOR_BUFFER_INDEX = 3;
    Status status = Status::Ready;
    const StaticArray<rhi::BufferHandle, JOB_BUFFER_COUNT> buffers{{job.Data,
                                                                    job.Parameters,
                                                                    job.Accumulation,
                                                                    job.Colors,
                                                                    state.Quad,
                                                                    state.Indices,
                                                                    job.ImageParameters,
                                                                    job.Texels}};
    StaticArray<rhi::GraphVersion, JOB_BUFFER_COUNT> versions{};
    for (usize i = 0; i < JOB_BUFFER_COUNT; ++i)
    {
        status = inoutResources.ImportBuffer(device, graph, buffers[i], versions[i]);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    const rhi::TextureHandle target = targetOrNull != nullptr ? *targetOrNull : job.Frames[image].Texture;
    rhi::GraphVersion texture;
    rhi::GraphVersion output;
    rhi::GraphVersion accumulation;
    rhi::GraphVersion colors;
    status = inoutResources.ImportTexture(device, graph, target, texture);
    if (status == Status::Ready)
    {
        status = rhi::NextGraphVersion(device, graph, texture, output);
    }
    if (status == Status::Ready)
    {
        status = rhi::NextGraphVersion(device, graph, versions[2], accumulation);
    }
    if (status == Status::Ready)
    {
        status = rhi::NextGraphVersion(device, graph, versions[COLOR_BUFFER_INDEX], colors);
    }
    if (status != Status::Ready)
    {
        return status;
    }
    const usize pixels = usize(job.Width) * job.Height;
    // GraphUse::Visibility defaults to RasterVisibility::Both; LLVM 23 overlooks that
    // member initializer for the value-initialized tail of this array (.clang-tidy).
    // NOLINTBEGIN(bugprone-invalid-enum-default-initialization)
    StaticArray<rhi::GraphUse, LUDUS_GRAPH_COMPUTE_BINDING_COUNT> computeUses{
        {{versions[0], rhi::GraphAccessMode::StorageRead, 0, job.DataBytes, rhi::RasterVisibility::Compute},
         {versions[1],
          rhi::GraphAccessMode::Uniform,
          0,
          sizeof(internal::GpuPreviewUniform),
          rhi::RasterVisibility::Compute},
         {accumulation,
          rhi::GraphAccessMode::StorageReadWrite,
          0,
          pixels * LUDUS_GPU_PREVIEW_ACCUMULATION_BYTES,
          rhi::RasterVisibility::Compute},
         {colors,
          rhi::GraphAccessMode::StorageReadWrite,
          0,
          pixels * LUDUS_GPU_PREVIEW_VECTOR_BYTES,
          rhi::RasterVisibility::Compute},
         {versions[TEXEL_BUFFER_INDEX],
          rhi::GraphAccessMode::StorageRead,
          0,
          job.TexelBytes,
          rhi::RasterVisibility::Compute}}};
    // NOLINTEND(bugprone-invalid-enum-default-initialization)
    usize computeUseCount = BASE_COMPUTE_USE_COUNT;
    for (usize slot = 0; state.HasPersistentBuffers == true && slot < GPU_GRAPH_BUFFER_COUNT; ++slot)
    {
        const GpuPreviewGraphBuffer& buffer = job.GraphBuffers[slot];
        rhi::GraphVersion previous;
        rhi::GraphVersion next;
        status = inoutResources.ImportBuffer(device, graph, buffer.Buffer, previous);
        if (status == Status::Ready)
        {
            status = rhi::NextGraphVersion(device, graph, previous, next);
        }
        if (status != Status::Ready)
        {
            return status;
        }
        inoutResources.SetBufferVersion(buffer.Buffer, next);
        computeUses[computeUseCount++] = {next,
                                          rhi::GraphAccessMode::StorageReadWrite,
                                          0,
                                          buffer.Size,
                                          rhi::RasterVisibility::Compute};
    }
    status = job.Storage.AppendUses(graph,
                                    {computeUses.GetData() + computeUseCount, computeUses.GetSize() - computeUseCount});
    if (status != Status::Ready)
    {
        return status;
    }
    computeUseCount += job.Storage.GetCount();
    if (visibilityOrNull != nullptr)
    {
        rhi::RasterTextureState visibilityState;
        rhi::GraphVersion previous;
        rhi::GraphVersion written;
        status = rhi::GetTextureState(device, job.VisibilityTexture, visibilityState);
        if (status == Status::Ready)
        {
            status = rhi::ImportGraphTexture(device,
                                             graph,
                                             job.VisibilityTexture,
                                             visibilityState,
                                             rhi::RasterTextureUse::SampledCompute,
                                             previous);
        }
        if (status == Status::Ready)
        {
            status = visibilityOrNull->ProducerOrNull->AppendVisibilityView(visibilityOrNull->View,
                                                                            graph,
                                                                            visibilityOrNull->Attachment,
                                                                            previous,
                                                                            written);
        }
        if (status != Status::Ready)
        {
            return status;
        }
        computeUses[computeUseCount++] = {written, rhi::GraphAccessMode::Sampled, 0, 0, rhi::RasterVisibility::Compute};
    }
    const rhi::ComputeDispatch dispatch{
        state.ComputePipeline,
        job.ComputeBindings,
        {(job.Width + LUDUS_GPU_PREVIEW_GROUP_WIDTH - 1) / LUDUS_GPU_PREVIEW_GROUP_WIDTH, job.Height, 1}};
    rhi::GraphPassDescription compute;
    compute.Name = "GPU preview transport";
    compute.Uses = computeUses.GetData();
    compute.UseCount = computeUseCount;
    compute.Dispatch = &dispatch;
    status = rhi::AddGraphPass(device, graph, compute);
    if (status != Status::Ready)
    {
        return status;
    }
    if (job.DenoisingQuality != 0)
    {
        rhi::GraphVersion denoised;
        status = rhi::NextGraphVersion(device, graph, colors, denoised);
        if (status != Status::Ready)
        {
            return status;
        }
        const rhi::GraphUse use{denoised,
                                rhi::GraphAccessMode::StorageReadWrite,
                                0,
                                pixels * LUDUS_GPU_PREVIEW_VECTOR_BYTES,
                                rhi::RasterVisibility::Compute};
        const rhi::ComputeDispatch denoising = job.Denoiser.GetDispatch();
        rhi::GraphPassDescription pass;
        pass.Name = "OIDN HDR denoising";
        pass.Uses = &use;
        pass.UseCount = 1;
        pass.Dispatch = &denoising;
        status = rhi::AddGraphPass(device, graph, pass);
        if (status != Status::Ready)
        {
            return status;
        }
        colors = denoised;
    }
    const StaticArray<rhi::RasterVertexSlice, IMAGE_VERTEX_SLICE_COUNT> slices{{{state.Quad, 0, QUAD_BYTES}}};
    const rhi::RasterDraw draw{state.ImagePipeline,
                               job.ImageBindings,
                               slices.AsSpan(),
                               state.Indices,
                               0,
                               QUAD_INDEX_COUNT,
                               QUAD_VERTEX_COUNT,
                               1};
    const StaticArray<rhi::GraphUse, IMAGE_USE_COUNT> imageUses{
        {{versions[QUAD_BUFFER_INDEX], rhi::GraphAccessMode::Vertex, 0, QUAD_BYTES},
         {colors,
          rhi::GraphAccessMode::StorageRead,
          0,
          pixels * LUDUS_GPU_PREVIEW_VECTOR_BYTES,
          rhi::RasterVisibility::Fragment},
         {versions[INDEX_BUFFER_INDEX], rhi::GraphAccessMode::Index, 0, INDEX_BYTES},
         {versions[IMAGE_PARAMETERS_INDEX],
          rhi::GraphAccessMode::Uniform,
          0,
          IMAGE_PARAMETERS_BYTES,
          rhi::RasterVisibility::Fragment},
         {output, rhi::GraphAccessMode::ColorWrite}}};
    rhi::GraphPassDescription present;
    present.Name = "GPU preview image";
    present.Attachment.Color = target;
    if (targetOrNull != nullptr)
    {
        present.Attachment.DepthStore = rhi::RasterStore::Discard;
    }
    present.Uses = imageUses.GetData();
    present.UseCount = imageUses.GetSize();
    present.Draws = &draw;
    present.DrawCount = 1;
    status = rhi::AddGraphPass(device, graph, present);
    if (status == Status::Ready)
    {
        status = rhi::AddGraphRoot(device, graph, accumulation, rhi::GraphRoot::History);
    }
    inoutResources.SetTextureVersion(target, output);
    return status;
}
} // namespace
Status BuildGpuGraphSequence(rhi::DeviceHandle device,
                             const GpuPreviewState& state,
                             BorrowedView<const GpuPreviewRecord*> jobs,
                             BorrowedView<const rhi::TextureHandle> targets,
                             rhi::OrderedGraph& outGraph) noexcept
{
    Status status = rhi::CreateOrderedGraph(device, outGraph);
    if (status != Status::Ready)
    {
        return status;
    }
    GpuGraphResources resources;
    for (usize index = 0; status == Status::Ready && index < jobs.GetSize(); ++index)
    {
        status = AppendGpuGraph(device, state, *jobs[index], 0, outGraph, resources, &targets[index]);
    }
    if (status == Status::Ready)
    {
        status = resources.AddRoots(device, outGraph);
    }
    rhi::GraphReport report;
    if (status == Status::Ready)
    {
        status = rhi::CompileOrderedGraph(device, outGraph, report);
    }
    if (status != Status::Ready)
    {
        static_cast<void>(rhi::DiscardOrderedGraph(device, outGraph));
    }
    return status;
}
Status BuildGpuGraph(rhi::DeviceHandle device,
                     const GpuPreviewState& state,
                     const GpuPreviewRecord& job,
                     uint32 image,
                     rhi::OrderedGraph& outGraph,
                     const rhi::TextureHandle* targetOrNull,
                     const VisibilityRasterPass* visibilityOrNull) noexcept
{
    Status status = targetOrNull == nullptr ? rhi::CreateOrderedGraph(device, outGraph) : Status::Ready;
    GpuGraphResources resources;
    if (status == Status::Ready)
    {
        status = AppendGpuGraph(device, state, job, image, outGraph, resources, targetOrNull, visibilityOrNull);
    }
    if (status == Status::Ready)
    {
        status = resources.AddRoots(device, outGraph);
    }
    rhi::GraphReport report;
    if (status == Status::Ready)
    {
        status = rhi::CompileOrderedGraph(device, outGraph, report);
    }
    return status;
}
} // namespace ludus::graphics::renderer::internal
