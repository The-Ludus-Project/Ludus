#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/gameplay/camera/evaluation.hpp>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/base/window.h>

#include <span>
#include <string_view>
#include <type_traits>

#if defined(__EMSCRIPTEN__)
#    include <emscripten.h>
#endif

#include "camera.hpp"
#include "cornell.h"

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
LUDUS_DEFINE_LOG_CATEGORY(LOG_SAMPLE, "CornellBox");

#if defined(__EMSCRIPTEN__)
// clang-format off
EM_JS(void, WebStatus, (const char* state, uint32 frames), {
    globalThis.ludusPlayerStatus(UTF8ToString(state), frames);
});
EM_ASYNC_JS(void, AwaitBrowserFrame, (), {
    await new Promise(done => requestAnimationFrame(done));
});
// clang-format on
#endif

// Five float4 rows match each emitted target; camera rays are renderer-owned.
struct alignas(16) Uniforms final
{
    float32 Viewport[4];
    cornell::CameraRays Camera;
};
static_assert(std::is_standard_layout_v<Uniforms>);
static_assert(offsetof(Uniforms, Viewport) == 0);
static_assert(offsetof(Uniforms, Camera) == 16);
static_assert(offsetof(cornell::CameraRays, Right) == 16 && offsetof(cornell::CameraRays, Up) == 32 &&
              offsetof(cornell::CameraRays, Forward) == 48);
static_assert(sizeof(Uniforms) == 80 && alignof(Uniforms) == 16);

struct Options final
{
    bool Headless = false;
    bool Flat = false;
    bool Help = false;
    uint32 Frames = 0;
};
bool ParseOptions(int32 count, const char* const* arguments, Options& options) noexcept
{
    for (int32 index = 1; index < count; ++index)
    {
        const std::string_view argument(arguments[index]);
        if (argument == "--headless")
        {
            options.Headless = true;
        }
        else if (argument == "--flat")
        {
            options.Flat = true;
        }
        else if (argument == "--help")
        {
            options.Help = true;
        }
        else if (argument.starts_with("--frames="))
        {
            const auto digits = argument.substr(9);
            uint32 frames = 0;
            if (digits.empty())
            {
                return false;
            }
            for (const char digit : digits)
            {
                if (digit < '0' || digit > '9' || frames > 10000)
                {
                    return false;
                }
                frames = frames * 10 + static_cast<uint32>(digit - '0');
            }
            if (frames == 0 || frames > 10000)
            {
                return false;
            }
            options.Frames = frames;
        }
        else
        {
            return false;
        }
    }
    return options.Headless == false || options.Frames != 0 || options.Help;
}

class Renderer final
{
public:
    ~Renderer()
    {
        rhi::Shutdown();
    }
    bool Start(const rhi::WindowInfo& window) noexcept
    {
        const auto started = rhi::Start({ .Name = "Ludus Cornell Box", .Version = 1 }, window);
#if defined(__EMSCRIPTEN__)
        // Browser device/pipeline callbacks need the event loop while the native
        // sample retains its synchronous lifetime. Asyncify is confined to this player.
        for (uint32 attempt = 0; rhi::GetStartup().State == rhi::StartupState::Pending && attempt < 3000; ++attempt)
        {
            emscripten_sleep(10);
        }
#endif
        if ((started != rhi::StartStatus::Ready && started != rhi::StartStatus::Pending) ||
            rhi::GetStartup().State != rhi::StartupState::Ready)
        {
            LUDUS_LOG_ERROR(LOG_SAMPLE, "RHI startup failed (error {}).", static_cast<uint32>(rhi::GetStartup().Error));
            return false;
        }
        if (ludus::shaders::cornell::Vertex().UniformSize > sizeof(Uniforms) ||
            ludus::shaders::cornell::Fragment().UniformSize != sizeof(Uniforms))
        {
            LUDUS_LOG_ERROR(LOG_SAMPLE, "Shader uniform reflection differs from the 80-byte CPU layout.");
            return false;
        }
        const auto ready = [](auto handle, rhi::ResourceStatus status) noexcept {
#if defined(__EMSCRIPTEN__)
            for (uint32 attempt = 0; status == rhi::ResourceStatus::Pending && attempt < 3000; ++attempt)
            {
                emscripten_sleep(10);
                status = rhi::GetStatus(handle);
            }
#else
            (void)handle;
#endif
            return status == rhi::ResourceStatus::Ready;
        };
        const auto vertex = rhi::CreateShader(ludus::shaders::cornell::Vertex(), mVertex);
        const auto fragment = rhi::CreateShader(ludus::shaders::cornell::Fragment(), mFragment);
        const auto uniform = rhi::CreateUniform(sizeof(Uniforms), mUniform);
        if (ready(mVertex, vertex) == false || ready(mFragment, fragment) == false || ready(mUniform, uniform) == false)
        {
            return false;
        }
        const auto pipeline =
            rhi::CreatePipeline({ .Vertex = mVertex, .Fragment = mFragment, .Uniform = mUniform }, mPipeline);
        if (ready(mPipeline, pipeline) == false)
        {
            LUDUS_LOG_ERROR(LOG_SAMPLE, "Could not create Cornell box shaders, uniform or pipeline.");
            return false;
        }
        return true;
    }
    rhi::FrameStatus Draw(uint32 width, uint32 height, bool flat) noexcept
    {
        const auto target = rhi::SetFrameTarget({ .Width = width, .Height = height });
        if (target != rhi::FrameStatus::Ready)
        {
            return target;
        }
        const auto begun = rhi::BeginFrameStatus();
        if (begun != rhi::FrameStatus::Ready)
        {
            return begun;
        }
        const auto frame = rhi::GetFrameInfo();
        using namespace ludus::gameplay::camera;
        CameraSample sample;
        cornell::CameraRays rays;
        if (TryEvaluateCamera(cornell::DefaultCamera(),
                              { .PresentationSequence = mPresentationSequence },
                              nullptr,
                              sample,
                              &mCameraTrace) != CameraStatus::Success ||
            cornell::TryBuildCameraRays(sample, frame.Width, frame.Height, rays) != CameraStatus::Success)
        {
            LUDUS_LOG_ERROR(LOG_SAMPLE, "Could not extract the Cornell camera for the acquired framebuffer.");
            rhi::Shutdown();
            return rhi::FrameStatus::Failed;
        }
        const Uniforms uniforms
        {
            .Viewport = {static_cast<float32>(frame.Width),
                         static_cast<float32>(frame.Height),
                         frame.Encoding == rhi::SurfaceEncoding::Unorm ? 1.0F : 0.0F,
                         flat ? 1.0F : 0.0F},
            .Camera = rays,
        };
        const auto bytes = std::as_bytes(std::span(&uniforms, 1));
        if (rhi::UpdateUniform(mUniform, {reinterpret_cast<const uint8*>(bytes.data()), bytes.size()}) !=
                rhi::ResourceStatus::Ready ||
            rhi::DrawFullscreen(mPipeline) != rhi::ResourceStatus::Ready)
        {
            // Shutdown aborts the open frame and releases partially created resources.
            rhi::Shutdown();
            return rhi::FrameStatus::Failed;
        }
        const auto ended = rhi::EndFrameStatus();
        if (ended == rhi::FrameStatus::Ready)
        {
            ++mPresentationSequence;
        }
        return ended;
    }

private:
    ludus::gameplay::camera::CameraTrace mCameraTrace;
    uint64 mPresentationSequence = 0;
    rhi::ShaderHandle mVertex;
    rhi::ShaderHandle mFragment;
    rhi::UniformHandle mUniform;
    rhi::PipelineHandle mPipeline;
};

