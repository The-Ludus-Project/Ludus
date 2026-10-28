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

/// Number of entries: every transform, then every moment (ltc_table_layout.h).
inline constexpr foundation::usize LTC_TABLE_ENTRY_COUNT = LUDUS_LTC_TABLE_ENTRY_COUNT;
using LtcTable = foundation::StaticArray<LtcTableEntry, LTC_TABLE_ENTRY_COUNT>;

/// The generated table.
extern const LtcTable LTC_TABLE;
} // namespace ludus::graphics::renderer::internal
