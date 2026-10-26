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
struct RasterLayout;
/// Input signature semantic borrowed from the container; output is preserved on failure.
/// Location is the native input register from target-specific Slang reflection.
struct DxilVertexInput final
{
    const char* NameOrNull = nullptr;
    foundation::uint32 Index = 0;
};
[[nodiscard]] bool ReadDxilVertexInput(std::span<const foundation::uint8> bytes,
                                       foundation::uint32 location,
                                       DxilVertexInput& outInput) noexcept;

/// Validate the complete PSV0 resource table against the scalar raster layout.
/// Rejects missing/extra bindings, register spaces, kinds, ranges and duplicates.
[[nodiscard]] bool HasDxilBindings(std::span<const ludus::foundation::uint8> bytes,
                                   const RasterLayout& layout) noexcept;

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
