# Terrain fields, drainage and material rules

`Ludus::ContentTerrain` (`modules/content/terrain`, headers under
`ludus/content/terrain/`) holds the headless serial reference kernels of terrain
slice T1 from the [terrain generation architecture](../../architecture/terrain-generation.md#ordered-pipeline).
It is Foundation-only (`FoundationBase`, `FoundationContainers`, `FoundationMath`),
builds for native and browser targets, and is in-tree until an SDK consumer
needs it. The World editor's default terrain samples its elevation through this
module, so a cooker, a test and the editor agree on every height.

The module computes fields; it owns no file format, streaming or rendering.
Those stay with the [terrain systems](../../architecture/terrain.md) proposal,
and material rendering is a later slice.

## Elevation recipe (`terrain_noise.h`)

`ComputeTerrainElevation(shape, key, x, z)` sums `TERRAIN_NOISE_OCTAVES` octaves
of addressed two-dimensional gradient noise and remaps the result into the
shape's elevation bounds. `TerrainNoiseShape` carries the minimum and maximum
elevation, the first octave's feature size in metres, the roughness ratio and a
`TerrainNoiseProfile` (`Hills`, `Ridges`, `Flat`). The lattice is global: every
gradient is sampled from FoundationMath's addressed Philox by its lattice
coordinates and octave, so the mesh resolution and traversal order never change
a height. `CanSampleTerrainNoise(shape, halfExtentMetres)` rejects shapes whose
highest octave would address a lattice point beyond
`MAX_TERRAIN_LATTICE_POSITION`.

`TERRAIN_NOISE_LAYOUT_VERSION` names the gradient set, fade and octave layout.
The module tests pin a digest of a golden grid; a different digest requires a
new layout version, because it would move every saved default World's terrain.

## Derived fields (`terrain_fields.h`)

All fields share a `TerrainFieldGrid` (columns along X, rows along Z, metres
between samples) and borrow caller storage through `BorrowedView`. Each
operation validates its inputs, allocates scratch, then writes; any failure
leaves every output unchanged and returns a `TerrainFieldStatus`.

| Operation | Output | Notes |
| --- | --- | --- |
| `TryComputeTerrainSlope` | Rise over run per sample | Central differences, one-sided at the border |
| `TryComputeTerrainDrainage` | Filled elevation, `TerrainFlowDirection`, accumulation and a `TerrainDrainageSummary` | Whole-domain Priority-Flood depression filling, then D8 steepest descent on the filled surface |
| `TryComputeTerrainWetness` | Wetness in [0, 1] | Topographic wetness index `ln(a / tan b)` remapped by `TerrainWetnessParameters` |

Drainage is deterministic and acyclic by construction. Depressions fill to
their spill level. A sample without a strictly lower neighbour, which happens on
flats and inside filled lakes, drains along the flood's spanning forest toward
its spill point instead of following an epsilon gradient, so filled heights stay
exact. Ties between equally steep neighbours resolve in `TerrainFlowDirection`
order. Accumulation counts the sample itself plus every upstream sample; the
summary's `OutletAccumulation` equals the sample count, which the tests use as
the conservation check. A streamed page is not a hydrology boundary: pass the
complete drainage domain.

## Material rules and patch palettes (`terrain_materials.h`)

A `TerrainMaterialRule` names a stable global material ID, a weight and three
feathered `TerrainMaterialRange`s over elevation, slope and wetness. Membership
is one inside a range and falls smoothly to zero over the feather width.
`TryEvaluateTerrainMaterialWeights` multiplies the memberships by the rule
weight and normalizes the rules at each sample to sum to one; a sample no rule
admits takes the first rule and is counted in the summary.

`TryBuildTerrainPatchPalettes` divides the grid's cells into patches of
`TerrainPatchLayout::CellsPerPatch` and gathers each patch's palette from its
footprint plus `HaloSamples` on every side, so neighbouring patches agree on the
materials at their shared edge. A palette lists at most
`TERRAIN_PATCH_MATERIAL_CHANNELS` material IDs in ascending order; rules that
share an ID share a channel. Channel weights are quantized so each sample sums
to `TERRAIN_MATERIAL_WEIGHT_ONE`. When a footprint needs more materials than
the channels allow, the build returns `PaletteOverflow` and the
`TerrainPaletteSummary` names the first overflowing patch and its material count
instead of dropping a layer, as the
[surface material contract](../../architecture/terrain.md#surface-materials-and-attached-systems)
requires. Physical soil remains a separate field; visual palette reduction never
changes it.

## Limits and status

| Limit | Value |
| --- | --- |
| Samples per grid axis | 2 through 4097 |
| Rules per rule set | 16 |
| Cells per patch edge | 1 through 256 |
| Halo samples | 0 through 4 |

Implemented: elevation recipe, slope, drainage, wetness, rules and palettes with
regression tests in `modules/content/terrain/tests`. Not yet implemented from the
pipeline: imports and stamps as module operations, thermal and hydraulic
erosion, cooked pages and the renderer's blended terrain material.

## References

- John F. O'Callaghan and David M. Mark, *The extraction of drainage networks
  from digital elevation data*, Computer Vision, Graphics, and Image Processing
  28(3), pp. 323–344, 1984. D8 routing and accumulation.
- Richard Barnes, Clarence Lehman and David Mulla, *Priority-Flood: An optimal
  depression-filling and watershed-labeling algorithm for digital elevation
  models*, Computers & Geosciences 62, pp. 117–127, 2014. Border-seeded
  depression filling; Ludus routes flats along the flood forest instead of
  adding an epsilon gradient.
- Keith J. Beven and Mike J. Kirkby, *A physically based, variable contributing
  area model of basin hydrology*, Hydrological Sciences Bulletin 24(1),
  pp. 43–69, 1979. Topographic wetness index.
- Ken Perlin, *Implementing Improved Perlin Noise*, GPU Gems 1, chapter 5.
  Smooth fade and balanced gradients.
- Ferenc Pintér, *Large-Scale Terrain Rendering for Outdoor Games*, GPU Pro 2,
  chapter II.3. Procedural soil maps combined with artist paint.
