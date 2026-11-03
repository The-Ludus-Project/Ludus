#pragma once

// Ludus scene source (.luscene.json) reader into SceneIR (ludus-scene-v1). The
// document is parsed and validated by Ludus::SceneSource; this front-end only maps
// its convex polygons, materials, emitters and camera onto the IR. The format is
// documented in docs/architecture/scene-source-format.md.

#include "../import_diagnostics.h"
#include "../scene_ir.h"

#include <ludus/content/scene_cook/scene_cook.h>
#include <ludus/content/scene_source/scene_source.h>

#include <span>
#include <string_view>

namespace ludus::content::scene_cook::importers::ludus_scene
{
/// Largest admitted entry file: the scene source format's own document limit.
inline constexpr usize MAX_ENTRY_BYTES = scene_source::MAX_SCENE_SOURCE_BYTES;

/// Read a scene source entry file into outScene.
/// @param entryBytes Complete entry file.
/// @param entryName Root-relative name recorded for the entry.
/// @param outScene Replaced. Each polygon becomes a triangle fan with its own vertices and
/// its flat geometric normal, so shading never blends across polygon edges.
/// @param inoutLog Receives the parse failure (line 0: JSON positions are not tracked) and
/// the approximations the cooked format makes.
/// @param cancelOrNull Optional hook polled per object.
/// @return Ok; Invalid when the document is not a valid scene source; Cancelled; OutOfMemory.
[[nodiscard]] Status ReadScene(std::span<const uint8> entryBytes,
                               std::string_view entryName,
                               SceneIr& outScene,
                               DiagnosticLog& inoutLog,
                               const Cancellation* cancelOrNull = nullptr) noexcept;
} // namespace ludus::content::scene_cook::importers::ludus_scene
