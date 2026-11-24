# Hair and skin qualification in a standalone game (#1400)

A standalone application built against the **installed Ludus SDK** that draws ONE
real, cooked character-bust World — its meshes, material slots, the hair opacity
texture and the skin inputs — through the packaged **forward-lit** graph (hair
cards) and the packaged **clustered-deferred** graph (real-time skin), so the
clean package's output can be compared with the Editor on the same fixture. It
depends on both raster features of
[hair and skin rendering](../../../docs/architecture/renderer-systems.md#hair-and-skin-rendering).

> **Target:** macOS arm64 / Metal. **Authored, not run** in the Kiro web sandbox
> (Amazon Linux 2023: no Clang 23, no CMake 3.29, no Slang, no Metal GPU). The
> native cook, build, launch and the Editor-vs-app visual comparison are deferred
> to the macOS Metal lane. `verify.py` is the repeatable sequence; see
> [`docs/development/hair-skin-standalone-qualification.md`](../../../docs/development/hair-skin-standalone-qualification.md).

## One real Editor-saved World

The Editor is the source of truth. The hidden Editor test *"The character bust
World exports its cooked scene and a matched capture"*
([`apps/editor/tests/render_graph_preview_tests.cpp`](../../../apps/editor/tests/render_graph_preview_tests.cpp),
tag `[.sdk-hair-skin-bust-export]`) imports `infinite-head-hair.gltf` (which
carries both the `Skin` and `Hair` slots), assigns the skin and hair-card
materials with Make Skin / Make Hair Card, **saves one World** (`bust.world.json`),
and **exports its cooked `scene.luscn`** plus a manifest (`ExportCookedWorldScene`
in `apps/editor/src/playable_world.cpp`). The public `ludus-cook scene` CLI cannot
import a glTF with slot overrides (it defaults to the pbrt reference profile), so
this editor export — the same `RenderScene::Load → AssembleImports → Merge` path as
Play, without Play's spawn gating — is the only faithful way to cook the bust.

The SDK app loads that **same** `scene.luscn` through `Ludus::Resources`
(`SceneDefinition::Decode`) and draws its real meshes, per-object material slots,
the alpha-masked hair opacity texture and the skin scattering inputs. The manifest
records the World, cooked-scene and graph SHA-256 hashes and the saved camera, so
`verify.py` proves both hosts consume the identical cooked scene.

## No Qt

The app links **no Qt**: it reads files through `Ludus::Content`, renders through
`Ludus::GraphicsRenderer`, and writes the raw RGBA capture back through
`Ludus::Content`. `verify.py` converts the raw RGBA to PNG (Pillow) and compares.
So the extracted package has no Homebrew-Qt runtime dependency, which the driver
asserts with `otool -L` / `ldd`.

## Two packaged graphs

No single raster profile shades both features: hair cards need the forward-lit
profile's authored strand inputs, and real-time skin diffusion is implemented
only by the clustered-deferred default profile. The CMake build cooks **both**
templates to `forward_lit.lurgrt` and `clustered_deferred.lurgrt`; the app selects
the matching one per capture and refuses an incompatible pairing before rendering.

## Layout

| File | Role |
| --- | --- |
| `capture.cpp` | The Qt-free installed-SDK app: loads the Editor-exported `scene.luscn`, builds the render-graph scene from its meshes/slots/textures/lights, renders through the selected packaged graph and writes `capture.rgba` + `capture.txt`. |
| `CMakeLists.txt`, `CMakePresets.json` | `find_package(Ludus CONFIG REQUIRED …)` (no Qt), the built-in shader recipes for both profiles, cooking both render-graph templates, and the relocatable install rules. |
| `verify.py` | The reproducible driver: run the Editor export, build the app, stage a clean extraction, launch each profile, assert no Qt/checkout dependency, test required-input removal, run the incompatible-profile checks and compare with the Editor. |

The one saved World is produced by the Editor from the committed bust fixtures
under [`tests/scenes/characters/infinite-head`](../../scenes/characters/infinite-head);
`verify.py` stages only the Editor-exported `scene.luscn` and the two cooked
graphs into the clean extraction, so it carries no engine-source or Editor paths.
