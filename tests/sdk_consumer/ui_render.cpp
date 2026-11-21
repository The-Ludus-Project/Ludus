// Installed-SDK U1 clip/hit qualification for Ludus::UiRender (issue #1219).
//
// One logical document runs at two integer framebuffer scales and after a resize. Each case
// renders offscreen through UiRender -> OverlayList -> OverlayRenderer, reads the RGBA8 target
// back and checks every pixel against the snapped paint list, and routes a press at every pixel
// centre through Ui to check the activated button. Explicit probes cover the inside of the
// effective clip, its half-open edge, the overflow outside it and the overlapping sibling.
// Contract: docs/architecture/ui.md#u1-render-adapter.

#include "renderer_overlay.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/overlays.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/graphics/ui_render/ui_render.hpp>
#include <ludus/ui/context.h>

#include <span>
#include <string_view>

#if defined(LUDUS_PLATFORM_WEB)
#    include <emscripten.h>
#    include <ludus/foundation/base/pointer.hpp>
#    include <ludus/platform/base/window.h>
#endif

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace ui = ludus::ui;
namespace ui_render = ludus::graphics::ui_render;
using Status = rhi::RasterStatus;

constexpr logging::LogCategory LOG_UI_RENDER{"UiRenderFixture"};
constexpr uint32 SURFACE_WIDTH = 96;
constexpr uint32 SURFACE_HEIGHT = 64;
constexpr usize CHANNEL_COUNT = 4;
constexpr usize MAX_IMAGE_BYTES = usize{SURFACE_WIDTH} * SURFACE_HEIGHT * CHANNEL_COUNT;
constexpr float64 PIXEL_CENTER = 0.5;
constexpr uint8 UNORM_MAX = 255;
/// Opaque solid fills have exact UNorm values; allow one byte for backend blend rounding.
constexpr int32 BYTE_TOLERANCE = 1;
constexpr int64 TIME_LIMIT_SECONDS = 20;
using Pixels = core::StaticArray<uint8, MAX_IMAGE_BYTES>;

// The document matches modules/graphics/ui_render/tests/ui_render_tests.cpp: button A overflows
// its clipped panel, sibling button B is painted after A and overlaps it inside the clip, and
// button C is anchored bottom-right so the resize moves it.
constexpr ui::WidgetId PANEL = 1;
constexpr ui::WidgetId OVERFLOW_BUTTON = 2;
constexpr ui::WidgetId SIBLING_BUTTON = 3;
constexpr ui::WidgetId CORNER_BUTTON = 4;
constexpr usize ELEMENT_COUNT = 4;
using Document = core::StaticArray<ui::Element, ELEMENT_COUNT>;
constexpr ui::Color RED{1, 0, 0, 1};
constexpr ui::Color GREEN{0, 1, 0, 1};
constexpr ui::Color BLUE{0, 0, 1, 1};
constexpr ui::Color GRAY{0.25F, 0.25F, 0.25F, 1};
constexpr float32 PANEL_OFFSET = 4;
constexpr float32 PANEL_WIDTH = 30.25F; // Right clip edge on a pixel centre at scale 2.
constexpr float32 PANEL_HEIGHT = 20;
constexpr float32 OVERFLOW_OFFSET_X = 16;
constexpr float32 OVERFLOW_OFFSET_Y = 4;
constexpr float32 OVERFLOW_WIDTH = 20;
constexpr float32 OVERFLOW_HEIGHT = 10;
constexpr float32 SIBLING_OFFSET_X = 24;
constexpr float32 SIBLING_OFFSET_Y = 12;
constexpr float32 SIBLING_WIDTH = 8;
constexpr float32 SIBLING_HEIGHT = 16;
constexpr float32 CORNER_EXTENT = 6;

