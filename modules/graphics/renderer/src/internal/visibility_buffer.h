#pragma once

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/graph.h>

namespace ludus::graphics::renderer::internal
{
/// Private portable visibility stream: duplicated triangle corners carry exact primitive halves.
struct VisibilityVertex final
{
    foundation::math::Vector3 PositionMesh;
    foundation::math::Vector2 Primitive;
};
static_assert(sizeof(VisibilityVertex) == sizeof(foundation::math::Vector3) + sizeof(foundation::math::Vector2));
/// Cold expansion; validates input and buffer bounds, never changes outputs on failure.
[[nodiscard]] rhi::RasterStatus
ExpandVisibilityGeometry(foundation::BorrowedView<const foundation::math::Vector3> positions,
                         foundation::BorrowedView<const foundation::uint32> indices,
                         foundation::usize bufferByteLimit,
                         foundation::Array<VisibilityVertex>& outVertices,
                         foundation::Array<foundation::uint32>& outIndices) noexcept;
/// Immutable prepared packet values; no Renderer state or host geometry ownership.
struct VisibilityDraw final
{
    rhi::BufferHandle Vertices;
    rhi::BufferHandle Indices;
    foundation::uint32 VertexCount;
    foundation::uint32 IndexCount;
    foundation::usize FirstInstance;
    foundation::uint32 InstanceCount;
    foundation::usize MeshIndex;
};
/// Synchronous graph-append inputs; borrowed resources become graph-retained on success.
struct VisibilityCommands final
{
    foundation::BorrowedView<const VisibilityDraw> Draws;
    rhi::BufferHandle Instances;
    foundation::usize InstanceCount;
    foundation::usize InstanceStride;
    rhi::RasterPipelineHandle Pipeline;
    rhi::BindingSetHandle Bindings;
    rhi::RasterPassDescription Attachment;
};
/// Append one Clear/Store visibility producer; partial failure requires discarding the caller's graph.
/// outWritten is preserved on failure. Caller validates the view's opaque, unbound visibility ABI.
[[nodiscard]] rhi::RasterStatus AppendVisibilityCommands(rhi::DeviceHandle device,
                                                         rhi::OrderedGraph& inoutGraph,
                                                         const VisibilityCommands& commands,
                                                         rhi::GraphVersion previous,
                                                         rhi::GraphVersion& outWritten) noexcept;
} // namespace ludus::graphics::renderer::internal
