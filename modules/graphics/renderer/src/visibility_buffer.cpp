#include "internal/visibility_buffer.h"

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
    if (draws.TryResize(commands.Draws.GetSize()) == false || slices.TryResize(commands.Draws.GetSize()) == false ||
        uses.TryEnsureCapacity(commands.Draws.GetSize() * 2 + 2) == false)
    {
        return Status::OutOfMemory;
    }
    if (uses.TryAdd({written, rhi::GraphAccessMode::ColorWrite}) == false)
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
            if (uses.TryAdd(
                    {vertices, rhi::GraphAccessMode::Vertex, 0, packet.VertexCount * sizeof(VisibilityVertex)}) ==
                    false ||
                uses.TryAdd({indices, rhi::GraphAccessMode::Index, 0, packet.IndexCount * sizeof(uint32)}) == false)
            {
                return Status::OutOfMemory;
            }
        }
        slices[index][0] = {packet.Vertices, 0, packet.VertexCount * sizeof(VisibilityVertex)};
        slices[index][1] = {commands.Instances,
                            packet.FirstInstance * commands.InstanceStride,
                            packet.InstanceCount * commands.InstanceStride};
        draws[index] = {commands.Pipeline,
                        commands.Bindings,
                        slices[index].AsSpan(),
                        packet.Indices,
                        0,
                        packet.IndexCount,
                        packet.VertexCount,
                        packet.InstanceCount};
    }
    rhi::GraphPassDescription pass;
    pass.Name = "Visibility raster";
    pass.Attachment = commands.Attachment;
    constexpr usize ALPHA_CHANNEL = 3;
    pass.Attachment.Clear[ALPHA_CHANNEL] = 0;
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
