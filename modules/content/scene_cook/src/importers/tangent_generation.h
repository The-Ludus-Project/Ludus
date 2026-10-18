#pragma once

// MikkTSpace tangent generation for SceneIR meshes.
//
// Thanks to Morten S. Mikkelsen, "Simulation of Wrinkled Surfaces Revisited",
// master's thesis, University of Copenhagen, 2008, and the reference
// MikkTSpace implementation (https://github.com/mmikk/MikkTSpace, vendored in
// third_party/mikktspace). Tangents are computed by that reference code, so they
// match bakers that use the same convention (glTF 2.0 requires it when TANGENT
// is absent). This wrapper only adapts indexed IR meshes to its per-corner
// interface and re-indexes the result.

#include "import_diagnostics.h"
#include "scene_ir.h"

#include <ludus/content/content.h>
#include <ludus/content/scene_cook/scene_cook.h>

namespace ludus::content::scene_cook::importers
{
/// Generate MikkTSpace tangents (xyzw, w = bitangent sign) for an indexed triangle
/// mesh with texture coordinates. Missing normals are first generated as normalised
/// area-weighted vertex normals. Corners whose tangent frame differs from another
/// corner of the same vertex get their own vertex, so positions, normals and uvs may
/// be duplicated and indices rewritten.
/// @return Ok; Invalid without uvs or beyond the reference's int range (inoutMesh is
/// unchanged); OutOfMemory (inoutMesh is unchanged).
[[nodiscard]] Status GenerateTangents(IrMesh& inoutMesh) noexcept;

/// Generate tangents for every mesh placed with a tangent-space normal texture that
/// lacks them. Generated tangents stay in the IR: LUSCN v3 cannot store them yet,
/// which the report states (TANGENTS_GENERATED); meshes without uvs are reported.
/// @return Ok, Cancelled or OutOfMemory.
[[nodiscard]] Status
GenerateMissingTangents(SceneIr& inoutScene, DiagnosticLog& inoutLog, const Cancellation* cancelOrNull) noexcept;
} // namespace ludus::content::scene_cook::importers
