# Hair and skin qualification in a standalone game (#1400)

A standalone application built against the **installed Ludus SDK** that cooks,
packages, extracts and launches the saved character-bust World and renders its
hair cards and real-time skin, so the clean package's output can be compared
with the Editor on the same fixture. It depends on both raster features of
[hair and skin rendering](../../../docs/architecture/renderer-systems.md#hair-and-skin-rendering)
and extends the saved-World pattern of [`raster_baseline`](../raster_baseline).

> **Target:** macOS arm64 / Metal. **Authored, not run** in the Kiro web
> sandbox (Amazon Linux 2023: no Clang 23, no CMake 3.29, no Slang, no Qt6, no
> GPU/Metal). The native build, package, extract, launch and the Editor-vs-app
> visual comparison are deferred to the macOS Metal lane. `verify.py` is the
> repeatable sequence Codex runs there; see
> [`docs/development/hair-skin-standalone-qualification.md`](../../../docs/development/hair-skin-standalone-qualification.md).

## Two raster profiles, one World

No single raster profile shades both features. Hair cards need authored strand
inputs, carried only by the **forward lit** profile; real-time skin diffusion is
implemented only by the **clustered deferred** profile (the engine's default
graph). `verify.py` therefore qualifies the one saved World through each profile
and inspects the feature that profile owns. The fixture refuses a profile that
cannot shade its requested feature with an actionable diagnostic before
packaging (`--check-profile`); see the negative check below.

## Layout

| File | Role |
| --- | --- |
| `authoring/bust.world.json` | The saved World: the imported bust meshes, the `Skin` and `Hair` slot material assignments and edited values, the matched camera and the key/fill lighting. |
| `authoring/bust.scene.json` | The public scene-source companions: the `skin` and `hair_card` materials with their edited values, used for the cook round-trip and the flat hair-card patch control. |
| `authoring/world.schema.json` | The World document schema the consumer validates the saved World against. |
| `capture.cpp` | The standalone installed-SDK app: cooks the saved World, assigns hair and skin, renders a matched capture through the selected profile and writes `capture.png` and `capture.json`. |
| `CMakeLists.txt`, `CMakePresets.json` | `find_package(Ludus CONFIG REQUIRED …)`, the built-in shader recipes for each profile, the cooked hair opacity texture, and the install rules used by packaging. |
| `verify.py` | The reproducible driver: build, package, extract to a clean directory, launch, record provenance, compare with the Editor and run the negative checks. |

The bust glTF, its colour/normal maps and the RGBA hair strand texture are the
committed fixtures under
[`tests/scenes/characters/infinite-head`](../../scenes/characters/infinite-head);
`verify.py` stages them beside the app so the clean extraction carries no
engine-source or Editor paths.
