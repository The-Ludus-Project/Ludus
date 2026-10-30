#include "mesh_compaction.h"

#include <ludus/foundation/containers/sort.hpp>

#include <cstring>

namespace ludus::content::scene_cook::importers
{
namespace
{
constexpr usize UV_COMPONENT_COUNT = 2;

[[nodiscard]] bool IsCancelled(const Cancellation* cancelOrNull) noexcept
{
    return cancelOrNull != nullptr && cancelOrNull->Requested != nullptr &&
           cancelOrNull->Requested(cancelOrNull->ContextOrNull) == true;
}

[[nodiscard]] int32 CompareAttribute(const Array<float32>& values, usize width, uint32 left, uint32 right) noexcept
{
    if (values.IsEmpty() == true)
    {
        return 0;
    }
    return std::memcmp(values.GetData() + usize{left} * width,
                       values.GetData() + usize{right} * width,
                       width * sizeof(float32));
}

[[nodiscard]] int32 CompareVertices(const IrMesh& mesh, uint32 left, uint32 right) noexcept
{
    int32 comparison = CompareAttribute(mesh.Positions, IR_VECTOR_COMPONENT_COUNT, left, right);
    if (comparison == 0)
    {
        comparison = CompareAttribute(mesh.Normals, IR_VECTOR_COMPONENT_COUNT, left, right);
    }
    if (comparison == 0)
    {
        comparison = CompareAttribute(mesh.Uvs, UV_COMPONENT_COUNT, left, right);
    }
    if (comparison == 0)
    {
        comparison = CompareAttribute(mesh.Tangents, IR_TANGENT_COMPONENT_COUNT, left, right);
    }
    return comparison;
}

void CopyAttribute(Array<float32>& inoutValues, usize width, usize source, usize destination) noexcept
{
    if (inoutValues.IsEmpty() == false)
    {
        for (usize component = 0; component < width; ++component)
        {
            inoutValues[destination * width + component] = inoutValues[source * width + component];
        }
    }
}

void ShrinkAttribute(Array<float32>& inoutValues, usize width, usize count) noexcept
{
    if (inoutValues.IsEmpty() == false)
    {
        // Shrinking initialized scalar arrays cannot allocate or fail.
        (void)inoutValues.TryResize(count * width);
    }
}
} // namespace

Status CompactMesh(IrMesh& inoutMesh, const Cancellation* cancelOrNull) noexcept
{
    if (IsCancelled(cancelOrNull) == true)
    {
        return Status::Cancelled;
    }
    const usize count = inoutMesh.GetVertexCount();
    if (inoutMesh.Positions.GetSize() != count * IR_VECTOR_COMPONENT_COUNT || count >= IR_NONE ||
        (inoutMesh.Normals.IsEmpty() == false && inoutMesh.Normals.GetSize() != count * IR_VECTOR_COMPONENT_COUNT) ||
        (inoutMesh.Uvs.IsEmpty() == false && inoutMesh.Uvs.GetSize() != count * UV_COMPONENT_COUNT) ||
        (inoutMesh.Tangents.IsEmpty() == false && inoutMesh.Tangents.GetSize() != count * IR_TANGENT_COMPONENT_COUNT))
    {
        return Status::Invalid;
    }
    for (const uint32 index : inoutMesh.Indices)
    {
        if (index >= count)
        {
            return Status::Invalid;
        }
    }
    if (count < 2 || inoutMesh.Normals.IsEmpty() == true)
    {
        return Status::Ok;
    }
    Array<uint32> order;
    Array<uint32> remap;
    if (order.TryResize(count) == false || remap.TryResize(count) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize vertex = 0; vertex < count; ++vertex)
    {
        order[vertex] = static_cast<uint32>(vertex);
    }
    foundation::Sort(order.GetData(), order.GetSize(), [&](uint32 left, uint32 right) noexcept {
        const int32 comparison = CompareVertices(inoutMesh, left, right);
        return comparison < 0 || (comparison == 0 && left < right);
    });
    uint32 representative = order[0];
    for (const uint32 vertex : order)
    {
        if (CompareVertices(inoutMesh, representative, vertex) != 0)
        {
            representative = vertex;
        }
        remap[vertex] = representative;
    }
    if (IsCancelled(cancelOrNull) == true)
    {
        return Status::Cancelled;
    }
    // Every representative precedes its duplicates. Resolve dense indices in
    // source order before compacting attributes; later comparisons need no data.
    usize uniqueCount = 0;
    for (usize vertex = 0; vertex < count; ++vertex)
    {
        if (remap[vertex] == vertex)
        {
            CopyAttribute(inoutMesh.Positions, IR_VECTOR_COMPONENT_COUNT, vertex, uniqueCount);
            CopyAttribute(inoutMesh.Normals, IR_VECTOR_COMPONENT_COUNT, vertex, uniqueCount);
            CopyAttribute(inoutMesh.Uvs, UV_COMPONENT_COUNT, vertex, uniqueCount);
            CopyAttribute(inoutMesh.Tangents, IR_TANGENT_COMPONENT_COUNT, vertex, uniqueCount);
            remap[vertex] = static_cast<uint32>(uniqueCount);
            ++uniqueCount;
        }
        else
        {
            remap[vertex] = remap[remap[vertex]];
        }
    }
    for (uint32& index : inoutMesh.Indices)
    {
        index = remap[index];
    }
    if (inoutMesh.SourceVertexCount == 0)
    {
        inoutMesh.SourceVertexCount = static_cast<uint32>(count);
    }
    ShrinkAttribute(inoutMesh.Positions, IR_VECTOR_COMPONENT_COUNT, uniqueCount);
    ShrinkAttribute(inoutMesh.Normals, IR_VECTOR_COMPONENT_COUNT, uniqueCount);
    ShrinkAttribute(inoutMesh.Uvs, UV_COMPONENT_COUNT, uniqueCount);
    ShrinkAttribute(inoutMesh.Tangents, IR_TANGENT_COMPONENT_COUNT, uniqueCount);
    return Status::Ok;
}
} // namespace ludus::content::scene_cook::importers
