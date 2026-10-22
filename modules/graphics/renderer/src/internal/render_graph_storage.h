#pragma once

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/graph.h>

namespace ludus::graphics::renderer::internal
{
/// Independent peak upload scratch and retained GPU byte ceilings for one preparation.
struct RenderGraphStorageBudget final
{
    foundation::usize ScratchBytes;
    foundation::usize GpuBytes;
};
/// Per-job storage lifetime. The device outlives this owner; all operations use its
/// owner thread between frames. Reset retires pending native uses through the RHI.
class RenderGraphStorage final
{
public:
    ~RenderGraphStorage() noexcept;
    RenderGraphStorage() noexcept = default;
    RenderGraphStorage(const RenderGraphStorage&) = delete;
    RenderGraphStorage& operator=(const RenderGraphStorage&) = delete;
    [[nodiscard]] rhi::RasterStatus
    Initialize(rhi::DeviceHandle device,
               foundation::BorrowedView<const content::render_graph_format::StorageBuffer> descriptions,
               foundation::uint32 width,
               foundation::uint32 height,
               RenderGraphStorageBudget budget) noexcept;
    [[nodiscard]] rhi::RasterStatus Poll() const noexcept;
    [[nodiscard]] rhi::RasterStatus AppendUses(rhi::OrderedGraph graph,
                                               foundation::BorrowedView<rhi::GraphUse> outUses) const noexcept;
    void Reset() noexcept;
    [[nodiscard]] LUDUS_INLINE foundation::usize GetCount() const noexcept
    {
        return mCount;
    }
    [[nodiscard]] LUDUS_INLINE foundation::usize GetBytes() const noexcept
    {
        return mBytes;
    }
    [[nodiscard]] rhi::RasterBindingResource GetBinding(foundation::usize index) const noexcept;

private:
    rhi::DeviceHandle mDevice{};
    foundation::usize mCount = 0;
    foundation::usize mBytes = 0;
    foundation::StaticArray<content::render_graph_format::StorageBuffer,
                            content::render_graph_format::MAX_STORAGE_BUFFERS>
        mDescriptions{};
    foundation::StaticArray<rhi::BufferHandle, content::render_graph_format::MAX_STORAGE_BUFFERS> mBuffers{};
    foundation::StaticArray<foundation::usize, content::render_graph_format::MAX_STORAGE_BUFFERS> mSizes{};
};
} // namespace ludus::graphics::renderer::internal
