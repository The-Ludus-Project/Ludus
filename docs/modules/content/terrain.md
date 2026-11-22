# Tiled terrain and generation kernels

`Ludus::ContentTerrain` (`modules/content/terrain`, headers under
`ludus/content/terrain/`) holds the canonical tiled surface and the serial reference kernels from the [terrain generation architecture](../../architecture/terrain-generation.md#ordered-pipeline).
It is Foundation-only (`FoundationBase`, `FoundationContainers`, `FoundationMath`),
builds for native and browser targets, and is in-tree until an SDK consumer
needs it. The World editor's default terrain samples its elevation through this
module, so a cooker, a test and the editor agree on every height, and it carves
rivers and bakes its surface colour with the kernels below.

## Canonical tiled terrain

The T0 surface uses square tiles of 1 through 256 cells per side, with one
extra sample on each axis. Heights are row-major with columns along +X, rows
along +Z and elevations along +Y. All tiles in a revision share the same cell
count, horizontal spacing, height origin and quantum. The caller owns revision
identity and keeps the tiles alive for the duration of queries. The bounded
World editor terrain keeps its existing topology and file representation.

1. Import a square window with `TryCookTerrainTile` (`terrain_tile.h`). Supply
   one bounded elevation field in metres, its `TerrainFieldGrid`, and the
   source column and row of the tile's first sample. Fields can come from
   imported height data or the generation kernels below. The complete source
   field uses the tile's spacing on both axes. Adjacent windows overlap by one
   sample. Use one source value per global coordinate.
2. Keep the tile's description alongside its height bytes. Version
   `TERRAIN_TILE_HEIGHT_LAYOUT_VERSION` is a raw sequence of unsigned 16-bit
   or signed two's-complement 32-bit codes in little-endian order. Code zero
   is at the declared origin. The decoder multiplies by the quantum and adds
   the origin in binary64. The cook rounds to the nearest code with ties toward
   positive infinity and rejects heights outside the closed codec range.
   Quantization contributes at most half a quantum in exact arithmetic.
   Binary32 input and metadata conversion and binary64 arithmetic contribute
   additional rounding error. When cooking reports `HeightOutOfRange`, correct
   the source or choose a common origin and quantum that cover the revision.
3. Restore one payload with `TryLoadTerrainTile`. It requires the exact byte
   count, valid metadata, and tile endpoints that fit signed 64-bit lattice
   coordinates. Validate the saved layout version before calling it. Use
   `CheckTerrainTileSeam` before composing neighbouring tiles, including
   diagonal neighbours. It compares all duplicated codes and rejects revision,
   layout or codec mismatches.

`TryLocateTerrainCell` uses floor division. With two cells per side, global
cell X = -1 belongs to tile X = -1 at local column 1. Cell ownership is
half-open. `TrySampleTerrainTile` (`terrain_surface.h`) also permits the
duplicated endpoints for explicit per-tile queries. It takes tile-local XZ in
cell units and returns height in metres and an upward unit contact normal.
Height is piecewise planar on the upward-wound triangles of the fixed
(0,0)–(1,1) diagonal. Equal codes yield equal shared-edge heights. The normal
belongs to the selected triangle and can change across a cell or tile edge.

`TryRaycastTerrainTile` uses tile-local XZ in metres and the same vertical
frame as the height codec. It returns the nearest triangle contact within a
finite segment, testing both sides of each triangle. The headless reference
scan restricts constant horizontal axes and covers the full tile along moving
axes. Supply a cell budget. `BudgetExceeded` means the scan could not establish
a result within that budget, while `Miss` means no triangle intersected.
Point and ray queries do no allocation, loading or graphics work.

Use `GetTerrainTileMeshCounts` to size the position and index buffers for
`TryExportTerrainTileMesh`. The export subtracts the caller's global lattice
origin before scaling XZ to metres in binary64. Use a common nearby origin
across tiles for identical shared positions. Positions and indices use the
same decoded triangles as the queries. Every non-success status preserves
the tile, contact or mesh buffers supplied to the operation.

This slice covers solid height tiles and the raw height payload. Holes, physical
materials, residency and renderer/physics adapters are governed by the
[terrain architecture](../../architecture/terrain.md#performance-and-delivery-gates).

## Versioned page cooking

`terrain_cook.h` cooks one complete elevation field into a terrain revision: a
manifest plus a pack of independently readable height pages. The wire layout is
owned by the header-only `Ludus::TerrainFormat` target
(`ludus/content/terrain_format/`), which both the cooker and any reader include.
It follows the [cooked format contract](../../architecture/terrain-generation.md#cooked-format-and-validation).

1. Describe the revision with a `TerrainRevisionLayout`: the revision number,
   your recipe version and source digest, the first page's tile address, the
   page counts on X and Z, cells per page, spacing and the shared height codec.
   The field has `PagesX * CellsPerPage + 1` columns and
   `PagesZ * CellsPerPage + 1` rows. Field sample (0, 0) is the first page's
   minimum global sample.
2. Call `TryCookTerrainRevision` to cook every page serially. To cook pages on
   several workers, call `TryCookTerrainPage` for each page address from the
   same immutable field, then pass all of them, in any order, to
   `TryAssembleTerrainRevision`. The output bytes do not depend on the order.
3. Store `GetManifestBytes()` and `GetPagePackBytes()`. To read, validate the
   manifest with `TryReadTerrainManifest`, then fetch one page's byte range
   from its `TerrainPageRecord` (`ByteOffset`, `ByteSize`). Decode it with
   `TryReadTerrainPage`, which needs no other page.
4. Use `TerrainPage::GetTile()` with the existing tile queries, mesh export and
   `CheckTerrainTileSeam`. `TryGetTerrainPageHeight` also reads the halo.

Each page stores its core samples and a one-sample halo. The core is exactly the
tile height payload version 1. The halo lets a page compute central-difference
normals at its border without loading a neighbour. Inside the domain a halo
sample repeats the code of the page that owns it. Outside the domain it repeats
the nearest domain sample, so the boundary uses a one-sided difference, as the
manifest's `ClampToEdge` boundary extension declares. Assembly checks every
shared edge, corner and halo sample against its owner and returns
`BorderMismatch` instead of publishing disagreeing pages.

Records carry the page address, the revision that last cooked the page, the
lowest and highest core code, and the largest quantization error in metres. They
also carry an XXH3-128 digest of the payload. Readers check the magic, the
format version, the exact size, the tags, the canonical page order, contiguous
byte ranges, code bounds and both digests before exposing a page. Truncation,
trailing bytes, out-of-range fields and pages from another range fail with a
`TerrainCookStatus` and leave outputs unchanged. `Ludus::TerrainFormat`'s
`limits.h` owns the admission limits.

### Recook after an edit

`TryCollectTerrainEditPages` lists the pages whose core or halo contains a
sample of an edited rectangle. Use it to show the rebuild scope before an
expensive edit. `TryRecookTerrainRevision` takes the base manifest and pack, the
same layout with a later revision, the complete new field and the edited
rectangle. It cooks those pages at the new revision and copies every other page
after checking its digest. Reused pages keep their content revision. The
result decodes to the same heights, and the same payload digests, as a full cook
of the new field. Declare every changed sample in the rectangle. If the
rectangle misses a change next to a page border, the border check rejects the
recook. A change elsewhere is not detected, so derive the rectangle from the
edit's support.

Only heights are cooked. Drainage, rivers and other regional products can
depend on the whole domain; recompute them from the complete field rather than
from the edited pages (see
[Drainage over decoded pages](#drainage-over-decoded-pages)). Material, hole
and coarse-level channels are later format versions. The cooker produces bytes
in memory. Write the manifest last, or into an immutable revision directory, so
an interrupted publication leaves the previous revision usable.

### Drainage over decoded pages

Pages hold quantized heights, not drainage. A consumer that reads decoded
pages and reruns `TryComputeTerrainDrainage` gets a diagnostic, not the source
drainage. Two fixtures in `terrain_drainage_cook_tests.cpp` measure the gap.

**A branching basin.** Two tributaries cross page edges, join, and drain to
one outlet. The fixture cooks the basin at 16-cell and 8-cell pages in
shuffled order, then reassembles the decoded field. Shared channel heights
agree exactly across every page edge, core or halo, and lie within half a
quantum of the source. Drainage over the decoded field keeps the control's
network, confluence and outlet accumulation. It still differs at a few
hillslope samples: there, two neighbours' drops differ by at most two
quanta per metre of spacing, and the decoded heights pick the other neighbour.
Accumulation along the channels moves by at most the samples those flips
carry.

A dam stamp diverts one tributary across a low divide. The recook rebuilds
only the pages the stamp reaches, and they decode exactly as a clean cook of
the final terrain. Drainage still changes on reused pages downstream of the
divide. Recompute drainage over the whole field after every edit.

**A shallow saddle.** A lake can spill over two passes on a page edge. The
fixture sets the two pass heights on either side of one quantum. Every decoded
height passes the half-quantum tolerance, and every partition and cook order
decodes the same field and drainage. The route depends on how the pass heights
fall relative to the codes:

| Pass heights | Decoded passes | Decoded route |
| --- | --- | --- |
| 1.25 quanta apart | Different codes | Source outlet kept |
| A quarter quantum apart across a code boundary | Different codes, one quantum apart | Source outlet kept |
| Half a quantum apart in one code; the source route's pass has the higher sample index | Tie | Whole lake moves to the other outlet |
| Half a quantum apart in one code; the source route's pass has the lower sample index | Tie | Outlet kept; one ridge sample moves to the other outlet |

Rounding to the nearest code is monotone. Quantization can therefore merge
two spill heights into one code but never reverses their order. When pass
heights differ by more than one quantum, their codes differ too, so the source
order survives. A merged pair resolves by the kernel's tie rules: the flood
visits equal heights in ascending sample index, and D8 picks equal drops in
`TerrainFlowDirection` order. The pointwise error bound says nothing about
which route that picks.

If a consumer needs the source drainage topology, choose one of these:

- **A codec scale.** Recompute from decoded heights only when every pair of
  competing spill points differs by more than one `QuantumMetres`. Choose the
  quantum against the smallest such difference, for example `Signed32` with a
  finer quantum. Generated and eroded terrain has no lower bound on that
  difference, so this works only for terrain whose saddles you control.
- **A canonical cook-stage drainage product.** Use drainage computed by
  `TryComputeTerrainDrainage` over the complete source field, versioned with
  `TERRAIN_FIELDS_LAYOUT_VERSION`, for rivers, lakes, wetness and materials.
  T1 cooks heights only and does not publish this product yet. Until it does,
  compute drainage from the source field where it is available, and treat
  drainage from decoded pages as a preview.

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

All fields share a `TerrainFieldGrid` (columns along X, rows along Z, and the
metres between samples on each axis) and borrow caller storage through
`BorrowedView`. Cells may be rectangular, as when a terrain's width and depth
differ but its sample counts match; distances, slopes and areas use each axis's
own spacing. Each
operation validates its inputs, allocates scratch, then writes; any failure
leaves every output unchanged and returns a `TerrainFieldStatus`.

| Operation | Output | Notes |
| --- | --- | --- |
| `TryComputeTerrainSlope` | Rise over run per sample | Central differences, one-sided at the border |
| `TryComputeTerrainDrainage` | Filled elevation, `TerrainFlowDirection`, accumulation and a `TerrainDrainageSummary` | Whole-domain Priority-Flood depression filling, then D8 steepest descent on the filled surface |
| `TryComputeTerrainWetness` | Wetness in [0, 1] | Topographic wetness index `ln(a / tan b)` remapped by `TerrainWetnessParameters`; `a` is the upslope area over one sample's contour width |

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

## River channels (`terrain_rivers.h`)

Rivers read the drainage outputs of one complete domain. A sample is a channel
when its contributing area, accumulation times the sample area, reaches
`TerrainRiverParameters::MinimumCatchmentSquareMetres`. Contributing area grows
downstream, so every channel drains through channels to an outlet.

`TryExtractTerrainRivers` turns the channels into a network of
`TerrainRiverSegment`s. A segment starts at a source or a confluence, visited in
ascending sample index, and runs downstream to the next confluence or outlet.
A segment that ends at a confluence repeats that sample as its last point, which
is also the first point of the downstream segment, so the polylines join. Each
segment links to its downstream segment and carries its Strahler order. Callers
supply the point and segment storage; too little storage returns
`CapacityExceeded` with the required counts in the summary. Routing that does
not strictly increase accumulation downstream is rejected as `InvalidRouting`,
which also rules out cycles.

`TryCarveTerrainRivers` cuts a channel profile into the terrain. Channel depth
and half-width are power laws of contributing area, with authored coefficients
and exponents in the parameters. The bed of a channel sample is its filled
elevation minus the depth, so the bed descends downstream wherever the filled
surface does. Samples within the half-width are lowered to the bed, and a smooth
bank of width depth over `BankSlope` blends back to the original surface. The
carved surface is the minimum over every profile and the original, so it never
raises terrain and does not depend on traversal order. Each sample takes its
water level from the channel that set its lowest target, so the cut and the
water agree. Profile distances are in metres, so rectangular cells cut the
same shape. A profile reaches at most `MAX_TERRAIN_RIVER_STENCIL_SAMPLES`
samples of the finer axis; wider profiles are clamped and counted.

Carving can move a watershed. Rerun `TryComputeTerrainDrainage` on the carved
surface before deriving slope, wetness or materials from it.

## Limits and status

| Limit | Value |
| --- | --- |
| Samples per grid axis | 2 through 4097 |
| Cells per tile edge | 1 through 256 |
| Rules per rule set | 16 |
| Cells per patch edge | 1 through 256 |
| Halo samples | 0 through 4 |
| River profile reach | 16 samples from the channel centre |
| Cells per cooked page edge | 1 through 256 |
| Cells per cooked revision axis | 4096 |
| Pages per cooked revision | 4096 |
| Halo samples per cooked page | 1 |

The tiled codec, queries and mesh export, elevation recipe, slope, drainage,
wetness, rules and palettes, river networks and carving, and versioned page
cooking have regression tests in `modules/content/terrain/tests`. File imports,
stamps, thermal and hydraulic erosion, material and hole channels, coarse page
levels and blended material textures remain future pipeline work.
The World editor uses
these kernels to carve rivers and bake its terrain surface; see
[World authoring](../../development/editor-workspace.md#s4-scene-authoring).

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
- Luna B. Leopold and Thomas Maddock Jr., *The hydraulic geometry of stream
  channels and some physiographic implications*, U.S. Geological Survey
  Professional Paper 252, 1953. Channel width and depth as power laws of
  discharge; Ludus uses contributing area as the discharge proxy.
- Arthur N. Strahler, *Quantitative analysis of watershed geomorphology*,
  Transactions, American Geophysical Union 38(6), pp. 913–920, 1957. Stream
  order.
- Kelley, Malin and Nielson, *Terrain Simulation Using a Model of Stream
  Erosion*, SIGGRAPH '88, pp. 263–268
  ([doi:10.1145/378456.378519](https://doi.org/10.1145/378456.378519)).
  Branching drainage networks as terrain structure; the branching-basin
  fixture uses a confluence across pages.
- Yuxiao Li, Mingze Xia, Xin Liang, Bei Wang and Hanqi Guo, *Preserving
  Discrete Morse-Smale Complexes in Error-Bounded Lossy Compression*, IEEE
  TVCG 32(7), pp. 6593–6609 ([arXiv:2409.17346](https://arxiv.org/abs/2409.17346)).
  A bounded pointwise error can still change critical points and their
  connections; the shallow-saddle fixture measures this for the height codec.
- Ken Perlin, *Implementing Improved Perlin Noise*, GPU Gems 1, chapter 5.
  Smooth fade and balanced gradients.
- Ferenc Pintér, *Large-Scale Terrain Rendering for Outdoor Games*, GPU Pro 2,
  chapter II.3. Procedural soil maps combined with artist paint.
