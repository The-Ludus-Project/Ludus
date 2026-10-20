#pragma once

#include "reference_data.h"
#include <ludus/graphics/renderer/render_graph.hpp>

#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::graphics::renderer::internal
{
/// Borrowed immutable mesh plus the graph's explicit flat material and World transform.
struct GraphRaySceneInput final
{
    const ReferenceMeshData* MeshOrNull = nullptr;
    math::Affine3 MeshToWorld{};
    math::Vector3 Color{};
    uint64 SourceId = 0;
};
/// Freeze only the triangle ray profile's inputs. No lighting or CPU transport.
/// Consumes borrowed inputs before return, copies every surface, and builds the
/// shared BVH against a bounded setup budget. Failure preserves a null job.
[[nodiscard]] rhi::RasterStatus BuildGraphRayScene(foundation::BorrowedView<const GraphRaySceneInput> inputs,
                                                   const ReferenceCaptureDescription& description,
                                                   usize byteBudget,
                                                   core::UniquePtr<ReferenceJob>& outJob) noexcept;
/// Freeze material-bound graph geometry, maps and lights, preserving output on failure.
[[nodiscard]] rhi::RasterStatus BuildMaterialGraphRayScene(const RenderGraphSceneDescription& scene,
                                                           const ReferenceCaptureDescription& description,
                                                           usize byteBudget,
                                                           core::UniquePtr<ReferenceJob>& outJob) noexcept;
} // namespace ludus::graphics::renderer::internal
