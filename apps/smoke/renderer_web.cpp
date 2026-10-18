#include "internal/renderer.h"
#include <ludus/foundation/base/core.h>
#include <ludus/graphics/rhi/render.h>

#include "smoke.h"

#include <span>
#include <type_traits>

// Backend-agnostic browser renderer. It drives only the public fullscreen
// rendering API with the SDK-generated generic shader (SPIR-V / WGSL / GLSL ES),
// so it animates identically on WebGPU and WebGL 2 with no backend handles or
// private probe headers. Resources are created once and reused every frame.
namespace ludus::smoke::renderer
{
namespace
{
using namespace foundation;
namespace rhi = graphics::rhi;

// Mirrors the generic shader's std140 block (see apps/smoke/shaders/smoke.slang).
struct alignas(16) SmokeUniforms final
{
    float32 Resolution[2];
    float32 Pulse;
    float32 MarkerSize;
    float32 Marker[3];
    float32 Padding;
    float32 Tint[4];
};
static_assert(std::is_standard_layout_v<SmokeUniforms>);
static_assert(offsetof(SmokeUniforms, Resolution) == 0);
static_assert(offsetof(SmokeUniforms, Pulse) == 8);
static_assert(offsetof(SmokeUniforms, MarkerSize) == 12);
static_assert(offsetof(SmokeUniforms, Marker) == 16);
static_assert(offsetof(SmokeUniforms, Tint) == 32);
static_assert(sizeof(SmokeUniforms) == 48 && alignof(SmokeUniforms) == 16);

rhi::ShaderHandle gVertex;
rhi::ShaderHandle gFragment;
rhi::UniformHandle gUniform;
rhi::PipelineHandle gPipeline;
bool gCreated = false;
bool gFailed = false;

bool Accepted(rhi::ResourceStatus status) noexcept
{
    return status == rhi::ResourceStatus::Ready || status == rhi::ResourceStatus::Pending;
}
bool Ready(rhi::ResourceStatus status) noexcept
{
    return status == rhi::ResourceStatus::Ready;
}
} // namespace

graphics::rhi::DeviceRequirements Requirements() noexcept
{
    return { .MinFrameDimension2D = 1, .MinUniformBufferSize = sizeof(SmokeUniforms) };
}
State Prepare() noexcept
{
    if (gFailed)
    {
        return State::Failed;
    }
    if (gCreated == false)
    {
        if (Accepted(rhi::CreateShader(ludus::shaders::smoke::Vertex(), gVertex)) == false ||
            Accepted(rhi::CreateShader(ludus::shaders::smoke::Fragment(), gFragment)) == false ||
            Accepted(rhi::CreateUniform(sizeof(SmokeUniforms), gUniform)) == false)
        {
            gFailed = true;
            return State::Failed;
        }
        gCreated = true;
    }
    // Poll the shader/uniform creations; Pending is never drawable.
    for (const auto status : {rhi::GetStatus(gVertex), rhi::GetStatus(gFragment), rhi::GetStatus(gUniform)})
    {
        if (status == rhi::ResourceStatus::Pending)
        {
            return State::Loading;
        }
        if (Ready(status) == false)
        {
            gFailed = true;
            return State::Failed;
        }
    }
    if (rhi::GetStatus(gPipeline) == rhi::ResourceStatus::InvalidHandle)
    {
        if (Accepted(rhi::CreatePipeline({gVertex, gFragment, gUniform}, gPipeline)) == false)
        {
            gFailed = true;
            return State::Failed;
        }
    }
    const auto pipeline = rhi::GetStatus(gPipeline);
    if (pipeline == rhi::ResourceStatus::Pending)
    {
        return State::Loading;
    }
    if (Ready(pipeline) == false)
    {
        gFailed = true;
        return State::Failed;
    }
    return State::Ready;
}
void Shutdown() noexcept
{
    // Resource IDs are invalidated by the engine on shutdown/loss; drop ours and
    // let the next Prepare recreate on a usable context.
    gVertex = {};
    gFragment = {};
    gUniform = {};
    gPipeline = {};
    gCreated = false;
    gFailed = false;
}
rhi::FrameStatus Render(const platform::browser::WindowState& window, const Simulation& simulation) noexcept
{
    const float64 pulse = simulation.Phase < 1 ? simulation.Phase : 2 - simulation.Phase;
    const auto target = rhi::SetFrameTarget(
    {
        .Width = window.FramebufferWidth,
        .Height = window.FramebufferHeight,
    });
    if (target != rhi::FrameStatus::Ready)
    {
        return target;
    }
    const auto begun = rhi::BeginFrameStatus();
    if (begun != rhi::FrameStatus::Ready)
    {
        return begun;
    }
    const auto info = rhi::GetFrameInfo();
    // Simulation X/Y are in [-1, 1]; place the marker in [0, 1] UV space.
    SmokeUniforms uniforms =
    {
        .Resolution = {static_cast<float32>(info.Width), static_cast<float32>(info.Height)},
        .Pulse = static_cast<float32>(pulse),
        .MarkerSize = 0.22F,
        .Marker =
            {
                static_cast<float32>(0.5 + simulation.X * 0.5),
                static_cast<float32>(0.5 - simulation.Y * 0.5),
                0.0F,
            },
        .Padding = 0,
        .Tint = {1.0F, 0.65F, 0.15F, 1.0F}, // orange marker (QA detects this)
    };
    const auto bytes = std::span<const uint8>(reinterpret_cast<const uint8*>(&uniforms), sizeof(uniforms));
    if (rhi::UpdateUniform(gUniform, bytes) != rhi::ResourceStatus::Ready)
    {
        return rhi::FrameStatus::Failed;
    }
    if (rhi::DrawFullscreen(gPipeline) != rhi::ResourceStatus::Ready)
    {
        return rhi::FrameStatus::Failed;
    }
    return rhi::EndFrameStatus();
}
} // namespace ludus::smoke::renderer
