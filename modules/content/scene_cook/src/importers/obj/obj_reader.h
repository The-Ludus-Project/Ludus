#pragma once

// Best-effort Wavefront OBJ + MTL scene reader into SceneIR (obj-best-effort-v1).
//
// Thanks to Wavefront Technologies / Alias, "Object Files (.obj)" and
// "Material Library File (.mtl)", Advanced Visualizer manual, appendices B1
// and B2 (https://paulbourke.net/dataformats/obj/,
// https://paulbourke.net/dataformats/mtl/), for the record grammar, relative
// indices and material statements, and to Robert Hardy's PBR extension notes
// (http://exocortex.com/blog/extending_wavefront_mtl_to_support_pbr) for the
// Pr/Pm keys. This is an original reader; the supported subset is documented
// in docs/development/best-effort-scene-import.md.

#include "../byte_source.h"
#include "../import_diagnostics.h"
#include "../scene_ir.h"
#include "../source_resolver.h"

#include <ludus/content/scene_cook/limits.h>
#include <ludus/content/scene_cook/scene_cook.h>

#include <string_view>

namespace ludus::content::scene_cook::importers::obj
{
/// Longest admitted logical line (after joining '\' continuations).
inline constexpr usize MAX_LINE_BYTES = usize{64} * 1024;
/// Most corners in one face; larger polygons are skipped (fan triangulation bound).
inline constexpr uint32 MAX_FACE_CORNERS = 64;
/// Most entries in each of the v, vt and vn pools.
inline constexpr uint32 MAX_POOL_ENTRIES = uint32{1} << 28;
/// Most face corners across the file.
inline constexpr uint64 MAX_TOTAL_CORNERS = uint64{1} << 30;
/// Largest admitted material library: the text source policy.
inline constexpr usize MAX_MTL_BYTES = MAX_SOURCE_BYTES;
/// Most material libraries read for one scene.
inline constexpr uint32 MAX_MATERIAL_LIBRARIES = 256;

/// Read an OBJ entry (and the MTL libraries it names) into outScene.
/// @param inoutEntry Streamed entry file; read to the end so its digest covers every byte.
/// @param entryName Root-relative name recorded for the entry.
/// @param inoutResolver Supplies material libraries; texture paths are resolved through
/// it too but read later by the cook.
/// @param outScene Replaced; meshes keep object space with identity placements.
/// @param inoutLog Receives diagnostics with file:line.
/// @param cancelOrNull Optional hook polled every few thousand lines.
/// @return Ok; Invalid when the log holds errors (malformed records, bad indices,
/// limits); IoError; Cancelled; OutOfMemory.
[[nodiscard]] Status ReadScene(ByteSource& inoutEntry,
                               std::string_view entryName,
                               SourceResolver& inoutResolver,
                               SceneIr& outScene,
                               DiagnosticLog& inoutLog,
                               const Cancellation* cancelOrNull = nullptr) noexcept;
} // namespace ludus::content::scene_cook::importers::obj
