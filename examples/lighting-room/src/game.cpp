#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/input/keyboard_event.h>
#include <ludus/platform/base/window.h>

#include <cstdio>
#include <string_view>

#include "forward_lit.h"
#include "observations.h"
#include "renderer_composite.h"
#include "renderer_flat.h"

namespace
{
using namespace ludus::foundation;
using namespace lighting_room;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace platform = ludus::platform;
namespace input = ludus::input;
namespace graph = ludus::render_graph_packages::forward_lit;
using Status = rhi::RasterStatus;
LUDUS_DEFINE_LOG_CATEGORY(LOG_ROOM, "LightingRoom");

struct Plane final
{
    math::Vector3 PositionWorld;
    math::Vector3 AxisX;
    math::Vector3 AxisY;
    math::Vector3 AxisZ;
    math::Vector3 Color;
    bool IsVisible;
};
#include "lighting_world.h"

constexpr uint32 WIDTH = 640;
constexpr uint32 HEIGHT = 480;
constexpr usize PLANE_COUNT = sizeof(PLANES) / sizeof(Plane);
constexpr usize LIGHT_COUNT = sizeof(LIGHTS) / sizeof(rr::DirectLight);
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1000000;
constexpr uint64 WAIT_NANOSECONDS = 30000000000ULL;
constexpr float32 NEAR_METRES = .01F;
constexpr float32 FAR_METRES = 20;
constexpr float32 VERTICAL_FOV_RADIANS = .8F;
constexpr math::Vector3 POSITIONS[] = {{-.5F, 0, -.5F}, {-.5F, 0, .5F}, {.5F, 0, .5F}, {.5F, 0, -.5F}};
constexpr math::Vector3 NORMALS[] = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
constexpr uint32 INDICES[] = {0, 1, 2, 0, 2, 3};
constexpr usize VERTEX_COUNT = sizeof(POSITIONS) / sizeof(math::Vector3);
constexpr usize INDEX_COUNT = sizeof(INDICES) / sizeof(uint32);

template <typename Poll>
bool WaitForReady(Status status, const Poll& poll) noexcept
{
    const uint64 started = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - started < WAIT_NANOSECONDS)
    {
        threading::YieldThread();
        status = poll();
    }
    return status == Status::Ready;
}

bool MakeView(const Observation& observation, uint32 width, uint32 height, rr::ViewDescription& outView) noexcept
{
    math::Vector3 targetWorld{0, .5F, 0};
    math::Vector3 eyeWorld = OVERVIEW_EYE;
    switch (observation.Target)
    {
        case Patch::Overview:
            break;
        case Patch::Bounce:
            targetWorld = PLANES[BOUNCE_RECEIVER].PositionWorld +
                          (PLANES[BOUNCE_RECEIVER].AxisZ - PLANES[BOUNCE_RECEIVER].AxisX) * TRIANGLE_CENTROID_OFFSET;
            eyeWorld = targetWorld + BOUNCE_VIEW_OFFSET;
            break;
        case Patch::Rough:
            targetWorld = PLANES[ROUGH_DIELECTRIC].PositionWorld + ROUGH_PATCH_OFFSET;
            eyeWorld = targetWorld + (observation.IsOblique == true ? OBLIQUE_VIEW_OFFSET : PEAK_VIEW_OFFSET);
            break;
        case Patch::Moving:
            targetWorld = RECEIVER_PATH[observation.PathIndex];
            eyeWorld = targetWorld + BOUNCE_VIEW_OFFSET;
            break;
    }
    math::Matrix4 worldToView;
    math::Matrix4 viewToClip;
    if (math::TryLookAt(eyeWorld, targetWorld, {0, 1, 0}, worldToView) != math::MathStatus::Success ||
        math::TryPerspectiveReverseZ(VERTICAL_FOV_RADIANS,
                                     static_cast<float32>(width) / static_cast<float32>(height),
                                     NEAR_METRES,
                                     FAR_METRES,
                                     viewToClip) != math::MathStatus::Success)
    {
        return false;
    }
    // The installed Forward Lit pass compares Less. Map reverse-Z to conventional clip depth.
    for (math::Vector4& column : viewToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    outView.WorldToClip = viewToClip * worldToView;
    outView.CameraPosition = eyeWorld;
    return true;
}

struct Room final
{
    rr::RenderGraph Graph;
    rr::Renderer Compositor;
    rr::Presentation Presentation;
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    rhi::TextureHandle Output;
    uint32 Width = 0;
    uint32 Height = 0;
    uint64 Revision = 0;
};

bool PrepareSize(Room& inoutRoom, uint32 width, uint32 height) noexcept
{
    rr::RenderGraph& renderGraph = inoutRoom.Graph;
    const bool isFirst = inoutRoom.Width == 0;
    if (isFirst == false && inoutRoom.Compositor.Release(inoutRoom.Presentation) != Status::Ready)
    {
        return false;
    }
    const Status status =
        isFirst == true ? renderGraph.Initialize(inoutRoom.Device,
                                                 graph::MakePlan(),
                                                 graph::MakePrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                     ludus::shaders::renderer_composite::Fragment()),
                                                 nullptr,
                                                 width,
                                                 height)
                        : renderGraph.Resize(width, height);
    if (WaitForReady(status, [&]() { return renderGraph.Poll(); }) == false)
    {
        return false;
    }
    rr::ViewMapping mapping;
    rhi::TextureAspect aspect;
    if (renderGraph.GetExportAllocation(0, inoutRoom.Output, aspect) != Status::Ready ||
        rr::ResolveViewMapping({static_cast<float32>(width), static_cast<float32>(height), rr::ScreenFit::Stretch},
                               {0, 0, width, height},
                               width,
                               height,
                               mapping) != Status::Ready)
    {
        return false;
    }
    inoutRoom.Width = width;
    inoutRoom.Height = height;
    return WaitForReady(inoutRoom.Compositor.PreparePresentation(inoutRoom.Output, mapping, inoutRoom.Presentation),
                        [&]() { return inoutRoom.Compositor.GetStatus(inoutRoom.Presentation); });
}

