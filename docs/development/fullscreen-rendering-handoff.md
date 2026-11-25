# Fullscreen SDK handoff for Ludus-Sandbox

## Patch dependency and ownership

Prompt 1 is merged as `9aeeade` (PR #48), with Slang 2026.1.2 verified by
ADR 0010. This public API/helper patch is additionally required; that revision
alone has no public shader resource API. Validation used the local patch on
`3ac97af8bb4e` (the later editor/audio documentation commits are unrelated).
Consume the SDK built from branch `codex/fullscreen-rendering-api` until this
patch is merged; an older installed SDK cannot provide this API. Rebuild downstream binaries for the new API.

Ludus owns resources, pipelines, uniform updates/binding, draw submission,
backend/lifecycle errors and the reusable shader build helper. Sandbox owns all
shader source, palette, wave formulas, tuning, scene state, camera choices and
UI/gameplay. No engine API or committed diagnostic shader contains ocean/game
settings. Link only public SDK targets; never include probe/internal headers or
borrow native/WebGPU objects. See [public usage/lifecycle](fullscreen-rendering.md)
and [ADR 0011](../decisions/0011-public-fullscreen-rendering.md).

## Pinned tools and build/install commands

Host tooling remains separate: native Clang/LLD/format/tidy 18, Emscripten 4.0.23
and its pinned WebGPU port, and Slang **2026.1.2** plus SPIRV-Tools
**2025.1~rc1-1~ubuntu0.24.04.2**. The latter are pinned in
`config/shader_toolchain.json` (Apache-2.0 WITH LLVM-exception and Apache-2.0).
The combined acquired host tools are supported on Linux x86_64 with glibc 2.38+;
other hosts still require separately pinned/validated packages. Archive digests,
licensing, acquisition and host limitations are in the
[toolchain handoff](shader-toolchain-handoff.md). The SDK helper checks exact
Slang version and the validator executable digest. Acquisition verifies archives.
No compiler/validator libraries are linked into the application, and users need
no Slang installation or runtime downloads.

From the Ludus root on the reference host:

```bash
./init.sh
./scripts/shader-probe bootstrap
./scripts/bootstrap web-emscripten-development
npm ci --prefix tools/web-browser-tests
tools/web-browser-tests/node_modules/.bin/playwright install chromium

export LUDUS_SHADER_COMPILER="$PWD/out/shader-tools/slang/bin/slangc"
export LUDUS_SHADER_VALIDATOR="$PWD/out/shader-tools/spirv-tools/usr/bin/spirv-val"
out/host-tools/venv/bin/cmake --preset linux-clang-development \
  -DLUDUS_SLANG_COMPILER="$LUDUS_SHADER_COMPILER" \
  -DLUDUS_SPIRV_VALIDATOR="$LUDUS_SHADER_VALIDATOR" \
  -DLUDUS_WARNINGS_AS_ERRORS=ON -DLUDUS_ENABLE_TIME_TRACE=OFF
./scripts/build linux-clang-development
./scripts/test linux-clang-development
out/host-tools/venv/bin/cmake --install out/build/linux-clang-development \
  --prefix "$PWD/out/install/render-api-native"

./scripts/build web-emscripten-development
./scripts/test web-emscripten-development
out/host-tools/venv/bin/cmake --install out/build/web-emscripten-development \
  --prefix "$PWD/out/install/render-api-web"
```

The separate prefixes preserve earlier SDK/assertion-policy variants. For
production SDK builds, `LUDUS_BUILD_TESTS=OFF` excludes the private readback oracle.
Acquisition is an explicit developer operation; subsequent shader builds are
offline. Keep native Conan dependencies available to CMake (below). Browser
linkage propagates `--use-port=emdawnwebgpu:cpp_bindings=false` through
Ludus::GraphicsRhi, with no Conan/Volk/Wayland dependency on that target.

## Installed consumer commands

These configure only through `find_package(Ludus CONFIG REQUIRED)` and use no
engine source include paths:

```bash
out/host-tools/venv/bin/cmake -S tests/sdk_consumer \
  -B out/build/render-api-sdk-native -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$PWD/out/host-tools/venv/bin/ninja" \
  -DCMAKE_CXX_COMPILER="$PWD/out/host-tools/bin/clang++" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$PWD/out/install/render-api-native;$PWD/out/conan/linux-clang-development" \
  -DLUDUS_SDK_RENDER_TESTS=ON \
  -DLUDUS_SLANG_COMPILER="$LUDUS_SHADER_COMPILER" \
  -DLUDUS_SPIRV_VALIDATOR="$LUDUS_SHADER_VALIDATOR"
out/host-tools/venv/bin/cmake --build out/build/render-api-sdk-native -j2
out/build/render-api-sdk-native/ludus_sdk_consumer
out/build/render-api-sdk-native/ludus_sdk_render_consumer

out/host-tools/emsdk/upstream/emscripten/emcmake out/host-tools/venv/bin/cmake \
  -S tests/sdk_consumer -B out/build/render-api-sdk-web -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$PWD/out/host-tools/venv/bin/ninja" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$PWD/out/install/render-api-web" \
  -DCMAKE_FIND_ROOT_PATH="$PWD/out/install/render-api-web" \
  -DLUDUS_SDK_RENDER_TESTS=ON \
  -DLUDUS_SLANG_COMPILER="$LUDUS_SHADER_COMPILER" \
  -DLUDUS_SPIRV_VALIDATOR="$LUDUS_SHADER_VALIDATOR" \
  -DLUDUS_LUSLC="$(./scripts/bootstrap-luslc)"
out/host-tools/venv/bin/cmake --build out/build/render-api-sdk-web -j2
node tests/sdk_consumer/browser-test.mjs
# Separately record what the flag-free default adapter does:
LUDUS_BROWSER_DEFAULT_ADAPTER=1 node tests/sdk_consumer/browser-test.mjs \
  out/build/render-api-sdk-web out/render-api-browser-default
```

Emscripten searches packages through its root-path policy: set both prefix and
find-root paths as above. Browser artifacts are `render.html` (also copied to
`index.html`), `render.js` and `render.wasm` in the consumer build directory.
`render.html?backend=webgpu` or `?backend=webgl2` forces an API; otherwise the
consumer uses Auto. `browser-test.mjs` uses Chromium's SwiftShader software
adapter by default. It renders on forced WebGPU, forced WebGL 2, Auto, and Auto
with `navigator.gpu` removed, and checks every captured frame's pixels and
orientation. It also runs the WebGPU fault cases.

Batches run the same harness on the installed raster fixture
(`tests/sdk_consumer/raster`), which builds this consumer for Emscripten. It is
the fourth test of the `raster` suite in `tests/sdk_consumer/browser-suites.json`,
with results under `fullscreen-diagnostic`. Serve over localhost or HTTPS:

```bash
python3 -m http.server 8768 --bind 127.0.0.1 \
  --directory out/build/render-api-sdk-web
```

Open in a WebGPU-capable browser; the diagnostic completes 32 frames and one
restart, alternating resolution and elapsed time. The test harness captures
portrait/landscape pixels, injects real invalid shader/pipeline descriptions,
destroys a real device, and delays real validation callbacks across cancellation.
Its JavaScript wrappers are test-only and are not the application integration path.

## Shader helper and artifacts

Use `ludus_compile_shader` as shown in the public guide with the application's
own Slang source/includes/defines. It compiles both backend outputs at build time:
vertex and fragment SPIR-V with `-profile spirv_1_3 -stage ... -entry ...`, and
combined WGSL with `-profile sm_6_0 -entry vertexMain -entry fragmentMain`.
Each SPIR-V module is validated by `spirv-val --target-env vulkan1.1`.
The generated descriptions carry SPIR-V entry `main` separately from WGSL entries
`vertexMain`/`fragmentMain`. The manifest stores complete expanded CLI commands.

Outputs belong under
`<consumer-binary-dir>/ludus-shaders/<TARGET>/<NAME>/`:

- `<NAME>.h`: generated native-word or browser-text storage and description factories.
- `<NAME>.vertex.spv`, `<NAME>.fragment.spv`, `<NAME>.wgsl`: backend artifacts.
- `vertex.reflection.json`, `fragment.reflection.json`, `wgsl.reflection.json`:
  separate target layout/binding evidence.
- `manifest.json`: pin, commands, profiles, entries, minimum uniform sizes and hashes.
- `shader.d` and stage depfiles: compiler-discovered source/import/include dependencies.

The source, includes/imports, explicit DEPENDS, compiler/validator, driver/lock and
options participate in rebuilds. Target and shader names isolate generated output
folders. Generated artifacts are embedded in the application; the raw metadata
can remain development evidence. No generated output is written into source trees.
Installed helper files live under `lib/cmake/Ludus/`, with driver/lock in `shaders/`;
only public headers are under `include/ludus/`. Tools/probes are not exported.

For the diagnostic only, the independently checked SPIR-V and emitted WGSL
contracts each give resolution offset 0/size 8, elapsed 8/4, direction 16/12,
tint 32/16; block 48 bytes/alignment 16. C++ offsetof/size/alignment assertions
check the padded upload struct. Binding is set/group 0, binding 0, uniform range
48, visible to both stages. The helper validates resource kind/binding and minimum
size; Sandbox must verify each new shader's full layout independently.

## Validation and remaining gates

Validation results and exact check commands are recorded below. Raw local logs
are `out/render-api-final-*.log`; [evidence snapshot](fullscreen-rendering-evidence.json)
records generated contracts and browser outcomes without treating compilation as
runtime proof. The shader source uses only f32 scalar/vector arithmetic, uniform
fields, vertex index, fragment position and location 0 output. More shader
features need their own validation even if Slang accepts them.

The sandbox exposes no `/dev/dri`, but the successful native runs outside it
select **Intel UHD Graphics 620 (WHL GT2), integrated GPU**, using Mesa
25.2.8-0ubuntu0.24.04.2 (device Vulkan API 1.4.318; instance 1.3.275).
The public API renders 80 submitted offscreen frames across two sessions, with
18 numerical image checks, an in-flight burst, and recorded-frame abort/restart.
Every checked channel matches within two 8-bit levels. A reachable Wayland
compositor is unavailable; live swapchain resize/presentation/fences remain
unexecuted. Software llvmpipe is additionally enumerated but was not selected
by these final native tests. Adapter details are in `out/render-api-native-adapter.log`. Vulkan validation
layers are not installed. Wayland requires Vulkan 1.1, a common graphics/present
queue, RGBA8/BGRA8 UNORM or sRGB, surface/swapchain maintenance1 and at most eight
swapchain images. Missing support fails explicitly. The preferred attachment is
BGRA8 UNORM; sRGB attachment behavior retains the earlier direct-probe evidence,
but public WSI sRGB and physical display color management remain open.

Browser runtime evidence is real Chromium/Dawn/SwiftShader through the installed
Emscripten C++ API, rather than a provider. It proves this subset and test path;
physical browser GPU operation, flag-free browser operation, other browser/OS combinations and
hosted HTTPS/iframe acceptance remain open. The flag-free run reports
AdapterUnavailable with zero frames (requestAdapter returned null). Controlled
provider tests additionally exercise resource readiness, stale IDs, capacity,
partial cleanup, dependency ownership and device-loss invalidation.


### Repository check outcomes

| Commands | Outcome |
| --- | --- |
| `./scripts/build linux-clang-debug`; `./scripts/test linux-clang-debug` | Warning-clean; 30 registered tests, zero failures, two live Wayland tests skipped. |
| `./scripts/build linux-clang-development`; `./scripts/test linux-clang-development` | Warning-clean/Werror; 31 registered tests, zero failures, two live Wayland tests skipped. |
| `./scripts/build linux-clang-asan-ubsan`; `./scripts/test linux-clang-asan-ubsan` | 28 registered tests, zero failures, two live Wayland tests skipped; no sanitizer/leak findings. |
| Final targeted RHI, public-header and foundational CTests in all three native trees after final resource/loss/resize changes | 5/5 checks in each tree; real physical-GPU pixels, controlled resource/lifecycle tests and public header/include gates pass. |
| `./scripts/build web-emscripten-development`; `./scripts/test web-emscripten-development` | Warning-clean; 14/14 semantic/header/frame/smoke tests pass. |
| `./scripts/check linux-clang-development --all`, followed by final `--format` and targeted clang-tidy 18 for changed RHI translation units | Passed. Initial pixel-test missing braces were corrected before the successful analysis. |
| `./scripts/check web-emscripten-development --tidy` | Passed with pinned Clang 18 and actual wasm target/sysroot/port include flags. |
| Native/web clean-prefix installs, public consumers and `engine.verify_sdk_install` | Passed: complete exports, matching manifest/assertion policy, no private headers or engine source/build paths. |
| Clang-tidy 18 on `tests/sdk_consumer/render.cpp` with actual native and wasm consumer compile flags | Passed. |
| `LUDUS_SHADER_PROBE_SOFTWARE=1 node tests/sdk_consumer/browser-test.mjs` | 5/5 real WebGPU scenarios; 16 screenshots have maximum channel error 0; successful runs submit/upload 32 frames with resources created only at setup/restart. |
| Flag-free `node tests/sdk_consumer/browser-test.mjs ... out/render-api-browser-default` | Failed acceptance: AdapterUnavailable, zero frames, requestAdapter null; recorded separately with no fallback. |
| `python3 tests/sdk_consumer/verify_shader_build.py` with installed SDK/tool paths below | Passed: no-op, source/include/option rebuilds, paths with spaces, reflected offsets, independent emitted-WGSL alignment, binding rejection and validator integrity rejection. |
| Python driver/test syntax parsed for Python 3.10 | Passed; execution used the host Python 3.12. |
| `git diff --check` | Passed. |

Repeat the shader build/dependency/layout check:

```bash
python3 tests/sdk_consumer/verify_shader_build.py \
  --sdk "$PWD/out/install/render-api-native" \
  --cmake "$PWD/out/host-tools/venv/bin/cmake" \
  --ninja "$PWD/out/host-tools/venv/bin/ninja" \
  --cxx "$PWD/out/host-tools/bin/clang++" \
  --slang "$LUDUS_SHADER_COMPILER" --validator "$LUDUS_SHADER_VALIDATOR" \
  --conan "$PWD/out/conan/linux-clang-development"
```

Native suites, socket-based SDK policy checks, leak inspection and browser
localhost/process execution ran outside sandbox restrictions. Earlier sandbox
runs are not acceptance evidence. The physical adapter result comes from the
same execution context as the final native GPU tests. Unrelated working-tree
edits were preserved and excluded from the rendering pull request.
