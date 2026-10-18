#include "internal/static_lighting_job.h"

#include <ludus/foundation/base/pointer.hpp>
#include <new>

namespace ludus::editor
{
using namespace foundation;
namespace rr = graphics::renderer;
namespace rhi = graphics::rhi;
StaticLightingJob::StaticLightingJob(const RenderScene& scene, const rr::StaticLightingBakeDescription& description)
    : mScene(scene), mDescription(description)
{
    if (description.Surfaces.IsValid() == false)
    {
        mStatus = rhi::RasterStatus::InvalidDescription;
    }
    else if (description.Surfaces.IsEmpty() == false &&
             mSurfaces.TryAddRange({description.Surfaces.GetDataOrNull(), description.Surfaces.GetSize()}) == false)
    {
        mStatus = rhi::RasterStatus::OutOfMemory;
    }
    mDescription.Surfaces = {};
}
StaticLightingJob::~StaticLightingJob() noexcept
{
    // The bake destructor cancels and drains its submission before freeing contexts.
    mBake.Reset();
}
rhi::RasterStatus StaticLightingJob::Start(threading::JobSystem& inoutSystem) noexcept
{
    if (mHasStarted == true)
    {
        return rhi::RasterStatus::InvalidState;
    }
    mHasStarted = true;
    if (mStatus != rhi::RasterStatus::Pending)
    {
        return mStatus;
    }
    mBake.Reset(new (std::nothrow) rr::StaticLightingBake);
    if (mBake.Get() == nullptr || mScene.Definition == nullptr)
    {
        mStatus = rhi::RasterStatus::OutOfMemory;
        return mStatus;
    }
    mDescription.Surfaces = {mSurfaces.GetData(), mSurfaces.GetSize()};
    // Submit copies the overrides, so the job's array need not stay borrowed.
    mStatus = mBake->Submit(inoutSystem,
                            *mScene.Definition,
                            mDescription,
                            {
                                .RangeCount = STATIC_LIGHTING_RANGE_COUNT,
                            });
    mDescription.Surfaces = {};
    return mStatus;
}
rhi::RasterStatus StaticLightingJob::Poll() noexcept
{
    if (mStatus == rhi::RasterStatus::Pending && mBake.Get() != nullptr)
    {
        mStatus = mBake->Poll();
    }
    return mHasStarted == true ? mStatus : rhi::RasterStatus::InvalidState;
}
rhi::RasterStatus StaticLightingJob::Wait() noexcept
{
    if (mStatus == rhi::RasterStatus::Pending && mBake.Get() != nullptr)
    {
        mStatus = mBake->Finish();
    }
    return mHasStarted == true ? mStatus : rhi::RasterStatus::InvalidState;
}
void StaticLightingJob::RequestInterruption() noexcept
{
    if (mBake.Get() != nullptr)
    {
        mBake->Cancel();
    }
}
uint32 StaticLightingJob::GetProgressPercent() const noexcept
{
    rr::StaticLightingBakeProgress progress;
    if (mBake.Get() != nullptr)
    {
        mBake->GetProgress(progress);
    }
    return progress.TotalSamples == 0 ? 0
                                      : static_cast<uint32>(progress.CompletedSamples * 100 / progress.TotalSamples);
}
rhi::RasterStatus StaticLightingJob::GetDiagnostics(rr::StaticLightingBakeDiagnostics& outDiagnostics) const noexcept
{
    return mBake.Get() != nullptr ? mBake->GetDiagnostics(outDiagnostics) : rhi::RasterStatus::InvalidState;
}
rhi::RasterStatus StaticLightingJob::GetSkippedTriangleCount(usize objectIndex, uint32& outCount) const noexcept
{
    return mBake.Get() != nullptr ? mBake->GetSkippedTriangleCount(objectIndex, outCount)
                                  : rhi::RasterStatus::InvalidState;
}
UniquePtr<rr::StaticLightingBake> StaticLightingJob::TakeBake() noexcept
{
    LUDUS_REQUIRE(mStatus == rhi::RasterStatus::Ready);
    return Move(mBake);
}
} // namespace ludus::editor