bool UpdateRoom(Room& inoutRoom, const Observation& observation) noexcept
{
    rr::SceneShaderMaterial materials[PLANE_COUNT]{};
    rr::RenderGraphItem items[PLANE_COUNT]{};
    uint32 bindings[PLANE_COUNT]{};
    rr::RenderGraphMesh mesh;
    mesh.Geometry.Positions = POSITIONS;
    mesh.Geometry.Normals = NORMALS;
    mesh.Geometry.VertexCount = VERTEX_COUNT;
    mesh.Geometry.Indices = INDICES;
    mesh.Geometry.IndexCount = INDEX_COUNT;
    const uint32 defaultMaterial = 0;
    mesh.MaterialBindings = {&defaultMaterial, 1};
    usize count = 0;
    for (usize i = 0; i < PLANE_COUNT; ++i)
    {
        const Plane& plane = PLANES[i];
        if ((i == BOUNCE_BLOCKER ? observation.HasBlocker : plane.IsVisible) == false)
        {
            continue;
        }
        bindings[i] = static_cast<uint32>(i);
        materials[i].BaseColor = i == BOUNCE_PANEL && observation.HasNeutralPanel == true ? NEUTRAL_COLOR : plane.Color;
        materials[i].Roughness = .5F;
        rr::RenderGraphItem& item = items[count++];
        item.SourceId = i + 1;
        item.MeshToWorld = {{plane.AxisX, plane.AxisY, plane.AxisZ},
                            i == MOVING_RECEIVER ? RECEIVER_PATH[observation.PathIndex] : plane.PositionWorld};
        item.MaterialOverrides = {&bindings[i], 1};
    }
    rr::DirectLight lights[LIGHT_COUNT]{};
    usize lightCount = 0;
    if (observation.HasSelectedLight == true)
    {
        lights[lightCount++] = LIGHTS[ALWAYS_ON];
    }
    if (observation.HasLiveLight == true)
    {
        lights[lightCount++] = LIGHTS[SWITCHABLE];
    }
    rr::SceneShaderInputs inputs;
    inputs.Materials = {materials, PLANE_COUNT};
    inputs.Lights = {lights, lightCount};
    rr::ViewDescription view;
    rr::RenderGraphReport report;
    ++inoutRoom.Revision;
    return MakeView(observation, inoutRoom.Width, inoutRoom.Height, view) == true &&
           WaitForReady(inoutRoom.Graph.Update({&mesh, 1},
                                               {items, count},
                                               {&view, 1},
                                               inoutRoom.Revision,
                                               rr::RenderGraphCoveragePolicy::RejectIncomplete,
                                               report,
                                               &inputs,
                                               inoutRoom.Revision),
                        [&]() { return inoutRoom.Graph.Poll(); });
}

bool DrawFrame(Room& inoutRoom) noexcept
{
    const bool isEncoded = inoutRoom.Graph.Encode(inoutRoom.Surface) == Status::Ready &&
                           inoutRoom.Compositor.DrawPresentation(inoutRoom.Presentation) == Status::Ready;
    rhi::SubmissionToken completion;
    if (rhi::EndFrame(inoutRoom.Device, completion) != Status::Ready || isEncoded == false)
    {
        return false;
    }
    const Status confirmed = inoutRoom.Graph.ConfirmSubmission(completion);
    return WaitForReady(confirmed, [&]() {
        static_cast<void>(rhi::PollLifetime(inoutRoom.Device));
        return rhi::GetStatus(inoutRoom.Device, completion);
    });
}

