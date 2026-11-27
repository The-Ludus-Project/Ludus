#include "internal/visibility_buffer.h"
#include <ludus/foundation/containers/borrowed_view.hpp>

#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
using Status = rhi::RasterStatus;
Status ExpandVisibilityGeometry(BorrowedView<const math::Vector3> positions,
                                BorrowedView<const uint32> indices,
                                usize bufferByteLimit,
                                Array<VisibilityVertex>& outVertices,
                                Array<uint32>& outIndices) noexcept
{
    constexpr usize CORNER_COUNT = 3;
    constexpr uint32 COMPONENT_BITS = 16;
    constexpr uint32 COMPONENT_MASK = 0xffff;
    if (positions.IsValid() == false || indices.IsValid() == false || positions.IsEmpty() == true ||
        indices.IsEmpty() == true || indices.GetSize() % CORNER_COUNT != 0)
    {
        return Status::InvalidDescription;
    }
    if (indices.GetSize() > ~uint32{0} || indices.GetSize() > bufferByteLimit / sizeof(VisibilityVertex))
    {
        return Status::CapacityExceeded;
    }
    Array<VisibilityVertex> vertices;
    Array<uint32> expandedIndices;
    if (vertices.TryResize(indices.GetSize()) == false || expandedIndices.TryResize(indices.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize corner = 0; corner < indices.GetSize(); ++corner)
    {
        if (indices[corner] >= positions.GetSize())
        {
            return Status::InvalidDescription;
        }
        const uint32 primitive = static_cast<uint32>(corner / CORNER_COUNT);
        vertices[corner] = {
            positions[indices[corner]],
            {static_cast<float32>(primitive & COMPONENT_MASK), static_cast<float32>(primitive >> COMPONENT_BITS)},
        };
        expandedIndices[corner] = static_cast<uint32>(corner);
    }
    outVertices.Swap(vertices);
    outIndices.Swap(expandedIndices);
    return Status::Ready;
}
Status AppendVisibilityCommands(rhi::DeviceHandle device,
                                rhi::OrderedGraph& inoutGraph,
                                const VisibilityCommands& commands,
                                rhi::GraphVersion previous,
                                rhi::GraphVersion& outWritten) noexcept
{
    constexpr usize STREAM_COUNT = 2;
    rhi::GraphVersion written;
    Status result = rhi::NextGraphVersion(device, inoutGraph, previous, written);
    if (result != Status::Ready)
    {
        return result;
    }
    Array<rhi::RasterDraw> draws;
    Array<StaticArray<rhi::RasterVertexSlice, STREAM_COUNT>> slices;
    Array<rhi::GraphUse> uses;
    const usize uniformBlocks = commands.UniformLayoutOrNull != nullptr ? commands.UniformLayoutOrNull->Count : 0;
    if (draws.TryResize(commands.Draws.GetSize()) == false || slices.TryResize(commands.Draws.GetSize()) == false ||
        uses.TryEnsureCapacity(commands.Draws.GetSize() * (2 + uniformBlocks) + 2) == false)
    {
        return Status::OutOfMemory;
    }
    // A Load attachment adds this pass's draws to the version an earlier producer wrote.
    const bool isLoaded = commands.Attachment.ColorLoad == rhi::RasterLoad::Load;
    if (uses.TryAdd(
            {written, isLoaded == true ? rhi::GraphAccessMode::ColorReadWrite : rhi::GraphAccessMode::ColorWrite}) ==
        false)
    {
        return Status::OutOfMemory;
    }
    if (commands.Draws.IsEmpty() == false)
    {
        rhi::GraphVersion instances;
        result = rhi::ImportGraphBuffer(device, inoutGraph, commands.Instances, instances);
        if (result != Status::Ready)
        {
            return result;
        }
        if (uses.TryAdd(
                {instances, rhi::GraphAccessMode::Vertex, 0, commands.InstanceCount * commands.InstanceStride}) ==
            false)
        {
            return Status::OutOfMemory;
        }
    }
    for (usize index = 0; index < commands.Draws.GetSize(); ++index)
    {
        const VisibilityDraw& packet = commands.Draws[index];
        bool isImported = false;
        for (usize earlier = 0; earlier < index; ++earlier)
        {
            isImported = isImported == true || commands.Draws[earlier].MeshIndex == packet.MeshIndex;
        }
        if (isImported == false)
        {
            rhi::GraphVersion vertices;
            rhi::GraphVersion indices;
            result = rhi::ImportGraphBuffer(device, inoutGraph, packet.Vertices, vertices);
            if (result == Status::Ready)
            {
                result = rhi::ImportGraphBuffer(device, inoutGraph, packet.Indices, indices);
            }
            if (result != Status::Ready)
            {
                return result;
            }
            if (uses.TryAdd({vertices, rhi::GraphAccessMode::Vertex, 0, packet.VertexCount * commands.VertexStride}) ==
                    false ||
                uses.TryAdd({indices, rhi::GraphAccessMode::Index, 0, packet.IndexCount * sizeof(uint32)}) == false)
            {
                return Status::OutOfMemory;
            }
        }
        if (packet.HasUniforms == true && commands.UniformLayoutOrNull == nullptr)
        {
            return Status::InvalidDescription;
        }
        bool isUniformImported = packet.HasUniforms == false;
        for (usize earlier = 0; earlier < index && isUniformImported == false; ++earlier)
        {
            isUniformImported =
                commands.Draws[earlier].HasUniforms == true && commands.Draws[earlier].Uniforms == packet.Uniforms;
        }
        if (isUniformImported == false)
        {
            // Declare each block the draw's binding snapshot reads, at its layout range and stages.
            rhi::GraphVersion uniforms;
            result = rhi::ImportGraphBuffer(device, inoutGraph, packet.Uniforms, uniforms);
            if (result != Status::Ready)
            {
                return result;
            }
            const FlatUniformLayout& layout = *commands.UniformLayoutOrNull;
            for (usize block = 0; block < layout.Count; ++block)
            {
                if (uses.TryAdd({uniforms,
                                 rhi::GraphAccessMode::Uniform,
                                 layout.Offsets[block],
                                 layout.Bindings[block].MinSize,
                                 layout.Bindings[block].Visibility}) == false)
                {
                    return Status::OutOfMemory;
                }
            }
        }
        slices[index][0] = {packet.Vertices, 0, packet.VertexCount * commands.VertexStride};
        slices[index][1] = {commands.Instances,
                            packet.FirstInstance * commands.InstanceStride,
                            packet.InstanceCount * commands.InstanceStride};
        draws[index] = {commands.Pipeline,
                        packet.HasUniforms == true ? packet.Bindings : commands.Bindings,
                        {slices[index].GetData(), slices[index].GetSize()},
                        packet.Indices,
                        0,
                        packet.IndexCount,
                        packet.VertexCount,
                        packet.InstanceCount};
    }
    rhi::GraphPassDescription pass;
    pass.Name = commands.Name;
    pass.Attachment = commands.Attachment;
    if (commands.ShouldClearTransparent == true)
    {
        constexpr usize ALPHA_CHANNEL = 3;
        pass.Attachment.Clear[ALPHA_CHANNEL] = 0;
    }
    pass.Uses = uses.GetData();
    pass.UseCount = uses.GetSize();
    pass.Draws = draws.GetData();
    pass.DrawCount = draws.GetSize();
    result = rhi::AddGraphPass(device, inoutGraph, pass);
    if (result == Status::Ready)
    {
        outWritten = written;
    }
    return result;
}
} // namespace ludus::graphics::renderer::internal