/// A logical probe and the widget expected both to paint it and to receive the press.
struct Probe final
{
    std::string_view Name;
    float32 X = 0;
    float32 Y = 0;
    ui::WidgetId Expected = 0;
};
/// Logical probes valid at every case's viewport; the panel clip spans x in [4, 34.25).
constexpr usize PROBE_COUNT = 6;
constexpr core::StaticArray<Probe, PROBE_COUNT> PROBES{{
    {"inside clip", 22, 10, OVERFLOW_BUTTON},
    {"last pixel inside half-open clip edge", 33.9F, 10, OVERFLOW_BUTTON},
    {"half-open clip edge", 34.25F, 10, 0},
    {"overflow outside clip", 36, 10, 0},
    {"topmost sibling over overflow button", 28, 14, SIBLING_BUTTON},
    {"sibling outside panel after scissor change", 28, 26, SIBLING_BUTTON},
}};

struct ScaleCase final
{
    std::string_view Name;
    float32 Scale = 1;
    uint32 Width = 0;
    uint32 Height = 0;
};
constexpr usize CASE_COUNT = 3;
constexpr core::StaticArray<ScaleCase, CASE_COUNT> CASES{{
    {"scale 1", 1, 48, 32},
    {"scale 2", 2, 96, 64},
    {"scale 2 after resize", 2, 80, 60},
}};

enum class Step : uint8
{
    StartOverlays,
    WaitOverlays,
    PrepareCase,
    WaitCase,
    Draw,
    ReadPixels,
    Complete,
};

rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
rr::OverlayRenderer gOverlays;
rr::OverlayList gList;
ui::Context gContext;
rr::PreparedOverlay gOverlay;
core::StaticArray<rhi::TextureHandle, CASE_COUNT> gTargets;
rhi::ReadbackTicket gReadback;
Step gStep = Step::StartOverlays;
usize gCaseIndex = 0;
ui_render::PaintReport gReport;
core::StaticArray<uint64, CASE_COUNT> gHashes;
/// Global, not a Tick local: the browser main loop runs on a 64 KiB stack.
Pixels gPixels;
bool isFixtureDone = false;
bool hasFixtureFailed = false;

#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// clang-format off
EM_JS(int32, SelectBackend, (), { const s = new URL(location.href).searchParams.get('backend'); return s === 'webgpu' ? 1 : s === 'webgl2' ? 2 : 0; });
EM_JS(void, ReportCase, (int32 index, uint32 scale, uint32 width, uint32 height, uint32 runs, uint32 quads, uint32 hashHigh, uint32 hashLow), {
    const node = document.querySelector('#status');
    node.dataset['case' + index] = [scale, width, height, runs, quads, (hashHigh >>> 0).toString(16).padStart(8, '0') + (hashLow >>> 0).toString(16).padStart(8, '0')].join(',');
});
EM_JS(void, ReportState, (int32 state, int32 backend, uint32 cases), {
    const node = document.querySelector('#status');
    node.dataset.state = state === 2 ? 'failed' : state === 1 ? 'passed' : 'loading';
    Object.assign(node.dataset, {cases, backend: backend === 1 ? 'webgpu' : backend === 2 ? 'webgl2' : 'native'});
    node.textContent = node.dataset.state + ' cases=' + cases;
});
// clang-format on
#else
int32 SelectBackend() noexcept
{
    return 0;
}
void ReportCase(int32, uint32, uint32, uint32, uint32, uint32, uint32, uint32) noexcept {}
void ReportState(int32, int32, uint32) noexcept {}
#endif

[[nodiscard]] bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}

[[nodiscard]] bool IsWaiting(Status status) noexcept
{
    return status == Status::Pending || status == Status::NotReady;
}

[[nodiscard]] uint32 GetBackend() noexcept
{
    return static_cast<uint32>(rhi::GetStartup().SelectedBackend);
}

