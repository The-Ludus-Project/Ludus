#pragma once

// Storage of the embedded LTC table, generated at build time by cmake/generate_ltc_table.py from
// modules/graphics/renderer/data/ggx_ltc_table.bin. Private to the renderer.

#include <ludus/graphics/renderer/ltc_table.hpp>

namespace ludus::graphics::renderer::internal
{
extern const LtcTableEntry LTC_TABLE_TRANSFORMS[LTC_TABLE_TRANSFORM_COUNT];
extern const LtcAlbedoEntry LTC_TABLE_ALBEDO[LTC_TABLE_ALBEDO_COUNT];
} // namespace ludus::graphics::renderer::internal