int32 Run(const Options& options) noexcept
{
    // Declare the window first: the renderer shuts down before window destruction.
    ludus::foundation::core::UniquePtr<ludus::platform::Window> window;
    rhi::WindowInfo native;
    native.Width = 720;
    native.Height = 720;
    if (options.Headless == false)
    {
        ludus::platform::WindowManager manager;
        if (manager.Initialize({}) == false ||
            manager.CreateWindow(
                {
                    .Name = options.Flat ? "Ludus Cornell Box - Materials" : "Ludus Cornell Box - Direct Lighting",
                    .Width = 720,
                    .Height = 720,
                },
                window) == false)
        {
            LUDUS_LOG_ERROR(LOG_SAMPLE, "Could not open a native window. Use a macOS desktop or a Wayland session.");
            return 1;
        }
        native = window->GetNativeWindowInfo();
        if (native.System == ludus::platform::WindowSystem::Headless)
        {
            LUDUS_LOG_ERROR(LOG_SAMPLE,
                            "This SDK has a headless Platform backend; use a windowed SDK or --headless --frames=3.");
            return 1;
        }
    }
    Renderer renderer;
    if (renderer.Start(native) == false)
    {
        return 1;
    }
    uint32 rendered = 0;
    uint32 attempts = 0;
    while (options.Frames == 0 || rendered < options.Frames)
    {
#if defined(__EMSCRIPTEN__)
        // Render on the browser's presentation cadence rather than an independent
        // timer, which can outpace composition and accumulate GPU submissions.
        AwaitBrowserFrame();
#endif
        if (window)
        {
            if (window->HandleEvent({}) == false)
            {
                break;
            }
            native = window->GetNativeWindowInfo();
        }
        const auto status = renderer.Draw(native.Width, native.Height, options.Flat);
        if (status == rhi::FrameStatus::Ready)
        {
            ++rendered;
#if defined(__EMSCRIPTEN__)
            WebStatus("playing", rendered);
#endif
            if (rendered == 1)
            {
                LUDUS_LOG_INFO(LOG_SAMPLE, "Cornell box rendered. Close the window to exit.");
            }
        }
        else if (status != rhi::FrameStatus::Skipped)
        {
            LUDUS_LOG_ERROR(LOG_SAMPLE, "Render failed (frame status {}).", static_cast<uint32>(status));
            return 1;
        }
        // Bounded runs must fail rather than pass/hang if no frames are acquired.
        if (options.Frames != 0 && ++attempts > options.Frames + 1000)
        {
            return 1;
        }
    }
    return options.Frames != 0 && rendered != options.Frames ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
    using namespace ludus::foundation::logging;
    LogConfig config;
    config.EnableConsole = true;
    config.EnableDebugger = false;
    config.EnableFile = false;
    LogSystem::Initialize(config);
    Options options;
    int result = 2;
    if (ParseOptions(argc, argv, options) == false)
    {
        LUDUS_LOG_ERROR(LOG_SAMPLE, "Usage: cornell_box [--flat] [--frames=1..10000] [--headless --frames=N]");
    }
    else if (options.Help)
    {
        LUDUS_LOG_INFO(LOG_SAMPLE, "Usage: cornell_box [--flat] [--frames=1..10000] [--headless --frames=N]");
        result = 0;
    }
    else
    {
        result = Run(options);
    }
    LogSystem::Shutdown();
#if defined(__EMSCRIPTEN__)
    if (result != 0)
    {
        WebStatus("failed", 0);
    }
#endif
    return result;
}
