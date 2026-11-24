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

## One real cooked scene

`verify.py` cooks a single `scene.luscn` with the **public `ludus-cook scene`
pipeline** — the same level schema the Editor's Make Hair Card / Make Skin
produce: `scene new` → `scene import` the bust glTF → inject the authored Hair and
Skin slot overrides (`authoring/bust-overrides.json`) onto the imported objects →
`scene assemble`. The app loads that cooked scene through `Ludus::Resources`
(`SceneDefinition::Decode`) and draws its real meshes, per-object material slots,
the alpha-masked hair opacity texture and the skin scattering inputs. No editor
internals are linked.

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
| `authoring/bust-overrides.json` | The authored Hair/Skin slot overrides (strand controls, mask cutoff, skin scattering distance, base colour, roughness) and the matched camera, injected onto the imported bust level before cooking. |
| `capture.cpp` | The Qt-free installed-SDK app: loads `scene.luscn`, builds the render-graph scene from its meshes/slots/textures/lights, renders through the selected packaged graph and writes `capture.rgba` + `capture.txt`. |
| `CMakeLists.txt`, `CMakePresets.json` | `find_package(Ludus CONFIG REQUIRED …)` (no Qt), the built-in shader recipes for both profiles, cooking both render-graph templates, and the relocatable install rules. |
| `verify.py` | The reproducible driver: cook the scene, build, stage a clean extraction, launch each profile, assert no Qt dependency, test runtime-dependency removal, run the negative checks and compare with the Editor. |

The bust glTF, its colour/normal maps and the RGBA hair strand texture are the
committed fixtures under
[`tests/scenes/characters/infinite-head`](../../scenes/characters/infinite-head);
`verify.py` stages them into a scratch project so the cook and the clean
extraction carry no engine-source or Editor paths.