void Stop(bool hasFailed) noexcept
{
    isFixtureDone = true;
    hasFixtureFailed = hasFailed;
    constexpr int32 PASSED = 1;
    constexpr int32 FAILED = 2;
    ReportState(hasFailed == true ? FAILED : PASSED, static_cast<int32>(GetBackend()), static_cast<uint32>(gCaseIndex));
    if (hasFailed == true)
    {
        LUDUS_LOG_ERROR(LOG_UI_RENDER,
                        "UiRender fixture failed at case {} on backend {}",
                        gCaseIndex + 1,
                        GetBackend());
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_cancel_main_loop();
#endif
    gOverlays.Reset();
    rhi::Shutdown();
    static_cast<void>(logging::LogSystem::Shutdown());
}

[[nodiscard]] bool Fail(std::string_view operation, Status status) noexcept
{
    LUDUS_LOG_ERROR(LOG_UI_RENDER, "{} failed with raster status {}", operation, static_cast<uint32>(status));
    Stop(true);
    return false;
}

[[nodiscard]] Document MakeDocument() noexcept
{
    Document document;
    ui::Element& panel = document[0];
    panel.Id = PANEL;
    panel.Placement.Width = {PANEL_WIDTH, ui::Unit::Pixels};
    panel.Placement.Height = {PANEL_HEIGHT, ui::Unit::Pixels};
    panel.Placement.OffsetX = PANEL_OFFSET;
    panel.Placement.OffsetY = PANEL_OFFSET;
    panel.Fill = GRAY;
    ui::Element& overflow = document[1];
    overflow.Id = OVERFLOW_BUTTON;
    overflow.Parent = 0;
    overflow.SemanticRole = ui::Role::Button;
    overflow.Placement.Width = {OVERFLOW_WIDTH, ui::Unit::Pixels};
    overflow.Placement.Height = {OVERFLOW_HEIGHT, ui::Unit::Pixels};
    overflow.Placement.OffsetX = OVERFLOW_OFFSET_X;
    overflow.Placement.OffsetY = OVERFLOW_OFFSET_Y;
    overflow.Fill = RED;
    ui::Element& sibling = document[2];
    sibling.Id = SIBLING_BUTTON;
    sibling.SemanticRole = ui::Role::Button;
    sibling.Placement.Width = {SIBLING_WIDTH, ui::Unit::Pixels};
    sibling.Placement.Height = {SIBLING_HEIGHT, ui::Unit::Pixels};
    sibling.Placement.OffsetX = SIBLING_OFFSET_X;
    sibling.Placement.OffsetY = SIBLING_OFFSET_Y;
    sibling.Fill = BLUE;
    ui::Element& corner = document[3];
    corner.Id = CORNER_BUTTON;
    corner.SemanticRole = ui::Role::Button;
    corner.Placement.Width = {CORNER_EXTENT, ui::Unit::Pixels};
    corner.Placement.Height = {CORNER_EXTENT, ui::Unit::Pixels};
    corner.Placement.Horizontal = ui::Align::End;
    corner.Placement.Vertical = ui::Align::End;
    corner.Fill = GREEN;
    return document;
}

[[nodiscard]] bool IsButton(ui::WidgetId id) noexcept
{
    return id == OVERFLOW_BUTTON || id == SIBLING_BUTTON || id == CORNER_BUTTON;
}

[[nodiscard]] ui_render::SurfaceMapping GetMapping(const ScaleCase& test) noexcept
{
    return {test.Scale, 0, 0, test.Width, test.Height};
}

[[nodiscard]] ui::WidgetId Press(float32 x, float32 y) noexcept
{
    static_cast<void>(gContext.Route({ui::EventKind::PointerDown, x, y, 0}));
    return gContext.Route({ui::EventKind::PointerUp, x, y, 0}).Activated;
}

[[nodiscard]] bool PrepareCase(const ScaleCase& test) noexcept
{
    const Document document = MakeDocument();
    const ui::Viewport viewport{
        {0, 0, static_cast<float32>(test.Width) / test.Scale, static_cast<float32>(test.Height) / test.Scale}};
    if (gContext.TrySetDocument({document.GetData(), document.GetSize()}, viewport) != ui::Status::Ok)
    {
        LUDUS_LOG_ERROR(LOG_UI_RENDER, "{}: Ui rejected the document", test.Name);
        Stop(true);
        return false;
    }
    gList.Clear();
    const Status appended = ui_render::AppendPaint(gList, gContext.GetPaintCommands(), GetMapping(test), gReport);
    if (appended != Status::Ready)
    {
        return Fail("appending Ui paint", appended);
    }
    // The panel (viewport clip), overflow button (panel clip) and sibling plus corner (viewport
    // clip) are three runs; the sibling's run follows a scissor change.
    constexpr usize EXPECTED_RUNS = 3;
    if (gReport.Quads != ELEMENT_COUNT || gReport.Runs != EXPECTED_RUNS || gReport.Culled != 0)
    {
        LUDUS_LOG_ERROR(LOG_UI_RENDER,
                        "{}: expected {} quads in {} runs, got {} quads, {} runs, {} culled",
                        test.Name,
                        ELEMENT_COUNT,
                        EXPECTED_RUNS,
                        gReport.Quads,
                        gReport.Runs,
                        gReport.Culled);
        Stop(true);
        return false;
    }
    const Status prepared = gOverlays.Prepare(gList, rhi::TextureHandle{}, gOverlay);
    if (IsAccepted(prepared) == false)
    {
        return Fail("preparing overlay", prepared);
    }
    return true;
}

[[nodiscard]] Status DrawCase(const ScaleCase& test) noexcept
{
    const rhi::DeviceStatus begun = rhi::BeginFrame(gDevice, gSurface);
    if (begun == rhi::DeviceStatus::Skipped)
    {
        // The surface skipped this frame; retry the whole case on the next tick.
        return Status::NotReady;
    }
    if (begun != rhi::DeviceStatus::Ready)
    {
        return Status::Failed;
    }
    rhi::RasterPassDescription pass;
    pass.Color = gTargets[gCaseIndex];
    // OverlayRenderer reopens the target once per run with read-only depth loaded, so the clearing
    // pass must store the depth it initializes.
    pass.DepthStore = rhi::RasterStore::Store;
    for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        pass.Clear[channel] = 0;
    }
    const Status begunPass = rhi::BeginRasterPass(gDevice, pass);
    const Status drawn = begunPass == Status::Ready ? gOverlays.Draw(gOverlay, pass) : begunPass;
    rhi::SubmissionToken completion;
    const Status submitted = rhi::EndFrame(gDevice, completion);
    if (drawn != Status::Ready)
    {
        return drawn;
    }
    if (submitted != Status::Ready)
    {
        return submitted;
    }
    const usize rowBytes = usize{test.Width} * CHANNEL_COUNT;
    return rhi::RequestTextureReadback(gDevice,
                                       gTargets[gCaseIndex],
                                       { .Width = test.Width, .Height = test.Height },
                                       rowBytes,
                                       gReadback);
}

