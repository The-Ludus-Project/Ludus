# Qualify hair and skin in a standalone game

This guide qualifies the engine's raster **hair cards** and **real-time skin
diffusion** in a standalone game built against the installed Ludus SDK, using
ONE real, cooked character-bust World. The standalone app draws the bust's
meshes, material slots, the hair opacity texture and the skin inputs through the
packaged render graphs, and its output is compared with the Editor on the same
fixture. It implements [Installed SDK #1400](https://github.com/The-Ludus-Project/Ludus/issues/1400)
and reuses the [independent project and SDK workflow](project-sdk-workflow.md) and
[release packaging](game-packaging-publishing.md).

The fixture is [`tests/sdk_consumer/hair_skin_bust`](https://github.com/The-Ludus-Project/Ludus/tree/develop/tests/sdk_consumer/hair_skin_bust);
its `verify.py` is the reproducible driver. Every batch runs it as the
`hair-skin-bust` check of the **macOS native backends and diagnostics** job
([`scripts/ci/macos_native.py`](https://github.com/The-Ludus-Project/Ludus/tree/develop/scripts/ci/macos_native.py);
see [batch job check plans](continuous-integration.md#batch-job-check-plans)).

## Target and raster profile

The declared target is **macOS arm64 / Metal** with a Development SDK. The bust
World depends on both raster features of
[hair and skin rendering](../architecture/renderer-systems.md#hair-and-skin-rendering),
and **no single raster profile shades both**:

- **Hair cards** need the authored strand direction, carried only by the
  [forward-lit profile](../architecture/renderer-systems.md#raster-hair-cards).
- **Real-time skin diffusion** is implemented only by the
  [clustered-deferred profile](../architecture/renderer-systems.md#raster-skin-diffusion),
  the engine's default render graph.

So the CMake build cooks **both** render-graph templates to
`forward_lit.lurgrt` and `clustered_deferred.lurgrt`, and the app draws the one
saved World through each: forward-lit for hair, clustered-deferred for skin. It
refuses an incompatible pairing before rendering (see [Negative checks](#negative-checks)).

## One real Editor-saved World

The Editor is the source of truth. The public `ludus-cook scene import` CLI cannot
import a glTF with hair/skin slot overrides — with no entry it falls back to the
pbrt reference profile — so the bust is cooked through the Editor. The hidden
Editor test *"The character bust World exports its cooked scene and matched
captures"* (`apps/editor/tests/render_graph_preview_tests.cpp`, tag
`[.sdk-hair-skin-bust-export]`) runs once:

1. imports `infinite-head-hair.gltf` (both the `Skin` and `Hair` slots, under a
   key and fill light);
2. assigns the skin and hair-card materials with Make Skin / Make Hair Card;
3. stores the lit face as the World's start view through the View menu (Front,
   back to Perspective, then **Set start view from this view**); the imported
   start view looks at the bust's back, away from the key and fill lights;
4. **saves one World** (`bust.world.json`);
5. **exports its cooked `scene.luscn` once**, with a manifest, through
   `ExportCookedWorldScene` (`apps/editor/src/playable_world.cpp`) — the same
   `RenderScene::Load → AssembleImports → RenderScene::Merge` path as Play, without
   Play's spawn/entity gating — and keeps the composed scene it wrote;
6. renders that **one exported composition** through both packaged profiles with
   the Editor's offscreen raster capture (`ViewportRasterCapture`, which the
   Reference comparison uses), on a fresh viewport device at the saved camera and
   the fixed 1600×1080 extent: the forward-lit package for hair, the default
   clustered-deferred package for skin. The capture is not the live viewport, so
   the viewport's drawable extent and navigation cannot replace its extent or
   camera.

`verify.py` sets `LUDUS_BUST_EXPORT_DIRECTORY`; the test writes `bust.world.json`,
`scene.luscn`, `scene.manifest.json`, `editor-hair.png` and `editor-skin.png`
there. The manifest adds the saved World file's digest (`savedWorldSha256`), the
capture view (`captureView`: extent, planes, depth convention) and each capture's
package and package digest. The SDK app then loads the **same** `scene.luscn` through
`Ludus::Resources` (`SceneDefinition::Decode`) and builds the render-graph scene
from its real meshes, per-object material slots, the alpha-masked hair opacity
texture (`SceneMaterial::Texture` with `IsAlphaMasked`/`AlphaCutoff`) and the skin
scattering inputs (`SceneMaterial::Scattering.K`). The export manifest records the
World, cooked-scene and graph SHA-256 hashes and the saved camera, so the driver
proves both hosts consume the identical cooked scene.

## No Qt in the packaged app

The app links **no Qt**. It reads files through `Ludus::Content`, renders through
`Ludus::GraphicsRenderer`, and writes the frame back as raw top-left RGBA8
(`capture.rgba`) through `Ludus::Content`; `verify.py` converts it to PNG with
Pillow and compares. So the extracted package has no Homebrew-Qt runtime
dependency, which `verify.py` asserts with `otool -L` (macOS) or `ldd` (Linux)
over the installed executable.

## Build, package, extract and launch

From a trusted Ludus checkout with the host tools prepared
([macOS creation and setup](project-sdk-workflow.md#macos-creation-and-setup)),
with an installed Development SDK:

```bash
out/host-tools/venv/bin/python -m pip install numpy Pillow
out/host-tools/venv/bin/python tests/sdk_consumer/hair_skin_bust/verify.py \
    --tools "$PWD" \
    --sdk   out/install/macos-clang-development \
    --editor out/build/macos-clang-development/apps/editor/ludus_editor_tests \
    --evidence out/hair-skin-bust
```

`verify.py` relocates the SDK to a fresh prefix with its bundled dependency
closure, runs the Editor export to produce the saved World's `scene.luscn` and
the matched captures, builds the Qt-free app (cooking both graphs), installs it to
a clean prefix (`bin/hair_skin_capture` with its runtime in `lib/`), stages the
Editor-exported `scene.luscn` and both `.lurgrt` beside it, and launches each
profile:

```bash
( cd "$package" && ./bin/hair_skin_capture --profile forward-lit )        # hair, forward_lit.lurgrt
( cd "$package" && ./bin/hair_skin_capture --profile clustered-deferred ) # skin, clustered_deferred.lurgrt
```

Run `verify.py --plan` to print the exact ordered sequence without executing it.

## Recorded provenance

`verify.py` writes `provenance.json` and the app writes `capture.txt`, recording
the inputs an acceptance claim needs: the **commit** (`git rev-parse HEAD`), the
**SDK version** and source revision (from the installed
`share/Ludus/LudusSdkManifest.json`), the **target** triple, the **renderer
profile** per feature, the **World SHA-256** and **cooked-scene SHA-256** (from the
Editor export manifest, re-checked against the staged `scene.luscn`), the two
**graph SHA-256** hashes, the saved **camera**, and the material-slot summary. SDK
identity is read from the manifest, never guessed.

## Packaging survival

Cooking and packaging must preserve the authored content. The run verifies, after
extraction, that the cooked `scene.luscn` the app loads carries:

- the `Skin` and `Hair` material **slot assignments** and their edited values —
  the skin base colour, roughness and the RGB scattering distances, and the hair
  strand direction, highlight shifts and weights (`SceneMaterial::Scattering`);
- the hair **opacity texture** and its alpha mask and cutoff
  (`SceneMaterial::Texture`, `IsAlphaMasked`, `AlphaCutoff`);
- the bust **meshes** and per-object material slots, drawn through the packaged
  graph's **shader dependencies** (`forward_lit.lurgrt` / `clustered_deferred.lurgrt`).

The app reports how many objects carry the feature's typed material; a lost slot
or value makes `--check-profile` or a launch fail before any comparison.

## Matched camera and lighting, and where to inspect

The app and the Editor share one camera, lighting, extent and display encoding.
The saved World carries the bust's warm key and cool fill directional lights
(imported from `infinite-head-hair.gltf`) and the saved front start view the
cooked scene stores. Both hosts build the same finite reverse-Z view from that camera: a
look-at from the scene's eye, target and up, its vertical field of view, 2 cm and
2 m planes, at 1600×1080. The app records the extent and planes in `capture.txt`,
and `verify.py` rejects a launch whose view differs from the manifest's
`captureView`. Both hosts translate the cooked scene into render-graph inputs as
the packaged first-person player does: tangents, linear sampling and every
material map, opaque draw coverage with the hair card's alpha mask on its
material, and the enabled lights. Both produce top-left opaque RGBA8 (the
Editor's `QImage::Format_RGBA8888`; the app's raw `capture.rgba`, read back in
row bands that fit the device's transfer budget). `verify.py` compares them with
`scripts/compare-render`.

Inspect each feature in the capture:

- **Hair-card mask.** The masked `Hair` slot discards below its alpha cutoff in
  the colour, depth and shadow passes. Inspect the ribbon silhouette where the
  strand texture's alpha crosses the cutoff; coverage must match between the
  Editor and the package.
- **Directional hair highlights.** The Kajiya–Kay highlight runs along the
  authored strand. Inspect the bright band across the ribbons.
- **Skin diffusion.** The clustered-deferred profile spreads the skin's diffuse
  lighting over the authored millimetre scattering distances, red reaching
  furthest. Inspect the softened terminator while the specular highlight stays
  sharp.

## Negative checks

Project preparation and packaging must name a problem and tell the author how to
fix it. The fixture checks:

- **No Homebrew Qt.** The extracted app's resolved dependencies must contain no
  Qt framework or Homebrew path.
- **Runtime-dependency removal.** Removing a real bundled runtime library from
  `lib/` makes a launch fail rather than crash silently; the failure is recorded.
- **Incompatible renderer profile.** `./bin/hair_skin_capture --check-profile
  hair clustered-deferred` (and `skin forward-lit`) exits with a distinct code and
  names the compatible profile.
- **Missing required dependency.** With the cooked `scene.luscn` removed, a launch
  exits with its own code and names the missing file.

## Status and acceptance

`verify.py` passed end to end on macOS arm64 / Metal (Apple M5) on 2026-11-27, at
commit `6de09ba51` with an SDK installed from the `macos-clang-development`
(Development) build of `eead4ece9` (SDK version 0.1.0, target
`arm64-apple-darwin`):

| Feature | Profile | SDK graph SHA-256 | Editor package | RMSE (limit 0.05) |
| --- | --- | --- | --- | --- |
| Hair cards | `forward-lit` | `784681d4…40a00bc` | `editor_forward_lit.lurgrt` | 0.0 |
| Skin | `clustered-deferred` | `b3e59d70…ed42a86a` | `editor_default_graph.lurgrt` | 0.0 |

- The app and the Editor drew the same exported `scene.luscn`: the manifest's
  `sceneSha256` matched the staged scene, the saved World matched
  `savedWorldSha256`, and both captures used the 1600×1080 reverse-Z view with
  2 cm and 2 m planes. Each run imports into a fresh project, so the World and
  scene hashes differ between runs; `provenance.json` records each run's values.
- The raw app frames equal the Editor captures in every channel. The hair-card
  ribbons keep their masked silhouette and the strand highlight across the
  crown; the clustered-deferred skin shows the softened terminator.
- `--check-profile hair clustered-deferred` and `skin forward-lit` exit 3 and
  name the compatible profile; a launch without `scene.luscn` exits 4 and names
  the file; removing `libOpenImageDenoise.2.dylib` from `lib/` stops the launch
  in the loader. The extracted app resolves no Qt or Homebrew library.

This table records that local run. CI now produces the same evidence: the
`hair-skin-bust` check of the macOS native batch job runs `verify.py` after the
`sdk-install` check, against `out/install/macos-clang-development` and the
job's `ludus_editor_tests`, in its own virtual environment with pinned NumPy
and Pillow. A failure fails the batch, so a promoted `main` has passed it. The
job uploads `out/hair-skin-bust/` (the Editor and SDK captures, the comparison
pages, `provenance.json`, the negative-check results and every log) as the
`hair-skin-bust-metal` artifact, kept for 14 days. To read a batch's evidence,
open the batch run's **macOS native backends and diagnostics** job: the job
summary lists the `hair-skin-bust` result, its log group holds the driver
output, and the artifact holds the files.

Run the check alone on a Mac with the build and SDK install in place:

```bash
out/host-tools/venv/bin/python scripts/python/ci_checks.py scripts/ci/macos_native.py --only hair-skin-bust
```
