#pragma once
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>

#include <ludus/graphics/rhi/render.h>

namespace ludus::graphics::rhi::internal
{
/// SPIR-V magic word and fixed module header width, in uint32 words.
inline constexpr ludus::foundation::uint32 SPIRV_MAGIC = 0x07230203U;
inline constexpr ludus::foundation::usize SPIRV_HEADER_WORDS = 5;
/// OpEntryPoint's literal entry name follows the instruction word, execution model and entry ID.
inline constexpr ludus::foundation::usize SPIRV_ENTRY_POINT_NAME_WORD_OFFSET = 3;
inline constexpr ludus::foundation::usize RESOURCE_CAPACITY = 8;
inline constexpr ludus::foundation::usize UNIFORM_CAPACITY = 16384;
// Stable process storage and monotonically assigned IDs; no application pointers.
void ResourceComplete(ludus::foundation::uint32 id, ResourceStatus status) noexcept;
void ReleaseResources() noexcept;
/// The fullscreen resource operations of render.h for the bound owner slot (R14c). The public
/// device-less forms bind the first slot; the explicit-device forms bind their device.
[[nodiscard]] ResourceStatus CreateShader(const ShaderDescription& description, ShaderHandle& outHandle) noexcept;
[[nodiscard]] ResourceStatus CreateUniform(ludus::foundation::usize size, UniformHandle& outHandle) noexcept;
[[nodiscard]] ResourceStatus CreatePipeline(const PipelineDescription& description, PipelineHandle& outHandle) noexcept;
[[nodiscard]] ResourceStatus GetStatus(ShaderHandle handle) noexcept;
[[nodiscard]] ResourceStatus GetStatus(UniformHandle handle) noexcept;
[[nodiscard]] ResourceStatus GetStatus(PipelineHandle handle) noexcept;
[[nodiscard]] ResourceStatus
UpdateUniform(UniformHandle handle, ludus::foundation::BorrowedView<const ludus::foundation::uint8> bytes) noexcept;
[[nodiscard]] ResourceStatus Destroy(ShaderHandle handle) noexcept;
[[nodiscard]] ResourceStatus Destroy(UniformHandle handle) noexcept;
[[nodiscard]] ResourceStatus Destroy(PipelineHandle handle) noexcept;
[[nodiscard]] ResourceStatus DrawFullscreen(PipelineHandle handle) noexcept;
} // namespace ludus::graphics::rhi::internal
namespace ludus::graphics::rhi::backend
{
ResourceStatus
CreateShader(ludus::foundation::usize slot, const ShaderDescription&, ludus::foundation::uint32 id) noexcept;
struct UniformDescription final
{
    ludus::foundation::usize Size = 0;
};
struct PipelineResources final
{
    ludus::foundation::usize Vertex = 0;
    ludus::foundation::usize Fragment = 0;
    ludus::foundation::usize Uniform = 0;
};
ResourceStatus
CreateUniform(ludus::foundation::usize slot, const UniformDescription&, ludus::foundation::uint32 id) noexcept;
ResourceStatus
CreatePipeline(ludus::foundation::usize slot, const PipelineResources&, ludus::foundation::uint32 id) noexcept;
void DestroyShader(ludus::foundation::usize slot) noexcept;
void DestroyUniform(ludus::foundation::usize slot) noexcept;
void DestroyPipeline(ludus::foundation::usize slot) noexcept;
void UpdateUniform(ludus::foundation::usize slot,
                   ludus::foundation::BorrowedView<const ludus::foundation::uint8> bytes) noexcept;
FrameInfo GetFrameInfo() noexcept;
ResourceStatus Draw(ludus::foundation::usize slot) noexcept;
} // namespace ludus::graphics::rhi::backend
