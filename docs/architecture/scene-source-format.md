# Ludus scene source format (`.luscene.json`)

Status: version 1, November 3, 2026. Reader `Ludus::SceneSource`
(`modules/content/scene_source`), importer profile `ludus-scene-v1` in
`Ludus::SceneCook`.

A Ludus scene source is a scene description that Ludus owns and that people can
read and edit by hand. It holds world-space convex polygons, parametric
[aperture screens](#aperture), materials with emission and an optional camera. The engine's test scenes are written in it, so
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

A scene's [apertures](#aperture) import into a default World as aperture entities
rather than cooked geometry. Import `tests/scenes/diffraction/single-slit.luscene.json`,
select **Slit screen** in the entity list, change **Opening** under **Aperture** in
**Properties** and choose **Apply Changes**: the viewport and the ray templates show
the wider or narrower slit at once, without a reimport.

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
| `diffraction/single-slit.luscene.json` | A single slit for edge diffraction, such as the Uniform Theory of Diffraction (UTD): a 3 cm one-sided square light 0.5 m in front of an opaque black screen, an [aperture](#aperture) whose vertical slit is 80 cm long with a 5 cm opening, and a white Lambertian wall 1 m behind the screen. The slit's long edges are knife edges; in a default World its opening is an editable entity field. The camera stands between the screen and the wall. At a design wavelength of 5 mm and the 5 cm opening, the Fresnel number is 1.5 and the light's transverse coherence width at the slit is 1.7 slit widths; scale every length with the wavelength. | The import and aperture-deferral checks of `ludus_scene_import_tests` and the `[aperture]` import test of `ludus_editor_tests`; no renderer test yet |
| `diffraction/knife-edge.luscene.json` | A thin half-plane with one central straight edge. The receiver crosses the geometric shadow boundary at x = 0. | Scene cook and the [diffraction reference setup](#diffraction-reference-scenes) |
| `diffraction/right-angle-wedge.luscene.json` | Two perpendicular faces with a shared vertical edge. The receiver crosses the geometric shadow boundary at x = 1 m. | Scene cook and the [diffraction reference setup](#diffraction-reference-scenes) |
| `diffraction/double-slit.luscene.json` | A thin screen with two fixed vertical openings, each 4 cm wide and 1 m tall, with their centres 20 cm apart. | Scene cook and the [diffraction reference setup](#diffraction-reference-scenes) |
| `materials/closure-program-block.luscene.json` | A weathered gold block on a grey floor under a square one-sided ceiling light. The block's [closure program](#closure-program) mixes a rust diffuse lobe, a gold conductor and a cosine-power sheen whose weights read the metallic and base colour inputs. | The closure-program cook check of `ludus_scene_import_tests` and the `[program]` World import test of `ludus_editor_tests` |
| `materials/skin-diffusion-bench.luscene.json` | A head-sized bench for [skin](#skin): a skin block beside a glossy white non-skin block at the same depth, a dark bar floating in front that casts a sharp shadow band across both faces, and a small one-sided light in front of them. The bar's silhouette is a depth edge over the skin and the white block a material edge at equal depth. Its scattering distances are exaggerated so the spread covers several pixels of the test image. | The skin cook and decode check of `ludus_scene_import_tests` |
| `materials/grain-mix-board.luscene.json` | A board leaning back on a grey floor under a square one-sided ceiling light. Its [closure program](#closure-program) reads a scalar grain mask from the base colour input: a white diffuse lobe takes the complement and a glossy gold conductor takes the mask. The authored base colour is an interior mask; a base texture supplies the grain. | The `[scene-source]` check of `ludus_render_graph_ray_tests`, which requires the file to compile to the [texture-weighted mixing](renderer-systems.md#texture-weighted-mixing-of-distinct-lobes) tests' program record, and the grain-mix cook check of `ludus_scene_import_tests` |
| `materials/varnished-wood-panel.luscene.json` | A varnished wood panel beside a black companion patch under the same coat, on a grey floor under a square one-sided ceiling light. Its [closure program](#closure-program) authors the first [opaque layered material](renderer-systems.md#the-first-opaque-layered-material): an ordered dielectric varnish coat over a diffuse wood substrate. A cosine-power lobe weighted by the coat's normal-incidence reflectance is the varnish highlight; the diffuse substrate is weighted by the coat's complementary transmittance, so the coat sits over the wood and attenuates it. The black patch's substrate is black, so its response is the coat reflection alone. | The varnished-wood cook check of `ludus_scene_import_tests`; import it into World to author the coat over the substrate and compare the two patches |
| `materials/hair-card-patch.luscene.json` | A flat vertical [hair card](#hair-card) with a vertical strand beside an ordinary metallic/roughness control card of the same base colour and roughness, under a square one-sided ceiling light above a grey floor. | The hair-card cook check of `ludus_scene_import_tests`; import it into World to compare the strand highlight with the control |
| `line_sampling/large-line-grid.luscene.json` | A 20 m floor and a ceiling 4 m above it, each tessellated into 160 × 160 quads (102,400 triangles), under a 2 m square light. 3.2 MB, because it lists every quad. | The [`[scene-capacity]` regression](renderer-systems.md#l51-budgeted-scene-capacity) of the line-sampling template |

The diffraction scenes, large line grid and skin bench have their own cameras. The other
scenes use the Cornell room camera of the GPU tests: eye at (0, 0, 3.4) looking down −z,
with a vertical field of view of 2·atan(1/2.2) ≈ 48.89°.
The large line grid's camera stands at (15, 0.6, 10) and looks along −x, with a
vertical field of view of 2·atan(1/1.2) ≈ 79.61°. The single slit's camera stands at
(0, 0, −0.1) and looks down −z at the wall, with a vertical field of view of
2·atan(0.4/0.9) ≈ 47.92°, which shows the wall from y = −0.4 to 0.4. The test camera renders 96 × 64
pixels; the viewport keeps its own aspect ratio, so it can show a little more or less
of the scene. The skin bench's camera stands at (0, 0.02, 0.45) and looks at
(0, −0.02, 0) with the Cornell field of view, which shows about 40 cm of the bench.

Some test rooms need what version 1 cannot describe, so they stay in the test code:

- the grating room of the Steinberg PLT tests, lit only by a point or directional
  light;
- the glossy Cornell room under a grey sky (an environment light), and the Cornell
  light with a 4 × 4 emission map (a texture);
- the line-visibility scenes, whose line light is the test kernel's segment and whose
  occluders differ from case to case;
- the large line scene read from an exported reference scene, such as Intel Sponza,
  which is not part of the repository.

### Character bust

`characters/infinite-head/infinite-head.gltf` is the static character bust of
[hair and skin rendering](renderer-systems.md#hair-and-skin-rendering): the
*Infinite, 3D Head Scan* by Lee Perry-Smith, from the three.js examples, with its
colour and tangent-space normal maps. It is glTF, not a scene source, and is not
covered by the engine's MIT license: its `LICENSE.txt` gives the Creative Commons
Attribution 3.0 terms, the source commit and the changes made for Ludus, and
**Import scene…** records that licence. The mesh is about 17 cm wide in metres,
so skin scattering distances keep their real millimetre scale. It has one
material slot, `Skin`, which imports as metallic-roughness; the scan has no
eyes, teeth or hair. `ludus_scene_import_tests` checks its slot, maps, licence
and scale. `infinite-head-lit.gltf` is the same bust standing on the ground with
a warm key and a cool fill directional light, for the
[skin measurements](../development/benchmark-scenes.md#measure-raster-skin-on-the-character-bust).
`infinite-head-hair.gltf` adds a second mesh and a separate `Hair` slot to
that same lit bust. The mesh has 44 overlapping ribbons in two layers. The
RGBA strand texture supplies colour and opacity, and the imported slot starts
as a masked dielectric at cutoff 0.5. The ribbons' UVs run along V from root
to tip. [Use Make Hair Card in the World inspector](../development/editor-workspace.md#make-a-slot-a-hair-card)
to set the strand direction and highlights.

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

### Diffraction reference scenes

Import these files into World to see the setup from behind the screen, or cook them with
`ludus-cook import`. The three fixtures use original Ludus meshes. Each has a single
5 mm square emitter at z = 0.5 m facing −z, a diffracting edge or screen at z = 0, and a
Lambertian receiver at z = −1 m. Use a design wavelength of 5 mm. The scene sources store
the geometry and preview materials. Set the wavelength and physical boundary conditions
in the reference calculation.

- **Knife edge.** Sweep the receiver along x at y = 0 through x = 0. The screen covers
  the negative-x side. [Sommerfeld's half-plane solution](https://pmc.ncbi.nlm.nih.gov/articles/PMC4844525/)
  gives an independent reference for the ideal conducting half-plane with plane-wave
  illumination. Use matched illumination when comparing the fixture's finite source.
- **Right-angle wedge.** The faces border the x ≤ 0, z ≤ 0 quadrant, leaving a 270°
  exterior (UTD wedge index 3/2). The source centre is at (−0.5, 0, 0.5) m. The wall
  sweep at y = 0 crosses the direct shadow boundary at x = 1 m. To test both the direct
  and reflected shadow boundaries, sweep virtual receivers on an arc centred on the
  edge in the xz plane. With angles measured from +z toward +x, the reflected and
  direct boundaries are at 45° and 135° respectively for the source centre.
  The smooth metal material is a preview proxy. Use the conducting boundary conditions
  and polarization of the chosen analytic comparison.
  [Kouyoumjian and Pathak's conducting wedge](https://ntrs.nasa.gov/citations/19750029381)
  and [ITU-R P.526-16 §6](https://www.itu.int/dms_pubrec/itu-r/rec/p/R-REC-P.526-16-202511-I%21%21PDF-E.pdf#page=42)
  describe the perfect and finite conductor cases, respectively.
- **Double slit.** Sweep the wall along x at y = 0 through the central fringe.
  Under the centred point-source approximation, the paraxial fringe spacing near
  the centre is λ times the screen-to-wall distance divided by the slit-centre
  separation, or about 25 mm. For a quantitative reference, sum the fields from the
  two openings before computing intensity, then integrate intensity over the
  area source for spatially incoherent emission. The geometry is a symmetric
  slit-only case inspired by
  [Steinberg and Pharr, *Wave Tracing*, Figure 3](https://doi.org/10.1111/cgf.70322),
  which compares UTD and Fraunhofer predictions and also studies a setup with reflectors.

For a quantitative comparison, use receiver irradiance or derive it from raw linear
wall radiance using the wall's Lambertian factor. Match the source and receiver geometry,
wavelength and material assumptions to the reference. The cook regression covers geometry
and lighting inputs. The [scalar paraxial Wave Tracing profile](../development/render-graph-authoring.md#trace-scene-optical-connections)
exercises angular-spectrum propagation in its own tests. Its
[UTD connection](../development/render-graph-authoring.md#utd-edge-diffraction-connections)
reproduces the knife-edge, wedge and double-slit geometry of these scenes in
`wave_edge_diffraction_tests.cpp`. Those tests compare it with Fresnel–Kirchhoff references for
absorbing screens; the conducting-wedge comparison remains open. Its
[Fraunhofer connection](../development/render-graph-authoring.md#fraunhofer-edge-diffraction-connections)
reproduces the knife-edge and double-slit screens in `wave_fraunhofer_diffraction_tests.cpp`,
against the Kirchhoff area integral of the connection's Gaussian beam.

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
| `model` | yes | `"diffuse"`: ideal Lambertian reflection. `"metallic_roughness"`: the engine's metallic-roughness model. `"mix"`: a weighted sum of Lambertian and rough conductor reflection. `"project_brdf"` selects a [project BRDF program](#project-brdf-program). `"program"` selects a [closure program](#closure-program). `"hair_card"` selects a two-sided [hair card](#hair-card). `"skin"` selects [skin](#skin). `"medium_interface"` makes the material the boundary of an [absorbing medium](#medium-interface) and takes only `name`, `model` and `medium`. |
| `base_color` | yes, except for a medium interface | Linear Rec. 709 reflectance, each channel in [0, 1]. |
| `metallic` | no | `metallic_roughness` and `program` only; [0, 1], default 0. |
| `roughness` | for project BRDF | Perceptual roughness in [0.045, 1] for `project_brdf`. Optional in [0, 1] for `metallic_roughness`, `program`, `hair_card` and `skin`, default 1. |
| `hair` | no | `hair_card` only: the strand controls described below. |
| `scattering_distance` | for skin | Linear Rec. 709 [skin](#skin) scattering distance in metres, each channel in [0, 0.1]. |
| `brdf` | for project BRDF | Versioned reflection program described below. |
| `program` | for program | Versioned closure program described below. |
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

### Closure program

A closure program describes a material as a weighted mixture of leaf lobes.
Named parameters compute the lobe weights and roughness from constants and from
the material's surface inputs. This example is a rusted metal. Its conductor
weight is the metallic input, and the rust's diffuse weight is the remainder
tinted orange:

```json
{
  "name": "Rust",
  "model": "program",
  "base_color": [0.6, 0.3, 0.1],
  "metallic": 0.7,
  "roughness": 0.4,
  "program": {
    "version": 1,
    "parameters": [
      {"name": "metal", "input": "metallic"},
      {"name": "rust", "one_minus": "metal"},
      {"name": "tint", "multiply": ["rust", [0.6, 0.3, 0.1]]},
      {"name": "rough", "input": "roughness"}
    ],
    "lobes": [
      {"operation": "diffuse", "weight": "tint"},
      {"operation": "conductor", "weight": "metal", "roughness": "rough",
       "eta": [0.2, 0.9, 1.1], "k": [3.9, 2.4, 2.2]}
    ]
  }
}
```

`parameters` is an optional list evaluated in order. Each entry has a unique
`name` and exactly one expression:

| Expression | Value |
| --- | --- |
| `"constant": c` | A number for all three channels, or three numbers. Magnitudes are at most 10000. |
| `"input": "base_color"` | The base colour, after its texture. `"roughness"` and `"metallic"` give that input in every channel. |
| `"add": [a, b]`, `"multiply": [a, b]` | Per-channel sum or product. |
| `"lerp": [a, b, t]` | Per-channel `a + (b - a) * t`. |
| `"one_minus": a`, `"clamp": a` | `1 - a`, or `a` clamped to [0, 1]. |

An operand names an earlier parameter or gives an inline constant. Name
references must point backwards, so a program cannot form a cycle. `lobes` lists
one to eight leaf lobes. Each lobe takes a `weight` operand whose three channels
scale the lobe:

| `operation` | Other members | Lobe |
| --- | --- | --- |
| `"diffuse"` | none | Lambertian reflection. |
| `"conductor"` | `roughness`, `eta`, `k` | Isotropic GGX conductor. Alpha is the square of the roughness operand. `eta` holds three positive and `k` three nonnegative channels, each at most 10000. |
| `"cosine_power"` | `roughness`, `power` | The [project BRDF](#project-brdf-program) cosine-power term with exponent `(1 - roughness) * power`, power in [0, 32]. |

A nested mix is written as a product of weights, so the mixture is always flat.
The material's `base_color`, `metallic` and `roughness` are the surface inputs.
They are also the metallic-roughness approximation that raster profiles draw.

An ordered coat over a substrate is parameter layering: the coat lobe takes the
coat's reflectance and the substrate lobe takes the complementary transmittance,
so the coat sits over the substrate and attenuates it. This example is the
[first opaque layered material](renderer-systems.md#the-first-opaque-layered-material),
a dielectric varnish over diffuse wood. The coat's normal-incidence reflectance
`((eta − 1) / (eta + 1))²` of index 1.5 is 0.04; the wood substrate is weighted
by `1 − 0.04`:

```json
{
  "name": "Varnished wood",
  "model": "program",
  "base_color": [0.32, 0.17, 0.08],
  "metallic": 0,
  "roughness": 0.18,
  "program": {
    "version": 1,
    "parameters": [
      {"name": "coat_reflectance", "constant": 0.04},
      {"name": "coat_transmittance", "one_minus": "coat_reflectance"},
      {"name": "wood", "input": "base_color"},
      {"name": "substrate", "multiply": ["coat_transmittance", "wood"]},
      {"name": "coat_roughness", "input": "roughness"}
    ],
    "lobes": [
      {"operation": "diffuse", "weight": "substrate"},
      {"operation": "cosine_power", "weight": "coat_reflectance",
       "roughness": "coat_roughness", "power": 24}
    ]
  }
}
```

The lobe order records the layer order: the substrate lobe first, the coat lobe
over it. Raising `coat_reflectance` deepens the varnish; a black `base_color`
under the same coat leaves the coat reflection alone, which is the companion
patch of the fixture.

The reader compiles the program into bounded register code:
- At most 32 values, counting inline constants.
- At most 8 distinct constants.
- At most 8 values live at once.

It drops parameters that no lobe uses, and it reuses a register after the last
read of its value. A program that exceeds a limit fails with `LIMIT_EXCEEDED`
and a message that names the fix. The cook stores programs in LUSCN version 11; see
[closure programs in the renderer](renderer-systems.md#material-closure-programs).

### Hair card

A `hair_card` material shades a two-sided card whose highlights follow an authored
strand direction. Base colour and roughness keep their usual meaning; hair has no
`metallic` member. Every `hair` member is optional.

```json
{"name": "Hair", "model": "hair_card", "base_color": [0.35, 0.22, 0.12], "roughness": 0.35,
 "hair": {"strand_direction_degrees": 90, "primary_shift_degrees": -5,
          "secondary_shift_degrees": 7.5, "primary_weight": 0.1, "secondary_weight": 0.05}}
```

| Member | Meaning |
| --- | --- |
| `hair.strand_direction_degrees` | Strand direction in the card's plane, in [−180, 180]. Zero follows the surface tangent (increasing texture U) and 90 the bitangent. A card without tangents or texture coordinates uses the horizontal tangent `+Y × normal`, so 90 runs up a vertical card. Default 0. |
| `hair.primary_shift_degrees` | Tilt of the untinted primary highlight toward the card normal, in [−30, 30]. Default −5. |
| `hair.secondary_shift_degrees` | Tilt of the base-colour-tinted secondary highlight, in [−30, 30]. Default 7.5. |
| `hair.primary_weight` | Primary highlight weight in [0, 1]. Default 0.1. |
| `hair.secondary_weight` | Secondary highlight weight in [0, 1]. Default 0.05. |

The cook stores the material as a LUSCN v13 `HairCard` record. Scene sources have
no texture coordinates or masks; import a textured, alpha-masked card from glTF for
coverage and [make its World slot a hair card](../development/editor-workspace.md#make-a-slot-a-hair-card). See [raster hair cards](renderer-systems.md#raster-hair-cards) for the
shading model and its limits.

### Skin

A `skin` material is a dielectric whose diffuse lighting spreads under the
surface over a per-channel distance, like light in human skin. Its surface
reflection is the metallic-roughness model with zero metallic: Lambertian
diffuse from `base_color` and a GGX specular lobe from `roughness`.

```json
{"name": "Skin", "model": "skin", "base_color": [0.8, 0.5, 0.4],
 "roughness": 0.45, "scattering_distance": [0.012, 0.006, 0.003]}
```

`scattering_distance` is required: write `[0, 0, 0]` for no spread, which shades
exactly like the same `metallic_roughness` material. `metallic` is rejected
because skin is a dielectric. Distances are in metres, so the object must be modelled at real scale; human skin spreads
red light furthest, over millimetres. The cook stores the material as a LUSCN
v14 `Skin` record. An imported material becomes skin per World slot with
[make a slot skin](../development/editor-workspace.md#make-a-slot-skin).

The clustered deferred profile, which the engine's default render graph uses,
spreads skin's diffuse lighting over the distance with a screen-space filter
([raster skin diffusion](renderer-systems.md#raster-skin-diffusion)). Other
raster profiles show skin as its metallic-roughness surface. Reference captures
shade the surface reflection only, without subsurface transport, until
[reference skin #1373](https://github.com/The-Ludus-Project/Ludus/issues/1373)
adds it. See [hair and skin rendering](renderer-systems.md#hair-and-skin-rendering).

### Medium interface

A `medium_interface` material is the invisible, index-matched boundary of one
bounded homogeneous absorbing medium, like pbrt-v4's `interface` material with an
interior medium. Its objects neither reflect nor occlude. Together they must form
a closed surface whose polygons are wound counterclockwise seen from outside, so
their front faces point out of the medium. Several disjoint closed pieces may share
the material.

```json
{"name": "Ink", "model": "medium_interface",
 "medium": {"type": "homogeneous", "absorption": [0.5, 1, 2]}}
```

| Member | Meaning |
| --- | --- |
| `medium.type` | `"homogeneous"`, the only supported type. Any other type, such as a density grid, is rejected as `is an unsupported medium type`. |
| `medium.absorption` | Linear Rec. 709 absorption coefficient `σ_a` of the enclosed medium per metre, each channel in [0, 10000]. Transmittance along a length `d` inside is `exp(-σ_a d)`; zero is vacuum. |

A document declares at most one medium interface material, because overlapping
and nested media are unsupported. Scattering, emission and density members are
not part of version 1. The cook stores the material as a LUSCN v12
`MediumInterface` record. RT1 CPU-oracle and GPU-compute reference captures
render it as described in the
[homogeneous absorbing medium reference](renderer-systems.md#homogeneous-absorbing-medium-reference).
A World cannot import a medium yet: opening a cooked bundle with a medium in the
Editor names the material and asks you to remove its objects, then author the box
with the World's [Absorbing Medium preset](../development/editor-workspace.md#absorbing-media),
or to use a reference capture of the bundle.

### Object

An object is either explicit polygons (`positions` and `polygons`) or one
[aperture](#aperture) (`aperture`), never both.

| Member | Meaning |
| --- | --- |
| `name` | Unique, nonempty name. |
| `material` | Name of a declared material. |
| `positions` | World-space positions in metres, each `[x, y, z]` with every coordinate within ±1,000,000. |
| `polygons` | Each polygon lists 3–64 indices into the object's `positions`. |
| `aperture` | A parametric screen with a slit, described below, instead of `positions` and `polygons`. |

### Aperture

An aperture is a flat, opaque rectangular screen with a centred rectangular slit,
whose long edges are knife edges (the screen has no thickness). Its sizes are
parameters, so changing the opening is one number:

```json
{
  "name": "Slit screen",
  "material": "Black screen",
  "aperture": {
    "center": [0, 0, 0],
    "normal": [0, 0, 1],
    "up": [0, 1, 0],
    "screen_width": 3,
    "screen_height": 3,
    "slit_length": 0.8,
    "opening": 0.05
  }
}
```

| Member | Meaning |
| --- | --- |
| `center` | World-space centre of the slit, metres. |
| `normal` | Direction the screen's front face looks along; nonzero, normalised by the reader. |
| `up` | Direction along the slit; nonzero and perpendicular to `normal` (cosine at most 10⁻⁹). Across the slit is `up` × `normal`. |
| `screen_width` | Screen extent across the slit, in (0, 2,000,000] metres. |
| `screen_height` | Screen extent along the slit, in (0, 2,000,000] metres. |
| `slit_length` | Slit extent along `up`, in (0, `screen_height`] metres. |
| `opening` | Slit width across it, in [0, `screen_width`) metres. Zero closes the slit. |

The reader expands an aperture into up to four strictly convex quads, left and right
of the slit and then above and below it, omitting a part with no area. Their corners
count towards the position and corner limits, must stay within ±1,000,000 m, and are
exactly the corners you would write by hand: the single slit cooks to the same
triangles as when its screen was written as polygons.

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
- `ludus-cook import` and tests cook an aperture as its quads, like any object. The
  Editor's **Import scene…** into a default World instead leaves each aperture out of
  the cooked bundle, with an `APERTURE_DEFERRED` note in the import report, and creates
  an aperture entity in the same undoable import. Its Transform places the aperture's
  frame and its **Aperture** component holds the sizes, so Properties edits the
  opening and World extraction regenerates the screen (see
  [apertures in the World](../development/editor-workspace.md#apertures)). The entity
  reflects as Lambertian with the material's base colour; the import warns when the
  material has other reflection terms or emission, which the entity does not keep.
  Reimporting the same file into the same World finds the entity it created and keeps
  its edits. A scene whose objects are all apertures has nothing left to cook, so its
  Editor import fails; add the scene's other geometry. Worlds with a project-owned
  schema cook apertures as quads.

## Implementation references

Polygon normals use Newell's method, credited to Martin E. Newell by Ivan E.
Sutherland, Robert F. Sproull and Robert A. Schumacker, "A Characterization of Ten
Hidden-Surface Algorithms", ACM Computing Surveys 6(1), 1974
(<https://doi.org/10.1145/356625.356626>).
