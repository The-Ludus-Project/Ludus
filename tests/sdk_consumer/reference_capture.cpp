#include "l6_scene.h"
#include "ray_query.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_lit.h"
#include "renderer_lit_shadow.h"
#include "renderer_output.h"
#include "renderer_shadow.h"
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/graphics/rhi/device.h>
#include <string_view>
// Installed-SDK evidence: CPU transport is explicit; native mode never falls back.
int main(int argc, char** argv)
{
    using namespace ludus::foundation;
    namespace rr = ludus::graphics::renderer;
    namespace rhi = ludus::graphics::rhi;
    const bool native = argc > 2 && std::string_view(argv[2]) == "native";
    rhi::DeviceDescription request;
    request.Required.PortableRaster = true;
    request.Required.RayQueries = native;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    if (rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, request, device, surface) != rhi::DeviceStatus::Ready)
        return 1;
    struct Shutdown final
    {
        ~Shutdown() noexcept
        {
            rhi::Shutdown();
        }
    } shutdown;
    rr::Renderer renderer;
    using S = rhi::RasterStatus;
    if (renderer.Initialize(device,
                            ludus::shaders::renderer_flat::Vertex(),
                            ludus::shaders::renderer_flat::Fragment()) != S::Ready ||
        renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                 ludus::shaders::renderer_composite::Fragment()) != S::Ready ||
        renderer.InitializeLighting(ludus::shaders::renderer_output::Vertex(),
                                    ludus::shaders::renderer_output::Fragment(),
                                    ludus::shaders::renderer_shadow::Fragment()) != S::Ready)
        return 2;
    const rr::ReferenceRenderingOptions options{native ? rr::ReferenceTraversal::NativeQueries
                                                       : rr::ReferenceTraversal::CpuOracle,
                                                usize{32} * 1024 * 1024,
                                                usize{64} * 1024 * 1024,
                                                512};
    if (renderer.EnableReferenceRendering(ludus::shaders::ray_query::Query(), options) != S::Ready)
        return 3;
    rr::MaterialProgram program;
    auto status = renderer.PrewarmLitProgram(ludus::shaders::renderer_lit::Vertex(),
                                             ludus::shaders::renderer_lit::Fragment(),
                                             ludus::shaders::renderer_lit_shadow::Fragment(),
                                             program);
    const auto programStarted = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - programStarted < 10 * time::NANOSECONDS_PER_SECOND)
        status = renderer.GetStatus(program);
    if (status != S::Ready)
        return 4;
    rr::Snapshot snapshot;
    if (ludus::qa::BuildL6Snapshot(renderer, program, snapshot) != S::Ready)
        return 5;
    rr::ReferenceCaptureDescription description;
    description.OrthographicCamera = true;
    description.Width = description.Height = 32;
    description.TargetSamples = 256;
    description.MaxDepth = 4;
    description.Seed = 871;
    description.Environment = {.3F, .4F, .5F};
    rr::ReferenceCapture capture;
    status = renderer.BeginReferenceCapture(snapshot, description, capture);
    if (status != S::Ready && status != S::Pending)
        return 6;
    status = S::Pending;
    const auto captureStarted = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - captureStarted < 45 * time::NANOSECONDS_PER_SECOND)
        status = renderer.AdvanceReferenceCapture(capture);
    if (status != S::Ready)
        return 7;
    rr::ReferenceCaptureInfo info;
    if (renderer.GetReferenceCaptureInfo(capture, info) != S::Ready || info.InvalidContributions != 0 ||
        info.Complete == false)
        return 8;
    Array<uint8> bytes;
    if (bytes.TryResize(256 + usize(description.Width) * description.Height * 16) == false)
        return 9;
    usize written = 0;
    if (renderer.EncodeReferenceHdr(capture, bytes.GetData(), bytes.GetSize(), written) != S::Ready)
        return 10;
    filesystem::WriteDirectory directory;
    if (directory.Open(".").Succeeded() == false)
        return 11;
    const auto output = argc > 1 ? std::string_view(argv[1]) : std::string_view("reference.luhdr");
    if (directory.Publish(output, {bytes.GetData(), written}).Outcome.Succeeded() == false)
        return 12;
    return 0;
}
