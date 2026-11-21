#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/content/foliage_format/limits.h>

#include <string_view>

/// @file
/// LUFOL cooked foliage placement page set format (Ludus::FoliageFormat).
///
/// This header is the single wire contract between the foliage placement cooker
/// and encoder and the validating runtime reader (Ludus::FoliagePages). It is
/// owned by neither side and depends on nothing but FoundationBase. The design
/// is in docs/architecture/foliage.md, "Cooked pages and publication".
///
/// A page set is little-endian and tightly packed, in this order:
///
///   1. A HEADER_BYTES header: magic, format and generator versions, the lattice
///      domain (schema, page size, persistent ID and terrain sample origin), the
///      terrain revision, the four table counts and the input digest.
///   2. LayerCount layer records (LAYER_RECORD_BYTES each), ID ascending.
///   3. SpeciesCount species records (SPECIES_RECORD_BYTES each), key ascending.
///   4. PageCount page records (PAGE_RECORD_BYTES each), the page directory,
///      ordered by page Z then page X.
///   5. PlacementCount placement records (PLACEMENT_RECORD_BYTES each). Each page
///      owns one contiguous run; runs follow the page order without gaps.
///
/// Every field is serialized individually at the offsets below, which are
/// derived from field widths rather than host structs, so host endianness and
/// padding never reach files. Signed fields are two's complement.
///
/// Version 1 page sets are canonical: equal cook results encode to equal bytes.
///
/// - Horizontal coordinates are lattice units relative to the domain origin.
///   One terrain sample spacing holds LATTICE_UNITS_PER_TERRAIN_SAMPLE units.
/// - A placement's root is CellX * CellSize + OffsetX (and likewise for Z), with
///   both offsets below its layer's CellSize, so every root lies in its own cell.
/// - Every layer's CellSize divides PageSize. A page owns the half-open lattice
///   square [PageX * PageSize, (PageX + 1) * PageSize) on each axis and holds
///   exactly the roots inside it, so a cell and its roots belong to one page.
/// - Within a page, placements are strictly increasing by (Layer, CellZ, CellX,
///   Slot), signed comparison. Page addresses are unique, so the full identity
///   tuple is unique across the page set.
/// - Root heights are ROOT_HEIGHT_UNITS_PER_METRE fixed point in the terrain's
///   vertical frame. Yaw is in 1/YAW_UNITS_PER_TURN turns, scale is
///   SCALE_UNITS_PER_ONE fixed point and nonzero. The up direction's X and Z
///   components are NORMAL_UNITS fixed point; zero means vertical.
/// - Page bounds conservatively enclose the page's instances, so they contain
///   every root. An empty page stores all-zero bounds.
namespace ludus::content::foliage_format
{
using foundation::int64;
using foundation::uint16;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

/// Eight-byte file magic, including the trailing NUL.
inline constexpr std::string_view MAGIC{"LUFOL01\0", 8};
/// Version 1: the first page set layout.
inline constexpr uint32 FORMAT_VERSION_1 = 1;
/// Version produced by the current encoder.
inline constexpr uint32 CURRENT_FORMAT_VERSION = FORMAT_VERSION_1;

/// Domain schema 1: lattice units of 1/LATTICE_UNITS_PER_TERRAIN_SAMPLE terrain samples, origin at a terrain sample.
inline constexpr uint32 DOMAIN_SCHEMA_1 = 1;
/// Lattice units in one terrain sample spacing under DOMAIN_SCHEMA_1.
inline constexpr uint32 LATTICE_UNITS_PER_TERRAIN_SAMPLE = 256;
/// Root height fixed-point units per metre (just under one millimetre per unit).
inline constexpr uint32 ROOT_HEIGHT_UNITS_PER_METRE = 1024;
/// Yaw units in one full turn.
inline constexpr uint32 YAW_UNITS_PER_TURN = uint32{1} << 16;
/// Scale fixed-point units for a scale of one; the largest scale is just under 16.
inline constexpr uint32 SCALE_UNITS_PER_ONE = 4096;
/// Fixed-point units of a unit up-direction component; X and Z lie in [-NORMAL_UNITS, NORMAL_UNITS].
inline constexpr uint32 NORMAL_UNITS = 32767;
/// Width of the producer-supplied input digest (for example SHA-256 of seeds, rules, species and masks).
inline constexpr usize INPUT_DIGEST_BYTES = 32;

/// Header field: MAGIC.size() bytes.
inline constexpr usize MAGIC_OFFSET = 0;
/// Header field: uint32 format version.
inline constexpr usize FORMAT_VERSION_OFFSET = MAGIC_OFFSET + MAGIC.size();
/// Header field: uint32 nonzero generator version; changes when cook output changes for unchanged inputs.
inline constexpr usize GENERATOR_VERSION_OFFSET = FORMAT_VERSION_OFFSET + sizeof(uint32);
/// Header field: uint32 lattice domain schema.
inline constexpr usize DOMAIN_SCHEMA_OFFSET = GENERATOR_VERSION_OFFSET + sizeof(uint32);
/// Header field: uint32 page edge in lattice units, 1..MAX_PAGE_SIZE.
inline constexpr usize PAGE_SIZE_OFFSET = DOMAIN_SCHEMA_OFFSET + sizeof(uint32);
/// Header field: uint64 nonzero persistent terrain domain ID.
inline constexpr usize DOMAIN_ID_OFFSET = PAGE_SIZE_OFFSET + sizeof(uint32);
/// Header field: int64 terrain sample column of the lattice origin.
inline constexpr usize ORIGIN_X_OFFSET = DOMAIN_ID_OFFSET + sizeof(uint64);
/// Header field: int64 terrain sample row of the lattice origin.
inline constexpr usize ORIGIN_Z_OFFSET = ORIGIN_X_OFFSET + sizeof(int64);
/// Header field: uint64 terrain revision the page set was cooked against.
inline constexpr usize TERRAIN_REVISION_OFFSET = ORIGIN_Z_OFFSET + sizeof(int64);
/// Header field: uint32 layer record count.
inline constexpr usize LAYER_COUNT_OFFSET = TERRAIN_REVISION_OFFSET + sizeof(uint64);
/// Header field: uint32 species record count.
inline constexpr usize SPECIES_COUNT_OFFSET = LAYER_COUNT_OFFSET + sizeof(uint32);
/// Header field: uint32 page record count.
inline constexpr usize PAGE_COUNT_OFFSET = SPECIES_COUNT_OFFSET + sizeof(uint32);
/// Header field: uint32 placement record count.
inline constexpr usize PLACEMENT_COUNT_OFFSET = PAGE_COUNT_OFFSET + sizeof(uint32);
/// Header field: INPUT_DIGEST_BYTES identifying every placement-affecting input across the halo.
inline constexpr usize INPUT_DIGEST_OFFSET = PLACEMENT_COUNT_OFFSET + sizeof(uint32);
/// Header size; the layer table starts here.
inline constexpr usize HEADER_BYTES = INPUT_DIGEST_OFFSET + INPUT_DIGEST_BYTES;

/// Layer field: uint16 authored layer ID, strictly increasing across the table.
inline constexpr usize LAYER_ID_FIELD = 0;
/// Layer field: uint16 candidate slots per cell, 1..MAX_SLOTS_PER_CELL.
inline constexpr usize LAYER_SLOTS_PER_CELL_FIELD = LAYER_ID_FIELD + sizeof(uint16);
/// Layer field: uint32 cell edge in lattice units, 1..MAX_CELL_SIZE, dividing the page size.
inline constexpr usize LAYER_CELL_SIZE_FIELD = LAYER_SLOTS_PER_CELL_FIELD + sizeof(uint16);
/// Size of one layer record.
inline constexpr usize LAYER_RECORD_BYTES = LAYER_CELL_SIZE_FIELD + sizeof(uint32);

/// Species field: uint64 nonzero persistent species key, strictly increasing across the table.
inline constexpr usize SPECIES_KEY_FIELD = 0;
/// Species field: uint64 revision of the species' placement-affecting properties.
inline constexpr usize SPECIES_PLACEMENT_REVISION_FIELD = SPECIES_KEY_FIELD + sizeof(uint64);
/// Species field: uint32 variant count, 1..MAX_VARIANTS_PER_SPECIES.
inline constexpr usize SPECIES_VARIANT_COUNT_FIELD = SPECIES_PLACEMENT_REVISION_FIELD + sizeof(uint64);
/// Size of one species record.
inline constexpr usize SPECIES_RECORD_BYTES = SPECIES_VARIANT_COUNT_FIELD + sizeof(uint32);

/// Page field: int32 page column.
inline constexpr usize PAGE_X_FIELD = 0;
/// Page field: int32 page row.
inline constexpr usize PAGE_Z_FIELD = PAGE_X_FIELD + sizeof(uint32);
/// Page field: uint32 index of the page's first placement record.
inline constexpr usize PAGE_FIRST_PLACEMENT_FIELD = PAGE_Z_FIELD + sizeof(uint32);
/// Page field: uint32 placement count, at most MAX_PLACEMENTS_PER_PAGE.
inline constexpr usize PAGE_PLACEMENT_COUNT_FIELD = PAGE_FIRST_PLACEMENT_FIELD + sizeof(uint32);
/// Page field: int64 conservative minimum X in lattice units.
inline constexpr usize PAGE_MINIMUM_X_FIELD = PAGE_PLACEMENT_COUNT_FIELD + sizeof(uint32);
/// Page field: int64 conservative minimum Z in lattice units.
inline constexpr usize PAGE_MINIMUM_Z_FIELD = PAGE_MINIMUM_X_FIELD + sizeof(int64);
/// Page field: int64 conservative maximum X in lattice units, inclusive.
inline constexpr usize PAGE_MAXIMUM_X_FIELD = PAGE_MINIMUM_Z_FIELD + sizeof(int64);
/// Page field: int64 conservative maximum Z in lattice units, inclusive.
inline constexpr usize PAGE_MAXIMUM_Z_FIELD = PAGE_MAXIMUM_X_FIELD + sizeof(int64);
/// Page field: int32 conservative minimum height in root height units.
inline constexpr usize PAGE_MINIMUM_HEIGHT_FIELD = PAGE_MAXIMUM_Z_FIELD + sizeof(int64);
/// Page field: int32 conservative maximum height in root height units, inclusive.
inline constexpr usize PAGE_MAXIMUM_HEIGHT_FIELD = PAGE_MINIMUM_HEIGHT_FIELD + sizeof(uint32);
/// Size of one page record.
inline constexpr usize PAGE_RECORD_BYTES = PAGE_MAXIMUM_HEIGHT_FIELD + sizeof(uint32);

/// Placement field: uint16 layer ID.
inline constexpr usize PLACEMENT_LAYER_FIELD = 0;
/// Placement field: uint16 slot ordinal within the cell.
inline constexpr usize PLACEMENT_SLOT_FIELD = PLACEMENT_LAYER_FIELD + sizeof(uint16);
/// Placement field: int32 cell column in the layer's lattice.
inline constexpr usize PLACEMENT_CELL_X_FIELD = PLACEMENT_SLOT_FIELD + sizeof(uint16);
/// Placement field: int32 cell row in the layer's lattice.
inline constexpr usize PLACEMENT_CELL_Z_FIELD = PLACEMENT_CELL_X_FIELD + sizeof(uint32);
/// Placement field: uint16 root X offset within the cell in lattice units.
inline constexpr usize PLACEMENT_OFFSET_X_FIELD = PLACEMENT_CELL_Z_FIELD + sizeof(uint32);
/// Placement field: uint16 root Z offset within the cell in lattice units.
inline constexpr usize PLACEMENT_OFFSET_Z_FIELD = PLACEMENT_OFFSET_X_FIELD + sizeof(uint16);
/// Placement field: int32 root height in root height units.
inline constexpr usize PLACEMENT_HEIGHT_FIELD = PLACEMENT_OFFSET_Z_FIELD + sizeof(uint16);
/// Placement field: uint16 yaw in 1/YAW_UNITS_PER_TURN turns.
inline constexpr usize PLACEMENT_YAW_FIELD = PLACEMENT_HEIGHT_FIELD + sizeof(uint32);
/// Placement field: uint16 nonzero uniform scale in SCALE_UNITS_PER_ONE fixed point.
inline constexpr usize PLACEMENT_SCALE_FIELD = PLACEMENT_YAW_FIELD + sizeof(uint16);
/// Placement field: int16 up-direction X component in NORMAL_UNITS fixed point.
inline constexpr usize PLACEMENT_NORMAL_X_FIELD = PLACEMENT_SCALE_FIELD + sizeof(uint16);
/// Placement field: int16 up-direction Z component in NORMAL_UNITS fixed point.
inline constexpr usize PLACEMENT_NORMAL_Z_FIELD = PLACEMENT_NORMAL_X_FIELD + sizeof(uint16);
/// Placement field: uint16 species table index.
inline constexpr usize PLACEMENT_SPECIES_FIELD = PLACEMENT_NORMAL_Z_FIELD + sizeof(uint16);
/// Placement field: uint16 variant index within the species.
inline constexpr usize PLACEMENT_VARIANT_FIELD = PLACEMENT_SPECIES_FIELD + sizeof(uint16);
/// Size of one placement record.
inline constexpr usize PLACEMENT_RECORD_BYTES = PLACEMENT_VARIANT_FIELD + sizeof(uint16);

/// Largest admitted page set in bytes.
inline constexpr usize MAX_PAGE_SET_BYTES =
    HEADER_BYTES + usize{MAX_LAYERS} * LAYER_RECORD_BYTES + usize{MAX_SPECIES} * SPECIES_RECORD_BYTES +
    usize{MAX_PAGES} * PAGE_RECORD_BYTES + usize{MAX_PLACEMENTS} * PLACEMENT_RECORD_BYTES;

/// Byte count every supported target can address, including 32-bit WebAssembly.
inline constexpr usize MAX_ADDRESSABLE_BYTES = usize{1} << 31;

// tests/foliage_format_tests.cpp pins the version 1 field offsets and record widths.
static_assert(MAX_PAGE_SET_BYTES < MAX_ADDRESSABLE_BYTES, "page sets must be addressable on 32-bit targets");
} // namespace ludus::content::foliage_format
