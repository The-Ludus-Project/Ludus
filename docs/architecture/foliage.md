# Terrain foliage architecture

**Status:** Proposed. Deliver a bounded World with authored grasses, shrubs and
trees. Cook placement once, then draw the same records in the World viewport
and a packaged native or browser game.

## Environmental systems today

| Capability | Ludus scope |
| --- | --- |
| Atmosphere | The World ships an [Earth clear-sky Atmosphere](renderer-systems.md#clear-sky-atmosphere-design) with atmospheric scattering and aerial perspective on supported render graphs. The [editor workflow](../development/editor-workspace.md#add-a-clear-sky-atmosphere) covers Sun, Moon and night sky. Celestial drivers consume date, time and location. |
| Clouds | [#1319](https://github.com/The-Ludus-Project/Ludus/issues/1319) tracks a bounded cloud-layer and aerial-perspective prototype. |
| Climate, weather and wind | [ContentTerrain](../modules/content/terrain.md) supplies terrain-derived slope, drainage and wetness. The [CPU fluid field](fluid-field.md) supplies grid simulation and [Sandbox owns its wind modes](drift-game-integration.md). The reviewed engine and Kanban have no shared climate, weather or World wind system. |
| Foliage | Ludus has [room-scoped deterministic scatter](procedural-content-generation.md#p0-room-scoped-scatter), terrain surface queries, indexed instancing, masked materials and authored mesh LOD. The room procedure is a starting point for the terrain cook. The 3D foliage authoring and player pipeline is proposed here. |

Foliage placement needs an authored terrain and species recipe. Calm is the
default presentation input. Future climate maps can supply long-term
suitability, for example through a versioned soil-moisture mask. Future weather
can supply time-varying wind through the World input. Changing weather or time
of day leaves placement and simulation identities intact. The existing
Atmosphere lights the scene and composes aerial perspective. Cloud composition
retains its own renderer contract.

## Ownership and data flow

| Owner | Contract |
| --- | --- |
| ContentTerrain | Supplies the canonical terrain-local surface, holes, fields and revision. Placement uses the canonical queries independently of the visual terrain cut. |
| ProceduralEnvironment and the content cooker | Resolve immutable species, terrain and mask inputs. Own addressed candidate sampling, eligibility, occupancy and placement order. Bake a bounded terrain through the existing query path. |
| Content format and Runtime Resources | Share versioned wire declarations in a content format target. Validate placement pages and retain exact dependencies through existing [resource leases](content-resources.md#loading-and-resource-leases). Meshes, materials and textures use existing resource paths. |
| World and gameplay | Bind source inputs and committed revisions, own save deltas and gameplay changes, and pass immutable page references and environmental samples through [World extraction](game-world.md#ownership-and-data-flow). |
| GraphicsRenderer and RHI | Own visibility, LOD, instance data, passes, uploads and completion-scoped GPU retirement. |
| Editor | Own the viewport tools and draft through the World document. Use the same cook and extraction as the SDK player. |

The cooker is headless. Page addresses are independent of terrain renderer
tiles. Begin with a finite World containing the entire admitted drawable set.
Streaming can later schedule the same placement products from terrain residency
events after Terrain T5. The bounded slice can use the canonical surface already
available in the World editor.

## Authored inputs and deterministic placement

A species asset names reusable mesh and material variants, authored mesh LODs,
a root pivot, root offset in metres, up convention and conservative bounds per
LOD. Declare positive uniform scales and allowable normal alignment. Each
species has exclusion footprints in metres and may later declare a trunk
collider proxy and versioned wind weights. Begin with curated variants and
ordinary opaque and cutout materials.

Each layer names a persistent ID, seed, terrain-local lattice, ordered stratum,
weighted species palette and candidate density per square metre of the XZ
projection. Ranges and feather widths for height, slope and wetness, along
with dimensionless mask values in [0, 1], modulate eligibility. A missing
optional channel has a neutral multiplier. If a rule requires a missing field,
report the field and the recipe that names it.

1. Identify candidates by the persistent terrain domain and layer, signed
   global lattice coordinates and slot ordinal. The domain includes the
   lattice schema, origin and spacing. Use checked arithmetic, floor division
   and half-open XZ ownership. Keep the full tuple for equality and ordering.
   The [Philox address](randomness.md#milestone-1-procedural-placement) must be
   injective within declared limits. Validate wider terrain addresses before
   adapting the existing room-scoped packing. Separate position, eligibility,
   priority, variant and transform dimensions.
2. Jitter fixed slots within each cell. Admit a candidate when a uniform
   threshold falls below density × cell area ÷ slots × suitability. Reject
   density above the slot capacity before cooking. Sample the canonical
   terrain triangle and reject holes, out-of-bounds roots and exclusions.
   Interpolate from canonical quantized terrain samples with checked
   fixed-point arithmetic for the portable cook. Specify rounding and ties
   for field comparisons and serialized transforms. Record the root, yaw,
   optional normal alignment, and scale in canonical form. Render and
   collision derive their transforms from that same record.
3. Evaluate trees and obstacles before understory, with explicit exclusion
   classes so trunk clearance and canopy occupancy can differ. Reuse the
   [finite-neighborhood thinning rule](procedural-content-generation.md#vegetation-and-scatter).
   An eligible candidate loses to any conflicting eligible candidate of
   greater total priority, with the full tuple breaking ties. Compare
   symmetric, scaled radii even when the higher-priority neighbor loses
   elsewhere. The result has finite support and may underfill.
4. Derive the input halo from filter support, maximum pairwise exclusion
   reach, and transitive dependencies on higher strata. Cook halo candidates
   under the same rules as interior candidates. Name a missing required
   input and keep the candidate pending or fail preparation. Emit only owned
   roots, sorted by identity. Expand edit invalidation by the same dependency
   reach. Large hydrology changes can invalidate the full wetness revision.

Density edits preserve candidate keys. Terrain edits reproject survivors.
An authored suppression remains associated with its key while that candidate
is absent. Lattice changes start a new domain. Show which overrides will be
affected and require an explicit undoable **Replace lattice and remove affected
overrides** action. Material-only edits refresh render bindings and retain
placement identities.

### Terrain cook

`Ludus::FoliageCook` (`modules/content/foliage_cook`) implements the rules
above for one finite terrain snapshot. It depends on ContentTerrain,
FoliagePages and the Foundation modules, and it builds for native and browser
targets. Like ContentTerrain, it stays in-tree until an SDK consumer needs it.

The input borrows the following:

- A row-major rectangle of tiles from one terrain revision.
- Optional per-sample wetness and mask channels in 1/65,535 units, and
  optional per-cell holes.
- The domain: ID, origin sample and page size.
- Layers in ID order and species in key order.
- Suppression keys in identity order.

Horizontal values are lattice units, and the World converts authored metres
to them. `ValidateFoliageCookInput` checks every rule before a page is cooked.
A failure reports the status, the violated rule and the index of the tile,
layer, species or suppression to repair. If a rule requires a missing channel,
the result is `MissingField`. It names the layer and either the wetness
channel or the mask ID. A missing optional channel is neutral.

**Identity and randomness.** A candidate is (layer, cell X, cell Z, slot) in
the page set's domain, with full `int32` cells. Address layout 1 puts the cell
Z and cell X bits in the 64-bit Philox scope, and the layer and slot in the
32-bit event. Every key therefore has a distinct address, and the room
scatter's 16-bit packing is not reused. Each layer seed derives one key per
engine-owned domain:

| Domain | Dimension | Meaning |
| --- | --- | --- |
| `FOLP` | 0, 1 | X and Z offsets below the cell size |
| `FOLP` | 2 | Admission word |
| `FOLP` | 3 | Thinning priority |
| `FOLS` | 0 | Weighted palette entry |
| `FOLS` | 1 | Scale above the species minimum |
| `FOLV` | 0 | Variant |
| `FOLT` | 0 | Yaw, the high 16 bits |

Variant and yaw are drawn only for accepted candidates and never affect
acceptance. The scale does affect acceptance, because exclusion compares
scaled radii.

#### Fixed-point sampling

The cook reads the tiles' quantized codes and uses the same triangles as
`TrySampleTerrainTile`. In each cell, the triangle on the X ≥ Z side of the
diagonal owns the diagonal. Binary32 metadata are exact dyadic rationals, so
each conversion below rounds an exact value once:

| Quantity | Rule |
| --- | --- |
| Sample height | `(origin + quantum × code) × 2^26`, nearest, ties toward +∞; within ±2^29 root height units |
| Spacing | `spacing × 2^26`, nearest, ties toward +∞ |
| Root height | Lattice-weighted sum of three corner heights ÷ 2^24, nearest, ties toward +∞ |
| Channel value | Lattice-weighted sum of three corner values ÷ 256, nearest, ties toward +∞ |
| Slope | `floor(4096 × gradient length ÷ spacing)`, at most 64 × 4096 |
| Up direction | Each horizontal component of `(−g_x, spacing, −g_z) ÷ length` times 32,767, truncated toward zero |

Comparisons use exact 128-bit products, so no floating-point operation reaches
a cooked byte. A root outside the terrain cells or in a hole cell is rejected.

**Suitability and admission.** Height, slope and wetness ranges have a
multiplier of one inside and fall linearly to zero across their feathers. Each
mask multiplies by its value. Multipliers apply in the order height, slope,
wetness, then masks, each as `floor(σ × m ÷ 2^16)`. The per-slot threshold at
suitability one is `floor(density × cell² × spacing² ÷ slots)`, which is 2^32
times the admission probability. Validation rejects a layer whose threshold
exceeds 2^32, because its density is above the slot capacity. A candidate is
admitted when its admission word is below `floor(threshold × σ ÷ 2^16)`.

**Strata and thinning.** Lower stratum ordinals are decided first. Each
species has up to four exclusion-class radii; use one class for trunk
clearance and another for canopy occupancy. Two candidates conflict when they
share a class and their root distance is less than the sum of their scaled
radii, `ceil(radius × scale ÷ 4096)`, in that class. A candidate is occupied
when it conflicts with an accepted placement of an earlier stratum. In its own
stratum, it loses to any unoccupied conflicting candidate of greater
(priority, identity), including one that loses elsewhere. A suppressed
candidate is absent. Its key stays in the input while its candidate is
missing, and it applies again when the candidate returns.

**Halo, pages and dirty recook.** A page owns the roots in its half-open
square. Its cook evaluates the halo that its owned decisions depend on. That
halo is the page expanded by twice each stratum's maximum scaled radius for
thinning, plus the sum of the radii of every earlier stratum for occupancy.
Pages cooked alone, together, in any order or on several threads therefore
produce the same records. `TryMergeFoliageCookedPages` joins separate cooks
and replaces recooked pages. `TryWriteFoliageCookedPageSet` writes the pages
in address order with every layer and species.

`TryGetFoliageSampleEditRoots` maps a terrain or channel edit to the roots
whose cells read the edited samples. `TryGetFoliageCandidateRoots` does the
same for a suppression edit. `TryGetFoliageDirtyPages` expands those roots by
the same reach. Recooking the dirty pages over the previous cook equals a
clean cook. Layer, species, seed and domain edits invalidate every page.

**Cache key and limits.** The input digest is the SHA-256 of the versions,
domain, terrain metadata and codes, channels, recipe, species placement
properties and suppressions. A species' `RenderRevision` is excluded, so a
material-only edit keeps every byte. Placement revisions change only the key.
A page and its halo may evaluate up to 2^20 candidates
(`MAX_FOLIAGE_COOK_CANDIDATES`). The grid holds at most 4,096 tiles, and the
radii and terrain coordinates have checked limits. Cells and pages beyond
`int32` fail with `CoordinateOutOfRange`.

The tests in `modules/content/foliage_cook/tests` check the following:

- Identical bytes from full, shuffled, isolated and four-thread cooks across
  the negative seam.
- Heights and up directions against the binary64 surface query.
- Holes, slope, wetness, masks and exclusion between strata.
- Dormant suppressions, signed-limit cells, dirty recooks of terrain and
  suppression edits, and render-only edits.
- Placement gaps against maximal fills and a randomized tiling comparator,
  described below.

The golden cook in `foliage_cook_golden.h` is shared with the WebAssembly math
probe, so native and browser builds must produce the same pinned bytes.

#### Placement gaps

Thinning removes a candidate whenever a higher-priority conflict exists, even
if that conflict is itself removed. The cook therefore keeps the minimum
spacing but leaves admissible area between placements. The tests in
`foliage_gap_tests.cpp` measure the two properties separately and compare the
cook with maximal Poisson-disk fills:

- **Fixture.** The page-cook fixture is flat, with one layer, one species, a
  64-unit radius (minimum distance d = 128 lattice units, 0.25 m) and density
  at capacity. A band of hole cells forms a keep-out beside a valid strip one
  terrain cell (2d) wide. The pages are 2,048 units across. The seam zone lies
  within d of a boundary between two pages, and the junction zone within d of
  a corner where four pages meet.
- **Control.** Hierarchical dart throwing (Cline, White and Egbert, RT 2007)
  fills the same eligible roots until none is admissible, the coverage
  objective of Dunbar and Humphreys (SIGGRAPH 2006). A cell is discarded only
  when one disk or the keep-out covers all of it. The quadtree starts at
  64-unit cells and ends at single roots, which are decided exactly. The
  smallest explored cell is therefore one root and the termination tolerance
  is zero.
- **Probe.** An independent probe checks every eligible root with exact
  integer distances. It reports nearest-neighbour spacing and the roots that
  no placement excludes, separately for the interior, the seam, the junctions
  and the narrow strip. It confirms that the control leaves no admissible root.

The quick test checks spacing, eligibility, the control's maximality and the
cook's gaps with sparse probes. The hidden `[.benchmark][gap]` case probes
every root and reports the following. Times are from release builds on an
Apple M5 under load.

| Point set | Interior | Seam | Junction | Strip | Admissible area left (interior, seam, junction, strip) | Largest empty distance | Time |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Cook, cell 128, 1 slot | 928 | 248 | 21 | 45 | 15.8 %, 18.1 %, 15.1 %, 16.2 % | 1.97 d | 1.9–2.0 ms |
| Cook, cell 64, 1 slot | 830 | 231 | 18 | 41 | 20.7 %, 21.4 %, 17.4 %, 21.2 % | 2.51 d | 4.3–6.8 ms |
| Dart throwing control | 1,771 | 489 | 33 | 86 | 0 % in every zone | below d | 7.8–11.9 ms |

The cook keeps 47–52 % of the maximal fill's count. Its nearest-neighbour
distances spread across [d, 2d), while 97 % of the control's lie in
[d, 1.25 d). The seams, junctions and narrow strip stay within about three
percentage points of the interior, so page halos and keep-outs add no
measurable gaps of their own. The finer lattice offers four times the
candidates but places fewer, consistent with more candidates being removed by
conflicts that are themselves removed. The control threw 24,036 darts and
discarded 75,722 covered cells. It held at most 13,184 active cells and
520,448 bytes. At about 10 ms, the control does not limit the study, so the
conditional raster conflict-culling trial (PixelPie, HPG 2013) is not needed.
These gaps are a property of the declared placement rule. Shuffled and
isolated cooks of the fixture still produce identical pages, and the golden
bytes remain the product contract.

#### Randomized tiling comparator

Wang and Suda (HPG 2017) build a large maximal pattern from randomized
clippings of one small pattern and repair conflicts and gaps at tile borders.
`foliage_tiling.h` evaluates a deterministic version on the same fixture. Its
clipping and repair rules are this comparator's own:

- **Seed.** Dart throwing over an unobstructed square of 2,048 units (16 d)
  gives 194 points; a 1,024-unit square gives 53.
- **Clipping.** Tiles of 1,024 units align with the pages, so every page seam
  and four-page junction is a tile border. Each tile copies a window of the
  seed at an offset, optionally under one of the eight symmetries of the
  square, drawn from Philox at the tile address. Tile addresses use floor
  division, so negative coordinates follow the same rule.
- **Repair.** A copied point is removed when it conflicts with a
  higher-priority copied point from another tile. A row sweep with exact
  integer chords then lists every eligible root that no kept point covers,
  and these gap roots are filled greedily in priority order. Priorities
  depend only on tile addresses, seed indices and roots. Visiting tiles and
  rows in reverse therefore produces the same points, which the quick test
  checks.
- **Repetition.** A point's configuration is the set of integer displacements
  to every neighbour closer than 2d. The benchmark counts configurations that
  occur exactly at another point, as a translated copy or under a symmetry of
  the square. Independent fills essentially never repeat a configuration of
  three or more displacements.
- **Isolation.** Each page is tiled alone in a window widened by a halo and
  compared with the full tiling.

The hidden `[.benchmark][tiling]` case reports the following from two release
runs. Every tiling leaves no admissible root in any zone, and every zone keeps
the minimum spacing.

| Point set | Points | Repeated configurations (translated, any symmetry) | Gap roots filled | Time | Peak bytes |
| --- | --- | --- | --- | --- | --- |
| Cook, cell 64 | 1,120 | 0 %, 0 % | — | 4.3–6.8 ms | — |
| Dart throwing control | 2,379 | 0 %, 0 % | — | 8–10 ms | 0.52 MB |
| Tiles 1,024 from seed 2,048, translations | 2,460 | 19.7 %, 19.7 % | 934,749 | 0.42–1.28 s | 27.7 MB |
| Tiles 1,024 from seed 2,048, symmetries | 2,459 | 10.1 %, 18.8 % | 722,203 | 0.28–0.63 s | 25.1 MB |
| Tiles 512 from seed 1,024, symmetries | 2,442 | 0 %, 1.0 % | 1,659,188 | 0.84–0.89 s | 52.1 MB |

| Halo around an isolated page | 0 | d | 2d | 4d, 8d and 16d |
| --- | --- | --- | --- | --- |
| Pages that differ from the full tiling, of 25 | 25 | 15–18 | 0–2 | 0 |
| Points that differ | 441–481 | 39–41 | 0–2 | 0 |

- **Coverage.** Every tiling fills the seams, junctions and narrow strip
  completely and places 2.6–3.4 % more points than the control.
- **Isolation.** A decision of the greedy fill can depend on a chain of
  earlier decisions, so isolated pages need a halo. Here pages agree from 4d,
  four times the cook's thinning halo of d. The sweep establishes this only
  for this fixture, not as a bound.
- **Repetition.** Symmetries halve the exact translated copies but leave the
  copies under a symmetry, so they disguise the repetition rather than remove
  it. The measure only finds neighbourhoods copied whole inside one clipping,
  so it understates repetition for 512-unit tiles.
- **Cost.** On the lattice, the repair visits every uncovered root: 0.7–1.7
  million gap roots, each with its own priority draw. The comparator is
  therefore 30 to more than 100 times slower than the whole-region control.

The comparator does not justify a production change. Complete seams would
cost a fourfold halo, seed-pattern repetition and more cook time, while the
cook already keeps page isolation and traversal order at a halo of d. The cook
remains the qualified placement rule.

## Cooked pages and publication

Use a versioned little-endian format with a page directory, terrain and input
revisions, conservative bounds, a species table and ordered placement records.
Encode fields individually. Reject unknown versions, malformed extents,
duplicate keys and invalid species references before publication. Establish
separate limits for encoded bytes, decoded data, concurrent generations,
temporary cook memory and per-frame uploads. Derive the GPU instance stream
from the decoded records and evolve its layout independently of the disk format.

### LUFOL page sets

`Ludus::FoliageFormat` owns the version 1 wire contract and its admission
limits in `lufol.h` and `limits.h`. `Ludus::FoliagePages` encodes page sets
for the cooker and validates and reads them for runtime consumers. Both
targets depend only on Foundation modules. The [terrain cook](#terrain-cook)
produces these records. [Page set publication](#page-set-publication) keeps
the previous generation after a failed replacement.

A page set is one terrain domain's pages, tightly packed in little-endian
order:

| Part | Contents |
| --- | --- |
| Header (104 bytes) | Magic `LUFOL01`, format and generator versions, domain schema, page size, nonzero domain ID, terrain sample origin, terrain revision, table counts and a 32-byte input digest |
| Layers (8 bytes each) | Layer ID, slots per cell and cell size, ID ascending |
| Species (20 bytes each) | Nonzero persistent key, placement-affecting revision and variant count, key ascending |
| Pages (56 bytes each) | Page address, contiguous placement run and conservative bounds, in (Z, X) order |
| Placements (32 bytes each) | Layer, slot, cell, in-cell root offset, root height, yaw, scale, up direction, species and variant |

Domain schema 1 measures horizontal positions in lattice units of 1/256
terrain sample spacing from the domain origin. A root is
`cell * CellSize + offset`, and each offset is below its layer's cell size,
so every root lies in its own cell. Each layer's cell size divides the page
size. A page owns the half-open square `[page * PageSize, (page + 1) *
PageSize)` on each axis, so a cell and its roots belong to exactly one page.
Placements in a page are strictly increasing by (layer, cell Z, cell X, slot),
which rejects duplicate keys. Root heights are 1/1024 m fixed point, yaw is
1/65,536 turn and scale is 1/4096 fixed point and nonzero. The up direction
stores X and Z in 1/32,767 units; zero is vertical. Page bounds must contain
every root, and an empty page stores zero bounds.

Version 1 is canonical: equal cook results produce equal bytes, and any page
set the reader accepts re-encodes to the same bytes. The species table holds
keys and placement revisions, not meshes or materials. A material-only edit
therefore leaves the page bytes unchanged. The input digest is the cache key
for seeds, rules, species revisions and masks across the halo. The reader
binds a page set to the expected domain ID and terrain revision and reports
`RevisionMismatch` for any other.

The encoder validates the whole description before it writes any byte, so a
rejected description leaves the output unchanged. The reader checks the byte
limit before reading any field. It then checks every header field, count,
reference, ordering and ownership rule, and it binds a zero-copy view only on
success. A failed read leaves the caller's previous view unchanged. Each
failure reports a status, the violated rule, the record index and, for reads,
the byte offset to repair. `FoliagePageLimits` lets a profile lower the
encoded-byte ceiling and the layer, species, page, placement and per-page
placement counts. It cannot raise them above the format limits. Concurrent
generations, temporary cook memory and per-frame uploads keep their own
owners.

The golden fixture in `modules/content/foliage_pages/tests` pins the encoded
bytes. The WebAssembly math probe encodes and reads the same fixture, so
native and browser builds must produce identical bytes.

### Page set publication

`FoliagePageSetPublication` (`foliage_page_publication.h` in
`Ludus::FoliagePages`) owns the accepted page set of one terrain domain. It
replaces that page set only with a validated candidate:

1. `TryStage` reads the caller's bytes against the expected domain and terrain
   revision and the publication's `FoliagePageLimits`. On success the
   candidate takes the bytes and becomes **Ready**. It records the accepted
   revision it was staged against. On failure it becomes **Failed** with the
   read diagnostic, and the caller keeps the bytes to inspect.
2. `TryPublish` accepts only a Ready candidate that this publication staged
   against the current accepted revision. It then exchanges storage with the
   candidate and advances the revision. Of several candidates staged from one
   revision, only the first one published is accepted. Later ones report
   `StaleCandidate`, so asynchronous cooks can finish in any order.
3. After a successful publication, the candidate becomes **Retired** and holds
   the previous page set. Views of the previous generation stay valid until
   the caller clears that candidate. This lets consumers finish with them.

Staging, failures and cleared candidates never change the accepted page set,
its bytes or its views. Staging and publication neither allocate nor copy
page bytes. The publication is single-owner. Readers may share the accepted
view while no method runs. The pages tests cover malformed, over-budget and
mismatched replacements, stale and foreign candidates, and retirement. The
cook tests publish a dirty-halo recook only after a damaged copy and a copy
bound to another terrain revision fail. The installed SDK consumer stages,
publishes and rejects a corrupt replacement. F1 owns drawable uploads and GPU
retirement for each accepted generation.

Key reuse by the generator and format versions, seed, domain, layer, page
address, rule and placement-affecting species revisions, and terrain and mask
inputs across the full halo. The manifest binds the page set to a coherent
terrain revision. Tag asynchronous work with World, source, and request
generations. Replace affected drawable terrain and foliage together when the
candidate is Ready. Failed, cancelled and stale results leave the last
drawable version available. The renderer/RHI retires submitted assets after
completion. Device recovery rebuilds GPU state from the committed CPU product.

A later streamed profile can pin a visual bootstrap set and schedule other
pages under independent render and collision budgets. Simulation admission
waits for required collision even off camera. Diagnose missing visual coverage.
Preserve root contact with the selected terrain cut within a fixture-defined
tolerance. Refine the cut within budget or explicitly narrow cosmetic draw
range when the tolerance cannot be met.

## Raster visibility and wind

Cull conservative page bounds on the CPU, then test intersecting pages by
species group or instance. Bounds include the union of authored LODs and
the maximum deformation after scaling. Select projected mesh LOD at actual
viewport extent with per-view hysteresis keyed by stable placement. Admit
finer geometry at near-plane intersections. Qualify hard mesh switches
before considering coverage dithering or impostors. For budgeted cosmetic
density reduction, use nested subsets by stable rank. Gameplay trunks retain
their declared presentation range.

Batch compatible geometry, material, LOD and pass into direct indexed
instanced draws. Use compact page runs and bounded view scratch rather than
expanding each plant into a World entity or a LUSCN object. Compute
camera-relative render transforms while retaining canonical placement. Shadow
visibility uses the light view and independent caster bounds, so an off-camera
tree can cast into the scene. Measure alpha-mask overdraw independently of CPU
submission. Start with opaque bark and two-sided masked leaves and grass.
Use common coverage, sidedness and deformed geometry in depth, color, shadow
and picking. Test front/back normals. Use the existing
[World feature diagnostics](../development/render-graph-authoring.md#world-features)
for unsupported graph profiles.

Distant coverage loss can supply evidence for
[#1489](https://github.com/The-Ludus-Project/Ludus/issues/1489) and
[#1490](https://github.com/The-Ludus-Project/Ludus/issues/1490). Thin-leaf
transmission needs a separate material qualification. A later RT1 foliage
profile must admit the same frozen placements, geometry, deformation time and
coverage on the CPU and GPU and satisfy the [reference parity contract](renderer-systems.md#cpu-and-gpu-reference-implementation-requirement).

Consume the [World environment wind snapshot](world-environment.md#clocks-positions-and-wind)
at the accepted presentation time. Foliage owns deformation and conservative
bounds. World owns the wind provider and any simulation-tick query.

Evaluate wind at the root with a world-anchored gust phase plus a stable
per-instance phase. Combine root-weighted main bend and bounded near-LOD
flutter. Preserve the root and any rigid volume represented by a collider.
Transform the shading frame consistently with the deformed geometry and
reuse the accepted time across passes. Anchor phase independently of
camera-relative projection and bound clock precision for long sessions.
Invalidate optional previous-pose history on a clock or revision change.
Calm reproduces rest pose, including on an explicitly reported lower profile.

## World authoring and packaged play

Add **Paint Foliage** to the terrain viewport with Properties hidden. Use a
dock for palette/layer selection, radius in metres, strength, density/erase and
inspection. Persist sparse terrain-local mask strokes through World undo and
save. Display the draft footprint, pending pages and counts. A pending or
failed preparation retains the last drawable generation and reports the
affected asset or layer and its repair in the dock. Apply the candidate only
when it matches the current draft and World. Saving and reopening the same
source reproduces the placement keys and bytes.

Qualify the selected native lit World viewport, WebGPU raster and the existing
WebGL 2 raster profile. The installed SDK fixture mounts cooked assets shipped
with the game. Verify camera motion, resize, reload, failed/successful
replacement and explicit limitations of each profile in both routes.

Interactive species later publish trunk proxies from canonical root transforms.
At a simulation boundary, commit a harvest suppression and optional entity
promotion together, or keep the original plant and proxy on failure. Persist
deltas by placement key and resolve them before tile reactivation. Use a
generation check and idempotent state change for repeated or stale commands.
Camera culling, deformation and failed GPU uploads leave collision authority
with simulation. Expose both revisions while presentation catches up.

## Qualification and backlog

Use a reproducible 256 × 256 m terrain, 32 m placement pages and 250,000 cooked
placements as a stress target. Include a negative-coordinate seam, a hole,
paths, mixed exclusions, a ground-level sweep, an off-camera caster and a dirty
page burst. Compare against the same terrain with foliage disabled for
incremental frame cost, and against small unbatched draws for output
correctness. Record p50/p95 timings, peak CPU/GPU bytes, upload bytes, cutout
overdraw and admitted/deferred instance counts. Initial targets on a named
native reference GPU at 1080p/60 Hz are 0.75 ms p95 incremental CPU cull and
submission and 2.5 ms p95 incremental GPU work. Browser software GPU runs
establish correctness. Browser performance claims require measurements on
declared hardware.

| Slice | Evidence |
| --- | --- |
| [F0 — Terrain placement and cook](https://github.com/The-Ludus-Project/Ludus/issues/1694) | Serial, shuffled, parallel and isolated seam cooks match golden bytes in the declared native/browser profile. Exercise negative coordinates, holes, dirty halos, occupancy and rejection of corrupt or excessive input. |
| [F1 — Direct raster](https://github.com/The-Ludus-Project/Ludus/issues/1695) | Verify depth, color, picking, off-camera shadows, LOD, failed uploads and the stress benchmark on declared profiles. |
| [F2 — World authoring and SDK](https://github.com/The-Ludus-Project/Ludus/issues/1696) | Paint, erase, inspect, undo/redo, save, reopen and exercise pending/failure/retry with Properties hidden. Run the same source through the installed SDK. |
| [F3 — Wind](https://github.com/The-Ludus-Project/Ludus/issues/1697) | Verify exact calm, bounded gusts, roots, pass agreement, rebases, long clocks and measured deformation cost. |
| [F4 — Interactive trees](https://github.com/The-Ludus-Project/Ludus/issues/1698) | Qualify one harvestable tree, matching collision, persistent promotion, failure rollback and independent collision residency. |
| [Conditional GPU visibility](https://github.com/The-Ludus-Project/Ludus/issues/1699) | Prototype only if CPU visibility or submission misses the budget. Compare same-scene visibility and LOD with bounded compaction, explicit overflow and direct fallback. Qualify compute-to-draw hazards and avoid synchronous count readback. Review indirect capabilities under [#1237](https://github.com/The-Ludus-Project/Ludus/issues/1237). |

## Reference decisions

The local `references/game-dev-gems-toc.md` identified the material below.
The proposal adapts the techniques to Ludus's current platform and World
contracts. Its performance targets require new measurements.

- Thanks to **Josh Klint**, *Vegetation Management in Leadwerks Game Engine 4*,
  *Game Engine Gems 3*, ch. 4, pp. 53–71. Compact placements share identity
  and transform across rendering and collision. Explicit cross-layer occupancy
  and persistent deltas cover the additional terrain use cases.
  [Existing chapter review](procedural-content-generation-gems-review.md#vegetation-placement-and-collision).
- Thanks to **Tiago Sousa**, *Vegetation Procedural Animation and Shading in
  Crysis*, *GPU Gems 3*, ch. 16 §§16.1–16.2, and **Renaldas Zioma**,
  *GPU-Generated Procedural Wind Animations for Trees*, ch. 6 §§6.3–6.5.
  Adopt independent main bend and leaf detail with stateless GPU evaluation.
  Qualify weighted vertex motion before adding hierarchies for particular
  species. [Sousa chapter](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis),
  [Zioma chapter](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-6-gpu-generated-procedural-wind-animations-trees).
- Thanks to **Sean Feeley**, *Interactive Wind and Vegetation in God of War*,
  SIGGRAPH 2019, slides 7–10, 72–78, 86 and 95–98, and **Rupert Renard**,
  *Wind Simulation in God of War*, GDC 2019. Their producer/receiver split
  informs the wind provider. Start with an analytic source, with a field and
  card cluster proxies available for later comparison against measured needs.
  [Feeley course](https://advances.realtimerendering.com/s2019/index.htm),
  [Renard slides](https://media.gdcvault.com/gdc2019/presentations/Renard_Rupert_Wind_Simulation.pdf).
- Thanks to **Kiyavash Kandar**, *Large Scale GPU-Based Skinning for Vegetation
  in Alan Wake 2*, GDC 2024, slides 13–23 and 26–31. Pre-skinning can reuse
  deformed vertices across passes and has a separate bandwidth, residency and
  synchronization cost. Benchmark it if repeated vertex work dominates in a
  named dense near-field workload.
  [Kandar slides](https://media.gdcvault.com/gdc2024/Slides/GDC%2Bslide%2Bpresentations/Kandar_Kiyavash_Large_Scale_GPU.pdf).