/// FNV-1a 64 of the readback, published so the browser harness can compare backends bytewise.
[[nodiscard]] uint64 HashPixels(std::span<const uint8> bytes) noexcept
{
    constexpr uint64 FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64 FNV_PRIME = 1099511628211ULL;
    uint64 hash = FNV_OFFSET;
    for (const uint8 byte : bytes)
    {
        hash = (hash ^ byte) * FNV_PRIME;
    }
    return hash;
}

[[nodiscard]] bool IsColorNear(const uint8* pixel, const ui::Color& expected) noexcept
{
    const core::StaticArray<float32, CHANNEL_COUNT> channels{{expected.R, expected.G, expected.B, expected.A}};
    for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        const int32 expectedByte =
            static_cast<int32>(static_cast<float64>(channels[channel]) * UNORM_MAX + PIXEL_CENTER);
        const int32 difference = static_cast<int32>(pixel[channel]) - expectedByte;
        if (difference < -BYTE_TOLERANCE || difference > BYTE_TOLERANCE)
        {
            return false;
        }
    }
    return true;
}

/// Check every pixel's color and routed press, then the named logical probes.
[[nodiscard]] bool CheckCase(const ScaleCase& test, std::span<const uint8> pixels) noexcept
{
    const ui_render::SurfaceMapping mapping = GetMapping(test);
    const std::span<const ui::PaintCommand> commands = gContext.GetPaintCommands();
    core::StaticArray<rhi::RasterRectangle, ELEMENT_COUNT> snapped;
    for (usize index = 0; index < commands.size(); ++index)
    {
        if (ui_render::TryComputePhysicalRectangle(commands[index].Bounds, mapping, snapped[index]) == false)
        {
            return false;
        }
    }
    constexpr ui::Color CLEAR{};
    usize colorMismatches = 0;
    usize hitMismatches = 0;
    for (uint32 y = 0; y < test.Height; ++y)
    {
        for (uint32 x = 0; x < test.Width; ++x)
        {
            const ui::Color* expectedColorOrNull = nullptr;
            ui::WidgetId expectedHit = 0;
            for (usize index = 0; index < commands.size(); ++index)
            {
                const rhi::RasterRectangle& rectangle = snapped[index];
                if (x >= rectangle.X && x < rectangle.X + rectangle.Width && y >= rectangle.Y &&
                    y < rectangle.Y + rectangle.Height)
                {
                    expectedColorOrNull = &commands[index].Fill;
                    expectedHit = IsButton(commands[index].Id) == true ? commands[index].Id : expectedHit;
                }
            }
            const uint8* pixel = pixels.data() + (usize{y} * test.Width + x) * CHANNEL_COUNT;
            if (IsColorNear(pixel, expectedColorOrNull == nullptr ? CLEAR : *expectedColorOrNull) == false)
            {
                if (colorMismatches == 0)
                {
                    LUDUS_LOG_ERROR(LOG_UI_RENDER,
                                    "{}: pixel ({}, {}) is {} {} {} {}",
                                    test.Name,
                                    x,
                                    y,
                                    pixel[0],
                                    pixel[1],
                                    pixel[2],
                                    pixel[3]);
                }
                ++colorMismatches;
            }
            const ui_render::LogicalPoint point =
                ui_render::MapPhysicalToLogical(mapping,
                                                static_cast<float64>(x) + PIXEL_CENTER,
                                                static_cast<float64>(y) + PIXEL_CENTER);
            if (Press(point.X, point.Y) != expectedHit)
            {
                ++hitMismatches;
            }
        }
    }
    usize probeFailures = 0;
    for (const Probe& probe : PROBES)
    {
        // Probe the physical pixel whose centre maps closest at or after the logical point.
        const uint32 x = static_cast<uint32>(static_cast<float64>(probe.X) * static_cast<float64>(test.Scale));
        const uint32 y = static_cast<uint32>(static_cast<float64>(probe.Y) * static_cast<float64>(test.Scale));
        const ui_render::LogicalPoint point = ui_render::MapPhysicalToLogical(mapping,
                                                                              static_cast<float64>(x) + PIXEL_CENTER,
                                                                              static_cast<float64>(y) + PIXEL_CENTER);
        const ui::WidgetId routed = Press(point.X, point.Y);
        const uint8* pixel = pixels.data() + (usize{y} * test.Width + x) * CHANNEL_COUNT;
        // Every probe without a button lies outside the panel too, so it must keep the clear color.
        const ui::Color expected = probe.Expected == OVERFLOW_BUTTON  ? RED
                                   : probe.Expected == SIBLING_BUTTON ? BLUE
                                                                      : CLEAR;
        if (routed != probe.Expected || IsColorNear(pixel, expected) == false)
        {
            LUDUS_LOG_ERROR(LOG_UI_RENDER,
                            "{}: probe '{}' at pixel ({}, {}) routed {} (expected {}), color {} {} {} {}",
                            test.Name,
                            probe.Name,
                            x,
                            y,
                            routed,
                            probe.Expected,
                            pixel[0],
                            pixel[1],
                            pixel[2],
                            pixel[3]);
            ++probeFailures;
        }
    }
    const uint64 hash = HashPixels(pixels);
    gHashes[gCaseIndex] = hash;
    constexpr uint32 HASH_HALF_BITS = 32;
    ReportCase(static_cast<int32>(gCaseIndex),
               static_cast<uint32>(test.Scale),
               test.Width,
               test.Height,
               static_cast<uint32>(gReport.Runs),
               static_cast<uint32>(gReport.Quads),
               static_cast<uint32>(hash >> HASH_HALF_BITS),
               static_cast<uint32>(hash));
    LUDUS_LOG_INFO(LOG_UI_RENDER,
                   "{} on backend {}: {}x{} physical, {} quads in {} runs, {} color and {} hit mismatches, "
                   "{} probe failures, hash {}",
                   test.Name,
                   GetBackend(),
                   test.Width,
                   test.Height,
                   gReport.Quads,
                   gReport.Runs,
                   colorMismatches,
                   hitMismatches,
                   probeFailures,
                   hash);
    return colorMismatches == 0 && hitMismatches == 0 && probeFailures == 0;
}

