#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/content/scene_format/limits.h>

namespace ludus::editor
{
/// Inclusive encoded JSON file size for the preview manifest and mesh sources.
inline constexpr foundation::usize MAX_PREVIEW_DOCUMENT_BYTES = foundation::usize{1024} * 1024;
/// Inclusive encoded PNG file size; decoded textures have separate SceneFormat caps.
inline constexpr foundation::usize MAX_PREVIEW_TEXTURE_FILE_BYTES = foundation::usize{4} * 1024 * 1024;
/// Most visible objects one preview snapshot admits: a version 5 (import profile) cooked scene's
/// objects. The preview's renderer and snapshots are sized to it (PreviewGpuGeneration).
inline constexpr foundation::usize MAX_PREVIEW_OBJECTS = content::scene_format::IMPORT_LIMITS.Objects;
/// Most meshes one preview admits: a version 5 cooked scene's meshes.
inline constexpr foundation::usize MAX_PREVIEW_MESHES = content::scene_format::IMPORT_LIMITS.Meshes;
/// Most textures one preview admits: a version 5 cooked scene's textures.
inline constexpr foundation::usize MAX_PREVIEW_TEXTURES = content::scene_format::IMPORT_LIMITS.Textures;
/// Most surfaces (materials) one preview admits: a version 5 cooked scene's materials.
inline constexpr foundation::usize MAX_PREVIEW_SURFACES = content::scene_format::IMPORT_LIMITS.Materials;
/// Most authored cameras one preview snapshot admits.
inline constexpr foundation::usize MAX_PREVIEW_CAMERAS = 128;
} // namespace ludus::editor
