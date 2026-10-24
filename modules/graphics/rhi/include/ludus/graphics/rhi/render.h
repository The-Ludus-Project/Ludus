#pragma once

#include <ludus/foundation/base/types.h>

#include <span>
#include <string_view>

namespace ludus::graphics::rhi
{
namespace internal
{
struct ResourceAccess;
}
/// @brief Values identify one owned resource in one session, never backend objects.
///
/// Copying a handle does not copy ownership. Destroy exactly once, between frames.
struct ShaderHandle final
{
private:
    ludus::foundation::uint32 Id = 0;
    friend struct internal::ResourceAccess;
};
struct UniformHandle final
{
private:
    ludus::foundation::uint32 Id = 0;
    friend struct internal::ResourceAccess;
};
struct PipelineHandle final
{
private:
    ludus::foundation::uint32 Id = 0;
    friend struct internal::ResourceAccess;
};
enum class ResourceStatus : ludus::foundation::uint8
{
    Ready,
    Pending,
    Failed,
    NotReady,
    InvalidHandle,
    InvalidState,
    InvalidDescription,
    CapacityExceeded,
    InUse,
    /// An explicit device-owner operation was stopped by device loss; recreate the session.
    DeviceLost,
};
/// Shader execution stage; Compute is accepted by the R4 reflected resource API.
enum class ShaderStage : ludus::foundation::uint8
{
    /// Vertex input and transform stage.
    Vertex,
    /// Raster fragment stage.
    Fragment,
    /// Buffer compute stage, used only by the R4 reflected shader API.
    Compute
};
/// Per-target shader artifacts, borrowed only during CreateShader.
/// Each backend consumes its own source/binary and explicit entry name.
/// A browser build carries both WGSL and GLSL ES for startup selection.
struct ShaderDescription final
{
    /// Shader stage; the selected entry must have this stage.
    ShaderStage Stage = ShaderStage::Vertex;
    /// Minimum occupied uniform bytes from the selected artifact's reflection.
    ludus::foundation::usize UniformSize = 0;
    /// Vulkan SPIR-V words.
    std::span<const ludus::foundation::uint32> Spirv;
    /// WebGPU WGSL source.
    std::string_view Wgsl;
    /// WebGL 2 GLSL ES 3.00 source.
    std::string_view GlslEs = "";
    /// Entry in Spirv.
    std::string_view SpirvEntry;
    /// Entry in Wgsl.
    std::string_view WgslEntry;
    /// Entry in GlslEs (main).
    std::string_view GlslEsEntry = "";
    /// Metal Shading Language 2.3 source, compiled at resource creation.
    std::string_view Msl = "";
    /// Entry in Msl, independently verified against the compiled stage.
    std::string_view MslEntry = "";
    /// @brief Direct3D 12 DXIL container (shader model 6.0), as written by the pinned DXC.
    ///
    /// The bytes are copied during CreateShader; the caller keeps them alive only for that
    /// call. The container must be signed by the DXIL validator, hold the program for
    /// Stage, and use at most one constant buffer at register b0, space 0.
    /// @note `ludus_compile_shader` emits DXIL only when CMake targets a Windows host.
    // Like Msl, defaulted so designated initializers that predate it stay complete under
    // -Wmissing-designated-field-initializers (which fires for the MSVC STL span).
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::span<const ludus::foundation::uint8> Dxil = {};
    /// @brief Entry in Dxil, checked against the container's pipeline state validation part.
    ///
    /// A container records one entry; a different name or stage fails CreateShader.
    std::string_view DxilEntry = "";
};
/// One binding: set/group 0, binding 0, visible to vertex and fragment.
struct PipelineDescription final
{
    ShaderHandle Vertex;
    ShaderHandle Fragment;
    UniformHandle Uniform;
};
/// @brief Bounded slice: eight resources of each kind; uniform size 16..16384, multiple
/// of 16, further bounded by StartupInfo.Capabilities.MaxUniformBufferSize.
///
/// Upload bytes follow the application's independently verified layouts.
[[nodiscard]] ResourceStatus CreateShader(const ShaderDescription&, ShaderHandle&) noexcept;
[[nodiscard]] ResourceStatus CreateUniform(ludus::foundation::usize size, UniformHandle&) noexcept;
/// @brief Pending means the output owns a resource.
///
/// Pending dependencies instead return NotReady without creating a pipeline; poll those dependencies before
/// retrying.
[[nodiscard]] ResourceStatus CreatePipeline(const PipelineDescription&, PipelineHandle&) noexcept;
[[nodiscard]] ResourceStatus GetStatus(ShaderHandle) noexcept;
[[nodiscard]] ResourceStatus GetStatus(UniformHandle) noexcept;
[[nodiscard]] ResourceStatus GetStatus(PipelineHandle) noexcept;
enum class SurfaceEncoding : ludus::foundation::uint8
{
    Unorm,
    Srgb
};
struct FrameInfo final
{
    ludus::foundation::uint32 Width = 0;
    ludus::foundation::uint32 Height = 0;
    SurfaceEncoding Encoding = SurfaceEncoding::Unorm;
};
/// Acquired extent/attachment conversion, valid only in an open frame.
/// Offscreen-only frames report their logical target extent; encoding remains the surface format.
[[nodiscard]] FrameInfo GetFrameInfo() noexcept;
/// @brief Full replacement between frames or before the open frame's first draw.
///
/// No allocation; latest bytes are snapshotted
/// into the next acquired frame. A uniform must be updated before its first draw.
[[nodiscard]] ResourceStatus UpdateUniform(UniformHandle, std::span<const ludus::foundation::uint8>) noexcept;
[[nodiscard]] ResourceStatus Destroy(ShaderHandle) noexcept;
[[nodiscard]] ResourceStatus Destroy(UniformHandle) noexcept;
[[nodiscard]] ResourceStatus Destroy(PipelineHandle) noexcept;
/// @brief Exactly one non-indexed triangle in an open frame, current full surface extent.
///
/// Pending/failed/stale resources and a second draw are rejected.
[[nodiscard]] ResourceStatus DrawFullscreen(PipelineHandle) noexcept;
} // namespace ludus::graphics::rhi