void Tick() noexcept
{
    if (isFixtureDone == true)
    {
        return;
    }
    rhi::DeviceInfo info;
    const rhi::DeviceStatus device = rhi::GetDeviceInfo(gDevice, info);
    if (device == rhi::DeviceStatus::Pending)
    {
        return;
    }
    if (device != rhi::DeviceStatus::Ready)
    {
        Stop(true);
        return;
    }
    const Status lifetime = rhi::PollLifetime(gDevice);
    if (lifetime != Status::Ready)
    {
        static_cast<void>(Fail("polling lifetime", lifetime));
        return;
    }
    if (gStep == Step::StartOverlays)
    {
        const Status started = gOverlays.Initialize(gDevice,
                                                    ludus::shaders::renderer_overlay::Vertex(),
                                                    ludus::shaders::renderer_overlay::Fragment());
        if (IsAccepted(started) == false)
        {
            static_cast<void>(Fail("initializing overlays", started));
            return;
        }
        for (usize index = 0; index < CASE_COUNT; ++index)
        {
            const Status created =
                rhi::CreateTexture(gDevice,
                                   {CASES[index].Width, CASES[index].Height, rhi::RasterFormat::Rgba8Unorm, true},
                                   {},
                                   gTargets[index]);
            if (IsAccepted(created) == false)
            {
                static_cast<void>(Fail("creating target", created));
                return;
            }
        }
        gStep = Step::WaitOverlays;
    }
    if (gStep == Step::WaitOverlays)
    {
        const Status polled = gOverlays.Poll();
        if (IsWaiting(polled) == true)
        {
            return;
        }
        if (polled != Status::Ready)
        {
            static_cast<void>(Fail("polling overlays", polled));
            return;
        }
        for (const rhi::TextureHandle target : gTargets)
        {
            const Status status = rhi::GetStatus(gDevice, target);
            if (IsWaiting(status) == true)
            {
                return;
            }
            if (status != Status::Ready)
            {
                static_cast<void>(Fail("waiting for target", status));
                return;
            }
        }
        gStep = Step::PrepareCase;
    }
    const ScaleCase& test = CASES[gCaseIndex];
    if (gStep == Step::PrepareCase)
    {
        if (PrepareCase(test) == false)
        {
            return;
        }
        gStep = Step::WaitCase;
    }
    if (gStep == Step::WaitCase)
    {
        const Status status = gOverlays.GetStatus(gOverlay);
        if (IsWaiting(status) == true)
        {
            return;
        }
        if (status != Status::Ready)
        {
            static_cast<void>(Fail("waiting for prepared overlay", status));
            return;
        }
        gStep = Step::Draw;
    }
    if (gStep == Step::Draw)
    {
        const Status drawn = DrawCase(test);
        if (drawn == Status::NotReady)
        {
            return;
        }
        if (IsAccepted(drawn) == false)
        {
            static_cast<void>(Fail(test.Name, drawn));
            return;
        }
        gStep = Step::ReadPixels;
    }
    if (gStep == Step::ReadPixels)
    {
        const Status ready = rhi::GetStatus(gDevice, gReadback);
        if (IsWaiting(ready) == true)
        {
            return;
        }
        const usize imageBytes = usize{test.Width} * test.Height * CHANNEL_COUNT;
        const Status copied =
            ready == Status::Ready ? rhi::CopyReadback(gDevice, gReadback, gPixels.GetData(), imageBytes) : ready;
        if (copied != Status::Ready)
        {
            static_cast<void>(Fail("copying readback", copied));
            return;
        }
        const Status releasedReadback = rhi::Release(gDevice, gReadback);
        if (releasedReadback != Status::Ready)
        {
            static_cast<void>(Fail("releasing readback", releasedReadback));
            return;
        }
        const Status released = gOverlays.Release(gOverlay);
        if (released != Status::Ready)
        {
            static_cast<void>(Fail("releasing overlay", released));
            return;
        }
        if (CheckCase(test, {gPixels.GetData(), imageBytes}) == false)
        {
            Stop(true);
            return;
        }
        ++gCaseIndex;
        gStep = gCaseIndex == CASE_COUNT ? Step::Complete : Step::PrepareCase;
    }
    if (gStep == Step::Complete)
    {
        LUDUS_LOG_INFO(LOG_UI_RENDER, "{} UiRender cases passed on backend {}", CASE_COUNT, GetBackend());
        Stop(false);
    }
}
} // namespace

