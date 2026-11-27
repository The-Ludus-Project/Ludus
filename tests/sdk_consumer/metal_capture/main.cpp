// G2 installed-SDK fixture: an application captures its own offscreen Metal frames through
// the public capture API, then verify.py checks the .gputrace documents and manifests.
// Guide: docs/architecture/graphics-debugging.md, "Metal on macOS".

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/foundation/time/timers.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/graphics_capture.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/rhi/render.h>

#include "diagnostic.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;

constexpr uint32 WIDTH = 96;
constexpr uint32 HEIGHT = 64;
/// Captures written per run, one per frame kind; repeated requests write fresh documents.
constexpr uint32 CAPTURE_COUNT = 2;
/// Room for the first trace of a process, which Metal takes seconds to write.
constexpr uint64 CAPTURE_DEADLINE_MILLISECONDS = 120'000;
constexpr uint64 POLL_MILLISECONDS = 10;
/// Exit status when the process must be relaunched with MTL_CAPTURE_ENABLED=1.
constexpr int RESTART_REQUIRED_EXIT = 3;
constexpr int EXPECTED_ARGUMENT_COUNT = 2;
constexpr std::string_view TEXTURE_NAME = "Fixture/OffscreenColor";
constexpr std::string_view VIEW_SCOPE = "Fixture/View";
constexpr std::string_view PASS_SCOPE = "Fixture/OffscreenPass";

/// Host mirror of diagnostic.lusl's 48-byte uniform block.
struct alignas(16) DiagnosticUniforms final
{
    float32 Resolution[2];
    float32 Elapsed;
    float32 Padding0;
    float32 Direction[3];
    float32 Padding1;
    float32 Tint[4];
};
static_assert(sizeof(DiagnosticUniforms) == 48 && offsetof(DiagnosticUniforms, Tint) == 32);

void WriteLine(std::string_view key, std::string_view value) noexcept
{
    std::fwrite(key.data(), 1, key.size(), stdout);
    std::fputc('=', stdout);
    std::fwrite(value.data(), 1, value.size(), stdout);
    std::fputc('\n', stdout);
}
[[nodiscard]] std::string_view GetErrorName(rhi::GraphicsCaptureError error) noexcept
{
    switch (error)
    {
        case rhi::GraphicsCaptureError::None:
            return "None";
        case rhi::GraphicsCaptureError::InvalidTarget:
            return "InvalidTarget";
        case rhi::GraphicsCaptureError::InvalidRequest:
            return "InvalidRequest";
        case rhi::GraphicsCaptureError::InvalidState:
            return "InvalidState";
        case rhi::GraphicsCaptureError::UnsupportedBackend:
            return "UnsupportedBackend";
        case rhi::GraphicsCaptureError::ToolUnavailable:
            return "ToolUnavailable";
        case rhi::GraphicsCaptureError::RestartRequired:
            return "RestartRequired";
        case rhi::GraphicsCaptureError::Busy:
            return "Busy";
        case rhi::GraphicsCaptureError::InvalidDestination:
            return "InvalidDestination";
        case rhi::GraphicsCaptureError::DeadlineExceeded:
            return "DeadlineExceeded";
        case rhi::GraphicsCaptureError::DeviceLost:
            return "DeviceLost";
        case rhi::GraphicsCaptureError::SessionEnded:
            return "SessionEnded";
        case rhi::GraphicsCaptureError::FrameFailed:
            return "FrameFailed";
        case rhi::GraphicsCaptureError::NativeCaptureFailed:
            return "NativeCaptureFailed";
        case rhi::GraphicsCaptureError::ManifestFailed:
            return "ManifestFailed";
        case rhi::GraphicsCaptureError::StaleIdentity:
            return "StaleIdentity";
    }
    return "Unknown";
}
[[nodiscard]] BorrowedView<const char> GetView(std::string_view text) noexcept
{
    return {text.data(), text.size()};
}
[[nodiscard]] bool IsActive(rhi::GraphicsCaptureState state) noexcept
{
    return state == rhi::GraphicsCaptureState::Armed || state == rhi::GraphicsCaptureState::Capturing ||
           state == rhi::GraphicsCaptureState::Finalizing;
}

