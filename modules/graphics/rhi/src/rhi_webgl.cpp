#include "internal/backend.h"
#include "internal/lifecycle.h"
#include "internal/measurement.h"
#include "internal/raster.h"
#include "internal/resources.h"
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>

#include <cstring>
#include <span>
#include <string_view>

#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

// Private WebGL 2 (GLES3) backend for the browser fallback. It implements only
// the existing fullscreen contract: generated GLSL ES 3.00 stages, one std140
// uniform block at binding 0, bounded buffers/pipelines, one non-indexed
// fullscreen triangle, frame info, resize, context loss/restoration and resource
// destruction. On the Auto build it is compiled into backend::webgl and the
// dispatcher (rhi_web.cpp) routes to it; standalone it defines backend::.
#if !defined(LUDUS_RHI_WEBGL_NAMESPACE)
#    define LUDUS_RHI_WEBGL_NAMESPACE backend
#endif
namespace ludus::graphics::rhi::LUDUS_RHI_WEBGL_NAMESPACE
{
namespace
{
using namespace ludus::foundation;
constexpr logging::LogCategory LOG_RHI{"RHI-WebGL2"};
constexpr uint32 UNIFORM_BINDING = 0;

EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gContext = 0;
uint32 gSession = 0;
uint32 gMaxDimension = 0;
FrameTarget gTarget;
FrameInfo gFrameInfo;
char gSelector[256]{};

// A WebGL context on a canvas is the commitment point: a canvas that already
// holds a WebGPU context cannot acquire WebGL. The dispatcher only attempts this
// backend when no live WebGPU context was committed on the canvas.
// clang-format off
EM_JS(int32, CanCreateContext, (const char* selector), {
    try {
        const canvas = document.querySelector(UTF8ToString(selector));
        if (!(canvas instanceof HTMLCanvasElement) || !canvas.isConnected) return 0;
        // Availability is checked by emscripten_webgl_create_context below.
        // A throwaway probe would allocate an extra GPU context on every restart.
        return 1;
    } catch (_) { return 0; }
});
// clang-format on

void* Userdata(uint32 token) noexcept
{
    // HTML5 callback userdata carries an opaque numeric generation, never memory.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<void*>(static_cast<usize>(token));
}
uint32 Token(void* userdata) noexcept
{
    return static_cast<uint32>(reinterpret_cast<usize>(userdata));
}

EM_BOOL ContextLost(int, const void*, void* userdata) noexcept
{
    // Browser context loss: stop drawing and transition to the lost state. The
    // facade invalidates every resource handle; restart recreates on a usable
    // context. Old handles are never resurrected.
    if (internal::Current(Token(userdata)))
    {
        LUDUS_LOG_WARN(LOG_RHI, "WebGL 2 context lost");
        internal::Fail(Token(userdata), StartupError::DeviceLost);
    }
    return EM_TRUE; // preventDefault so the browser may later restore the context
}

bool ClampDimension(uint32 value) noexcept
{
    return value <= gMaxDimension;
}
bool ValidColor(float64 value) noexcept
{
    return value >= 0 && value <= 1;
}
} // namespace

void TimingBegin() noexcept;
void TimingEnd() noexcept;
void TimingSubmitted() noexcept;
Backend Kind() noexcept
{
    return Backend::WebGL2;
}
bool Supports(BackendSelection selection) noexcept
{
    return selection == BackendSelection::Auto || selection == BackendSelection::WebGL2;
}
StartupError Start(const ApplicationInfo&, const WindowInfo& window, uint32 token, BackendSelection) noexcept
{
    if (window.System != ludus::platform::WindowSystem::WebCanvas || window.CanvasSelector == nullptr)
    {
        return StartupError::InvalidWindow;
    }
    const auto available = CanCreateContext(window.CanvasSelector);
    if (available == 0)
    {
        return StartupError::InvalidWindow;
    }
    const auto length = std::string_view(window.CanvasSelector).size();
    if (length == 0 || length >= sizeof(gSelector))
    {
        return StartupError::InvalidWindow;
    }
    std::memcpy(gSelector, window.CanvasSelector, length);
    gSelector[length] = '\0';

    EmscriptenWebGLContextAttributes attributes;
    emscripten_webgl_init_context_attributes(&attributes);
    attributes.majorVersion = 2; // WebGL 2 / GLES3
    attributes.minorVersion = 0;
    attributes.alpha = EM_TRUE;
    attributes.depth = EM_TRUE;
    attributes.stencil = EM_FALSE;
    attributes.antialias = EM_FALSE;
    attributes.preserveDrawingBuffer = EM_FALSE;
    attributes.premultipliedAlpha = EM_TRUE;
    attributes.failIfMajorPerformanceCaveat = EM_FALSE;
    attributes.powerPreference = EM_WEBGL_POWER_PREFERENCE_DEFAULT;
    // Acquiring the context commits the live canvas; this is the point of no
    // return for context ownership on this canvas.
    gContext = emscripten_webgl_create_context(gSelector, &attributes);
    if (gContext <= 0)
    {
        gContext = 0;
        return StartupError::AdapterUnavailable;
    }
    if (emscripten_webgl_make_context_current(gContext) != EMSCRIPTEN_RESULT_SUCCESS)
    {
        emscripten_webgl_destroy_context(gContext);
        gContext = 0;
        return StartupError::DeviceUnavailable;
    }
    GLint maxDimension = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxDimension);
    GLint maxViewport[2] = {0, 0};
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maxViewport);
    const GLint viewportLimit = maxViewport[0] < maxViewport[1] ? maxViewport[0] : maxViewport[1];
    if (viewportLimit > 0 && viewportLimit < maxDimension)
    {
        maxDimension = viewportLimit;
    }
    if (maxDimension <= 0)
    {
        emscripten_webgl_destroy_context(gContext);
        gContext = 0;
        return StartupError::DeviceUnavailable;
    }
    GLint maxUniformSize = 0;
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maxUniformSize);
    if (maxUniformSize < 16)
    {
        emscripten_webgl_destroy_context(gContext);
        gContext = 0;
        return StartupError::DeviceUnavailable;
    }
    gMaxDimension = static_cast<uint32>(maxDimension);
    gSession = token;
    emscripten_set_webglcontextlost_callback(gSelector, Userdata(token), EM_FALSE, ContextLost);
    // WebGL creation is synchronous; the session is immediately usable.
    internal::Complete(
        token,
        StartupError::None,
        { .MaxFrameDimension2D = gMaxDimension, .MaxUniformBufferSize = static_cast<uint32>(maxUniformSize) });
    return StartupError::None;
}

