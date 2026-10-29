#pragma once

// The renderer's linearly transformed cosine (LTC) table, embedded in the renderer library. The definition is generated
// at build time by cmake/shaders/generate_ltc_table.py from data/ltc_table.bin; shaders/ltc_table_layout.h, generated
// with that binary by tools/ltc-fit, describes the entries. ltc_table.cpp reads the table on the CPU and uploads it for
// shaders. See docs/development/ltc-table.md.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>

#include "../../shaders/ltc_table_layout.h"

namespace ludus::graphics::renderer::internal
{
/// One four-float table entry, laid out as the shader's float4.
struct LtcTableEntry final
{
    foundation::float32 X = 0.0F;
    foundation::float32 Y = 0.0F;
    foundation::float32 Z = 0.0F;
    foundation::float32 W = 0.0F;
};
/// float32 values per entry.
inline constexpr foundation::usize LTC_TABLE_ENTRY_FLOATS = 4;
static_assert(sizeof(LtcTableEntry) == LTC_TABLE_ENTRY_FLOATS * sizeof(foundation::float32),
              "LTC table entries are tightly packed float4s");

/// Entries in one F0 slice: roughness rows times view columns. The layout macros multiply in int, so C++ indices use
/// these usize forms.
inline constexpr foundation::usize LTC_TABLE_SLICE_ENTRY_COUNT =
    foundation::usize{LUDUS_LTC_TABLE_ROUGHNESS_COUNT} * LUDUS_LTC_TABLE_VIEW_COUNT;
/// Index of the first moment entry, after every F0 slice's transforms.
inline constexpr foundation::usize LTC_TABLE_MOMENT_OFFSET =
    foundation::usize{LUDUS_LTC_TABLE_FRESNEL_COUNT} * LTC_TABLE_SLICE_ENTRY_COUNT;
/// Number of entries: every transform, then every moment (ltc_table_layout.h).
inline constexpr foundation::usize LTC_TABLE_ENTRY_COUNT = LTC_TABLE_MOMENT_OFFSET + LTC_TABLE_SLICE_ENTRY_COUNT;
static_assert(LTC_TABLE_ENTRY_COUNT == foundation::usize{LUDUS_LTC_TABLE_ENTRY_COUNT},
              "the usize offsets describe ltc_table_layout.h");
using LtcTable = foundation::StaticArray<LtcTableEntry, LTC_TABLE_ENTRY_COUNT>;

/// The generated table.
// This is a declaration, not an initializer. The generated definition is a constant aggregate of float literals.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern const LtcTable LTC_TABLE;
} // namespace ludus::graphics::renderer::internal
