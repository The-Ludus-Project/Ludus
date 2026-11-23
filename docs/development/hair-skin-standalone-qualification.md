# Qualify hair and skin in a standalone game

This guide qualifies the engine's raster **hair cards** and **real-time skin
diffusion** in a standalone game built against the installed Ludus SDK, using the
static character bust and its saved World. It cooks and packages the same World
the Editor authors, launches it from a clean extraction with no engine-source or
Editor paths, and compares the clean package's output with the Editor on the same
fixture. It implements [Installed SDK #1400](https://github.com/The-Ludus-Project/Ludus/issues/1400)
and reuses the [independent project and SDK workflow](project-sdk-workflow.md) and
[release packaging](game-packaging-publishing.md).

The fixture is [`tests/sdk_consumer/hair_skin_bust`](https://github.com/The-Ludus-Project/Ludus/tree/develop/tests/sdk_consumer/hair_skin_bust).
Its `verify.py` is the reproducible driver; the macOS Metal CI step is
[`.github/actions/hair-skin-bust`](https://github.com/The-Ludus-Project/Ludus/tree/develop/.github/actions/hair-skin-bust).

## Target and raster profile

The declared target is **macOS arm64 / Metal** with a Development SDK. The
authored bust World depends on both raster features of
[hair and skin rendering](../architecture/renderer-systems.md#hair-and-skin-rendering),
and **no single raster profile shades both**:

- **Hair cards** need the authored strand direction, carried only by the
  [forward lit profile](../architecture/renderer-systems.md#raster-hair-cards).
  The clustered deferred profile packs the facing normal and shades a hair card
  with ordinary lobes.
- **Real-time skin diffusion** is implemented only by the
  [clustered deferred profile](../architecture/renderer-systems.md#raster-skin-diffusion),
  the engine's default render graph. Forward profiles shade skin as its
  metallic/roughness surface.

So the qualification renders the one saved World through **each** profile and
inspects the feature that profile owns. Before packaging, the app verifies that
the selected profile can shade the requested feature and refuses an incompatible
pairing with an actionable diagnostic (see [Negative checks](#negative-checks)).

## Build, package, extract and launch

From a trusted Ludus checkout with the host tools prepared
([macOS creation and setup](project-sdk-workflow.md#macos-creation-and-setup)),
with an installed Development SDK at `out/install/macos-clang-development`:

```bash
# 1. Relocate the installed SDK to a fresh prefix with its bundled dependency
#    closure, so the game links no engine-source or checkout path.
tools="$PWD"; scratch="$(mktemp -d)"
out/host-tools/venv/bin/cmake --install out/build/macos-clang-development --prefix "$scratch/sdk"
out/host-tools/venv/bin/python scripts/python/engine.py bundle-sdk-dependencies \
    macos-clang-development --prefix "$scratch/sdk"
mv "$scratch/sdk" "$scratch/relocated-sdk"

# 2. Configure and build the standalone game against ONLY the relocated SDK.
cp -R tests/sdk_consumer/hair_skin_bust "$scratch/consumer"
out/host-tools/venv/bin/cmake -S "$scratch/consumer" -B "$scratch/build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$scratch/relocated-sdk" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_COMPILER="$tools/out/host-tools/bin/clang++" \
    -DCMAKE_MAKE_PROGRAM="$tools/out/host-tools/venv/bin/ninja" \
    -DLUDUS_SLANG_COMPILER="$tools/out/shader-tools/slang/bin/slangc" \
    -DLUDUS_SPIRV_VALIDATOR="$tools/out/shader-tools/spirv-tools/usr/bin/spirv-val"
out/host-tools/venv/bin/cmake --build "$scratch/build"

# 3. Install (package) the game to a clean prefix and extract it there.
out/host-tools/venv/bin/cmake --install "$scratch/build" --prefix "$scratch/package"

# 4. Launch each profile from the clean extraction.
( cd "$scratch/package" && ./hair_skin_capture --profile forward-lit )        # hair cards
( cd "$scratch/package" && ./hair_skin_capture --profile clustered-deferred ) # skin diffusion
```

The one-command equivalent runs every step above, stages the saved World and the
bust fixtures beside the app, records provenance, drives the Editor capture and
compares them:

```bash
out/host-tools/venv/bin/python tests/sdk_consumer/hair_skin_bust/verify.py \
    --tools "$PWD" --sdk out/install/macos-clang-development \
    --editor out/build/macos-clang-development/apps/editor/ludus_editor_tests \
    --graph  out/build/macos-clang-development/apps/editor/ludus-graphs/.../editor_renderer_static.lurgrt \
    --evidence out/hair-skin-bust
```

Run `verify.py --plan` to print the exact ordered sequence without executing it.

## Recorded provenance

Each launch writes `capture.json`, and `verify.py` writes `provenance.json`,
recording the inputs an acceptance claim needs: the **commit** (`git rev-parse
HEAD` of the tools checkout), the **SDK version** and source revision (from the
installed `share/Ludus/LudusSdkManifest.json`), the **target** triple, the
**renderer profile** per feature, and the saved **World revision** and its
SHA-256. The SDK version and target are read from the SDK manifest, never guessed
from a version string.

## Packaging survival

Cooking and packaging the saved World must preserve the authored content. The
driver verifies, after extraction, that:

- the `Skin` and `Hair` material **slot assignments** and their edited values
  survive — the skin base colour, roughness and the RGB scattering distances, and
  the hair base colour, roughness, authored strand direction and highlight shifts
  and weights (the app reads them from `authoring/bust.scene.json` and builds the
  typed materials through `MakeHairCardScattering` and the skin scattering record,
  so a lost value fails material validation);
- the hair **opacity texture** (`infinite-head/hair-cards.png`) and the bust's
  colour and normal maps are present in the package beside the app;
- the cooked **render-graph shader dependencies** (`graph.lurgrt`) decode and
  activate — a package cooked before the hair-card (LUSCN v13) or skin (v14)
  material words would be rejected and asked to recook.

## Matched camera and lighting, and where to inspect

The app and the Editor share one camera and lighting setup. The saved World
(`authoring/bust.world.json`) fixes the capture camera (eye, target, up, vertical
field of view) and the two directional lights — a warm key and a cool fill — plus
the constant environment irradiance. The Editor renders the same saved World
through its `[skin][hair-card]` acceptance test
([`render_graph_preview_tests.cpp`](https://github.com/The-Ludus-Project/Ludus/blob/develop/apps/editor/tests/render_graph_preview_tests.cpp)),
whose `LUDUS_EDITOR_WORLD_CAPTURE_HAIR` / `LUDUS_EDITOR_WORLD_CAPTURE_SKIN` hook
saves the matched viewport image. `verify.py` compares the two with
`scripts/compare-render`.

Inspect each feature in the capture:

- **Hair-card mask.** The masked `Hair` slot discards below its 0.5 alpha cutoff
  in the colour, depth and shadow passes. Inspect the ribbon silhouette where the
  strand texture's alpha crosses the cutoff; the coverage must match between the
  Editor and the package, and between the colour and depth passes.
- **Directional hair highlights.** The Kajiya–Kay highlight runs along the
  authored strand (90° from the card tangent, up the ribbon). Inspect the bright
  band across the ribbons; rotating the authored strand rotates the band, and the
  ordinary metallic/roughness control card keeps an isotropic highlight.
- **Skin diffusion.** The clustered deferred profile spreads the skin's diffuse
  lighting over the authored millimetre scattering distances, with red reaching
  furthest. Inspect the softened terminator and the light bleeding a short
  distance across albedo edges, while the specular highlight on the nose and brow
  stays sharp; the matched PBR control shows the unspread terminator.

## Negative checks

Project preparation and packaging must name a problem and tell the author how to
fix it. The fixture checks two cases:

- **Incompatible renderer profile.** `./hair_skin_capture --check-profile hair
  clustered-deferred` (and `skin forward-lit`) runs before any render, exits with
  a distinct code and prints which feature the selected profile cannot shade and
  the profile to select instead. `verify.py` asserts the exit code and that the
  diagnostic names the compatible profile.
- **Missing required dependency.** With the authored scene source removed, a
  launch exits with its own code and names the missing file rather than crashing
  or rendering a wrong image. `verify.py` asserts both.

## Status and acceptance

The implementation is **authored, not run** in the Kiro web sandbox, which lacks
the pinned Clang 23 toolchain, Slang, a macOS host and a Metal GPU. The native
build, package, launch and the Editor-vs-app visual comparison are deferred to the
macOS Metal lane; `verify.py` is the repeatable sequence to run there, and its
captures, provenance and comparison HTML are the acceptance evidence for #1400.