void Shutdown() noexcept
{
    // The facade invalidates callback tokens before this runs, so a late context
    // loss cannot re-enter. Unregister listeners and release the context.
    if (gSelector[0] != '\0')
    {
        emscripten_set_webglcontextlost_callback(gSelector, nullptr, EM_FALSE, nullptr);
    }
    if (gContext > 0)
    {
        emscripten_webgl_make_context_current(gContext);
        emscripten_webgl_destroy_context(gContext);
    }
    gContext = 0;
    gSession = 0;
    gMaxDimension = 0;
    gTarget = {};
    gFrameInfo = {};
    gSelector[0] = '\0';
}
bool Initialize(const ApplicationInfo&) noexcept
{
    return false;
}
bool ConnectWindow(const WindowInfo&) noexcept
{
    return false;
}
bool InitializeRendering() noexcept
{
    return false;
}
void ShutdownRendering() noexcept {}
bool BeginFrame() noexcept
{
    return false;
}
bool EndFrame() noexcept
{
    return false;
}
FrameStatus SetTarget(const FrameTarget& target) noexcept
{
    if (target.ShouldPresentToSurface == false)
    {
        return FrameStatus::Unsupported;
    }
    if (ClampDimension(target.Width) == false || ClampDimension(target.Height) == false ||
        ValidColor(target.Red) == false || ValidColor(target.Green) == false || ValidColor(target.Blue) == false ||
        ValidColor(target.Alpha) == false)
    {
        return FrameStatus::InvalidState;
    }
    gTarget = target;
    return FrameStatus::Ready;
}
FrameStatus Begin() noexcept
{
    if (gTarget.Width == 0 || gTarget.Height == 0)
    {
        return FrameStatus::Skipped;
    }
    if (gContext <= 0 || emscripten_is_webgl_context_lost(gContext))
    {
        internal::Fail(gSession, StartupError::DeviceLost);
        return FrameStatus::Failed;
    }
    if (emscripten_webgl_make_context_current(gContext) != EMSCRIPTEN_RESULT_SUCCESS)
    {
        internal::Fail(gSession, StartupError::RenderingUnavailable);
        return FrameStatus::Failed;
    }
    TimingBegin();
    // Default framebuffer (the canvas). Resize is a viewport change; the backing
    // store is sized by Platform's resize observer, which the app mirrors into
    // the FrameTarget width/height.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, static_cast<GLsizei>(gTarget.Width), static_cast<GLsizei>(gTarget.Height));
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(static_cast<GLfloat>(gTarget.Red),
                 static_cast<GLfloat>(gTarget.Green),
                 static_cast<GLfloat>(gTarget.Blue),
                 static_cast<GLfloat>(gTarget.Alpha));
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gFrameInfo = { .Width = gTarget.Width, .Height = gTarget.Height, .Encoding = SurfaceEncoding::Unorm };
    return FrameStatus::Ready;
}
FrameStatus End() noexcept
{
    TimingEnd();
    if (gContext <= 0 || emscripten_is_webgl_context_lost(gContext))
    {
        return FrameStatus::Failed;
    }
    if (glGetError() != GL_NO_ERROR)
    {
        internal::Fail(gSession, StartupError::RenderingUnavailable);
        return FrameStatus::Failed;
    }
    // The browser presents the default framebuffer after the frame callback
    // returns; there is no explicit present. Context loss is surfaced by the
    // registered webglcontextlost callback (ContextLost), which the facade turns
    // into DeviceLost. Begin also checks loss before encoding any work.
    return FrameStatus::Ready;
}
FrameInfo GetFrameInfo() noexcept
{
    return gFrameInfo;
}

