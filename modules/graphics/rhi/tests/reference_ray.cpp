#include "internal/ray_backend.h"
#include "reference_raster.h"

// R16a/R17a reference ray backend: the static-triangle inline-query profile with controllable
// build completion, so GDI admission, rollback and lifetime run without native ray hardware.
// No traversal happens here; hit/miss results are qualified by the native ray tests.
namespace ludus::graphics::rhi::backend
{
namespace
{
constexpr foundation::uint32 REFERENCE_RAY_INSTANCES = 16;
constexpr foundation::uint32 REFERENCE_RAY_COUNT = 64;
constexpr foundation::usize REFERENCE_RAY_BUFFER_BYTES = foundation::usize{1} << 20;
RasterStatus StartBuild(RayAllocation& outAllocation) noexcept
{
    outAllocation = {reference::RayResidentBytes, reference::RayScratchBytes};
    return reference::RayBuildResult;
}
} // namespace
RayCapabilities RayLimits() noexcept
{
    if (reference::IsRayAvailable == false)
    {
        return {};
    }
    RayCapabilities limits;
    limits.StaticTriangles = true;
    limits.InlineQueries = true;
    limits.Updates = true;
    limits.CanCompact = true;
    limits.MaxInstances = REFERENCE_RAY_INSTANCES;
    limits.MaxRays = REFERENCE_RAY_COUNT;
    limits.MaxBufferBytes = REFERENCE_RAY_BUFFER_BYTES;
    return limits;
}
RasterStatus RayCreateProgram(foundation::usize, const ShaderDescription&) noexcept
{
    return reference::IsRayAvailable == true ? RasterStatus::Ready : RasterStatus::Unsupported;
}
RasterStatus RayCreateGeometry(foundation::usize, const RayGeometryDescription&, RayAllocation& outAllocation) noexcept
{
    return reference::IsRayAvailable == true ? StartBuild(outAllocation) : RasterStatus::Unsupported;
}
RasterStatus
RayCreateScene(foundation::usize, const internal::NativeRaySceneDescription&, RayAllocation& outAllocation) noexcept
{
    return reference::IsRayAvailable == true ? StartBuild(outAllocation) : RasterStatus::Unsupported;
}
RasterStatus RayUpdateGeometry(foundation::usize,
                               foundation::usize,
                               const RayGeometryDescription&,
                               RayAllocation& outAllocation) noexcept
{
    ++reference::RayGeometryUpdates;
    return StartBuild(outAllocation);
}
RasterStatus RayUpdateScene(foundation::usize,
                            foundation::usize,
                            const internal::NativeRaySceneDescription&,
                            RayAllocation& outAllocation) noexcept
{
    ++reference::RaySceneUpdates;
    return StartBuild(outAllocation);
}
RasterStatus RayCompactGeometry(foundation::usize,
                                const internal::NativeRayCompaction& compaction,
                                RayAllocation& outAllocation) noexcept
{
    ++reference::RayCompactions;
    if (reference::RayCompactedBytes > compaction.ByteBudget)
    {
        return RasterStatus::CapacityExceeded;
    }
    outAllocation = {reference::RayCompactedBytes, 0};
    return reference::RayBuildResult;
}
RasterStatus RaySubmit(foundation::usize, const internal::NativeRaySubmission&, RayAllocation&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RayPoll(internal::RasterKind, foundation::usize) noexcept
{
    return reference::RayBuildPoll;
}
RasterStatus RayRead(foundation::usize, RayResult*, foundation::usize) noexcept
{
    return RasterStatus::Unsupported;
}
void RayDestroy(internal::RasterKind, foundation::usize) noexcept {}
void RayDrain() noexcept {}
} // namespace ludus::graphics::rhi::backend
