# Best-effort scene import (pbrt-v4, glTF 2.0, OBJ)

Status: glTF 2.0/GLB and Wavefront OBJ/MTL front-ends, October 18, 2026, on top
of the authored-lighting pbrt import of October 13, 2026. Importer ids
`pbrt-best-effort-v1`, `gltf-best-effort-v1` and `obj-best-effort-v1`; report
schema `1`.

`Ludus::SceneCook` has two pbrt paths. `pbrt-reference-v1` is the frozen,
byte-identical cooker for the recorded goldens; it rejects anything it does
not model exactly. The best-effort importers accept general pbrt-v4, glTF 2.0
(JSON or GLB) and Wavefront OBJ scenes, approximate what the runtime cannot
represent, fit the result into the LUSCN01 v3 limits and explain every
compromise in a versioned JSON report. All of them are native C++; neither the
cook nor its tests run Python.

The three best-effort front-ends only parse: each builds the same
format-neutral intermediate representation (SceneIR,
`modules/content/scene_cook/src/importers/scene_ir.h`) and hands it to one
shared budget planner, texture cook, bake and report writer. The IR keeps the
complete source data a front-end can express, not the LUSCN capacities, so a
later cooked-format revision can lift the limits without touching the parsers
(see [what LUSCN v3 cannot carry](#what-luscn-v3-cannot-carry-yet)).

## Running it

```sh
ludus-cook pbrt   path/to/scene.pbrt out/scene-dir    # writes scene.luscn, import-report.json, LICENSE.txt
ludus-cook gltf   path/to/scene.glb  out/scene-dir    # .gltf or .glb
ludus-cook obj    path/to/scene.obj  out/scene-dir
ludus-cook import path/to/any-entry  out/scene-dir    # importer chosen from the file's content
ludus-cook import path/to/any-entry  --dry-run        # parses and plans; prints the report only
```

A dry run reads geometry (so counts and budgets are real) but decodes no
images and packs no bundle. The same entry points are public API in
`ludus/content/scene_cook/best_effort_import.h`: `ImportPbrtBestEffort`,
`ImportGltfBestEffort`, `ImportObjBestEffort` and the detecting
`ImportSceneBestEffort`.

**Format detection** looks at the first 4 KiB, not the extension: the GLB magic
or a leading `{` selects glTF, an OBJ record keyword (`v`, `vt`, `f`, `mtllib`,
…) selects OBJ, and a capitalised pbrt directive selects pbrt. Only a
comment-only prefix falls back to the `.pbrt`, `.gltf`/`.glb` or `.obj`
extension; anything else is reported as unsupported.

The entry file's directory is the import root. Every dependency (pbrt
includes, PLY meshes, MTL libraries, external glTF buffers and images) must
resolve inside it; parsers receive dependencies through a bounded resolver and
never perform file or network I/O themselves. Remote URIs are not fetched.

## What is read

- A streaming, locale-free tokenizer (64 KiB window, 4096-byte token limit)
  with `file:line` on every diagnostic.
- Graphics state: every transform directive, `AttributeBegin`/`End`,
  `TransformBegin`/`End`, `CoordinateSystem`/`CoordSysTransform`,
  `ReverseOrientation`, named materials, textures and media.
- `Include` and `Import`, contained in the entry file's directory, limited to
  a depth of 32 and with cycle detection. A failed include is a warning; a
  syntax error stops only the file that contains it.
- `ObjectBegin`/`ObjectInstance` are kept as objects and instances in the IR
  and expanded when meshes are baked.
- Shapes: `plymesh` (ASCII, binary little- and big-endian; cached per file),
  `trianglemesh`, `bilinearmesh`, `disk` and `sphere` (tessellated) and the
  `loopsubdiv` control cage. Curves and other shapes are skipped and counted.
- Images: see [texture and environment decoding](#texture-and-environment-decoding).
  An unreadable surface texture falls back to a swatch with `TEXTURE_UNREADABLE`,
  while an unreadable lighting environment rejects the import rather than
  discarding illumination.

## glTF 2.0 / GLB profile (`gltf-best-effort-v1`)

The static triangle-mesh subset of the
[glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html).
JSON is read through the strict FoundationParsing facade (duplicate keys,
invalid UTF-8 and over-limit documents fail); positions in the report are file
names only, because JSON values carry no line numbers.

| Feature | Support |
| --- | --- |
| Containers | `.gltf` JSON, `.glb` (JSON + BIN chunks; unknown chunks skipped); `asset.version` 2.x |
| Buffers | GLB BIN chunk, base64 `data:` URIs, external files (percent-decoded, inside the import root) |
| Buffer views and accessors | offsets, interleaved `byteStride`, every component type, `normalized`, accessors without a buffer view (zeros); bounds checked before any allocation |
| Primitives | `TRIANGLES`; `TRIANGLE_STRIP`/`TRIANGLE_FAN` converted to lists; indexed (`uint8/16/32`) and non-indexed; points/lines skipped (`PRIMITIVE_SKIPPED`) |
| Attributes | `POSITION`, `NORMAL`, `TEXCOORD_0`, `TANGENT`; others (`TEXCOORD_1+`, `COLOR_0`, joints, weights) reported (`ATTRIBUTE_IGNORED`). Without `NORMAL` every triangle corner gets its own vertex so generated normals are flat, as the specification requires |
| Nodes | hierarchy of `matrix` or TRS (unit quaternion) transforms, default `scene` (or all root nodes without scenes); cycles, multiple parents and non-finite transforms fail |
| Instancing | a mesh placed by several nodes stays one IR object with one instance per node; mirrored placements flip winding |
| Materials | `baseColorFactor`/`baseColorTexture`, metallic/roughness factors and `metallicRoughnessTexture` (blue/green, averaged into scalars), `emissiveFactor` (+ `KHR_materials_emissive_strength`), `normalTexture`, `occlusionTexture`, `emissiveTexture`, `alphaMode`, `alphaCutoff`, `doubleSided`; the glTF default material (white, metallic 1, roughness 1) |
| Samplers | repeat or clamp; mirrored repeat and per-axis modes approximated (`TEXTURE_APPROXIMATED`) |
| Images | PNG and JPEG by URI, data URI or buffer view |
| Cameras | the first perspective camera in scene order (`yfov`, `aspectRatio`, `znear`, `zfar`); orthographic is approximated |
| Lights | `KHR_lights_punctual` directional (lux), point and spot (candela) with colour, intensity and cone angles; `range` reported |
| Extensions | implemented: `KHR_lights_punctual`, `KHR_materials_emissive_strength`, `KHR_mesh_quantization`. Any other *required* extension fails with `EXTENSION_REQUIRED`; optional ones are ignored per the specification with `EXTENSION_IGNORED` (and `KHR_texture_transform` with `TEXTURE_APPROXIMATED`) |
| Not imported (reported) | skins and animation (`ANIMATION_IGNORED`, rest pose), morph targets, sparse accessors, Draco/meshopt without an uncompressed fallback, `KHR_texture_basisu`/WebP images, `KHR_materials_*` other than emissive strength |

glTF is right-handed with +Y up and metres, matching the engine's world
convention, so nodes are not converted. glTF texture coordinates already have a
top-left origin.

## Wavefront OBJ/MTL profile (`obj-best-effort-v1`)

Following the Wavefront/Alias [OBJ](https://paulbourke.net/dataformats/obj/) and
[MTL](https://paulbourke.net/dataformats/mtl/) descriptions. The entry is
streamed; every diagnostic carries `file:line`.

| Feature | Support |
| --- | --- |
| Records | `v` (extra colour components reported), `vt` (v flipped to the top-down convention), `vn`, `f`, `o`, `g`, `s`, `usemtl`, `mtllib`; `\` line continuations |
| Indices | positive one-based and negative relative indices for `v`, `vt`, `vn`; zero, malformed or out-of-range indices fail with `MALFORMED_INPUT` |
| Faces | polygons up to 64 corners fan-triangulated (exact for convex polygons); larger polygons skipped; degenerate triangles removed (`GEOMETRY_REPAIRED`) |
| Grouping | one IR mesh per (object/group, material) pair (at most 2^18 pairs); equal corner tuples are welded |
| Smoothing | `s off`/`s 0` faces keep their own corners, so generated normals stay flat; numbered groups share them |
| Missing data | missing `vn` → area-weighted normals; a group mixing faces with and without `vt`/`vn` is made uniform with `ATTRIBUTE_IGNORED` |
| Not imported (reported) | `l`/`p`, free-form curves and surfaces, display attributes |
| MTL statements | `newmtl`, `Kd`, `Ks`, `Ke`, `Ns`, `d` (incl. `-halo`), `Tr`, `illum`, `Pr`, `Pm`; `Ka`, `Ni`, `Tf`, sheen/clearcoat keys reported or ignored |
| MTL maps | `map_Kd`, `map_Ks`, `map_Ns`, `map_d`, `map_Ke`, `map_Pr`, `map_Pm`, `bump`/`map_Bump`/`map_bump` (height), `norm` (tangent space) with `-clamp`, `-bm`, `-imfchan`, `-o/-s/-t` (ignored, reported); `\` path separators are normalised |

OBJ has no units or handedness declaration; coordinates are read as metres in
the engine's right-handed, +Y-up convention.

**Phong to metallic-roughness (deterministic):**

- When `Pr`, `Pm`, `map_Pr` or `map_Pm` is present the PBR extension wins:
  base colour `Kd`, roughness `Pr` (default 1), metallic `Pm` (default 0).
- Otherwise base colour is `Kd` (default 0.5 grey), metallic 0, and roughness is
  `(2 / (Ns + 2))^(1/4)`: Walter et al.'s Phong-to-Beckmann relation
  `alpha = sqrt(2 / (Ns + 2))`, with the Beckmann width taken as the GGX alpha
  (= roughness²). `illum 0/1` (no specular) gives roughness 1.
- A material with no diffuse term (`max(Kd) < 0.04`), a strong specular colour
  (`max(Ks) > 0.5`), a specular illumination model and no `map_Kd` becomes a
  conductor with base colour `Ks` (mirrors and polished metals).
- `d < 1`, `Tr > 0` or `map_d` keep the opacity in the IR and cook opaque
  (`ALPHA_APPROXIMATED`); refractive `illum` models report `TRANSMISSION_DROPPED`.

## Texture and environment decoding

| Format | Decoder | Notes |
| --- | --- | --- |
| PNG | vendored stb_image | 8-bit; 16-bit samples are reduced by stb |
| JPEG | vendored stb_image | baseline and progressive; EXIF orientation and ICC profiles ignored |
| Radiance RGBE (`.hdr`) | vendored stb_image | linear float; colour textures clamp and sRGB-encode |
| OpenEXR | private subset reader | single-part scanline or tiled (level 0), HALF/FLOAT/UINT channels, NONE/RLE/ZIPS/ZIP; R/G/B or Y. PIZ, PXR24, B44(A), DWAA/DWAB, deep and multi-part files are `Unsupported` |
| TGA, PFM | private readers | unchanged |

Formats are detected from magic bytes before the extension. Every decoder checks
the declared raster from the header first and rejects images above 8192 × 8192
pixels (sides up to 16384), so a few bytes cannot declare an enormous decode. The stb
configuration is private (`third_party/stb/stb_image_config.h`); every stb heap
allocation goes through a bounded per-thread allocator, so an oversized decode
reports out-of-memory instead of exhausting the process. Radiance and OpenEXR
environments keep linear energy and are box-halved (preserving mean radiance)
until they fit 4096 texels and the bundle budget; values beyond the profile's
radiance bound still reject the import as before.

## Tangents

When a material has a tangent-space normal map and a mesh has texture
coordinates but no tangents, MikkTSpace tangents are generated with the
reference implementation by Morten S. Mikkelsen (vendored, unmodified, in
`third_party/mikktspace`), using the OpenGL texture convention (v up) that
glTF and common bakers assume. Corners whose frames differ get their own
vertices. LUSCN v3 does not store tangents yet, so the report says
`TANGENTS_GENERATED … not stored`; meshes without texture coordinates are
reported instead.

## What LUSCN v3 cannot carry yet

The planner fits the IR into LUSCN v3 (6 meshes, 8 materials, 8 textures, 128
objects, 500k vertices, base-colour-only materials, flattened instancing). The
IR already holds, and the cook reports dropping:

- per-vertex tangents and normal/height maps (`NORMALMAP_DROPPED`);
- occlusion, emissive, specular and opacity textures (`TEXTURE_DROPPED`,
  `EMISSION_APPROXIMATED`); metallic/roughness textures are averaged into the
  scalar terms (`TEXTURE_APPROXIMATED`);
- alpha mode, cutoff and opacity (`ALPHA_APPROXIMATED`) and double-sidedness
  (`SIDEDNESS_IGNORED`);
- camera aspect ratio and clip planes, and glTF light ranges;
- instancing (objects and instances are expanded into world-space meshes).

## How it is approximated

| pbrt | Ludus |
| --- | --- |
| `diffuse`, `coateddiffuse` | base colour, roughness from the coat |
| `conductor`, `coatedconductor` | metallic, colour from the named eta/k table |
| `dielectric`, `thindielectric` | F0 0.04, transmission dropped |
| `mix` | the first material |
| other materials | grey default, `MATERIAL_DEFAULTED` |
| area lights | RGB/Y photometric emissive material, source sidedness and optional power; unequal resolved radiances stay separate |
| point, spot, distant lights | transformed world position/direction, RGB candela or lux, optional power/illuminance and spot cone |
| infinite light | authored constant or HDR environment, converted from PBRT equal-area mapping to engine equirectangular mapping |
| unsupported light types | skipped and counted |
| media | skipped |

| glTF / OBJ | Ludus |
| --- | --- |
| glTF metallic-roughness | base colour factor x texture, metallic/roughness factors x averaged texture channels |
| glTF / MTL emissive (`emissiveFactor` x strength, `Ke`) | linear RGB nits; *decorative* emission (see the budget planner); emissive textures dropped |
| MTL Phong terms | the deterministic conversion in the OBJ profile above |
| `KHR_lights_punctual` | the analytic light path below: colour x intensity in candela or lux |

Roughness follows pbrt: alpha is `sqrt(r)` when `remaproughness` is true and
`r` otherwise; the engine value is `sqrt(alpha)`, clamped to [0.045, 1].
Spectra are mapped to RGB through a small table, and blackbody emitters use a
cited curve fit. Every approximation adds a diagnostic code.

## Authored lighting profile

The shared scene profile is [LUSCN01 version 3](../architecture/content-resources.md), with stable
placed-object and source identities, emission sidedness, requested lumens,
analytic lights, HDR environment pixels and independent material/bake UVs.
Older v1/v2 bundles remain readable with their historical two-sided emission.

PBRT's spectral photometry is approximated by fixed Rec.709 RGB luminance:
source color is divided by `0.2126 R + 0.7152 G + 0.0722 B`, then multiplied by
`scale`. Requested area-light power resolves per placed shape after scaling as
`RGB/Y × scale × lumens / (π × world area × side count)`. Point power divides by
`4π`; distant illuminance is lux. The engine uses squared spot falloff, so power
normalization integrates that lobe rather than PBRT's smoothstep lobe. Outer
half angle is PBRT `coneangle`; inner is `coneangle - conedeltaangle`.

Constant environments normalize RGB/Y, with optional illuminance divided by π.
HDR PFM preserves linear energy; PNG/TGA environments decode sRGB. The source
light transform rotates the environment into world space. Optional HDR
illuminance uses bounded angular quadrature over the source light-space +Z
hemisphere. Lighting files enter the source digest closure, so changed sky bytes
invalidate the import preview. Values above the profile bounds reject, rather
than tone-map the lighting source. One infinite light is supported per PBRT
source scene; multiple directives reject explicitly. Separate imported scenes
combine through the durable scene-asset assembly contract.

The RGB proxy, material approximations, finite light range and coarse triangle
charts prevent pixel parity with PBRT's spectral path tracer. EXR, media,
transmission and textured area emission remain unsupported. See the
[renderer lighting contract](../architecture/renderer-systems.md) for viewport
area-light, shadow and indirect-transport limits.

## Budget planner

The cooked scene must fit 6 meshes, 8 materials, 8 textures, 500k vertices,
128 objects and 64 MiB. Each cooked mesh carries one material, so the planner:

1. reserves one slot per distinct emitter radiance, sidedness and power-normalization
   group; groups record their source shapes and area-light directives in the report;
2. ranks ordinary placements by world-space area and keeps the largest material
   groups in the remaining slots;
3. merges each remaining material into the nearest kept slot
   (`MATERIALS_MERGED`);
4. merges all geometry of a slot into one world-space mesh;
5. gives each triangle unique corners and a separate guttered bake chart, then
   drops the smallest ordinary placements if the expanded vertex budget is exceeded
   (`GEOMETRY_DROPPED`); required emitter geometry is never silently dropped;
6. ranks textures and halves the largest until the byte budget is met
   (`TEXTURE_DOWNSCALED`). Geometry and authored lighting bytes are reserved first.
   Untextured slots get a 4×4 sRGB swatch.

A scene rejects when distinct emitter groups cannot fit the six mesh slots,
required emitter geometry exceeds capacity, lighting exceeds 128 analytic lights,
or authored energy cannot fit the finite profile. It does not merge all emitters
into the brightest light or clamp their energy to make the import succeed.

pbrt area lights are *required* emitters. glTF emissive materials and MTL `Ke`
are *decorative*: their units are not photometric, so when emitter slots run
out (keeping one slot for ordinary surfaces) the smallest decorative emitter
groups, or decorative emission beyond the radiance profile, are cooked as
ordinary surfaces with `EMISSION_DROPPED` instead of failing the import.

The output is deterministic: the same input yields the same bundle and
report bytes. Paths in the report are relative to the entry directory.

## Importing in the Editor

World **Import scene…** (and the Level surface's **Import**) accepts `.pbrt`,
`.gltf`, `.glb` and `.obj` files (the browser editor also accepts a scene folder
or `.zip` containing one). glTF and OBJ entries are routed to their best-effort
importers by content. For pbrt, Ludus::SceneCook picks the importer from the
file name:

- `scene-v4.pbrt` is cooked with `pbrt-reference-v1` first, so staircase2 keeps
  its byte-identical reference path. Only when the reference profile rejects
  the scene does the import fall back to `pbrt-best-effort-v1`; the preview
  says why.
- Any other `.pbrt` file uses `pbrt-best-effort-v1`.
- A source directory (the older flow and its tests) stays on the reference
  profile with no fallback.

Choosing a file first runs a dry-run preview on the scene worker thread, with
progress and **Cancel scene operation**. The preview panel lists source and
cooked counts, budget use against the runtime limits and every warning-level
fallback. Nothing is written until **Import previewed scene**; **Discard
preview** drops it.

A best-effort import stores its full report in the scene manifest, beside the
input digests that guard against a source changing mid-import. Opening or
reassembling a World returns these reports as `import_reports`, and the
**Import report** toggle beside the World path opens them in the scrollable
inspector. Reports start collapsed so their length does not squeeze the viewport
or hierarchy controls.
Reference imports have no such report; their approximations still appear as
warnings.

## Tests

`ludus_best_effort_import_tests` writes a synthetic fixture from C++ that
covers nested, cyclic and escaping includes, every shape and image format,
fallback materials, lights, media and instancing. It checks diagnostics,
runtime decoding, determinism, merging, budget drops and dry runs.

`ludus_scene_import_tests` covers the glTF, OBJ, image and tangent paths with
checked-in CC0 fixtures under `modules/content/scene_cook/tests/fixtures`:
an OBJ/MTL room (quads, negative indices, smoothing groups, Phong and PBR
materials, an emitter, a normal map), an instanced glTF scene with an external
buffer and the same scene as a self-contained GLB (whose cooked bundles must be
byte-identical), a data-URI JPEG asset, and image fixtures written by
independent encoders (Pillow JPEG, the OpenEXR library, Ward RGBE). The
generators `generate_scene_fixtures.py` and `generate_image_fixtures.py` beside
them document provenance; the build does not run them. Malformed, truncated,
out-of-range, unsupported, escaping-path, cancellation and budget cases are
generated in memory. `ludus_scene_assets_tests` imports the glTF, GLB and OBJ
fixtures through the editor's preview, import, commit and assemble path.

Configure with `-DLUDUS_BUILD_SCENE_COOK_FUZZERS=ON` (native Clang) to build
`ludus_scene_cook_pbrt_fuzzer`, a libFuzzer target over the pbrt reader, PLY
reader and image decoders, and `ludus_scene_cook_import_fuzzer` over the glTF
and OBJ readers, the OpenEXR/Radiance/JPEG decoders and tangent generation.
Seed it with the fixture directory.

### Local smoke test with licensed scenes

Licensed scenes are never committed. To import them locally:

```sh
LUDUS_PBRT_SCENES_DIR=~/Ludus/references/resources/scenes \
  ctest --test-dir out/build/linux-clang-development -R best_effort --output-on-failure
LUDUS_PBRT_SCENES_DIR=... LUDUS_PBRT_SMOKE_OUTPUT=/tmp/staircase \
  out/build/.../ludus_best_effort_import_tests "[licensed]"
```

The test skips when `staircase/scene-v4.pbrt` is missing under the directory.

For glTF and OBJ collections (for example the Khronos glTF sample assets or the
McGuire Computer Graphics Archive), set `LUDUS_GLTF_SCENES_DIR` and/or
`LUDUS_OBJ_SCENES_DIR`; every `.gltf`, `.glb` and `.obj` below them must import
or fail with a report that explains why:

```sh
LUDUS_GLTF_SCENES_DIR=~/glTF-Sample-Assets/Models LUDUS_OBJ_SCENES_DIR=~/mcguire \
  out/build/linux-clang-development/modules/content/scene_cook/ludus_scene_import_tests "[licensed]"
```

## Third-party code

| Dependency | Use | Pin and licence |
| --- | --- | --- |
| [stb_image](https://github.com/nothings/stb) 2.30 (Sean Barrett) | PNG, JPEG, Radiance decoding and the zlib inflater used by the EXR reader | `third_party/stb`, SHA-256 checked, MIT/public domain |
| [MikkTSpace](https://github.com/mmikk/MikkTSpace) (Morten S. Mikkelsen) | tangent generation | `third_party/mikktspace`, SHA-256 checked, zlib |

Both are C, exception-free and private to `Ludus::SceneCook`. OpenEXR is read by
an original subset reader instead of the OpenEXR library, which needs C++
exceptions and Imath and supports far more than environment maps require;
the subset's unsupported compressions report `Unsupported` explicitly.
