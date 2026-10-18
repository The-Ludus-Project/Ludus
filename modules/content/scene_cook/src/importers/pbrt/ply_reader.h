#pragma once

// General PLY reader (ascii, binary_little_endian, binary_big_endian) into an IR mesh.
//
// Thanks to Greg Turk, "The PLY Polygon File Format" (Stanford, 1994), for the
// header grammar and element/property layout.

#include "../import_diagnostics.h"
#include "../scene_ir.h"

#include <span>
#include <string_view>

namespace ludus::content::scene_cook::importers
{
/// Largest PLY header in bytes.
inline constexpr usize MAX_PLY_HEADER_BYTES = 65536;

/// Facts from a header-only pre-pass.
struct PlyHeaderSummary final
{
    uint32 VertexCount = 0;
    uint32 FaceCount = 0;
    bool HasNormals = false;
    bool HasUvs = false;
    bool HasExtraFaceProperties = false;
};

/// Read only the header of data (the planner's pre-pass).
/// @return false with outMessage set for malformed headers.
[[nodiscard]] bool
ReadPlyHeader(std::span<const uint8> data, PlyHeaderSummary& outSummary, std::string_view& outMessage) noexcept;

/// Decode a whole PLY file. Polygons are fan-triangulated; uv v is flipped to top-down.
/// Accepts vertex properties x y z, nx ny nz, and u v | s t | texture_u texture_v | texture_s texture_t.
/// @param outMesh Replaced on success.
/// @param outMessage Static description of the failure.
/// @param outSummary Header facts (valid on success).
[[nodiscard]] bool ReadPly(std::span<const uint8> data,
                           IrMesh& outMesh,
                           PlyHeaderSummary& outSummary,
                           std::string_view& outMessage) noexcept;
} // namespace ludus::content::scene_cook::importers