int main()
{
    rhi::WindowInfo window;
    window.Width = SURFACE_WIDTH;
    window.Height = SURFACE_HEIGHT;
#if defined(LUDUS_PLATFORM_WEB)
    ludus::platform::WindowManager manager;
    ludus::platform::WindowBase::CreateInfo settings;
    settings.Width = SURFACE_WIDTH;
    settings.Height = SURFACE_HEIGHT;
    settings.CanvasSelector = "#canvas";
    settings.CaptureBrowserInput = false;
    if (manager.Initialize({}) == false || manager.CreateWindow(settings, gWindow) == false)
    {
        return 1;
    }
    window = gWindow->GetNativeWindowInfo();
#endif
    static_cast<void>(logging::LogSystem::Initialize(
    {
        .EnableFile = false,
        .EnableDebugger = false,
        .Directory = {},
        .FlushIntervalMilliseconds = 0,
    }));
    if (gContext.TryInitialize(ELEMENT_COUNT) != ui::Status::Ok || gList.Initialize() != Status::Ready)
    {
        return 1;
    }
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const int32 selection = SelectBackend();
    description.Selection = selection == 1   ? rhi::BackendSelection::WebGPU
                            : selection == 2 ? rhi::BackendSelection::WebGL2
                                             : rhi::BackendSelection::Auto;
    rhi::StartupInfo startup;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, window, description, gDevice, gSurface, &startup);
    if (started != rhi::DeviceStatus::Ready && started != rhi::DeviceStatus::Pending)
    {
        LUDUS_LOG_ERROR(LOG_UI_RENDER,
                        "Device startup failed with status {}, error {}, requirement {}",
                        static_cast<uint32>(started),
                        static_cast<uint32>(startup.Error),
                        static_cast<uint32>(startup.UnmetRequirement));
        Stop(true);
        return 1;
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, 1);
#else
    const uint64 start = time::NowTicks();
    while (isFixtureDone == false && time::NowTicks() - start < TIME_LIMIT_SECONDS * time::NANOSECONDS_PER_SECOND)
    {
        Tick();
    }
    if (isFixtureDone == false)
    {
        Stop(true);
    }
#endif
    return hasFixtureFailed == true ? 1 : 0;
}
