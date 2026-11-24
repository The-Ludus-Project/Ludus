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
its `verify.py` is the reproducible driver and the macOS Metal CI step is
[`.github/actions/hair-skin-bust`](https://github.com/The-Ludus-Project/Ludus/tree/develop/.github/actions/hair-skin-bust).

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

## One real cooked scene

The scene is cooked once with the **public `ludus-cook scene` pipeline** — the
same level schema the Editor's Make Hair Card / Make Skin produce:

```bash
cook="$PWD/out/build/macos-clang-development/modules/content/scene_cook/ludus-cook"
project="$scratch/bust-project"
"$cook" scene new    "$project" bust.lulevel --name "Character bust"
"$cook" scene import "$project" bust.lulevel "$scratch/bust-gltf/infinite-head-hair.gltf"
# verify.py injects authoring/bust-overrides.json onto the imported Hair and Skin
# objects here (the hair_card strand controls and cutoff, the skin scattering
# distance, and the matched camera), then:
"$cook" scene assemble "$project" bust.lulevel "$scratch/scene.luscn"
```

The app loads `scene.luscn` through `Ludus::Resources`
(`SceneDefinition::Decode`) and builds the render-graph scene from its real
meshes, per-object material slots, the alpha-masked hair opacity texture
(`SceneMaterial::Texture` with `IsAlphaMasked`/`AlphaCutoff`) and the skin
scattering inputs (`SceneMaterial::Scattering.K`). No editor internals are linked.

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
    --cook  out/build/macos-clang-development/modules/content/scene_cook/ludus-cook \
    --sdk   out/install/macos-clang-development \
    --editor out/build/macos-clang-development/apps/editor/ludus_editor_tests \
    --evidence out/hair-skin-bust
```

`verify.py` relocates the SDK to a fresh prefix with its bundled dependency
closure, cooks the scene, builds the Qt-free app (cooking both graphs), installs
it to a clean prefix (`bin/hair_skin_capture` with its runtime in `lib/`), stages
`scene.luscn` and both `.lurgrt` beside it, and launches each profile:

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
profile** per feature, the saved **World (level) revision** and the cooked
`scene.luscn` SHA-256, and the rendered scene counts (meshes, materials, objects,
lights, textures). SDK identity is read from the manifest, never guessed.

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

The app and the Editor share one camera and lighting setup. The cooked scene
carries the matched capture camera (from `authoring/bust-overrides.json`) and the
bust's warm key and cool fill directional lights (imported from
`infinite-head-hair.gltf`). The Editor renders the same saved World through its
`[skin][hair-card]` acceptance test
([`render_graph_preview_tests.cpp`](https://github.com/The-Ludus-Project/Ludus/blob/develop/apps/editor/tests/render_graph_preview_tests.cpp)),
whose `LUDUS_EDITOR_WORLD_CAPTURE_HAIR` / `LUDUS_EDITOR_WORLD_CAPTURE_SKIN` hook
saves the matched viewport. `verify.py` compares the two with
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

The implementation is **authored, not run** in the Kiro web sandbox, which lacks
the pinned Clang 23 toolchain, Slang, a macOS host and a Metal GPU. The native
cook, build, launch and the Editor-vs-app visual comparison are deferred to the
macOS Metal lane; `verify.py` is the repeatable sequence to run there, and its
captures, provenance and comparison HTML are the acceptance evidence for #1400.