namespace
{
// A shader slot holds the compiled GLES3 stage object plus its source stage.
struct Shader final
{
    GLuint Object = 0;
    ShaderStage Stage = ShaderStage::Vertex;
};
struct Uniform final
{
    GLuint Buffer = 0;
    uint8 Bytes[internal::UNIFORM_CAPACITY]{};
    usize Size = 0;
};
struct Pipeline final
{
    GLuint Program = 0;
    GLuint VertexArray = 0;
    usize UniformSlot = 0;
    bool Ok = false;
};
Shader gShaders[internal::RESOURCE_CAPACITY];
Uniform gUniforms[internal::RESOURCE_CAPACITY];
Pipeline gPipelines[internal::RESOURCE_CAPACITY];

GLuint Compile(GLenum type, std::string_view source) noexcept
{
    if (source.size() > 0x7fffffffU)
    {
        return 0;
    }
    const GLuint object = glCreateShader(type);
    if (object == 0)
    {
        return 0;
    }
    const GLchar* data = source.data();
    const GLint length = static_cast<GLint>(source.size());
    glShaderSource(object, 1, &data, &length);
    glCompileShader(object);
    GLint compiled = GL_FALSE;
    glGetShaderiv(object, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE)
    {
        GLchar log[512]{};
        GLsizei written = 0;
        glGetShaderInfoLog(object, sizeof(log) - 1, &written, log);
        LUDUS_LOG_WARN(LOG_RHI, "GLSL ES compile failed: {}", std::string_view(log, static_cast<usize>(written)));
        glDeleteShader(object);
        return 0;
    }
    return object;
}
} // namespace

