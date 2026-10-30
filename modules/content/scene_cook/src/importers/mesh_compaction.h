#pragma once

#include "scene_ir.h"

#include <ludus/content/scene_cook/scene_cook.h>

namespace ludus::content::scene_cook::importers
{
/// Share byte-identical position/normal/UV/tangent records within one mesh. Keeps
/// triangle order, winding, seams and the first occurrence's vertex order. Meshes
/// without authored normals stay unchanged so generated smoothing cannot change.
/// @return Ok, Invalid for inconsistent attributes/indices, OutOfMemory or Cancelled.
/// Failure preserves inoutMesh. Temporary storage is two uint32 arrays per vertex.
[[nodiscard]] Status CompactMesh(IrMesh& inoutMesh, const Cancellation* cancelOrNull) noexcept;
} // namespace ludus::content::scene_cook::importers
