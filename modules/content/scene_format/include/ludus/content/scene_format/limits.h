#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::content::scene_format
{
using foundation::uint32;
using foundation::usize;

// Inclusive admission ceilings shared by producers and runtime decoders.
/// Largest admitted bundle.
inline constexpr usize MAX_BUNDLE_BYTES = usize{64} * 1024 * 1024;
/// Most meshes in a bundle.
inline constexpr uint32 MAX_MESHES = 6;
/// Most materials in a bundle.
inline constexpr uint32 MAX_MATERIALS = 8;
/// Most textures in a bundle.
inline constexpr uint32 MAX_TEXTURES = 8;
/// Most placed objects in a bundle.
inline constexpr uint32 MAX_OBJECTS = 128;
/// Largest texture width or height in texels.
inline constexpr uint32 MAX_TEXTURE_DIMENSION = 4096;
/// Largest single texture in bytes.
inline constexpr usize MAX_TEXTURE_BYTES = usize{16} * 1024 * 1024;
/// Largest combined texture bytes in a bundle.
inline constexpr usize MAX_TOTAL_TEXTURE_BYTES = usize{32} * 1024 * 1024;
/// Most vertices per mesh and combined across all meshes.
inline constexpr uint32 MAX_VERTICES = 500000;
/// Most indices per mesh.
inline constexpr uint32 MAX_INDICES = 1500000;
/// Most indices combined across all meshes of a version 4 bundle. Version 4
/// consumers expand every triangle corner to derive bake charts, so this bounds
/// their expanded vertex count; version 3 stored those corners as vertices and
/// MAX_VERTICES bounded them.
inline constexpr uint32 MAX_TOTAL_INDICES = 1500000;
} // namespace ludus::content::scene_format
