#pragma once

/// @file
/// Shared host/shader masks for selecting groups from one immutable Scene bundle.
#ifdef __cplusplus
#    include <ludus/foundation/base/types.h>
namespace ludus::content::render_graph_format
{
/// Exact unsigned host scalar used for scene group masks.
using SceneDataValue = foundation::uint32;
#else
/// Unsigned shader scalar used for scene group masks.
typedef uint SceneDataValue;
#endif

/// Vertices, indices, instances, transforms and surface attributes. Required for rendering.
static const SceneDataValue SCENE_DATA_GEOMETRY = 1;
/// Material records and retained material texture views.
static const SceneDataValue SCENE_DATA_MATERIALS = 2;
/// Analytic lights, geometric emitters and environment; emitter emission is a material dependency.
static const SceneDataValue SCENE_DATA_LIGHTS = 4;
/// Every admitted scene data group, selected by a full Scene connection.
static const SceneDataValue SCENE_DATA_ALL = SCENE_DATA_GEOMETRY | SCENE_DATA_MATERIALS | SCENE_DATA_LIGHTS;

#ifdef __cplusplus
} // namespace ludus::content::render_graph_format
#endif
