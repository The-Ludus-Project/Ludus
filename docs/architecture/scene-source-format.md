# Ludus scene source format (`.luscene.json`)

Status: version 1, November 3, 2026. Reader `Ludus::SceneSource`
(`modules/content/scene_source`), importer profile `ludus-scene-v1` in
`Ludus::SceneCook`.

A Ludus scene source is a scene description that Ludus owns and that people can
read and edit by hand. It holds world-space convex polygons, materials with
emission and an optional camera. The engine's test scenes are written in it, so
the scenes the GPU tests render are files you can import into any project's World.

It is a source format, like pbrt-v4, glTF and OBJ: World **Import scene…** cooks it
into the project's LUSCN assets through the same planner, report and catalog as the
[best-effort importers](../development/best-effort-scene-import.md). It is not the
cooked runtime format ([LUSCN](content-resources.md)), and it is not a World
document. A World refers to cooked project assets; a scene source is what you import
to create them.

## Import an engine test scene into a project

1. Open the project in the Editor and open or create a World (**World → New World…**
   or **Open World…**).
2. Choose **Import scene…** and select a `.luscene.json` file, for example
   `tests/scenes/light_occlusion/louvre-near-emitter.luscene.json` from your Ludus
   checkout.
3. Check the preview: one area light, the cooked counts, and a
   `MATERIAL_APPROXIMATED` warning for the light's surface (see
   [what the cook changes](#what-the-cook-changes)). Choose **Import previewed scene**.
4. The viewport opens at the scene's camera (its
   [start view](../development/editor-workspace.md#scene-start-view)). Select a ray
   template, for example the Veach and Guibas MIS template, to see the light's
   soft shadows. **Save World** keeps the import.

The command line cooks the same files: `ludus-cook import <file> <output-directory>`.

## Engine test scenes

The files live in `tests/scenes`, with a `LICENSE.txt` beside them (the engine's
MIT license), which the import records like any other source license.

| File | Scene | Used by |
| --- | --- | --- |
| `light_occlusion/louvre-near-emitter.luscene.json` | A Lambertian wall lit by one rectangular, one-sided ceiling light through black slats just below the light. Most of the wall sees the light through several gaps. | `[shadow-occlusion]`: [partially occluded light, against an exact reference](../development/render-graph-authoring.md#compare-light-and-bsdf-sampling-with-mis) |
| `light_occlusion/louvre-at-wall.luscene.json` | The same wall and light with low slats that touch the wall: contact shadows with umbrae, and penumbrae that narrow towards the wall. | `[shadow-occlusion]` |
| `light_occlusion/occluded-penumbra.luscene.json` | After Peters 2021, Fig. 10: a blue diffuse plane and a white glossy plane under a rectangular light, with a horizontal wall that casts a large penumbra over their lower halves. | The occluded-penumbra check of the [polygonal-light-sampling template](../development/render-graph-authoring.md#compare-brdf-importance-sampling-for-polygonal-lights) |
| `cornell_box/cornell-box.luscene.json` | The Cornell room of the GPU tests: a white room spanning [−1, 1] on every axis, open towards the camera, with red and green side walls, a square one-sided ceiling light and the floating top of a block that shadows the floor. | Most ray-template tests, for example the [compute ray-tracing template](../development/render-graph-authoring.md#use-the-compute-ray-tracing-template) and the diffuse room of the [polygonal-light-sampling template](../development/render-graph-authoring.md#compare-brdf-importance-sampling-for-polygonal-lights) |
| `cornell_box/cornell-box-glossy.luscene.json` | The Cornell room with glossy metal (roughness 0.3) on its white surfaces and no sky. | The glossy room of the polygonal-light-sampling template |
| `many_lights/many-emitter-room.luscene.json` | The Cornell room with its ceiling light replaced by a 4 × 4 grid of small emitters, each its own material, whose radiance spans a factor of 64. | The many-emitter room of the [RIS projected-solid-angle template](../development/render-graph-authoring.md#resample-many-area-lights-with-projected-solid-angle-sampling) and the polygonal-light-sampling template's triangle-budget check |
| `many_lights/emitter-field-diffuse.luscene.json` | The Cornell room with a 40 × 40 field of small emitters (3200 emissive triangles) in four radiance levels and three tints; one object per level and tint; rough dielectric walls. | The diffuse variant of the hidden `[.ris-projected-solid-angle-benchmark]` |
| `many_lights/emitter-field-specular.luscene.json` | The same field over glossy metal (roughness 0.2), which mirrors the emitters. | The specular variant of `[.ris-projected-solid-angle-benchmark]` |
| `diffraction/single-edge.luscene.json` | Diffraction set, step 1: one black jaw whose edge, a 30° wedge (UTD n = 11/6), lies along y at x = 0, between a 3 cm one-sided square light and a white Lambertian wall. The canonical edge: lit for x < 0, shadowed for x > 0. | The edge-diffraction tests (`[edge-diffraction]` in `ludus_renderer_tests`); see [diffraction references](#diffraction-references) |
| `diffraction/single-slit.luscene.json` | Diffraction set, step 2: the same light and wall with two such jaws 5 cm apart: a slit whose two edges interfere. | The same |
| `diffraction/slit-partial-occluder.luscene.json` | Diffraction set, step 3: the slit with a black 2 cm rod between it and the light, which partially occludes the light and hides one slit edge from part of it. | The same |
| `line_sampling/large-line-grid.luscene.json` | A 20 m floor and a ceiling 4 m above it, each tessellated into 160 × 160 quads (102,400 triangles), under a 2 m square light. 3.2 MB, because it lists every quad. | The [`[scene-capacity]` regression](renderer-systems.md#l51-budgeted-scene-capacity) of the line-sampling template |

All but the diffraction scenes and the large line grid use the Cornell room camera of the GPU tests: eye at
(0, 0, 3.4) looking down −z, with a vertical field of view of 2·atan(1/2.2) ≈ 48.89°.
The large line grid's camera stands at (15, 0.6, 10) and looks along −x, with a
vertical field of view of 2·atan(1/1.2) ≈ 79.61°. The diffraction scenes' camera stands at
(0, 0, −0.1) and looks down −z at the wall, with a vertical field of view of
2·atan(0.4/0.9) ≈ 47.92°, which shows the wall from y = −0.4 to 0.4. The test camera renders 96 × 64
pixels; the viewport keeps its own aspect ratio, so it can show a little more or less
of the scene.

Some test rooms need what version 1 cannot describe, so they stay in the test code:

- the grating room of the Steinberg PLT tests, lit only by a point or directional
  light;
- the glossy Cornell room under a grey sky (an environment light), and the Cornell
  light with a 4 × 4 emission map (a texture);
- the line-visibility scenes, whose line light is the test kernel's segment and whose
  occluders differ from case to case;
- the large line scene read from an exported reference scene, such as Intel Sponza,
  which is not part of the repository.

### Diffraction references

The three diffraction scenes are a graded set for implementing edge diffraction, such as the Uniform Theory of
Diffraction (UTD). Each step adds one effect, so a failure points at one cause:

1. **Single edge.** One edge, lit and shadowed sides. This tests the diffraction coefficient, its transition
   function and the wedge angle.
2. **Single slit.** Two edges whose diffracted fields interfere with each other and with the direct light.
3. **Slit with a partial occluder.** A rod hides part of the light from the slit and hides the right slit edge from
   the left half of the light. This tests partial occlusion of an extended light and the visibility of the incident and
   diffracted paths.

All three share one layout:

- A 3 cm one-sided square light of radiance 10000 at z = 0.5.
- Black jaws at z = 0. Each jaw is a closed 2 cm slab spanning y in [−1.5, 1.5]. Its edge is a 30° wedge, flat towards
  the light and bevelled towards the wall, so the edge's exterior angle is 330° (n = 11/6) and the bevel stays in shadow.
  The partial occluder's rod has four 90° edges (n = 3/2).
- A white Lambertian wall at z = −1.
- A camera between the jaws and the wall, facing the wall.

The scenes are sized for a design wavelength of 5 mm. Every result depends only on lengths measured in wavelengths, so
a different wavelength needs every length scaled with it. At 5 mm, the slit's Fresnel number is 1.5, and the light's
transverse coherence width at the slit is 1.7 slit widths. A visible-wavelength slit would have to be under a
millimetre wide, and the cooked format's radiance limit of 10000 could not light it at exposure one.

#### The oracle

The edge-diffraction oracle (`modules/graphics/renderer/tests/edge_diffraction_oracle.h`) reads a scene file and
computes, in float64, the radiance that leaves the wall:

| Model | Meaning |
| --- | --- |
| `geometric` | Straight-line visibility: what a renderer without diffraction gives. |
| `kirchhoff` | The Fresnel-Kirchhoff integral over the aperture of a black, infinitely thin screen. It applies only to the single slit, whose jaws leave one aperture in a shared plane. |
| `utd-absorbing` | First-order UTD with faces that reflect nothing: geometric optics plus one Kouyoumjian-Pathak diffraction at every edge, along paths no obstacle blocks. It keeps the coefficient's incident-field terms, the mean of the soft and hard amplitudes; for a half-plane that is the black screen's edge. This is the reference for these black scenes. |
| `utd-soft`, `utd-hard` | First-order UTD for perfectly conducting faces, including their reflections, with Dirichlet or Neumann edges. |
| `utd-unpolarized` | The mean of the soft and hard intensities. |

The light is a spatially incoherent (thermal) emitter, so the oracle adds the intensities of its points. It splits the
light at every position where an obstacle corner changes a path's visibility and integrates each piece by
Gauss-Legendre quadrature, so shadows on the light cost no accuracy.

The oracle's tests check the following. Run them with `ludus_renderer_tests "[edge-diffraction]"`.

- The Fresnel integrals and the transition function against tabulated values.
- UTD against Sommerfeld's exact half-plane solution, to 1e-9.
- The UTD coefficient against Keller's non-uniform limit away from the boundaries.
- Reciprocity, and no diffraction where there is no edge.
- A continuous total field across every shadow and reflection boundary of a 30° wedge, and across the scenes' shadow
  boundaries.
- Conserved power through the slit.
- Agreement of absorbing UTD, unpolarized UTD and Kirchhoff within 0.03 near the slit's beam.

#### Compare a renderer with the reference

The comparison works on LUHDR01 captures, the linear HDR files that reference captures save. It compares each image
column summed over every row. The scenes vary little along y, so Monte Carlo noise averages out and a column's ratio
measures the renderer's diffraction. A capture passes when its camera and wavelength match the reference's and every
significant column agrees within the tolerance. A column is significant when it holds at least 1% of the largest
column's light.

1. **Capture the scene.** Import it into a new project: in the Editor with **Import scene…** and **Save World**, or
   headlessly with the hidden `[.prepare-full-sponza]` editor test. That test takes
   `LUDUS_FULL_SPONZA_PROJECT=<new project directory>` and `LUDUS_FULL_SPONZA_ENTRY=<scene file>` and saves the World
   as `assets/sponza.world.json`. Then capture the World from the scene's camera, at pixel centres and with optical
   transport at the design wavelength. From a build tree:

   ```bash
   QT_QPA_PLATFORM=offscreen LUDUS_REFERENCE_TEST_PROJECT=<project> LUDUS_REFERENCE_TEST_WORLD=sponza LUDUS_REFERENCE_TEST_TRAVERSAL=2 LUDUS_REFERENCE_TEST_AUTHORED_CAMERA=1 LUDUS_REFERENCE_TEST_CENTER_RAYS=1 LUDUS_REFERENCE_TEST_EXTENT=480x320 LUDUS_REFERENCE_TEST_SAMPLES=256 LUDUS_REFERENCE_TEST_OPTICAL_WAVELENGTH=0.005 LUDUS_REFERENCE_TEST_OUTPUT=<directory> apps/editor/ludus_editor_tests "[.supplied-reference-capture]"
   ```

   Omit `LUDUS_REFERENCE_TEST_OPTICAL_WAVELENGTH` for a geometric capture. A render graph's own capture works as
   well, if it saves LUHDR01 from the scene's camera.

2. **Compare it.** This renders the reference at the capture's extent and wavelength and compares:

   ```bash
   LUDUS_DIFFRACTION_SCENE=<ludus>/tests/scenes/diffraction/single-slit.luscene.json LUDUS_DIFFRACTION_CAPTURE=<capture.luhdr> LUDUS_DIFFRACTION_OUTPUT=<directory> modules/graphics/renderer/ludus_renderer_tests "[.diffraction-compare]"
   ```

   It prints the camera and wavelength checks, the worst column's ratio, the total ratio and the per-pixel relative
   L1. It fails unless the capture passes. `<directory>` receives the reference and `comparison-profile.csv`, one
   line per column: the wall x, the centre row's and the column's mean radiance for the reference and the capture,
   and the ratio of the column sums.

The comparison reads these optional variables:

| Variable | Default | Meaning |
| --- | --- | --- |
| `LUDUS_DIFFRACTION_MODEL` | `utd-absorbing` for an optical capture, `geometric` otherwise | The oracle model. |
| `LUDUS_DIFFRACTION_WAVELENGTH` | The capture's | The reference's wavelength, in metres. |
| `LUDUS_DIFFRACTION_TOLERANCE` | 0.05 | The accepted deviation of a column's ratio from one. |
| `LUDUS_DIFFRACTION_LIGHT_NODES` | 6 | Gauss-Legendre nodes per light piece and axis. |
| `LUDUS_DIFFRACTION_WORKERS` | 8 | Threads that render the reference. |
| `LUDUS_DIFFRACTION_REFERENCE` | — | A reference `.luhdr` to reuse instead of rendering one. |

`ludus_renderer_tests "[.diffraction-reference]"` writes a reference on its own. It takes `LUDUS_DIFFRACTION_SCENE`,
`LUDUS_DIFFRACTION_OUTPUT` and the variables above, plus `LUDUS_DIFFRACTION_EXTENT` (default `480x320`). It writes
`reference-<model>.luhdr` and `reference-<model>-profile.csv`. A UTD reference of 480 × 320 pixels takes a few minutes
on eight threads in a debug build. The reference's flags mark the CPU oracle, centre rays and reverse-Z depth, so
`scripts/compare-render` and the editor's comparison read it like any capture.

#### What the wall shows

The centre row of each 480 × 320 reference, which lies 1.25 mm below y = 0, in W/(sr·m²). Without obstacles the
wall's centre receives 1.018.

**Single edge** (the edge at x = 0; the jaw covers x > 0):

| x | −50 cm | −25 cm | −12 cm | −8 cm | −4 cm | 0 | 4 cm | 8 cm | 12 cm | 25 cm | 50 cm |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Geometric | 0.824 | 0.964 | 1.005 | 1.013 | 1.017 | 0.531 | 0 | 0 | 0 | 0 | 0 |
| UTD absorbing | 0.822 | 0.959 | 1.211 | 1.163 | 0.660 | 0.283 | 0.115 | 0.051 | 0.026 | 0.0061 | 0.0014 |

**Single slit** (symmetric in x):

| x | 0 | 4 cm | 8 cm | 12 cm | 20 cm | 25 cm | 50 cm |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Geometric | 1.018 | 1.017 | 0.443 | 0 | 0 | 0 | 0 |
| Kirchhoff | 1.228 | 0.811 | 0.223 | 0.097 | 0.018 | 0.021 | 0.0041 |
| UTD absorbing | 1.227 | 0.811 | 0.224 | 0.097 | 0.019 | 0.021 | 0.0043 |

**Slit with a partial occluder**:

| x | −25 cm | −12 cm | −8 cm | −4 cm | 0 | 4 cm | 8 cm | 12 cm | 25 cm |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Geometric | 0 | 0 | 0.400 | 0.775 | 0.517 | 0.233 | 0 | 0 | 0 |
| UTD absorbing, first order | 0.012 | 0.108 | 0.184 | 0.277 | 0.278 | 0.327 | 0.113 | 0.022 | 0.0034 |

What to look for:

- **Single edge.** On the shadow boundary the wall receives about a quarter of the unobstructed light. The lit side
  fringes, peaking 20% above it near −12 cm, and the shadow decays smoothly.
- **Single slit.** The centre is about 20% brighter and narrower than the geometric strip. Fringes about 12 cm apart
  carry light deep into the geometric shadow.
- **Agreement between methods.** The single slit's Kirchhoff and absorbing-UTD images, two independent methods, pass
  the comparison against each other: worst column 2.0%, total ratio 0.999, per-pixel relative L1 0.25%.
- **Validation of the comparison.** The engine's GPU reference capture of the single slit, with geometric transport,
  passes against the `geometric` reference: worst column 2.5% (Monte Carlo noise in the penumbra), total ratio
  1.0001. The reference renderer's optical transport (paraxial Gaussian-mode coupling, not edge diffraction) fails
  against `utd-absorbing`: total ratio 0.68.

#### Limits

- The oracle treats the 3 m edges as infinitely long and leaves out diffraction from their ends (corner diffraction,
  which UTD does not describe). At the wall's centre row the ends lie 1.5 m away.
- First-order UTD leaves out light diffracted from edge to edge. In the slit, that light is weak. In the partial
  occluder it is not: the right slit edge lies 1.5 wavelengths inside the rod's shadow for the left half of the light.
  First-order UTD also switches a rod edge's diffracted ray off where its path to the wall starts to pass a jaw edge,
  so the third scene's reference jumps at about −12, +3 and +13 cm. That reference is what a first-order
  implementation must produce, not physics: edge-to-edge diffraction smooths the jumps and is the next step the scene
  tests.
- Kirchhoff applies only to the single slit.

### Keeping the files and the GPU fixtures in step

The GPU tests build their geometry in `modules/graphics/renderer/tests`. The
`[scene-source]` cases of `ludus_render_graph_ray_tests` parse each file and require
it to equal its fixture. Slat and light polygons, which the exact-irradiance oracle
reads, must match corner for corner in float64; everything else must match after
narrowing to float32, which is what the GPU receives. For the Cornell, many-light and
line rooms, each polygon's fan from its first corner must give the fixture mesh's
next triangles of the polygon's material, so the cooked triangles are the fixture's. These checks need no GPU. If
you change a fixture, regenerate the file's polygons from it (the louvre slats come
from `TryBuildLouvre` in `shadow_occlusion_oracle.cpp`) and print each coordinate
with the shortest decimal that round-trips, so it reads back as the same binary64
value.

## Document layout

```json
{
  "format": "ludus.scene",
  "version": 1,
  "name": "Lit wall",
  "description": "A wall under one ceiling light.",
  "camera": {
    "eye": [0, 0, 3.4],
    "target": [0, 0, -1],
    "up": [0, 1, 0],
    "vertical_fov_degrees": 48.88790956083307
  },
  "materials": [
    {"name": "Wall", "model": "diffuse", "base_color": [0.8, 0.5, 0.25]},
    {"name": "Light", "model": "diffuse", "base_color": [0, 0, 0],
     "emission": {"radiance": [6, 6, 6], "two_sided": false}}
  ],
  "objects": [
    {
      "name": "Wall",
      "material": "Wall",
      "positions": [[-3.2, -2.2, -1], [3.2, -2.2, -1], [3.2, 1.1, -1], [-3.2, 1.1, -1]],
      "polygons": [[0, 1, 2, 3]]
    },
    {
      "name": "Ceiling light",
      "material": "Light",
      "positions": [[-0.8, 1.1, -0.8], [0.8, 1.1, -0.8], [0.8, 1.1, 0.2], [-0.8, 1.1, 0.2]],
      "polygons": [[0, 1, 2, 3]]
    }
  ]
}
```

### Root

| Member | Required | Meaning |
| --- | --- | --- |
| `format` | yes, and first | `"ludus.scene"`. Being first lets an importer recognise the file from a short prefix. |
| `version` | yes | `1`. |
| `name` | yes | Display name, 1–1024 bytes. |
| `description` | no | Free text, at most 1024 bytes. |
| `camera` | no | Becomes the imported World's start view. Without it the import frames the scene's bounds. |
| `materials` | yes | 1–256 materials. |
| `objects` | yes | 1–4096 objects. |

### Camera

| Member | Meaning |
| --- | --- |
| `eye` | Centre of projection, metres. |
| `target` | A point the camera looks at, distinct from `eye`. |
| `up` | Approximate up direction, not parallel to the view direction. |
| `vertical_fov_degrees` | Full vertical field of view, in (0, 180) degrees. |

### Material

| Member | Required | Meaning |
| --- | --- | --- |
| `name` | yes | Unique name that objects refer to. |
| `model` | yes | `"diffuse"`: ideal Lambertian reflection. `"metallic_roughness"`: the engine's metallic-roughness model. `"mix"`: a weighted sum of Lambertian and rough conductor reflection. `"project_brdf"` selects a [project BRDF program](#project-brdf-program). |
| `base_color` | yes | Linear Rec. 709 reflectance, each channel in [0, 1]. |
| `metallic` | no | `metallic_roughness` only; [0, 1], default 0. |
| `roughness` | for project BRDF | Perceptual roughness in [0.045, 1] for `project_brdf`. Optional in [0, 1] for `metallic_roughness`, default 1. |
| `brdf` | for project BRDF | Versioned reflection program described below. |
| `weight` | for Mix | Conductor fraction in [0, 1]. The Lambertian lobe uses the complementary weight and `base_color`. |
| `conductor` | for Mix | Contains `alpha` (isotropic GGX width in [0.0001, 1]), `eta` (three positive real IOR channels), and `k` (three nonnegative extinction channels). Optical constants are at most 10000. |
| `emission` | no | Makes every polygon of the material an area light. |
| `emission.radiance` | yes | Constant emitted linear Rec. 709 radiance in W/(sr·m²) per channel; finite and nonnegative. |
| `emission.two_sided` | no | `true` emits from both faces; default `false`, the front face only. |

### Project BRDF program

Give the material a unique `project/name` within the source document. An object
can use it by name, for example `"material": "demo/glaze"`.

```json
{
  "name": "demo/glaze",
  "model": "project_brdf",
  "base_color": [0.3, 0.5, 0.7],
  "roughness": 0.35,
  "brdf": {
    "version": 1,
    "weights": [0.25, 0.5, 0.25],
    "powers": [0, 4, 12],
    "fixed_dimension_reflection": true
  }
}
```

Version 1 evaluates a weighted sum of three reciprocal cosine-power terms.
Enter three relative weights in [0, 1] with a total of at least 0.001 and
three powers in [0, 32]. A zero weight disables its term. A zero power gives
a Lambertian term. Roughness brings every term toward Lambertian as it
approaches 1. The optional `fixed_dimension_reflection` boolean declares
continuous reflection with exactly three random values per proposal.
Its default is `false`.

See [assign and edit a project BRDF in World](../development/editor-workspace.md#assign-and-edit-a-project-brdf)
and the [transport contract](renderer-systems.md#project-brdf-programs).

### Object

| Member | Meaning |
| --- | --- |
| `name` | Unique, nonempty name. |
| `material` | Name of a declared material. |
| `positions` | World-space positions in metres, each `[x, y, z]` with every coordinate within ±1,000,000. |
| `polygons` | Each polygon lists 3–64 indices into the object's `positions`. |

## Conventions

- Space is the engine's world space: right-handed, +y up, metres. There are no
  per-object transforms or instances in version 1.
- A polygon's front face is the side from which its corners appear
  counterclockwise; its geometric normal follows the right-hand rule. One-sided
  emission leaves the front face.
- Polygons must be planar and strictly convex with distinct corners: every other
  corner lies strictly to the left of each edge (a sine above 10⁻⁹ relative to the
  edge and corner distances), and every corner lies within 10⁻⁹ times the longest
  edge of the plane through the first corner. This rejects collinear and repeated
  corners, reflex corners and self-intersecting orders, so fan triangulation and
  exact polygon oracles both apply.
- Numbers are read as IEEE binary64. Write the shortest decimal that round-trips
  when a value must be reproduced exactly.

## Validation and limits

The reader rejects the whole document on the first problem and names it by its JSON
path, for example `objects[2].polygons[0]: must be planar and strictly convex, with
distinct corners`. The import report shows that message as `MALFORMED_INPUT` (or
`LIMIT_EXCEEDED`). Unknown, missing and repeated members are errors, so a member
added in a later version cannot be silently ignored by an older reader.

| Limit | Value |
| --- | --- |
| Document size | 16 MiB |
| Names and description | 1024 bytes |
| Materials | 256 |
| Objects | 4096 |
| Corners per polygon | 64 |
| Positions in the document | 2²⁰ |
| Polygon corners in the document | 2²² |

These are the source format's own limits. The cooked scene must also fit the
[import profile](content-resources.md#import-profile-and-alpha-masks-luscn-v5),
whose emission and capacity limits the cook enforces.

## What the cook changes

- Each polygon becomes a triangle fan from its first corner, with its own vertices
  and its flat geometric normal, so shading never blends across polygon edges.
  Positions narrow to float32.
- An untextured base colour is kept exactly, as the linear reflectance factor over
  a white swatch, not as an 8-bit sRGB swatch.
- Emitting materials become required area lights with their exact radiance. Like
  pbrt area lights, their surfaces reflect as white rough `metallic_roughness` in
  LUSCN; the report records `MATERIAL_APPROXIMATED` when the authored surface
  differs.
- LUSCN stores no reflection sidedness, so how a renderer shades back faces is its own
  choice, as for other imports. Emission keeps its sidedness.

## Implementation references

Polygon normals use Newell's method, credited to Martin E. Newell by Ivan E.
Sutherland, Robert F. Sproull and Robert A. Schumacker, "A Characterization of Ten
Hidden-Surface Algorithms", ACM Computing Surveys 6(1), 1974
(<https://doi.org/10.1145/356625.356626>).
