#pragma once

// RT1's GGX directional-albedo table for ReferenceMaterialProfile::MultipleScatteringGgx, embedded in the renderer
// library. The definition is generated at build time by cmake/shaders/generate_ggx_albedo_table.py from
// data/ggx_albedo_table.bin; shaders/ggx_albedo_table_layout.h describes the values. reference_bsdf.cpp reads the table
// on the CPU and reference_compute.cpp uploads it for renderer_reference.slang, whose ggx_albedo_table.slang lookup
// repeats this arithmetic. See docs/architecture/renderer-systems.md#multiple-scattering-ggx-profile.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>

#include "../../shaders/ggx_albedo_table_layout.h"

namespace ludus::graphics::renderer::internal
{
/// float32 values in the table: the directional-albedo grid followed by one average deficit per roughness row.
inline constexpr foundation::usize GGX_ALBEDO_TABLE_FLOAT_COUNT = foundation::usize{LUDUS_GGX_ALBEDO_TABLE_FLOAT_COUNT};
static_assert(GGX_ALBEDO_TABLE_FLOAT_COUNT % LUDUS_GGX_ALBEDO_TABLE_ENTRY_FLOATS == 0,
              "shaders read the table as whole float4 entries");
/// The table's values in layout order.
using GgxAlbedoTable = foundation::StaticArray<foundation::float32, GGX_ALBEDO_TABLE_FLOAT_COUNT>;

/// The generated table.
// This is a declaration, not an initializer. The generated definition is a constant aggregate of float literals.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern const GgxAlbedoTable GGX_ALBEDO_TABLE;

/// Look up the unit-Fresnel directional albedo E(cosine, alpha) of RT1's single-scattering GGX lobe.
/// @param alpha GGX alpha, clamped to [0, 1].
/// @param cosine Cosine between the direction and the shading normal, clamped to [0, 1].
/// @return The bilinear interpolant over sqrt(alpha) and the stretched-view coordinate, clamped to [0, 1].
[[nodiscard]] foundation::float32 LookupGgxDirectionalAlbedo(foundation::float32 alpha,
                                                             foundation::float32 cosine) noexcept;
/// Look up the average deficit 2 * integral of (1 - E(mu)) mu dmu over [0, 1], one minus the average albedo.
/// @param alpha GGX alpha, clamped to [0, 1].
/// @return The rows' deficits, linearly interpolated over sqrt(alpha) and clamped to [0, 1].
[[nodiscard]] foundation::float32 LookupGgxAverageDeficit(foundation::float32 alpha) noexcept;
} // namespace ludus::graphics::renderer::internal
