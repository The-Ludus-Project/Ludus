#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/platform/base/window.h>

#include <emscripten.h>

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
constexpr uint32 VISIBLE_EXTENT = 64;
constexpr uint32 TEXTURE_EXTENT = 4;
constexpr uint32 OFFSCREEN_FRAMES = 3;
constexpr usize COLOR_CHANNELS = 4;
constexpr uint8 MAX_CHANNEL = 255;
enum class Stage : uint8
{
    Device,
    Texture,
    Visible,
    VisibleCompletion,
    Offscreen,
    OffscreenCompletion,
    Readback,
    ReadbackCompletion,
    Passed,
    Failed,
};
UniquePtr<ludus::platform::Window> gWindow;
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
rhi::TextureHandle gTexture;
rhi::SubmissionToken gSubmission;
rhi::ReadbackTicket gReadback;
Stage gStage = Stage::Device;
uint32 gOffscreenFrames = 0;
uint32 gVisibleAcquisitions = 0;

// Test-only instrumentation is installed by offscreen.html before the module.
// clang-format off
EM_JS(uint32, GetAcquisitions, (), { return globalThis.surfaceAcquisitions; });
EM_JS(void, Report, (int32 isPassed, uint32 frames, uint32 stage), {
    const status = document.getElementById('status');
    status.dataset.state = isPassed ? 'passed' : 'failed';
    status.dataset.offscreenFrames = frames;
    status.dataset.stage = stage;
    status.dataset.surfaceAcquisitions = globalThis.surfaceAcquisitions;
    status.textContent = isPassed
      ? 'PASS: offscreen texture is green; the visible canvas stays red; no extra surface acquisition.'
      : 'FAIL: offscreen frame or preserved-canvas contract failed at stage ' + stage;
});
// clang-format on
void Fail() noexcept
{
    Report(0, gOffscreenFrames, static_cast<uint32>(gStage));
    gStage = Stage::Failed;
    emscripten_cancel_main_loop();
}
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
bool IsSubmissionReady() noexcept
{
    const rhi::RasterStatus status = rhi::GetStatus(gDevice, gSubmission);
    if (status == rhi::RasterStatus::Pending)
    {
        return false;
    }
    if (status != rhi::RasterStatus::Ready)
    {
        Fail();
        return false;
    }
    gSubmission = {};
    return true;
}
void Frame() noexcept
{
    rhi::DeviceInfo info;
    const rhi::DeviceStatus device = rhi::GetDeviceInfo(gDevice, info);
    if (device == rhi::DeviceStatus::Pending)
    {
        return;
    }
    if (device != rhi::DeviceStatus::Ready || rhi::PollLifetime(gDevice) != rhi::RasterStatus::Ready)
    {
        Fail();
        return;
    }
    switch (gStage)
    {
        case Stage::Device:
            if (IsAccepted(rhi::CreateTexture(gDevice,
                                              { .Width = TEXTURE_EXTENT, .Height = TEXTURE_EXTENT, .Attachment = true },
                                              {},
                                              gTexture)) == false)
            {
                Fail();
                return;
            }
            gStage = Stage::Texture;
            return;
        case Stage::Texture:
            if (rhi::GetStatus(gDevice, gTexture) == rhi::RasterStatus::Pending)
            {
                return;
            }
            if (rhi::GetStatus(gDevice, gTexture) != rhi::RasterStatus::Ready)
            {
                Fail();
                return;
            }
            gStage = Stage::Visible;
            return;
        case Stage::Visible:
            if (rhi::SetFrameTarget(gDevice,
                                    gSurface,
                                    {
                                        .Width = VISIBLE_EXTENT,
                                        .Height = VISIBLE_EXTENT,
                                        .Red = 1,
                                    }) != rhi::DeviceStatus::Ready ||
                rhi::BeginFrame(gDevice, gSurface) != rhi::DeviceStatus::Ready ||
                rhi::EndFrame(gDevice, gSubmission) != rhi::RasterStatus::Ready)
            {
                Fail();
                return;
            }
            gStage = Stage::VisibleCompletion;
            return;
        case Stage::VisibleCompletion:
            if (IsSubmissionReady() == true)
            {
                gVisibleAcquisitions = GetAcquisitions();
                if (gVisibleAcquisitions != 1)
                {
                    Fail();
                    return;
                }
                gStage = Stage::Offscreen;
            }
            return;
        case Stage::Offscreen: {
            if (rhi::SetFrameTarget(gDevice, gSurface, { .Width = 1, .Height = 1, .ShouldPresentToSurface = false }) !=
                    rhi::DeviceStatus::Ready ||
                rhi::BeginFrame(gDevice, gSurface) != rhi::DeviceStatus::Ready ||
                rhi::BeginRasterPass(gDevice, {}) != rhi::RasterStatus::InvalidState)
            {
                Fail();
                return;
            }
            rhi::RasterPassDescription pass;
            pass.Color = gTexture;
            pass.Clear[1] = pass.Clear[COLOR_CHANNELS - 1] = 1;
            if (rhi::BeginRasterPass(gDevice, pass) != rhi::RasterStatus::Ready ||
                rhi::EndFrame(gDevice, gSubmission) != rhi::RasterStatus::Ready)
            {
                Fail();
                return;
            }
            gStage = Stage::OffscreenCompletion;
            return;
        }
        case Stage::OffscreenCompletion:
            if (IsSubmissionReady() == true)
            {
                if (GetAcquisitions() != gVisibleAcquisitions)
                {
                    Fail();
                    return;
                }
                ++gOffscreenFrames;
                gStage = gOffscreenFrames == OFFSCREEN_FRAMES ? Stage::Readback : Stage::Offscreen;
            }
            return;
        case Stage::Readback:
            if (rhi::RequestTextureReadback(gDevice,
                                            gTexture,
                                            {
                                                .Width = 1,
                                                .Height = 1,
                                            },
                                            COLOR_CHANNELS,
                                            gReadback) != rhi::RasterStatus::Pending)
            {
                Fail();
                return;
            }
            gStage = Stage::ReadbackCompletion;
            return;
        case Stage::ReadbackCompletion: {
            const rhi::RasterStatus status = rhi::GetStatus(gDevice, gReadback);
            if (status == rhi::RasterStatus::Pending)
            {
                return;
            }
            StaticArray<uint8, COLOR_CHANNELS> pixel;
            if (status != rhi::RasterStatus::Ready ||
                rhi::CopyReadback(gDevice, gReadback, pixel.GetData(), COLOR_CHANNELS) != rhi::RasterStatus::Ready ||
                pixel[0] != 0 || pixel[1] != MAX_CHANNEL || pixel[2] != 0 || pixel[COLOR_CHANNELS - 1] != MAX_CHANNEL ||
                GetAcquisitions() != gVisibleAcquisitions)
            {
                Fail();
                return;
            }
            static_cast<void>(rhi::Release(gDevice, gReadback));
            gStage = Stage::Passed;
            Report(1, gOffscreenFrames, static_cast<uint32>(gStage));
            emscripten_cancel_main_loop();
            return;
        }
        case Stage::Passed:
        case Stage::Failed:
            return;
    }
}
} // namespace
int main()
{
    ludus::platform::WindowManager manager;
    if (manager.Initialize({}) == false || manager.CreateWindow({}, gWindow) == false)
    {
        Report(0, 0, 0);
        return 1;
    }
    rhi::DeviceDescription description;
    description.Selection = rhi::BackendSelection::WebGPU;
    description.Required.PortableRaster = true;
    const rhi::DeviceStatus status = rhi::CreateDevice({ .Name = "Offscreen frame qualification" },
                                                       gWindow->GetNativeWindowInfo(),
                                                       description,
                                                       gDevice,
                                                       gSurface);
    if (status != rhi::DeviceStatus::Ready && status != rhi::DeviceStatus::Pending)
    {
        Report(0, 0, 0);
        return 1;
    }
    emscripten_set_main_loop(Frame, 0, true);
    return 0;
}
