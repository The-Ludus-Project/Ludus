#pragma once
#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/render.h>

#include <span>
#include <string_view>

// Bounded reader for the DXIL container that DXC writes and Direct3D 12 consumes
// (docs/architecture/windows-direct3d12.md#dxil-artifacts). It is portable so the
// facade validates ShaderDescription::Dxil on every build and tests run off Windows.
namespace ludus::graphics::rhi::internal
{
/// Bytes in the fixed container header: four-character code, 16-byte hash, version,
/// total size and part count.
inline constexpr ludus::foundation::usize DXIL_CONTAINER_HEADER_BYTES = 32;

/// True when bytes start with a DXBC container header whose recorded total size is
/// exactly bytes.size(). Part contents are not inspected.
[[nodiscard]] bool IsDxilContainer(std::span<const ludus::foundation::uint8> bytes) noexcept;

/// True when the container's parts lie within bytes, its DXIL program is of stage's
/// kind, and its pipeline state validation part (PSV0, runtime info version 3 or
/// later) names entry with the same stage. Any malformed or missing part is false.
[[nodiscard]] bool
HasDxilEntry(std::span<const ludus::foundation::uint8> bytes, ShaderStage stage, std::string_view entry) noexcept;
} // namespace ludus::graphics::rhi::internal
