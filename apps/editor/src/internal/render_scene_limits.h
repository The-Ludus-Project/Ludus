#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/renderer/renderer.hpp>

namespace ludus::editor
{
/// Inclusive encoded JSON file size for the preview manifest and mesh sources.
inline constexpr foundation::usize MAX_PREVIEW_DOCUMENT_BYTES = foundation::usize{1024} * 1024;
/// Inclusive encoded PNG file size; decoded textures have separate SceneFormat caps.
inline constexpr foundation::usize MAX_PREVIEW_TEXTURE_FILE_BYTES = foundation::usize{4} * 1024 * 1024;
/// Most visible objects one preview snapshot admits: the Renderer's default per-snapshot capacity,
/// which the preview's renderer keeps. Merged cooked scenes stay within content::scene_format::MAX_OBJECTS.
inline constexpr foundation::usize MAX_PREVIEW_OBJECTS = graphics::renderer::Limits{}.Objects;
/// Most authored cameras one preview snapshot admits.
inline constexpr foundation::usize MAX_PREVIEW_CAMERAS = 128;
} // namespace ludus::editor
