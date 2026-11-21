#include "player_renderer.h"
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/views.hpp>

#include "player_raster.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>

namespace ludus::first_person
{
using namespace foundation;
namespace rr = graphics::renderer;
namespace rhi = graphics::rhi;
namespace graph = render_graph_packages::player_raster;
using Status = rhi::RasterStatus;
namespace
{
template <typename Poll>
bool WaitReady(Status status, const Poll& poll) noexcept
{
    constexpr uint64 PREPARE_TIMEOUT = 60 * time::NANOSECONDS_PER_SECOND;
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < PREPARE_TIMEOUT)
    {
        threading::YieldThread();
        status = poll();
    }
    return status == Status::Ready;
}
} // namespace
bool PlayerRenderer::Open(rhi::DeviceHandle device) noexcept
{
    mDevice = device;
    return WaitReady(
               mCompositor.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment()),
               [&] { return mCompositor.Poll(); }) == true &&
           WaitReady(mCompositor.InitializeViews(shaders::renderer_composite::Vertex(),
                                                 shaders::renderer_composite::Fragment()),
                     [&] { return mCompositor.PollViews(); }) == true;
}

bool PlayerRenderer::Resize(uint32 width, uint32 height) noexcept
{
    if (mWidth == 0)
    {
        const rr::RenderGraphPlan plan = graph::MakePlan();
        if (WaitReady(mGraph.Initialize(mDevice,
                                        plan,
                                        graph::MakePrograms(shaders::renderer_composite::Vertex(),
                                                            shaders::renderer_composite::Fragment()),
                                        nullptr,
                                        width,
                                        height),
                      [&] { return mGraph.Poll(); }) == false)
        {
            return false;
        }
        bool hasColorExport = false;
        for (usize i = 0; i < plan.Resources.GetSize(); ++i)
        {
            if (plan.Resources[i].IsExported != 0 &&
                plan.Resources[i].Kind == content::render_graph_format::Aspect::Color)
            {
                mColorResource = static_cast<uint32>(i);
                hasColorExport = true;
                break;
            }
        }
        if (hasColorExport == false)
        {
            return false;
        }
    }
    else
    {
        const Status resized = mGraph.Resize(width, height);
        if (resized != Status::Ready && resized != Status::Pending)
        {
            return false;
        }
    }
    mWidth = width;
    mHeight = height;
    mShouldPreparePresentation = true;
    return true;
}

bool PlayerRenderer::preparePresentation() noexcept
{
    if (mHasPresentation == true && mCompositor.Release(mPresentation) != Status::Ready)
    {
        return false;
    }
    rr::ViewMapping mapping;
    rhi::TextureHandle output;
    rhi::TextureAspect aspect;
    if (rr::ResolveViewMapping({static_cast<float32>(mWidth), static_cast<float32>(mHeight), rr::ScreenFit::Stretch},
                               {0, 0, mWidth, mHeight},
                               mWidth,
                               mHeight,
                               mapping) != Status::Ready ||
        mGraph.GetExportAllocation(mColorResource, output, aspect) != Status::Ready ||
        WaitReady(mCompositor.PreparePresentation(output, mapping, mPresentation),
                  [&] { return mCompositor.GetStatus(mPresentation); }) == false)
    {
        return false;
    }
    mHasPresentation = true;
    mShouldPreparePresentation = false;
    return true;
}

bool PlayerRenderer::Update(const PlayerScene& scene, const rr::ViewDescription& view) noexcept
{
    rr::RenderGraphReport report;
    if (WaitReady(mGraph.Update(scene.Meshes.AsView(),
                                scene.Items.AsView(),
                                {&view, 1},
                                ++mViewRevision,
                                rr::RenderGraphCoveragePolicy::RejectIncomplete,
                                report,
                                &scene.Inputs,
                                1),
                  [&] { return mGraph.Poll(); }) == false)
    {
        return false;
    }
    // The graph publishes replacement attachments only with a drawable scene at the new size.
    return mShouldPreparePresentation == false || preparePresentation() == true;
}
Status PlayerRenderer::Present(rhi::SurfaceHandle surface) noexcept
{
    const rhi::DeviceStatus acquired = rhi::BeginFrame(mDevice, surface);
    if (acquired != rhi::DeviceStatus::Ready)
    {
        return acquired == rhi::DeviceStatus::Skipped ? Status::NotReady : Status::Failed;
    }
    const bool isDrawn =
        mGraph.Encode(surface) == Status::Ready && mCompositor.DrawPresentation(mPresentation) == Status::Ready;
    rhi::SubmissionToken completion;
    const Status ended = rhi::EndFrame(mDevice, completion);
    const Status confirmed =
        mGraph.HasUnconfirmedEncoding() == true ? mGraph.ConfirmSubmission(completion) : Status::Failed;
    if (ended != Status::Ready || isDrawn == false || WaitReady(confirmed, [&] {
                                                          static_cast<void>(rhi::PollLifetime(mDevice));
                                                          return rhi::GetStatus(mDevice, completion);
                                                      }) == false)
    {
        return Status::Failed;
    }
    return Status::Ready;
}
bool PlayerRenderer::ReadFixturePixel(uint32 x, uint32 y, uint32& outRgb) noexcept
{
    rhi::TextureHandle output;
    rhi::TextureAspect aspect;
    if (mGraph.GetExport(mColorResource, output, aspect) != Status::Ready)
    {
        return false;
    }
    constexpr usize CHANNEL_COUNT = content::scene_format::TEXEL_BYTES;
    StaticArray<uint8, CHANNEL_COUNT> pixel;
    rhi::ReadbackTicket ticket;
    const Status requested = rhi::RequestTextureReadback(mDevice,
                                                         output,
                                                         {
                                                             .X = x,
                                                             .Y = y,
                                                             .Width = 1,
                                                             .Height = 1,
                                                         },
                                                         CHANNEL_COUNT,
                                                         ticket);
    if (requested != Status::Ready && requested != Status::Pending)
    {
        return false;
    }
    const bool isRead = WaitReady(requested,
                                  [&] {
                                      static_cast<void>(rhi::PollLifetime(mDevice));
                                      return rhi::GetStatus(mDevice, ticket);
                                  }) == true &&
                        rhi::CopyReadback(mDevice, ticket, pixel.GetData(), pixel.GetSize()) == Status::Ready;
    static_cast<void>(rhi::Release(mDevice, ticket));
    if (isRead == true)
    {
        constexpr uint32 CHANNEL_BITS = 8;
        outRgb = (static_cast<uint32>(pixel[0]) << (2 * CHANNEL_BITS)) |
                 (static_cast<uint32>(pixel[1]) << CHANNEL_BITS) | static_cast<uint32>(pixel[2]);
    }
    return isRead;
}

rr::RenderGraphReport PlayerRenderer::GetReport() const noexcept
{
    return mGraph.GetReport();
}
} // namespace ludus::first_person
