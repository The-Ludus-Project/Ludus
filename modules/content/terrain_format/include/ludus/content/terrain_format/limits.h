#pragma once

#include <ludus/foundation/base/types.h>

// Admission limits for the cooked terrain revision format (Ludus::TerrainFormat).
//
// These constants are the single owner of terrain revision admission policy (AGENTS.md, "Admission limits have
// one owner per policy"; docs/architecture/content-resources.md#limit-ownership). The ContentTerrain cooker and
// reader reuse them when cooking, reading, validating and assembling a revision. Encoded manifest and page bytes
// are bounded separately from the decoded sample counts; peak cook memory is the caller's elevation field plus
// the encoded output, never one of these constants.
namespace ludus::content::terrain_format
{
using foundation::uint32;
using foundation::usize;

/// Most cells on one page edge. Every page is a legal ContentTerrain tile, so this never exceeds its tile limit.
inline constexpr uint32 MAX_PAGE_AXIS_CELLS = 256;
/// Most cells on one axis of a revision's domain (a 4097-sample elevation field).
inline constexpr uint32 MAX_DOMAIN_AXIS_CELLS = 4096;
/// Most pages in one revision; 64 by 64 pages of 64 cells cover the largest domain.
inline constexpr uint32 MAX_REVISION_PAGES = 4096;
/// Neighbouring samples stored around each page core on every side. Version 1 admits exactly this width.
inline constexpr uint32 PAGE_HALO_SAMPLES = 1;
} // namespace ludus::content::terrain_format
