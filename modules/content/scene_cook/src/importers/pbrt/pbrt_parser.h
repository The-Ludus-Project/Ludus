#pragma once

// Best-effort pbrt-v4 scene reader into SceneIR (pbrt-best-effort-v1).
//
// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, "pbrt-v4 Input File
// Format" (https://pbrt.org/fileformat-v4) and Physically Based Rendering,
// 4th edition (https://pbr-book.org/4ed), for the directive, graphics-state,
// instancing, shape, material and light semantics mapped here. This is an
// original converter, not a pbrt renderer or copied parser.

#include "../import_diagnostics.h"
#include "../scene_ir.h"

#include <ludus/content/scene_cook/limits.h>
#include <ludus/content/scene_cook/scene_cook.h>

#include <string_view>

namespace ludus::content::scene_cook::importers::pbrt
{
/// Deepest admitted Include/Import nesting.
inline constexpr uint32 MAX_INCLUDE_DEPTH = 32;
/// Most ObjectInstance placements kept in the IR before INSTANCES_TRUNCATED.
inline constexpr uint32 MAX_INSTANCES = 1000000;
/// Largest admitted PLY file: the binary scene data policy.
inline constexpr usize MAX_PLY_BYTES = MAX_SOURCE_DATA_BYTES;

/// Read entryFile (and everything it includes) into outScene.
/// Paths are resolved relative to the entry file's directory, which is the import root;
/// nothing outside it is read. Unsupported content becomes diagnostics, never a failure.
/// @param entryFile Native path of the .pbrt entry file.
/// @param outScene Replaced; source file names are root-relative.
/// @param inoutLog Receives diagnostics with file:line.
/// @param outRoot Resolved import root directory.
/// @param cancelOrNull Optional cancellation hook polled per directive.
/// @return Ok; Cancelled; NotFound/IoError for an unreadable entry file; Invalid when the
/// log holds errors (syntax errors stop the file they occur in); OutOfMemory.
[[nodiscard]] Status ReadScene(std::string_view entryFile,
                               SceneIr& outScene,
                               DiagnosticLog& inoutLog,
                               foundation::String& outRoot,
                               const Cancellation* cancelOrNull = nullptr) noexcept;

/// Same as ReadScene over an in-memory entry file; Include and file-backed shapes
/// resolve below root. Used by tests and the fuzz target (root may be empty to
/// forbid all file access).
/// An in-memory entry file for ReadSceneFromMemory.
struct MemoryEntryFile final
{
    /// pbrt source text of the entry file.
    std::span<const uint8> Text;
    /// Directory that Include and file-backed shapes resolve below; empty forbids file access.
    std::string_view Root;
    /// Name reported in diagnostics.
    std::string_view EntryName;
};

[[nodiscard]] Status ReadSceneFromMemory(const MemoryEntryFile& entry,
                                         SceneIr& outScene,
                                         DiagnosticLog& inoutLog,
                                         const Cancellation* cancelOrNull = nullptr) noexcept;
} // namespace ludus::content::scene_cook::importers::pbrt
