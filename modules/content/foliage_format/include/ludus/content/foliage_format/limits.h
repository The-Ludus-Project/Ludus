#pragma once

#include <ludus/foundation/base/types.h>

/// @file
/// Admission limits for LUFOL cooked foliage placement page sets (Ludus::FoliageFormat).
///
/// These constants are the single owner of foliage page admission policy
/// (AGENTS.md, "Admission limits have one owner per policy";
/// docs/architecture/content-resources.md, "Limit ownership"). The cooker, the
/// encoder and the validating reader reuse them, and a runtime profile may only
/// lower them through Ludus::FoliagePages' FoliagePageLimits. Encoded-byte and
/// decoded-count ceilings are distinct. Concurrent generations, temporary cook
/// memory and per-frame uploads have separate owners and are not set here.
namespace ludus::content::foliage_format
{
using foundation::uint32;
using foundation::usize;

/// Most placement layers in one page set.
inline constexpr uint32 MAX_LAYERS = 64;
/// Most candidate slots in one layer cell; slot ordinals are below this.
inline constexpr uint32 MAX_SLOTS_PER_CELL = 64;
/// Largest layer cell edge in lattice units (256 terrain samples); keeps cell offsets in 16 bits.
inline constexpr uint32 MAX_CELL_SIZE = uint32{1} << 16;
/// Largest page edge in lattice units (65,536 terrain samples).
inline constexpr uint32 MAX_PAGE_SIZE = uint32{1} << 24;
/// Most species in one page set; placement records reference them with 16-bit indices.
inline constexpr uint32 MAX_SPECIES = 4096;
/// Most mesh/material variants in one species; placement records reference them with 16-bit indices.
inline constexpr uint32 MAX_VARIANTS_PER_SPECIES = 256;
/// Most pages in one page set.
inline constexpr uint32 MAX_PAGES = 16384;
/// Most placements owned by one page.
inline constexpr uint32 MAX_PLACEMENTS_PER_PAGE = uint32{1} << 16;
/// Most placements in one page set (decoded-count admission ceiling).
inline constexpr uint32 MAX_PLACEMENTS = uint32{1} << 21;

/// Distinct values of a 16-bit record field; bounds the indices and ordinals stored in 16 bits.
inline constexpr uint32 UINT16_VALUE_COUNT = uint32{1} << 16;

static_assert(MAX_SPECIES <= UINT16_VALUE_COUNT, "species indices are stored in 16 bits");
static_assert(MAX_VARIANTS_PER_SPECIES <= UINT16_VALUE_COUNT, "variant indices are stored in 16 bits");
static_assert(MAX_SLOTS_PER_CELL <= UINT16_VALUE_COUNT, "slot ordinals are stored in 16 bits");
static_assert(MAX_CELL_SIZE <= UINT16_VALUE_COUNT, "in-cell root offsets are stored in 16 bits");
static_assert(MAX_CELL_SIZE <= MAX_PAGE_SIZE, "a page holds at least one cell of every layer");
static_assert(MAX_PLACEMENTS_PER_PAGE <= MAX_PLACEMENTS, "one page fits in a page set");
} // namespace ludus::content::foliage_format