bool RecordPatch(const Room& room, const Observation& observation) noexcept
{
    constexpr usize RGBA_BYTES = 4;
    uint8 pixel[RGBA_BYTES]{};
    rhi::TextureHandle output;
    rhi::TextureAspect aspect;
    if (room.Graph.GetExport(0, output, aspect) != Status::Ready)
    {
        return false;
    }
    rhi::ReadbackTicket ticket;
    const Status requested =
        rhi::RequestTextureReadback(room.Device,
                                    output,
                                    { .X = room.Width / 2, .Y = room.Height / 2, .Width = 1, .Height = 1 },
                                    RGBA_BYTES,
                                    ticket);
    if (WaitForReady(requested, [&]() {
            static_cast<void>(rhi::PollLifetime(room.Device));
            return rhi::GetStatus(room.Device, ticket);
        }) == false)
    {
        return false;
    }
    const bool isCopied = rhi::CopyReadback(room.Device, ticket, pixel, sizeof(pixel)) == Status::Ready;
    static_cast<void>(rhi::Release(room.Device, ticket));
    if (isCopied == false)
    {
        return false;
    }
    rhi::GpuTiming timing;
    if (rhi::GetGpuTiming(room.Device, timing) != Status::Ready || timing.State != rhi::GpuTimingState::Available)
    {
        return false;
    }
    const rr::RenderGraphSceneResourceReport resources = room.Graph.GetReport().Resources;
    std::printf("%s rgb8=%u %u %u chart_draws=%zu probe_draws=%zu gpu_ms=%.6f\n",
                observation.Name,
                static_cast<uint32>(pixel[0]),
                static_cast<uint32>(pixel[1]),
                static_cast<uint32>(pixel[2]),
                resources.SurfaceChartDraws,
                resources.ProbeReceiverDraws,
                timing.DurationNanoseconds / NANOSECONDS_PER_MILLISECOND);
    return resources.SurfaceChartDraws == 0 && resources.ProbeReceiverDraws == 0;
}

struct Controls final
{
    usize Selected = 0;
    Observation Current = OBSERVATIONS[0];
    bool IsChanged = true;

    void Advance() noexcept
    {
        Selected = (Selected + 1) % OBSERVATION_COUNT;
        Current = OBSERVATIONS[Selected];
        IsChanged = true;
    }
};

void HandleKey(void* context, const input::KeyboardRecord& record) noexcept
{
    if (record.Transition != input::KeyTransition::Down || record.Repeat == true)
    {
        return;
    }
    Controls& controls = *static_cast<Controls*>(context);
    switch (record.PhysicalKey)
    {
        case input::Key::Space:
            controls.Advance();
            break;
        case input::Key::KeyL:
            controls.Current.HasLiveLight = controls.Current.HasLiveLight == false;
            break;
        case input::Key::ArrowRight:
            controls.Current.PathIndex = (controls.Current.PathIndex + 1) % PATH_COUNT;
            break;
        case input::Key::ArrowLeft:
            controls.Current.PathIndex = (controls.Current.PathIndex + PATH_COUNT - 1) % PATH_COUNT;
            break;
        default:
            return;
    }
    controls.IsChanged = true;
}