ResourceStatus CreateShader(usize slot, const ShaderDescription& description, uint32 id) noexcept
{
    // Compile the generated GLSL ES stage now; linking happens at pipeline time
    // when both stages are known. Diagnostics route through the engine logger.
    auto& shader = gShaders[slot];
    shader = {};
    shader.Stage = description.Stage;
    const GLenum type = description.Stage == ShaderStage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    shader.Object = Compile(type, description.GlslEs);
    const ResourceStatus status = shader.Object != 0 ? ResourceStatus::Ready : ResourceStatus::Failed;
    internal::ResourceComplete(id, status);
    return status;
}
ResourceStatus CreateUniform(usize slot, const backend::UniformDescription& description, uint32 id) noexcept
{
    auto& uniform = gUniforms[slot];
    uniform = {};
    uniform.Size = description.Size;
    glGenBuffers(1, &uniform.Buffer);
    if (uniform.Buffer == 0)
    {
        internal::ResourceComplete(id, ResourceStatus::Failed);
        return ResourceStatus::Failed;
    }
    glBindBuffer(GL_UNIFORM_BUFFER, uniform.Buffer);
    // Allocate the fixed byte range once; draws reuse it with glBufferSubData.
    glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(uniform.Size), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    const ResourceStatus status = glGetError() == GL_NO_ERROR ? ResourceStatus::Ready : ResourceStatus::Failed;
    internal::ResourceComplete(id, status);
    return status;
}
ResourceStatus CreatePipeline(usize slot, const backend::PipelineResources& resources, uint32 id) noexcept
{
    auto& pipeline = gPipelines[slot];
    pipeline = {};
    pipeline.UniformSlot = resources.Uniform;
    pipeline.Program = glCreateProgram();
    if (pipeline.Program == 0)
    {
        internal::ResourceComplete(id, ResourceStatus::Failed);
        return ResourceStatus::Failed;
    }
    glAttachShader(pipeline.Program, gShaders[resources.Vertex].Object);
    glAttachShader(pipeline.Program, gShaders[resources.Fragment].Object);
    glLinkProgram(pipeline.Program);
    GLint linked = GL_FALSE;
    glGetProgramiv(pipeline.Program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
    {
        GLchar log[512]{};
        GLsizei written = 0;
        glGetProgramInfoLog(pipeline.Program, sizeof(log) - 1, &written, log);
        LUDUS_LOG_WARN(LOG_RHI, "GLSL ES link failed: {}", std::string_view(log, static_cast<usize>(written)));
        glDeleteProgram(pipeline.Program);
        pipeline.Program = 0;
        internal::ResourceComplete(id, ResourceStatus::Failed);
        return ResourceStatus::Failed;
    }
    // Reject a missing or incompatible uniform block before accepting the draw.
    GLint blocks = 0;
    glGetProgramiv(pipeline.Program, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
    const GLuint resolved = blocks == 1 ? 0U : GL_INVALID_INDEX;
    if (resolved == GL_INVALID_INDEX)
    {
        LUDUS_LOG_WARN(LOG_RHI, "GLSL ES program has no single uniform block at binding 0");
        glDeleteProgram(pipeline.Program);
        pipeline.Program = 0;
        internal::ResourceComplete(id, ResourceStatus::Failed);
        return ResourceStatus::Failed;
    }
    GLint blockSize = 0;
    glGetActiveUniformBlockiv(pipeline.Program, resolved, GL_UNIFORM_BLOCK_DATA_SIZE, &blockSize);
    if (blockSize <= 0 || static_cast<usize>(blockSize) > gUniforms[resources.Uniform].Size)
    {
        LUDUS_LOG_WARN(LOG_RHI, "GLSL ES block size {} exceeds uniform buffer", blockSize);
        glDeleteProgram(pipeline.Program);
        pipeline.Program = 0;
        internal::ResourceComplete(id, ResourceStatus::Failed);
        return ResourceStatus::Failed;
    }
    glUniformBlockBinding(pipeline.Program, resolved, UNIFORM_BINDING);
    // Fullscreen triangle uses gl_VertexID with no attributes; a VAO is still
    // required to be bound for a draw in a core WebGL 2 context.
    glGenVertexArrays(1, &pipeline.VertexArray);
    if (pipeline.VertexArray == 0 || glGetError() != GL_NO_ERROR)
    {
        if (pipeline.VertexArray != 0)
        {
            glDeleteVertexArrays(1, &pipeline.VertexArray);
        }
        glDeleteProgram(pipeline.Program);
        pipeline = {};
        internal::ResourceComplete(id, ResourceStatus::Failed);
        return ResourceStatus::Failed;
    }
    pipeline.Ok = true;
    internal::ResourceComplete(id, ResourceStatus::Ready);
    return ResourceStatus::Ready;
}
void DestroyShader(usize slot) noexcept
{
    if (gShaders[slot].Object != 0)
    {
        glDeleteShader(gShaders[slot].Object);
    }
    gShaders[slot] = {};
}
void DestroyUniform(usize slot) noexcept
{
    if (gUniforms[slot].Buffer != 0)
    {
        glDeleteBuffers(1, &gUniforms[slot].Buffer);
    }
    gUniforms[slot] = {};
}
void DestroyPipeline(usize slot) noexcept
{
    auto& pipeline = gPipelines[slot];
    if (pipeline.VertexArray != 0)
    {
        glDeleteVertexArrays(1, &pipeline.VertexArray);
    }
    if (pipeline.Program != 0)
    {
        glDeleteProgram(pipeline.Program);
    }
    pipeline = {};
}
void UpdateUniform(usize slot, std::span<const uint8> bytes) noexcept
{
    // Snapshot into the CPU shadow; the next draw uploads it. No allocation.
    std::memcpy(gUniforms[slot].Bytes, bytes.data(), bytes.size());
}
ResourceStatus Draw(usize slot) noexcept
{
    const auto& pipeline = gPipelines[slot];
    const auto& uniform = gUniforms[pipeline.UniformSlot];
    glBindBuffer(GL_UNIFORM_BUFFER, uniform.Buffer);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(uniform.Size), uniform.Bytes);
    glBindBufferBase(GL_UNIFORM_BUFFER, UNIFORM_BINDING, uniform.Buffer);
    glUseProgram(pipeline.Program);
    glBindVertexArray(pipeline.VertexArray);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    return ResourceStatus::Ready;
}
} // namespace ludus::graphics::rhi::LUDUS_RHI_WEBGL_NAMESPACE

#include "internal/raster_webgl.h"

#include "internal/lifetime_webgl.h"

#include "internal/measurement_webgl.h"
