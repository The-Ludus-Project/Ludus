# Building the browser editor

The Pages application compiles `apps/editor` with Qt Widgets for WebAssembly.
The UI, project/configuration models and serializers are shared with the desktop
editor. Browser file import/download and explicitly unavailable process/audio
adapters are private implementations selected by the editor target. The game
viewport remains a separate graphics concern; this port does not replace Qt
with the engine's unfinished UI toolkit.

Qt can remain the common editor shell for Linux, macOS and the browser. macOS
editor setup, launching and native validation remain separate work, as does
its Metal game viewport. Platform-specific shells would duplicate commands and
workspace behavior without solving the renderer/backend gap.

## Reproduce the Pages payload

Use Python 3.12 or newer on Linux x64, with Clang/LLD 18 installed:

```sh
python scripts/build-web-editor --bootstrap
python scripts/build-web-editor --check-package
python -m unittest discover -s scripts/python -p test_editor_web.py -v
```

The bootstrap installs the pinned CMake/Ninja, Emscripten SDK, official Qt host
tools and verified Qt source archive into ignored `out/` directories. Qt's pin
is in `config/editor_web_toolchain.json`; the compiler/WebGPU pins remain in
`config/web_toolchain.json`. Qt is built from source with the engine SDK because
prebuilt WebAssembly Qt requires its matching Emscripten version. A source build
with Ludus's newer pinned SDK needs validation whenever either pin changes.

The same build also prepares the pinned Luau compiler/analyzer and shader tools,
then builds Cornell Box, Live Edit Game and Scripted Game as standalone static
web players. These compile the existing sample C++ implementations; Scripted
Game cooks its actual Luau package on the build host and links the Wasm provider.
Cornell Box yields asynchronous device/pipeline creation through Asyncify. The
players use the public browser RHI with WebGPU/WebGL 2 fallback and independent
canvases; closing their iframe releases the entire player runtime. Native
GameHost processes and dynamic module loading are not needed for web playback.

Player assets under `players/<sample>/` are required and hash-checked alongside
the editor payload. Every Wasm module uses the bounded single-threaded memory
policy. The package includes Luau and WebGPU notices. A project opts into a
shipped player with `ludus.web.json` (`version: 1`, `player` equal to a shipped
sample identifier). Arbitrary URLs and unknown identifiers are rejected.

This static Qt build uses no C++ exceptions, one thread and Asyncify for the
existing modal dialogs. It needs no cross-origin isolation headers. The editor
starts with 32 MiB of WebAssembly memory and may grow to 2 GiB (`QT_WASM_MAXIMUM_MEMORY`), enough to cook and
preview photographic scenes in process. Do not silently
switch it to shared memory: Pages cannot supply arbitrary response headers.

An existing host Qt installation of the pinned version can be supplied with
`--qt-host /absolute/path/to/Qt/6.10.3/gcc_64` (or `macos`). To rebuild modified
Qt sources in `out/editor-tools/src/qtbase`, use `--rebuild-qt`. The source and
rebuild scripts are available to users of the statically linked LGPL Qt build;
license texts and bundled dependency notices accompany the payload.

The bounded import wrapper uses Qt 6.10.3's private WebAssembly event callbacks
and file reader behind one private `.cpp` boundary. Its admission callback checks size
before reading, and Qt owns the suspend/resume dispatch. This is an intentional
version coupling: editor upgrades must verify this adapter in a browser. Browser
downloads use a separate JavaScript bridge with copied bytes and explicit names.

Static analysis still uses Clang 23. `web_build.analysis_commands` enables
`-frelaxed-template-template-args` for the standard template-matching rule that
is already the default in the newer SDK compiler; this is required to parse
Emscripten's `val.h`. The language mode remains C++23 and diagnostics remain errors.

## Scene import and viewport

World authoring works on the session's in-memory (MEMFS) project. **Import
scene…** offers a scene folder, or one `.zip` (stored or deflated entries, no
ZIP64) or a `.pbrt` with its companion files. The selection is copied into
MEMFS, then the linked SceneCook importer cooks it to LUSCN in process, exactly
as on the desktop; MEMFS has no `flock`, so the cook lock is skipped there
(one thread owns the file system). MEMFS also has no hard links: immutable
artifacts use an existence check followed by rename without yielding. Existing
identical artifacts are reused, while conflicting contents reject publication.
These writes publish session memory and promise no disk durability. Native cooks
retain their atomic hard-link publication. The build is single-threaded, so the cook
runs on the UI thread after the status text has painted; large scenes block
input until the cook finishes.

The interactive viewport renders imported scenes through the engine's GPU raster
renderer on a separate **WebGPU** canvas. Qt retains its own canvas for widgets. Scene meshes, textures and materials use the same GPU generation and
camera path as the native editor; the renderer tone-maps its display image and
composites it to the browser surface in one submission, without CPU pixel
readback or image upload. Orbit, pan, zoom, focus, shading and resize update the
current scene. Output is bounded to 1024 physical pixels on its longest side.

**Solid** is the neutral navigation visualization; **Albedo** shows unlit surface
colors; **Lit** uses admitted authored direct lights, emission and environment.
The browser profile does not submit shadow maps or run the native worker-based
static lighting bake. Emissive geometry does not imply real-time indirect lighting.
Pending resource preparation retains the last presented view; rejected candidates
report a failure without replacing it. A completed image is required before an
import is published.

WebGPU requires a supported browser/device and a secure context (HTTPS or
localhost). If device admission fails, the viewport explains that WebGPU is
unavailable and document editing remains usable; it does not silently rasterize
on the CPU. Device loss stops the GPU session and preserves documents. Reopen the
scene or restart the preview to request a fresh device. The separate canvas
follows the Qt viewport rectangle and hides with its tab, popups and modal dialogs;
mouse and keyboard navigation continue through Qt.

## Preview the complete website

Build MkDocs and the API reference first, then copy the editor payload; MkDocs
cleans its output directory on each build:

```sh
python -m mkdocs build --strict
python scripts/build-api --bootstrap
cp -R out/editor-pages/. out/wiki/editor/
python scripts/check-wiki
```

Serve the complete site beneath `/Ludus/` to match the production project path.
Test loading, editing, importing and downloaded JSON in an actual browser after
changing Qt, the SDK or the editor. Artifact checks cannot prove the application
runs. The [browser editor guide](../wiki/guides/browser-editor.md) describes the
supported workflows and session storage contract.

## Pages and caching

The wiki workflow validates the editor independently and downloads its artifact
only after MkDocs/API generation. The final editor cache key includes editor,
engine/dependency, CMake/toolchain, loader and HTML sources. Ordinary guide and
API prose updates reuse that payload. A second cache retains the source-built
Qt and SDK for editor source changes. Each cache hit still runs package
validation; only the main Pages deployment publishes the combined artifact.
