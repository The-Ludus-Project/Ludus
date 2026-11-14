---
description: Understand the tiled terrain core, generation, editing and planned streaming boundaries.
---

# Terrain and generation

Use the canonical engineering documents below to work with the tiled terrain
core or plan its generation, editing and streaming extensions. They
contain the contracts, implementation gates and reference attribution; this page
provides a reading path.

| Your task | Read |
| --- | --- |
| Cook tiles from height fields, query the surface or export meshes | [Canonical tiled terrain](../../modules/content/terrain.md#canonical-tiled-terrain) |
| Choose the terrain representation and integration boundaries | [Terrain systems architecture](../../architecture/terrain.md) |
| Implement coordinates, queries, LOD, materials, editing or residency | [Terrain contracts](../../architecture/terrain.md#coordinates-and-canonical-surface) |
| Design the World viewport workflow for adding and editing terrain | [Terrain authoring in the World viewport](../../architecture/terrain.md#terrain-authoring-in-the-world-viewport) |
| Design source layers, deterministic recipes, hydrology, erosion or cooking | [Terrain generation](../../architecture/terrain-generation.md) |
| Assess which Gems articles changed the design and which were deferred | [Gems and research review](../../architecture/terrain-gems-review.md) |
| Plan research after the baseline is implemented and validated | [Post-baseline research queue](../../architecture/terrain-gems-review.md#post-baseline-research-queue) |
| Plan delivery and performance acceptance | [Terrain delivery gates](../../architecture/terrain.md#performance-and-delivery-gates) and [generation validation](../../architecture/terrain-generation.md#validation-and-delivery) |

For existing engine integration, continue with [platform and rendering](rendering.md),
[world publication](world.md) and [content ownership](content.md). Check
[current capabilities](../getting-started/status.md) before treating a proposed
adapter or renderer as an available SDK facility.
