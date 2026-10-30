#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::content::scene_format
{
using foundation::uint32;
using foundation::usize;

// Inclusive admission ceilings shared by producers and runtime decoders.
// The unprefixed constants are the resident reference profile of versions 1-4,
// sized for the first reference scene; versions 5 and 6 admit the larger import
// profile (MAX_IMPORT_*). Select them with GetBundleLimits(version) in luscn.h.
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

/// Most meshes in a version 5 or 6 bundle; the import profile keeps one mesh per material.
inline constexpr uint32 MAX_IMPORT_MESHES = 256;
/// Most materials in a version 5 or 6 bundle.
inline constexpr uint32 MAX_IMPORT_MATERIALS = 256;
/// Most textures in a version 5 or 6 bundle.
inline constexpr uint32 MAX_IMPORT_TEXTURES = 256;
/// Most placed objects in a version 5 or 6 bundle.
inline constexpr uint32 MAX_IMPORT_OBJECTS = 1024;
/// Largest combined texture bytes in a version 5 or 6 bundle: uncompressed RGBA8 without
/// mips, enough for Sponza-scale scenes' base-colour textures at full resolution.
inline constexpr usize MAX_IMPORT_TOTAL_TEXTURE_BYTES = usize{256} * 1024 * 1024;
/// Largest version 5 or 6 bundle. Geometry is admitted by its encoded bytes,
/// with textures sharing the remaining allowance; there is no independent vertex or triangle cap.
inline constexpr usize MAX_IMPORT_BUNDLE_BYTES = usize{384} * 1024 * 1024;
/// Number of float32 components in a VertexRecord: position XYZ, normal XYZ and UV.
inline constexpr usize VERTEX_COMPONENTS = 8;
/// Conservative vertex-count bound implied by the bundle byte ceiling and the
/// smallest import vertex record (version 5). Version 6's tangents consume more
/// bytes; producers and decoders also validate the actual encoded payload.
inline constexpr uint32 MAX_IMPORT_VERTICES =
    static_cast<uint32>(MAX_IMPORT_BUNDLE_BYTES / (VERTEX_COMPONENTS * sizeof(foundation::float32)));
/// Conservative index-count bound implied by the bundle byte ceiling. This is
/// not a separate triangle budget; vertices and all other records share those bytes.
inline constexpr uint32 MAX_IMPORT_TOTAL_INDICES = static_cast<uint32>(MAX_IMPORT_BUNDLE_BYTES / sizeof(uint32));

/// Admission ceilings of one bundle version (see GetBundleLimits).
struct BundleLimits final
{
    uint32 Meshes = 0;           ///< Most meshes.
    uint32 Materials = 0;        ///< Most materials.
    uint32 Textures = 0;         ///< Most textures.
    uint32 Objects = 0;          ///< Most placed objects.
    usize TotalTextureBytes = 0; ///< Largest combined RGBA8 texture bytes.
    uint32 Vertices = 0;         ///< Most vertices combined across meshes.
    uint32 TotalIndices = 0;     ///< Most indices combined across meshes (versions 4 to 6).
    usize BundleBytes = 0;       ///< Largest bundle.
};
/// Versions 1-4: the resident reference profile. Versions 1-3 store expanded bake
/// corners as vertices, so only versions 4 and later check TotalIndices.
inline constexpr BundleLimits RESIDENT_REFERENCE_LIMITS
{
    .Meshes = MAX_MESHES,
    .Materials = MAX_MATERIALS,
    .Textures = MAX_TEXTURES,
    .Objects = MAX_OBJECTS,
    .TotalTextureBytes = MAX_TOTAL_TEXTURE_BYTES,
    .Vertices = MAX_VERTICES,
    .TotalIndices = MAX_TOTAL_INDICES,
    .BundleBytes = MAX_BUNDLE_BYTES,
};
/// Versions 5 and 6: the import profile for best-effort reference scenes.
inline constexpr BundleLimits IMPORT_LIMITS
{
    .Meshes = MAX_IMPORT_MESHES,
    .Materials = MAX_IMPORT_MATERIALS,
    .Textures = MAX_IMPORT_TEXTURES,
    .Objects = MAX_IMPORT_OBJECTS,
    .TotalTextureBytes = MAX_IMPORT_TOTAL_TEXTURE_BYTES,
    .Vertices = MAX_IMPORT_VERTICES,
    .TotalIndices = MAX_IMPORT_TOTAL_INDICES,
    .BundleBytes = MAX_IMPORT_BUNDLE_BYTES,
};
} // namespace ludus::content::scene_format