/// Device, resources and named scopes of the captured frames.
class Fixture final
{
public:
    [[nodiscard]] bool Create() noexcept
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        return rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, mDevice, mSurface) ==
                   rhi::DeviceStatus::Ready &&
               rhi::CreateShader(mDevice, ludus::shaders::diagnostic::Vertex(), mVertex) ==
                   rhi::ResourceStatus::Ready &&
               rhi::CreateShader(mDevice, ludus::shaders::diagnostic::Fragment(), mFragment) ==
                   rhi::ResourceStatus::Ready &&
               rhi::CreateUniform(mDevice, sizeof(DiagnosticUniforms), mUniform) == rhi::ResourceStatus::Ready &&
               rhi::CreatePipeline(mDevice, {mVertex, mFragment, mUniform}, mPipeline) == rhi::ResourceStatus::Ready &&
               rhi::CreateTexture(mDevice, { .Width = WIDTH, .Height = HEIGHT, .Attachment = true }, {}, mTexture) ==
                   rhi::RasterStatus::Ready &&
               rhi::SetDebugName(mDevice, mTexture, GetView(TEXTURE_NAME)) == rhi::RasterStatus::Ready &&
               rhi::MakeGpuScope(1, GetView(VIEW_SCOPE), mView) == true &&
               rhi::MakeGpuScope(2, GetView(PASS_SCOPE), mPass) == true;
    }
    void Destroy() noexcept
    {
        static_cast<void>(rhi::DestroyDevice(mDevice));
    }
    [[nodiscard]] rhi::DeviceHandle GetDevice() const noexcept
    {
        return mDevice;
    }
    /// A zero-size target opens no frame, so it never consumes an armed capture.
    [[nodiscard]] bool SkipFrame() noexcept
    {
        return rhi::SetFrameTarget(mDevice, mSurface, {}) == rhi::DeviceStatus::Ready &&
               rhi::BeginFrame(mDevice, mSurface) == rhi::DeviceStatus::Skipped;
    }
    /// A frame holds either the legacy fullscreen draw or raster passes. A shader frame draws
    /// the diagnostic into the target; a raster frame clears the named texture in a named
    /// pass. Both record inside the named view scope.
    [[nodiscard]] bool RenderFrame(bool isRasterFrame) noexcept
    {
        const DiagnosticUniforms uniforms =
        {
            .Resolution = {static_cast<float32>(WIDTH), static_cast<float32>(HEIGHT)},
            .Elapsed = 0,
            .Padding0 = 0,
            .Direction = {0.2F, 0.4F, 0.6F},
            .Padding1 = 0,
            .Tint = {0.6F, 0.4F, 0.2F, 1.0F},
        };
        const BorrowedView<const uint8> bytes(reinterpret_cast<const uint8*>(&uniforms), sizeof(uniforms));
        if (rhi::SetFrameTarget(mDevice, mSurface, { .Width = WIDTH, .Height = HEIGHT }) != rhi::DeviceStatus::Ready ||
            rhi::BeginFrame(mDevice, mSurface) != rhi::DeviceStatus::Ready)
        {
            return false;
        }
        bool isRecorded = rhi::BeginGpuScope(mDevice, mView) == rhi::RasterStatus::Ready;
        if (isRecorded == true && isRasterFrame == true)
        {
            const rhi::GpuScope pass(mDevice, mPass);
            isRecorded = rhi::BeginRasterPass(mDevice, { .Color = mTexture }) == rhi::RasterStatus::Ready;
        }
        else if (isRecorded == true)
        {
            isRecorded = rhi::UpdateUniform(mDevice, mUniform, bytes) == rhi::ResourceStatus::Ready &&
                         rhi::DrawFullscreen(mDevice, mPipeline) == rhi::ResourceStatus::Ready;
        }
        isRecorded = isRecorded == true && rhi::EndGpuScope(mDevice) == rhi::RasterStatus::Ready;
        return rhi::EndFrame(mDevice) == rhi::DeviceStatus::Ready && isRecorded == true;
    }

private:
    rhi::DeviceHandle mDevice;
    rhi::SurfaceHandle mSurface;
    rhi::ShaderHandle mVertex;
    rhi::ShaderHandle mFragment;
    rhi::UniformHandle mUniform;
    rhi::PipelineHandle mPipeline;
    rhi::TextureHandle mTexture;
    rhi::GpuScopeDescription mView;
    rhi::GpuScopeDescription mPass;
};

