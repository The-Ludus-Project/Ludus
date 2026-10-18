#pragma once

// Best-effort glTF 2.0 / GLB scene reader into SceneIR (gltf-best-effort-v1).
//
// Thanks to the Khronos Group 3D Formats Working Group, "glTF 2.0
// Specification" (https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html),
// and the KHR_lights_punctual, KHR_materials_emissive_strength,
// KHR_mesh_quantization and KHR_texture_transform extension specifications
// (https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos), for
// the container, accessor, node, material, camera and light semantics mapped
// here. This is an original reader over the strict FoundationParsing JSON
// facade; the supported subset is documented in
// docs/development/best-effort-scene-import.md.

#include "../import_diagnostics.h"
#include "../scene_ir.h"
#include "../source_resolver.h"

#include <ludus/content/scene_cook/limits.h>
#include <ludus/content/scene_cook/scene_cook.h>
#include <ludus/foundation/parsing/json.hpp>

#include <span>
#include <string_view>

namespace ludus::content::scene_cook::importers::gltf
{
/// Largest admitted entry file (.gltf JSON or .glb container): binary scene data policy.
inline constexpr usize MAX_ENTRY_BYTES = MAX_SOURCE_DATA_BYTES;
/// Largest admitted JSON chunk or .gltf document: the text source policy.
inline constexpr usize MAX_JSON_BYTES = MAX_SOURCE_BYTES;
/// Largest admitted external or data-URI buffer: binary scene data policy.
inline constexpr usize MAX_BUFFER_BYTES = MAX_SOURCE_DATA_BYTES;
/// Largest admitted embedded image (bufferView or data URI): the source image policy.
inline constexpr usize MAX_EMBEDDED_IMAGE_BYTES = MAX_SOURCE_IMAGE_BYTES;
/// Most entries in each glTF array (nodes, meshes, accessors, ...): the JSON facade's
/// default array element budget.
inline constexpr usize MAX_ARRAY_ENTRIES = foundation::parsing::JsonLimits{}.MaxArrayElements;
/// Deepest admitted node hierarchy.
inline constexpr uint32 MAX_NODE_DEPTH = 256;
/// Most mesh placements (node x primitive pairs) kept in the IR.
inline constexpr uint32 MAX_PLACEMENTS = 1000000;

/// Read a glTF JSON or GLB entry file into outScene.
/// @param entryBytes Complete entry file; the container is detected from its magic.
/// @param entryName Root-relative name recorded for the entry.
/// @param inoutResolver Supplies external buffers (read now) and resolves external
/// image paths (read later by the cook). Data URIs never touch it.
/// @param outScene Replaced.
/// @param inoutLog Receives diagnostics (line 0: JSON positions are not tracked).
/// @param cancelOrNull Optional hook polled per node and primitive.
/// @return Ok; Invalid when the log holds errors (malformed container or JSON,
/// out-of-range accessors, unsupported required extensions); Cancelled; OutOfMemory.
[[nodiscard]] Status ReadScene(std::span<const uint8> entryBytes,
                               std::string_view entryName,
                               SourceResolver& inoutResolver,
                               SceneIr& outScene,
                               DiagnosticLog& inoutLog,
                               const Cancellation* cancelOrNull = nullptr) noexcept;
} // namespace ludus::content::scene_cook::importers::gltf
