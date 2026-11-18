#include "hybrid_diffuse_scene.h"
#include "hybrid_indirect_diffuse_package.h"
#include "renderer_composite.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <bit>

namespace
{
using namespace ludus::foundation;
using namespace ludus::tests::hybrid_diffuse;
namespace rhi = ludus::graphics::rhi;
namespace composite = ludus::shaders::renderer_composite;
using Status = rhi::RasterStatus;
constexpr uint32 WIDTH = 16;
constexpr uint32 HEIGHT = 16;
constexpr uint32 SHADE_PASS = 1;
constexpr uint64 TIMEOUT = 60 * time::NANOSECONDS_PER_SECOND;
constexpr int SKIPPED = 77;
constexpr uint32 WARM_FRAMES = 8;
constexpr usize WORD_COUNT = usize{WIDTH} * HEIGHT * renderer::RENDER_GRAPH_ACCUMULATION_WORDS;
using Records = StaticArray<math::Vector4, WORD_COUNT>;

template <typename Poll>
bool WaitUntilReady(Status status, Poll poll) noexcept
{
    const uint64 beginning = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - beginning < TIMEOUT)
    {
        threading::YieldThread();
        status = poll();
    }
    return status == Status::Ready;
}

bool Prepare(rhi::DeviceHandle device,
             uint64 revision,
             const Room& room,
             const renderer::RenderGraphPackage& package,
             renderer::RenderGraph& inoutGraph) noexcept
{
    const renderer::RenderGraphPrograms programs = package.GetPrograms(composite::Vertex(), composite::Fragment());
    const Status initialized = inoutGraph.Initialize(device, package.GetPlan(), programs, nullptr, WIDTH, HEIGHT);
    if (WaitUntilReady(initialized, [&] { return inoutGraph.Poll(); }) == false)
    {
        return false;
    }
    renderer::RenderGraphReport report;
    const Status updated = inoutGraph.Update(room.Meshes,
                                             room.Items,
                                             {&room.Camera, 1},
                                             revision,
                                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                             report,
                                             &room.Inputs,
                                             0,
                                             room.Revisions);
    return WaitUntilReady(updated, [&] { return inoutGraph.Poll(); });
}

bool Render(rhi::DeviceHandle device, rhi::SurfaceHandle surface, renderer::RenderGraph& inoutGraph) noexcept
{
    rhi::SubmissionToken completion;
    return inoutGraph.Render(surface, WIDTH, HEIGHT, completion) == Status::Ready &&
           WaitUntilReady(Status::Pending, [&] { return rhi::GetStatus(device, completion); }) == true;
}

bool ReadSamples(rhi::DeviceHandle device, const renderer::RenderGraph& graphInstance, Records& outRecords) noexcept
{
    rhi::BufferHandle buffer;
    if (graphInstance.GetAccumulation(SHADE_PASS, buffer) != Status::Ready)
    {
        return false;
    }
    rhi::ReadbackTicket ticket;
    const Status requested = rhi::RequestBufferReadback(device, buffer, 0, sizeof(outRecords), ticket);
    if (WaitUntilReady(requested, [&] { return rhi::GetStatus(device, ticket); }) == false)
    {
        return false;
    }
    const Status copied =
        rhi::CopyReadback(device, ticket, reinterpret_cast<uint8*>(outRecords.GetData()), sizeof(outRecords));
    return rhi::Release(device, ticket) == Status::Ready && copied == Status::Ready;
}

int Run() noexcept
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Required.Compute = true;
    description.Preferred.RayQueries = true;
    description.Storage.ResourcesPerGraph = renderer::VISIBILITY_GRAPH_RESOURCE_CAPACITY;
    rhi::StartupInfo failure;
    if (rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, device, surface, &failure) !=
        rhi::DeviceStatus::Ready)
    {
        return failure.Error == rhi::StartupError::AdapterUnavailable ? SKIPPED : 1;
    }
    rhi::RayCapabilities rays;
    if (rhi::GetRayCapabilities(device, rays) != Status::Ready)
    {
        return SKIPPED;
    }

    renderer::RenderGraphPackage package;
    if (package.Decode(HYBRID_INDIRECT_DIFFUSE_PACKAGE) != renderer::RenderGraphPackageStatus::Ready)
    {
        return 2;
    }
    Room room;
    room.Apply(STATES[0]);
    renderer::RenderGraph live;
    constexpr uint64 ORIGINAL_REVISION = 1;
    if (Prepare(device, ORIGINAL_REVISION, room, package, live) == false)
    {
        return 2;
    }
    for (usize state = 1; state < STATE_COUNT; ++state)
    {
        const uint64 oldRevision = state;
        const uint64 newRevision = state + 1;
        for (uint32 frame = 0; frame < WARM_FRAMES; ++frame)
        {
            if (Render(device, surface, live) == false)
            {
                return 3;
            }
        }

        rhi::SubmissionToken previous;
        if (live.Render(surface, WIDTH, HEIGHT, previous) != Status::Ready)
        {
            return 4;
        }
        room.Apply(STATES[state]);
        renderer::RenderGraphReport report;
        const Status staged = live.Update(room.Meshes,
                                          room.Items,
                                          {&room.Camera, 1},
                                          newRevision,
                                          renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                          report,
                                          &room.Inputs,
                                          0,
                                          room.Revisions);
        rhi::BufferHandle history;
        if (WaitUntilReady(staged, [&] { return live.Poll(); }) == false ||
            WaitUntilReady(Status::Pending, [&] { return rhi::GetStatus(device, previous); }) == false ||
            live.GetDrawableSceneRevision() != newRevision || live.GetOutputSceneRevision() != oldRevision ||
            live.GetAccumulation(SHADE_PASS, history) != Status::NotReady || live.GetSubmittedRaySamples() != 0)
        {
            return 5;
        }
        renderer::RenderGraph fresh;
        if (Prepare(device, newRevision, room, package, fresh) == false)
        {
            return 6;
        }
        Records actual{};
        Records expected{};
        for (uint32 frame = 1; frame <= 2; ++frame)
        {
            if (Render(device, surface, live) == false || Render(device, surface, fresh) == false ||
                live.GetOutputSceneRevision() != newRevision || ReadSamples(device, live, actual) == false ||
                ReadSamples(device, fresh, expected) == false)
            {
                return 7;
            }
            bool hasBounce = false;
            for (usize word = 0; word < WORD_COUNT; word += renderer::RENDER_GRAPH_ACCUMULATION_WORDS)
            {
                const math::Vector4 liveSample = actual[word];
                if (std::bit_cast<uint32>(liveSample.W) != frame || liveSample != expected[word])
                {
                    return 8;
                }
                hasBounce = hasBounce == true || liveSample.X > 0;
            }
            if (hasBounce == false)
            {
                return 9;
            }
        }
    }
    return 0;
}
} // namespace

int main()
{
    const int result = Run();
    ludus::graphics::rhi::Shutdown();
    return result;
}