bool Play(Room& inoutRoom, platform::Window& inoutWindow, bool shouldQualify, uint64 started) noexcept
{
    if (WaitForReady(inoutRoom.Compositor.Initialize(inoutRoom.Device,
                                                     ludus::shaders::renderer_flat::Vertex(),
                                                     ludus::shaders::renderer_flat::Fragment()),
                     [&]() { return inoutRoom.Compositor.Poll(); }) == false ||
        WaitForReady(inoutRoom.Compositor.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                                          ludus::shaders::renderer_composite::Fragment()),
                     [&]() { return inoutRoom.Compositor.PollViews(); }) == false)
    {
        LUDUS_LOG_ERROR(LOG_ROOM, "Could not initialize the presentation shaders.");
        return false;
    }
    Controls controls;
    bool isFirstFrame = true;
    bool isTimingArmed = false;
    if (shouldQualify == false)
    {
        inoutWindow.AttachKeyboardSink({ .OnRecord = HandleKey, .UserData = &controls });
    }
    while (inoutWindow.HandleEvent({}) == true)
    {
        const platform::NativeWindowInfo window = inoutWindow.GetNativeWindowInfo();
        if (window.Width == 0 || window.Height == 0)
        {
            threading::YieldThread();
            continue;
        }
        const bool isResized = window.Width != inoutRoom.Width || window.Height != inoutRoom.Height;
        if (isResized == true &&
            (rhi::SetFrameTarget(inoutRoom.Device,
                                 inoutRoom.Surface,
                                 { .Width = window.Width, .Height = window.Height }) != rhi::DeviceStatus::Ready ||
             PrepareSize(inoutRoom, window.Width, window.Height) == false))
        {
            LUDUS_LOG_ERROR(LOG_ROOM, "Could not prepare the drawable size.");
            return false;
        }
        if ((isResized == true || controls.IsChanged == true) && UpdateRoom(inoutRoom, controls.Current) == false)
        {
            LUDUS_LOG_ERROR(LOG_ROOM,
                            "World update failed at stage {}",
                            static_cast<uint32>(inoutRoom.Graph.GetLastFailureStage()));
            return false;
        }
        controls.IsChanged = false;
        if (shouldQualify == true && isTimingArmed == false)
        {
            if (rhi::ArmGpuTiming(inoutRoom.Device) != Status::Ready)
            {
                return false;
            }
            isTimingArmed = true;
        }
        const rhi::DeviceStatus acquired = rhi::BeginFrame(inoutRoom.Device, inoutRoom.Surface);
        if (acquired == rhi::DeviceStatus::Skipped)
        {
            // A hosted consumer may still own every image. Pump events before trying again.
            threading::YieldThread();
            continue;
        }
        isTimingArmed = false;
        if (acquired != rhi::DeviceStatus::Ready || DrawFrame(inoutRoom) == false)
        {
            LUDUS_LOG_ERROR(LOG_ROOM, "Could not present the Forward Lit frame.");
            return false;
        }
        if (isFirstFrame == true)
        {
            if (shouldQualify == true)
            {
                rhi::MeasurementAdapter adapter;
                if (rhi::GetMeasurementAdapter(inoutRoom.Device, adapter) != Status::Ready)
                {
                    return false;
                }
                std::printf("Metal adapter=%s\n", adapter.Name);
            }
            std::printf("Forward Lit world=%s hosted=%u size=%ux%u startup_ms=%.3f\n",
                        WORLD_SHA256,
                        static_cast<uint32>(window.System == platform::WindowSystem::Hosted),
                        window.Width,
                        window.Height,
                        static_cast<float64>(time::NowTicks() - started) / NANOSECONDS_PER_MILLISECOND);
        }
        isFirstFrame = false;
        if (shouldQualify == true)
        {
            if (RecordPatch(inoutRoom, controls.Current) == false)
            {
                LUDUS_LOG_ERROR(LOG_ROOM, "Failed to measure {}", controls.Current.Name);
                return false;
            }
            controls.Advance();
            if (controls.Selected == 0)
            {
                return true;
            }
        }
    }
    return shouldQualify == false;
}

bool Run(bool shouldQualify) noexcept
{
    const uint64 started = time::NowTicks();
    platform::WindowManager manager;
    core::UniquePtr<platform::Window> window;
    if (manager.Initialize({}) == false ||
        manager.CreateWindow({ .Name = "Lighting Room", .Width = WIDTH, .Height = HEIGHT }, window) == false)
    {
        LUDUS_LOG_ERROR(LOG_ROOM, "Open Lighting Room in a macOS desktop session.");
        return false;
    }
    bool isSuccessful = false;
    {
        Room room;
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.EnableGpuTiming = shouldQualify;
        const rhi::DeviceStatus status = rhi::CreateDevice({ .Name = "Lighting Room", .Version = 1 },
                                                           window->GetNativeWindowInfo(),
                                                           description,
                                                           room.Device,
                                                           room.Surface);
        if (status == rhi::DeviceStatus::Ready)
        {
            isSuccessful = Play(room, *window, shouldQualify, started);
        }
        else
        {
            LUDUS_LOG_ERROR(LOG_ROOM, "Could not create Metal device: {}", static_cast<uint32>(status));
        }
    }
    rhi::Shutdown();
    return isSuccessful;
}
} // namespace

int main(int argc, char** argv)
{
    const bool shouldQualify = argc == 2 && std::string_view(argv[1]) == "--qualify";
    if (argc != 1 && shouldQualify == false)
    {
        return 2;
    }
    logging::LogConfig config;
    config.EnableConsole = true;
    config.EnableFile = false;
    logging::LogSystem::Initialize(config);
    const bool isSuccessful = Run(shouldQualify);
    logging::LogSystem::Shutdown();
    return isSuccessful == true ? 0 : 1;
}
