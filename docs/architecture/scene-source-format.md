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
| `line_sampling/large-line-grid.luscene.json` | A 20 m floor and a ceiling 4 m above it, each tessellated into 160 × 160 quads (102,400 triangles), under a 2 m square light. 3.2 MB, because it lists every quad. | The [`[scene-capacity]` regression](renderer-systems.md#l51-budgeted-scene-capacity) of the line-sampling template |

All but the large line grid use the Cornell room camera of the GPU tests: eye at
(0, 0, 3.4) looking down −z, with a vertical field of view of 2·atan(1/2.2) ≈ 48.89°.
The large line grid's camera stands at (15, 0.6, 10) and looks along −x, with a
vertical field of view of 2·atan(1/1.2) ≈ 79.61°. The test camera renders 96 × 64
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
| `model` | yes | `"diffuse"`: ideal Lambertian reflection. `"metallic_roughness"`: the engine's metallic-roughness model. |
| `base_color` | yes | Linear Rec. 709 reflectance, each channel in [0, 1]. |
| `metallic` | no | `metallic_roughness` only; [0, 1], default 0. |
| `roughness` | no | `metallic_roughness` only; perceptual roughness in [0, 1], default 1. |
| `emission` | no | Makes every polygon of the material an area light. |
| `emission.radiance` | yes | Constant emitted linear Rec. 709 radiance in W/(sr·m²) per channel; finite and nonnegative. |
| `emission.two_sided` | no | `true` emits from both faces; default `false`, the front face only. |

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
