#pragma once

#include "render_graph_gpu_fixture.h"

#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/timers.hpp>

namespace ludus::graphics::renderer::testing
{
inline rhi::RasterStatus PollAtmosphereGraph(GraphGpuFixture& inoutFixture, rhi::RasterStatus status)
{
    foundation::time::Deadline deadline;
    REQUIRE(deadline.Arm(foundation::time::Now(),
                         foundation::time::Seconds(LUDUS_RENDERER_TEST_COLD_GPU_TIMEOUT_SECONDS)) ==
            foundation::time::TimeStatus::Ok);
    while (status == rhi::RasterStatus::Pending && deadline.IsExpired(foundation::time::Now()) == false)
    {
        foundation::threading::SleepFor(foundation::time::Milliseconds(1));
        status = inoutFixture.Graph.Poll();
    }
    return status;
}

inline void ReadAtmosphereFrame(GraphGpuFixture& inoutFixture)
{
    rhi::SubmissionToken submission;
    REQUIRE(inoutFixture.Graph.Render(inoutFixture.Surface, WIDTH, HEIGHT, submission) == rhi::RasterStatus::Ready);
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(inoutFixture.Graph.GetExportAllocation(0, texture, aspect) == rhi::RasterStatus::Ready);
    REQUIRE(aspect == rhi::TextureAspect::Color);
    rhi::ReadbackTicket readback;
    constexpr usize ROW_BYTES = WIDTH * TEXEL_BYTES;
    REQUIRE(rhi::RequestTextureReadback(inoutFixture.Device,
                                        texture,
                                        { .Width = WIDTH, .Height = HEIGHT },
                                        ROW_BYTES,
                                        readback) == rhi::RasterStatus::Pending);
    rhi::RasterStatus status = rhi::GetStatus(inoutFixture.Device, readback);
    foundation::time::Deadline deadline;
    REQUIRE(deadline.Arm(foundation::time::Now(), foundation::time::Seconds(60)) == foundation::time::TimeStatus::Ok);
    while (status == rhi::RasterStatus::Pending && deadline.IsExpired(foundation::time::Now()) == false)
    {
        foundation::threading::SleepFor(foundation::time::Milliseconds(1));
        REQUIRE(rhi::PollLifetime(inoutFixture.Device) == rhi::RasterStatus::Ready);
        status = rhi::GetStatus(inoutFixture.Device, readback);
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CopyReadback(inoutFixture.Device,
                              readback,
                              inoutFixture.Pixels.GetData(),
                              inoutFixture.Pixels.GetSize()) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Release(inoutFixture.Device, readback) == rhi::RasterStatus::Ready);
}
} // namespace ludus::graphics::renderer::testing
