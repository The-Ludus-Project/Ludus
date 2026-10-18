#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/renderer/renderer.hpp>

#include "internal/preview_shading.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"

#include <QImage>

namespace ludus::editor
{
/// Independent CPU rasterization oracle for small editor test fixtures.
///
/// Production viewports use the GPU renderer. This test helper draws every object with a
/// depth buffer, perspective-correct texture coordinates, object/surface tint, surface emission
/// and fixed editor shading (one directional preview light plus ambient). Triangles are clipped
/// against the reverse-Z near and far planes. This is a navigation preview, not the reference path tracer.
/// @param scene Decoded scene; meshes, textures and surfaces are read only.
/// @param view World-to-clip transform, for example from RenderScene::View.
/// @param width Output width in pixels; zero returns a null image.
/// @param height Output height in pixels; zero returns a null image.
/// @param mode Solid draws neutral grey with the preview light; Albedo draws unlit base colour
///        (tint and texture, no emission); Lit adds the preview light and emission.
/// @return An ARGB32 image with a top-left origin, or a null image for an empty size.
[[nodiscard]] QImage RenderSoftwareView(const RenderScene& scene,
                                        const graphics::renderer::ViewDescription& view,
                                        foundation::uint32 width,
                                        foundation::uint32 height,
                                        PreviewShading mode);
} // namespace ludus::editor
