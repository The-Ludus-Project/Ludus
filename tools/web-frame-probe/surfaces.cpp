#include <ludus/foundation/base/core.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/rhi.h>

#include <emscripten.h>

// R14b browser qualification. Canvases of one WebGPU device acquire, present, resize and
// retire independently; a stale, foreign or duplicate target is rejected; and late
// configuration or completion callbacks of a destroyed surface are ignored. The probe runs
// the real port and C API against node_surface_fixture.js, which records one WebGPU
// context per canvas. See docs/development/fullscreen-rendering.md, "Multiple surfaces per
// device (R14b)".
namespace
{
using namespace ludus::foundation;
using namespace ludus::graphics;

constexpr uint32 PRIMARY_WIDTH = 64;
constexpr uint32 PRIMARY_HEIGHT = 32;
constexpr uint32 SECOND_SIZE = 48;
constexpr uint32 RESIZED_WIDTH = 20;
constexpr uint32 RESIZED_HEIGHT = 10;
constexpr uint32 THIRD_SIZE = 16;
constexpr uint32 LOSS_SIZE = 24;
constexpr float64 PRIMARY_RED = 0.25;
constexpr float64 SECOND_RED = 0.75;
/// Clear values the fixture records, in hundredths.
constexpr int32 PRIMARY_RED_PERCENT = 25;
constexpr int32 SECOND_RED_PERCENT = 75;
/// Delay of injected late callbacks, and the 8 ms frames the probe waits for them.
constexpr int32 LATE_CALLBACK_MILLISECONDS = 40;
constexpr uint32 LATE_CALLBACK_FRAMES = 12;
/// Configurations of the primary canvas: the 1x1 startup validation and the first target.
constexpr int32 PRIMARY_CONFIGURATIONS = 2;

// clang-format off
EM_JS(void, SetNextScope, (int32 shouldFail, int32 delayMilliseconds), {
    Module.surfaceFixture.setNextScope(shouldFail !== 0, delayMilliseconds);
});
EM_JS(void, SetCompletionDelay, (int32 delayMilliseconds), {
    Module.surfaceFixture.setCompletionDelay(delayMilliseconds);
});
EM_JS(void, DisconnectCanvas, (const char* selector), {
    Module.surfaceFixture.disconnect(UTF8ToString(selector));
});
EM_JS(int32, ReadCanvas, (const char* selector, const char* key), {
    return Module.surfaceFixture.read(UTF8ToString(selector), UTF8ToString(key));
});
EM_JS(int32, HasFrameHandles, (), {
    return Module.surfaceFixture.hasFrameHandles();
});
EM_JS(void, Finish, (int32 isPassed, const char* label), {
    Module.surfaceFixture.finish(isPassed !== 0, UTF8ToString(label));
});
// clang-format on

/// Probe stages; each runs once, after the previous stage's wait.
enum class Stage : uint8
{
    WaitReady,
    Admission,
    Counters,
    StaleConfiguration,
    StaleConfigurationCheck,
    StaleCompletion,
    StaleCompletionCheck,
    PendingLossBegin,
    PendingLossEnd,
    Disconnect,
    Teardown,
    Finished,
};

/// Same selector text as the primary in distinct storage: one canvas, another pointer.
char gPrimaryAlias[] = "#canvas";
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gPrimary;
rhi::SurfaceHandle gSecond;
rhi::SurfaceHandle gThird;
rhi::SurfaceHandle gStaleThird;
Stage gStage = Stage::WaitReady;
uint32 gWaitFrames = 0;
const char* gFailureOrNull = nullptr;

void Expect(bool isSatisfied, const char* label) noexcept
{
    if (isSatisfied == false && gFailureOrNull == nullptr)
    {
        gFailureOrNull = label;
    }
}
/// Pixel extent a probe canvas is borrowed with.
struct CanvasExtent final
{
    uint32 Width = 0;
    uint32 Height = 0;
};
rhi::WindowInfo MakeCanvas(const char* selector, const CanvasExtent& extent) noexcept
{
    rhi::WindowInfo window;
    window.System = ludus::platform::WindowSystem::WebCanvas;
    window.CanvasSelector = selector;
    window.Width = extent.Width;
    window.Height = extent.Height;
    return window;
}
bool RenderFrame(rhi::SurfaceHandle surface, const rhi::FrameTarget& target) noexcept
{
    return rhi::SetFrameTarget(gDevice, surface, target) == rhi::DeviceStatus::Ready &&
           rhi::BeginFrame(gDevice, surface) == rhi::DeviceStatus::Ready &&
           rhi::EndFrame(gDevice) == rhi::DeviceStatus::Ready;
}
rhi::SurfaceInfo GetInfo(rhi::SurfaceHandle surface) noexcept
{
    rhi::SurfaceInfo info;
    Expect(rhi::GetSurfaceInfo(gDevice, surface, info) == rhi::DeviceStatus::Ready, "surface info");
    return info;
}
bool IsDeviceReady() noexcept
{
    rhi::DeviceInfo info;
    return rhi::GetDeviceInfo(gDevice, info) == rhi::DeviceStatus::Ready;
}
/// Create, refuse and render on surfaces; resize the second while the primary keeps its extent.
void Admit() noexcept
{
    Expect(rhi::CreateSurface(gDevice,
                              MakeCanvas("#second",
                                         {
                                             .Width = SECOND_SIZE,
                                             .Height = SECOND_SIZE,
                                         }),
                              gSecond) == rhi::DeviceStatus::Ready,
           "second canvas creation");
    rhi::SurfaceHandle refused;
    Expect(rhi::CreateSurface(gDevice,
                              MakeCanvas(gPrimaryAlias, { .Width = SECOND_SIZE, .Height = SECOND_SIZE }),
                              refused) == rhi::DeviceStatus::InvalidDescription &&
               refused == rhi::SurfaceHandle{},
           "primary canvas named by another selector pointer is borrowed");
    Expect(rhi::CreateSurface(gDevice,
                              MakeCanvas("#missing", { .Width = SECOND_SIZE, .Height = SECOND_SIZE }),
                              refused) == rhi::DeviceStatus::InvalidDescription,
           "missing canvas");
    Expect(rhi::CreateSurface(gDevice,
                              MakeCanvas("#webgl",
                                         {
                                             .Width = SECOND_SIZE,
                                             .Height = SECOND_SIZE,
                                         }),
                              refused) == rhi::DeviceStatus::Failed,
           "canvas holding a WebGL 2 context");
    rhi::SurfaceHandle third;
    rhi::SurfaceHandle fourth;
    Expect(rhi::CreateSurface(gDevice, MakeCanvas("#third", { .Width = THIRD_SIZE, .Height = THIRD_SIZE }), third) ==
                   rhi::DeviceStatus::Ready &&
               rhi::CreateSurface(gDevice,
                                  MakeCanvas("#fourth", { .Width = THIRD_SIZE, .Height = THIRD_SIZE }),
                                  fourth) == rhi::DeviceStatus::Ready,
           "device hosts MAX_DEVICE_SURFACES canvases");
    Expect(rhi::CreateSurface(gDevice, MakeCanvas("#fifth", { .Width = THIRD_SIZE, .Height = THIRD_SIZE }), refused) ==
               rhi::DeviceStatus::CapacityExceeded,
           "surface capacity");
    Expect(rhi::DestroySurface(gDevice, third) == rhi::DeviceStatus::Ready &&
               rhi::DestroySurface(gDevice, fourth) == rhi::DeviceStatus::Ready,
           "retire capacity canvases");

    Expect(RenderFrame(gPrimary, { .Width = PRIMARY_WIDTH, .Height = PRIMARY_HEIGHT, .Red = PRIMARY_RED }),
           "primary frame");
    Expect(rhi::SetFrameTarget(gDevice, gSecond, { .Width = SECOND_SIZE, .Height = SECOND_SIZE, .Red = SECOND_RED }) ==
                   rhi::DeviceStatus::Ready &&
               rhi::BeginFrame(gDevice, gSecond) == rhi::DeviceStatus::Ready,
           "second frame");
    rhi::FrameInfo frame;
    Expect(rhi::GetFrameInfo(gDevice, gPrimary, frame) == rhi::DeviceStatus::InvalidState &&
               rhi::BeginFrame(gDevice, gPrimary) == rhi::DeviceStatus::InvalidState,
           "one open frame per device");
    Expect(rhi::GetFrameInfo(gDevice, gSecond, frame) == rhi::DeviceStatus::Ready && frame.Width == SECOND_SIZE &&
               frame.Height == SECOND_SIZE,
           "second frame extent");
    Expect(rhi::EndFrame(gDevice) == rhi::DeviceStatus::Ready, "second frame end");
    Expect(ReadCanvas("#canvas", "width") == static_cast<int32>(PRIMARY_WIDTH) &&
               ReadCanvas("#canvas", "height") == static_cast<int32>(PRIMARY_HEIGHT) &&
               ReadCanvas("#second", "width") == static_cast<int32>(SECOND_SIZE) &&
               ReadCanvas("#canvas", "clear") == PRIMARY_RED_PERCENT &&
               ReadCanvas("#second", "clear") == SECOND_RED_PERCENT,
           "each canvas receives its own extent and clear");

    Expect(RenderFrame(gSecond, { .Width = RESIZED_WIDTH, .Height = RESIZED_HEIGHT, .Red = SECOND_RED }),
           "resized second frame");
    Expect(ReadCanvas("#second", "width") == static_cast<int32>(RESIZED_WIDTH) &&
               ReadCanvas("#second", "height") == static_cast<int32>(RESIZED_HEIGHT) &&
               ReadCanvas("#canvas", "width") == static_cast<int32>(PRIMARY_WIDTH) &&
               ReadCanvas("#canvas", "configures") == PRIMARY_CONFIGURATIONS,
           "resizing the second canvas leaves the primary configuration alone");
    // An offscreen frame renders without presenting a canvas texture.
    Expect(RenderFrame(gSecond, { .Width = RESIZED_WIDTH, .Height = RESIZED_HEIGHT, .ShouldPresentToSurface = false }),
           "offscreen second frame");
    const rhi::SurfaceInfo second = GetInfo(gSecond);
    const rhi::SurfaceInfo primary = GetInfo(gPrimary);
    Expect(second.State == rhi::SurfaceState::Ready && second.IsPrimary == false && second.Width == RESIZED_WIDTH &&
               second.Height == RESIZED_HEIGHT && second.SubmittedFrames == 3 && second.PresentedFrames == 2,
           "second surface counters");
    Expect(primary.IsPrimary == true && primary.SubmittedFrames == 1 && primary.PresentedFrames == 1,
           "primary surface counters");
}
/// Completion arrives after the query GetSurfaceInfo issued.
void CheckCounters() noexcept
{
    Expect(GetInfo(gSecond).RenderedFrames == 3 && GetInfo(gPrimary).RenderedFrames == 1,
           "render completion is reported per surface, apart from presentation");
}
/// A failing configuration result arrives after its surface was destroyed and the slot reused.
void BeginStaleConfiguration() noexcept
{
    SetNextScope(1, LATE_CALLBACK_MILLISECONDS);
    Expect(rhi::CreateSurface(gDevice, MakeCanvas("#third", { .Width = THIRD_SIZE, .Height = THIRD_SIZE }), gThird) ==
               rhi::DeviceStatus::Ready,
           "third canvas with a late failing configuration");
    gStaleThird = gThird;
    Expect(rhi::DestroySurface(gDevice, gThird) == rhi::DeviceStatus::Ready && gThird == rhi::SurfaceHandle{},
           "destroy before the configuration result");
    Expect(rhi::CreateSurface(gDevice, MakeCanvas("#third", { .Width = THIRD_SIZE, .Height = THIRD_SIZE }), gThird) ==
               rhi::DeviceStatus::Ready,
           "third canvas again");
    rhi::SurfaceInfo stale;
    rhi::SurfaceHandle staleCopy = gStaleThird;
    Expect(rhi::GetSurfaceInfo(gDevice, gStaleThird, stale) == rhi::DeviceStatus::InvalidHandle &&
               rhi::DestroySurface(gDevice, staleCopy) == rhi::DeviceStatus::InvalidHandle &&
               rhi::BeginFrame(gDevice, gStaleThird) == rhi::DeviceStatus::InvalidHandle,
           "stale surface handle");
}
void CheckStaleConfiguration() noexcept
{
    Expect(GetInfo(gThird).State == rhi::SurfaceState::Ready && IsDeviceReady() == true,
           "late configuration failure of a destroyed surface is ignored");
    Expect(RenderFrame(gThird, { .Width = THIRD_SIZE, .Height = THIRD_SIZE }), "new third surface renders");
}
/// A completion query is in flight when its surface is destroyed and the slot reused.
void BeginStaleCompletion() noexcept
{
    SetCompletionDelay(LATE_CALLBACK_MILLISECONDS);
    Expect(GetInfo(gThird).SubmittedFrames == 1, "third frame submitted");
    Expect(rhi::DestroySurface(gDevice, gThird) == rhi::DeviceStatus::Ready &&
               rhi::CreateSurface(gDevice,
                                  MakeCanvas("#third",
                                             {
                                                 .Width = THIRD_SIZE,
                                                 .Height = THIRD_SIZE,
                                             }),
                                  gThird) == rhi::DeviceStatus::Ready,
           "replace the third surface during its completion query");
}
void CheckStaleCompletion() noexcept
{
    const rhi::SurfaceInfo third = GetInfo(gThird);
    Expect(third.SubmittedFrames == 0 && third.RenderedFrames == 0 && third.PresentedFrames == 0,
           "late completion of a destroyed surface is ignored");
    SetCompletionDelay(0);
}
/// The configuration of a resized created canvas fails while its frame is open.
void BeginPendingLoss() noexcept
{
    SetNextScope(1, 0);
    Expect(rhi::SetFrameTarget(gDevice, gThird, { .Width = LOSS_SIZE, .Height = LOSS_SIZE }) ==
                   rhi::DeviceStatus::Ready &&
               rhi::BeginFrame(gDevice, gThird) == rhi::DeviceStatus::Ready,
           "frame on the failing third canvas");
}
void EndPendingLoss() noexcept
{
    Expect(rhi::EndFrame(gDevice) == rhi::DeviceStatus::Ready, "open frame of a failing canvas ends");
    Expect(rhi::BeginFrame(gDevice, gThird) == rhi::DeviceStatus::SurfaceLost &&
               GetInfo(gThird).State == rhi::SurfaceState::Lost && ReadCanvas("#third", "configured") == 0,
           "configuration failure loses only the created canvas");
    Expect(RenderFrame(gPrimary, { .Width = PRIMARY_WIDTH, .Height = PRIMARY_HEIGHT }) &&
               RenderFrame(gSecond, { .Width = RESIZED_WIDTH, .Height = RESIZED_HEIGHT }) && IsDeviceReady() == true,
           "the device and other surfaces keep working after a configuration loss");
}
/// A created canvas that leaves the document is lost alone.
void Disconnect() noexcept
{
    DisconnectCanvas("#second");
    Expect(rhi::BeginFrame(gDevice, gSecond) == rhi::DeviceStatus::SurfaceLost &&
               ReadCanvas("#second", "configured") == 0,
           "removed canvas is lost");
    const rhi::SurfaceInfo second = GetInfo(gSecond);
    Expect(second.State == rhi::SurfaceState::Lost && second.SubmittedFrames == 4 && second.PresentedFrames == 3,
           "lost surface keeps its counters");
    Expect(RenderFrame(gPrimary, { .Width = PRIMARY_WIDTH, .Height = PRIMARY_HEIGHT }) && IsDeviceReady() == true,
           "primary keeps presenting after another canvas is lost");
    Expect(rhi::DestroySurface(gDevice, gSecond) == rhi::DeviceStatus::Ready &&
               rhi::DestroySurface(gDevice, gThird) == rhi::DeviceStatus::Ready,
           "lost surfaces are destroyed");
    Expect(rhi::DestroySurface(gDevice, gPrimary) == rhi::DeviceStatus::InvalidState,
           "the primary surface is retired by DestroyDevice");
}
void Teardown() noexcept
{
    Expect(rhi::DestroyDevice(gDevice) == rhi::DeviceStatus::Ready, "device destruction");
    Expect(ReadCanvas("#canvas", "configured") == 0 && HasFrameHandles() == 0, "teardown releases every canvas");
}
void Advance(Stage next, uint32 waitFrames) noexcept
{
    gStage = next;
    gWaitFrames = waitFrames;
}
void Frame() noexcept
{
    if (gWaitFrames != 0)
    {
        --gWaitFrames;
        return;
    }
    switch (gStage)
    {
        case Stage::WaitReady:
            if (IsDeviceReady() == true)
            {
                Advance(Stage::Admission, 0);
            }
            return;
        case Stage::Admission:
            Admit();
            Advance(Stage::Counters, 1);
            return;
        case Stage::Counters:
            CheckCounters();
            Advance(Stage::StaleConfiguration, 0);
            return;
        case Stage::StaleConfiguration:
            BeginStaleConfiguration();
            Advance(Stage::StaleConfigurationCheck, LATE_CALLBACK_FRAMES);
            return;
        case Stage::StaleConfigurationCheck:
            CheckStaleConfiguration();
            Advance(Stage::StaleCompletion, 0);
            return;
        case Stage::StaleCompletion:
            BeginStaleCompletion();
            Advance(Stage::StaleCompletionCheck, LATE_CALLBACK_FRAMES);
            return;
        case Stage::StaleCompletionCheck:
            CheckStaleCompletion();
            Advance(Stage::PendingLossBegin, 0);
            return;
        case Stage::PendingLossBegin:
            BeginPendingLoss();
            Advance(Stage::PendingLossEnd, 1);
            return;
        case Stage::PendingLossEnd:
            EndPendingLoss();
            Advance(Stage::Disconnect, 0);
            return;
        case Stage::Disconnect:
            Disconnect();
            Advance(Stage::Teardown, 0);
            return;
        case Stage::Teardown:
            Teardown();
            Advance(Stage::Finished, 0);
            Finish(gFailureOrNull == nullptr ? 1 : 0,
                   gFailureOrNull == nullptr ? "canvases of one device are independent" : gFailureOrNull);
            return;
        case Stage::Finished:
            return;
    }
}
} // namespace
int main()
{
    rhi::DeviceDescription description;
    description.Selection = rhi::BackendSelection::WebGPU;
    if (rhi::CreateDevice({ .Name = "R14b surfaces" },
                          MakeCanvas("#canvas", { .Width = PRIMARY_WIDTH, .Height = PRIMARY_HEIGHT }),
                          description,
                          gDevice,
                          gPrimary) != rhi::DeviceStatus::Pending)
    {
        Finish(0, "device creation");
        return 0;
    }
    emscripten_set_main_loop(Frame, 0, true);
}
