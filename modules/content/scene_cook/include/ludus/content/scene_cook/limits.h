#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::content::scene_cook
{
using foundation::usize;

/// Largest source file admitted by the cooker (bundles are bounded by scene_format::MAX_BUNDLE_BYTES).
/// Also bounds each best-effort text source: pbrt includes, MTL libraries and glTF JSON.
inline constexpr usize MAX_SOURCE_BYTES = usize{64} * 1024 * 1024;
/// Largest binary scene data source read by the best-effort importers: a PLY mesh, an
/// external or base64 glTF buffer, or a whole GLB container. Encoded bytes only; decoded
/// geometry has its own budgets in the readers and the planner.
inline constexpr usize MAX_SOURCE_DATA_BYTES = usize{1024} * 1024 * 1024;
/// Largest encoded image read for a texture or environment map, including an image
/// embedded in a glTF buffer view or data URI.
inline constexpr usize MAX_SOURCE_IMAGE_BYTES = usize{256} * 1024 * 1024;
/// Inclusive source byte ceiling for the strict reference profile's entry document.
inline constexpr usize MAX_REFERENCE_DOCUMENT_BYTES = usize{1024} * 1024;
/// Inclusive source byte ceiling for the strict reference profile's license text.
inline constexpr usize MAX_LICENSE_BYTES = usize{1024} * 1024;
/// Largest artifact, bundle or resident closure in bytes.
inline constexpr usize MAX_ASSET_BYTES = usize{64} * 1024 * 1024;
/// Largest level or metadata document in bytes.
inline constexpr usize MAX_METADATA_BYTES = usize{1024} * 1024;
/// Largest project catalog (assets/catalog.json) in bytes. Every artifact's build
/// record carries the full source_inputs digest map of its import (it feeds the
/// frozen artifact key), so a catalog grows with resources x source files:
/// two imports of ~800-file pbrt scenes already pass 1 MiB. Matches the
/// artifact limit rather than the small-document limit.
inline constexpr usize MAX_CATALOG_BYTES = usize{64} * 1024 * 1024;

} // namespace ludus::content::scene_cook
