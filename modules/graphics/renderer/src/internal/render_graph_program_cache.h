#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer::internal
{
/// Immutable dependencies of the bilinear upscale program, shared through one RenderGraphProgramCache.
/// The cache creates and destroys every handle; leaseholders borrow them and never Destroy them.
struct RenderGraphUpscaleDependencies final
{
    rhi::DeviceHandle Device{};
    rhi::RasterShaderHandle Vertex{};
    rhi::RasterShaderHandle Fragment{};
    rhi::BindingLayoutHandle Layout{};
    rhi::SamplerHandle Sampler{};
    rhi::BufferHandle Vertices{};
    rhi::BufferHandle Indices{};
    /// Canonical field-by-field encoding of the vertex and fragment descriptions that created the set.
    foundation::Array<foundation::uint8> Canonical;
    /// Program leases currently borrowing the set; the final release destroys it.
    foundation::uint32 Leases = 0;
    /// Latest combined preparation status; a failure is terminal and the set is never shared again.
    rhi::RasterStatus Status = rhi::RasterStatus::Pending;
    /// Whether the slot holds a live set.
    bool IsLive = false;
};
/// Renderer program side of RenderGraphProgramCache; owner thread, between frames.
struct RenderGraphProgramCacheAccess final
{
    /// Whether the cache is initialized and bound to device, which is the RHI's ready owner.
    [[nodiscard]] static bool IsUsable(const RenderGraphProgramCache& cache, rhi::DeviceHandle device) noexcept;
    /// The device the cache is bound to; a default handle when the cache is uninitialized.
    [[nodiscard]] static rhi::DeviceHandle GetDevice(const RenderGraphProgramCache& cache) noexcept;
    /// Lease the bilinear upscale dependencies for these composite stages: join a live, unfailed set whose
    /// canonical description is equal, or create a new set.
    /// @pre IsUsable; the stages match the bilinear interface (validated by RenderGraphUpscaleProgram).
    /// @param outDependencies Receives the leased set on Ready/Pending; preserved on failure.
    /// @return Ready or Pending with a lease; OutOfMemory or CapacityExceeded for bookkeeping; otherwise the
    /// failing RHI creation status, after which no lease is held and nothing is retained.
    [[nodiscard]] static rhi::RasterStatus AcquireUpscale(RenderGraphProgramCache& inoutCache,
                                                          const rhi::RasterShaderDescription& vertex,
                                                          const rhi::RasterShaderDescription& fragment,
                                                          RenderGraphUpscaleDependencies*& outDependencies) noexcept;
    /// Advance and report the combined preparation status of a leased set without waiting.
    [[nodiscard]] static rhi::RasterStatus Poll(RenderGraphUpscaleDependencies& inoutDependencies) noexcept;
    /// Return a lease and clear the pointer; the final lease destroys the set's handles.
    static void Release(RenderGraphProgramCache& inoutCache,
                        RenderGraphUpscaleDependencies*& inoutDependenciesOrNull) noexcept;
};
} // namespace ludus::graphics::renderer::internal