/// Poll a request between frames until it is terminal or its deadline passes.
[[nodiscard]] bool WaitForReport(rhi::GraphicsCaptureIdentity identity, rhi::GraphicsCaptureReport& outReport) noexcept
{
    time::Deadline deadline;
    if (deadline.Arm(time::Now(), time::Milliseconds(CAPTURE_DEADLINE_MILLISECONDS)) != time::TimeStatus::Ok)
    {
        return false;
    }
    while (rhi::GetGraphicsCaptureReport(identity, outReport) == rhi::GraphicsCaptureError::None)
    {
        if (IsActive(outReport.State) == false || deadline.IsExpired(time::Now()) == true)
        {
            return true;
        }
        threading::SleepFor(time::Milliseconds(POLL_MILLISECONDS));
    }
    return false;
}

[[nodiscard]] bool
CaptureFrame(Fixture& inoutFixture, BorrowedView<const char> destination, bool isRasterFrame) noexcept
{
    const rhi::GraphicsCaptureRequest request =
    {
        .Destination = destination,
        .DeadlineMilliseconds = CAPTURE_DEADLINE_MILLISECONDS,
    };
    rhi::GraphicsCaptureIdentity identity;
    const rhi::GraphicsCaptureError requested =
        rhi::RequestGraphicsCapture(inoutFixture.GetDevice(), request, identity);
    WriteLine("request", GetErrorName(requested));
    if (requested != rhi::GraphicsCaptureError::None || inoutFixture.SkipFrame() == false ||
        inoutFixture.RenderFrame(isRasterFrame) == false)
    {
        return false;
    }
    rhi::GraphicsCaptureReport report;
    if (WaitForReport(identity, report) == false)
    {
        return false;
    }
    const bool isComplete = report.State == rhi::GraphicsCaptureState::Complete;
    WriteLine("state", isComplete == true ? "Complete" : "Failed");
    WriteLine("error", GetErrorName(report.Error));
    WriteLine("artifact", report.Artifact.GetData());
    WriteLine("manifest", report.Manifest.GetData());
    WriteLine("toolVersion", report.ToolVersion.GetData());
    WriteLine("shaderSource", report.ShaderSource.GetData());
    WriteLine("diagnostic", report.Diagnostic.GetData());
    return isComplete == true;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    if (argumentCount != EXPECTED_ARGUMENT_COUNT)
    {
        WriteLine("usage", "ludus_metal_capture_consumer <existing-empty-directory>");
        return 1;
    }
    const BorrowedView<const char> destination(arguments[1], std::strlen(arguments[1]));
    Fixture fixture;
    if (fixture.Create() == false)
    {
        WriteLine("result", "device or resource creation failed");
        fixture.Destroy();
        return 1;
    }
    rhi::GraphicsCaptureCapabilities capabilities;
    if (rhi::GetGraphicsCaptureCapabilities(fixture.GetDevice(), capabilities) != rhi::GraphicsCaptureError::None)
    {
        fixture.Destroy();
        return 1;
    }
    WriteLine("capabilities", GetErrorName(capabilities.Unavailable));
    if (capabilities.IsRestartRequired == true)
    {
        // The repair is a relaunch: the capture layer is inserted only at process start.
        WriteLine("fix", "relaunch with MTL_CAPTURE_ENABLED=1 in the environment");
        fixture.Destroy();
        return RESTART_REQUIRED_EXIT;
    }
    // The first frame is drawn before any request, so it is never part of a capture. The first
    // capture holds the shader frame and the second the raster frame.
    bool isCaptured = capabilities.CanCaptureFrame == true && fixture.RenderFrame(false) == true;
    for (uint32 i = 0; i < CAPTURE_COUNT && isCaptured == true; ++i)
    {
        isCaptured = CaptureFrame(fixture, destination, i != 0);
    }
    // Later frames are outside every request.
    isCaptured = isCaptured == true && fixture.RenderFrame(true) == true;
    fixture.Destroy();
    WriteLine("result", isCaptured == true ? "passed" : "failed");
    return isCaptured == true ? 0 : 1;
}
