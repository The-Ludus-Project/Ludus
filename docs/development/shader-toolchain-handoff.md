# Shader-toolchain spike handoff

This page records the original offline compiler spike. Current optional native
compiler/API clients, live reload and headless workflows are documented in
[native shader worker and live development builds](render-graph-authoring.md#native-shader-worker-and-live-development-builds).
That opt-in capability supersedes this spike's blanket restriction on embedding
the compiler; ordinary renderer/runtime targets remain compiler-independent.

## Selection and boundary

Slang **2026.1.2** succeeds for this probe's vertex and fragment shaders on both
SPIR-V and WGSL. Use the CLI at build time, and package only generated assets.
Do not link Slang, DXC, or a validator into gameplay or download tools at runtime.
The separate lock is `config/shader_toolchain.json`; Clang 18 and
`config/web_toolchain.json` are unchanged.

The inspected checkout's installed RHI has lifecycle, surface/frame, and browser
clear operations, with no public shader/resource/pipeline API. The smoke app
still privately owns a WGSL triangle pipeline via `internal/webgpu_probe.h`.
This spike instead owns direct Vulkan and browser WebGPU resources under
`tools/shader-probe/`. Vulkan is headless/offscreen; the browser harness is plain
JS and also presents an animated canvas. Neither proves the RHI's swapchain or
Emscripten upload path. No probe headers, assets, compiler, or target are exported
or installed. An external game must wait for a separately scoped public
shader/resource/pipeline API to integrate its generated assets with Ludus.

## Reproducible tools and host support

Run the explicit network acquisition step on **Linux x86_64, Ubuntu 24.04**:

```bash
./scripts/shader-probe bootstrap
npm ci --prefix tools/web-browser-tests
tools/web-browser-tests/node_modules/.bin/playwright install chromium
```

The Slang archive is the official `slang-2026.1.2-linux-x86_64-glibc-2.27.tar.gz`:
SHA256 `bcffdccca2c16057d39f290a07f6f237b9a765238afc1974b68d09762e874b09`.
The digest was matched against the official release API asset digest and the
acquired bytes. Bootstrap verifies before extraction, bounds extraction to its
own directory, and retains the archive/LICENSE. Slang reports `2026.1.2`.
Its license is **Apache-2.0 WITH LLVM-exception**; the supplied license describes
embedded compilation-output permissions. Preserve licenses/notices for tool
redistribution and review upstream third-party notices as appropriate.

The selected Slang release also publishes Windows x86_64/aarch64, macOS
x86_64/aarch64, Linux aarch64, and wasm packages. Only the Linux x86_64 package is
pinned and exercised here. Its glibc floor is 2.27; this combined bootstrap has a
2.38 floor because the validator package needs glibc 2.38 and libstdc++ 13.1.
Other hosts need separately pinned assets and verification before being supported.

SPIRV-Tools is the Ubuntu package **2025.1~rc1-1~ubuntu0.24.04.2**; the executable
reports SPIRV-Tools v2025.1. Package SHA256:
`529e396c337d15beb8ed0f03fcbeb2a066e5a818e032e197b6ef2d3328dc7ae9`.
Bootstrap extracts it locally without a system install. Every compile verifies
`spirv-val` and `spirv-dis` executable digests against the lock. Licensing is
Apache-2.0; package notices are retained under `usr/share/doc/spirv-tools`.

WGSL is validated by **Chromium 140.0.7339.186 / revision 1193**, obtained through
integrity-locked **Playwright 1.55.1** in `tools/web-browser-tests/package-lock.json`.
The runner rejects a different browser version. Chromium's Dawn/Tint compiler
provides `getCompilationInfo()` and actual `createRenderPipelineAsync()` validation.
Playwright is Apache-2.0; Chromium has BSD and bundled third-party notices.
These browser/test tools are not shipped. Recorded local Node was 22.23.1.

All compiler tools live under `out/shader-tools/`. Builds neither acquire tools
nor invoke Conan/network downloads. Native Vulkan headers/Volk come from the
existing Conan pins. A Vulkan loader and usable adapter are runtime prerequisites.

## Exact build and validation commands

Prepare ordinary Ludus native dependencies with `./init.sh` if missing, then:

```bash
out/host-tools/venv/bin/cmake --preset linux-clang-development \
  -DLUDUS_BUILD_SHADER_PROBE=ON -DLUDUS_WARNINGS_AS_ERRORS=ON \
  -DLUDUS_ENABLE_TIME_TRACE=OFF
./scripts/build linux-clang-development
out/build/linux-clang-development/tools/shader-probe/ludus_shader_probe
out/host-tools/venv/bin/ctest --test-dir out/build/linux-clang-development \
  -R ludus_shader_probe --output-on-failure
./scripts/shader-probe check
```

CMake generates into `out/build/linux-clang-development/tools/shader-probe/generated`.
For just the assets use `./scripts/shader-probe compile` (default `out/shader-probe`),
or `compile --output PATH`. It checks reflection, emitted layout/bindings, and
SPIR-V validity before emitting the C++ header. CMake reruns it when the source,
lock, script, or browser harness changes. Exact expanded CLI commands, tool pins,
source/artifact hashes, reflection, disassembly, and `contract.json` accompany
each build in the output directory. These are compiler evidence, not runtime results.

The exact compilation recipe, with `S=tools/shader-probe/probe.slang`,
`C=out/shader-tools/slang/bin/slangc`, and `O=out/shader-probe`:

```bash
S=tools/shader-probe/probe.slang
C=out/shader-tools/slang/bin/slangc
O=out/shader-probe
$C $S -target spirv -profile spirv_1_3 -entry vertexMain -stage vertex \
  -o $O/probe.vertex.spv -reflection-json $O/vertex.reflection.json
$C $S -target spirv -profile spirv_1_3 -entry fragmentMain -stage fragment \
  -o $O/probe.fragment.spv -reflection-json $O/fragment.reflection.json
$C $S -target wgsl -profile sm_6_0 -entry vertexMain -entry fragmentMain \
  -o $O/probe.wgsl -reflection-json $O/wgsl.reflection.json
out/shader-tools/spirv-tools/usr/bin/spirv-val --target-env vulkan1.1 $O/probe.vertex.spv
out/shader-tools/spirv-tools/usr/bin/spirv-val --target-env vulkan1.1 $O/probe.fragment.spv
out/shader-tools/spirv-tools/usr/bin/spirv-dis $O/probe.fragment.spv
```

The wrapper supplies these paths automatically. WGSL's entries
have `[shader]` stage annotations in the source. `sm_6_0` is the Slang frontend
profile, not a claim that WebGPU supports every Shader Model 6 feature.

Browser real software-GPU test:

```bash
LUDUS_SHADER_PROBE_SOFTWARE=1 node tools/shader-probe/browser-test.mjs
```

This consumes the native build's generated folder by default. It writes
`out/shader-probe-browser/report.json` and `probe.png`. A different folder can be
its first argument; output directory is its second. Omit the environment variable
for the default adapter/flag-free test; failure remains failure. For manual
hardware testing serve the generated directory over localhost/HTTPS and open it:

```bash
python3 -m http.server 8768 --bind 127.0.0.1 \
  --directory out/build/linux-clang-development/tools/shader-probe/generated
```

Expect eight readback cases to pass, then a round blue circle over a red/right,
green/down gradient with changing color. Save the visible report, console, and
adapter/device/browser/OS details. Native hardware uses the same executable;
it logs the selected adapter (currently the first enumerated graphics adapter).
A software adapter's success does not close a physical-GPU gate.

## Binding and independent layout contract

The source declares `[[vk::binding(0, 0)]] ConstantBuffer<ProbeUniforms> probe`.
Vulkan uses descriptor **set 0, binding 0**, type UNIFORM_BUFFER, fragment
visibility. Browser WebGPU uses **group 0, binding 0**, uniform buffer, fragment
visibility, `minBindingSize=48`, range 48, offset 0. No dynamic offsets or vertex
buffers. The vertex stage uses vertex index to generate a fullscreen triangle.

| Source entry | Emitted SPIR-V entry | Emitted WGSL entry |
| --- | --- | --- |
| vertexMain | main (vertex module) | vertexMain |
| fragmentMain | main (fragment module) | fragmentMain |

Fragment output is location 0; fragment position uses SPIR-V OriginUpperLeft
and WGSL builtin position. Vulkan uses a positive-height viewport and no culling.
Both uploaded resolutions are framebuffer pixels, not CSS dimensions.

| Field | Source type | SPIR-V offset/size | WGSL offset/size |
| --- | --- | --- | --- |
| resolution | float2 | 0 / 8 | 0 / 8 |
| elapsedTime | float | 8 / 4 | 8 / 4 |
| direction | float3 | 16 / 12 | 16 / 12 |
| tint | float4 | 32 / 16 | 32 / 16 |

Both complete blocks are **48 bytes**, **16-byte alignment** for this source.
Unused bytes 12–15 and 28–31 are zeroed on upload. Vulkan's emitted std140 struct,
member decorations, vector types, and per-target reflection establish its
contract. WGSL is checked separately from emitted member types and `@align`
annotations: resolution 16, elapsedTime 8, direction 16, tint 16. Applying WGSL
AlignOf/SizeOf yields the table and the rounded struct size independently of
Slang reflection. See the [WGSL layout specification](https://www.w3.org/TR/WGSL/#memory-layouts).

`uniform_layout.h` declares separate VulkanUniforms and WebGpuUniforms, each
with explicit padding plus standard-layout/offsetof/sizeof/alignof assertions.
Clang 18 compiles both checks. Browser CPU uploads use DataView little-endian
float writes at the separately verified WGSL offsets; this spike does not test
an Emscripten C++ upload struct. Equality here is measured, not an assumption of
HLSL/SPIR-V/WGSL packing equivalence. Dynamic binding alignment is a separate
backend limit, not this struct's 16-byte alignment. The verifier deliberately
supports only this shape and fails closed if it changes.

## Exercised subset and outcomes

The supported evidence is 32-bit floating scalar/vector arithmetic, one mixed
uniform block, vertex index, builtin fragment position, fullscreen triangle,
and one RGBA output. No matrices, arrays in uniforms, interpolated varyings,
textures/samplers, storage buffers, push constants, f16, subgroup operations,
compute, depth, blending, or culling are proven. No DXC evaluation was needed
because the preferred Slang path passed the actual shaders and pipelines.

On October 1, 2026 (Toronto), Vulkan on llvmpipe LLVM 20.1.2 / Mesa
25.2.8-0ubuntu0.24.04.2 and Chromium's SwiftShader passed **8/8 cases each**.
Each renders all 6,144 pixels at 96×64 and 64×96, at elapsed 0 and 2, through
RGBA8 UNORM and RGBA8 sRGB targets. Every RGBA channel is checked against an
independent numerical expectation with a tolerance of two levels on each 8-bit channel. Browser
maximum observed error was 0; native mismatches beyond tolerance were 0.
The elapsed update changes RGB by the uploaded direction times elapsed×0.05.
The circle radius is 0.2×framebuffer height in both dimensions, so the portrait
and landscape comparisons check aspect correction. The spatial red/green
oracle detects a flipped readback/origin. sRGB targets check standard linear to
sRGB encoding; UNORM checks linear quantization. Canvas presentation uses the
preferred `bgra8unorm`; browser/monitor color management is not established by
the offscreen color tests.

Chromium reported no compilation or validation errors and continued animating
23 frames in the first captured report. Its launch flags are recorded in the
report and match the existing software-GPU runner. No mock/provider is used.
The run without software-GPU flags failed with `No WebGPU adapter`; it
rendered zero frames and is recorded separately, without a fallback. Physical
`/dev/dri` was unavailable. Vulkan validation layers were not installed;
SPIRV-Tools validation, Vulkan return codes and pixel readback are the Vulkan
evidence. Remaining gates: real native Vulkan GPU, flag-free browser WebGPU on
physical GPUs, target browser/OS combinations and hosted HTTPS/iframe behavior,
plus future public RHI integration. A compact committed
[evidence snapshot](shader-toolchain-evidence.json) records the exact shader
hashes, commands, contracts, eight-case results, and default-browser failure.

## Repository checks

Validation commands and final results are recorded below after the run. Local
raw logs are under `out/shader-probe-*.log`; generated artifacts and the browser
report remain available under `out/` and are intentionally not committed.

| Command | Outcome |
| --- | --- |
| `./scripts/shader-probe bootstrap` | Passed: verified cached official archives and re-extracted tools; version and validator digests matched. |
| `./scripts/shader-probe compile` | Passed: both SPIR-V stages validated for Vulkan 1.1; WGSL emitted; per-target contract checks passed. |
| `./scripts/shader-probe check` | Passed with clang-format/clang-tidy 18, actual native compile flags, and private probe header coverage. |
| `python3 tools/shader-probe/verify_contract.py out/shader-probe` | 3 tests passed, including rejection of reflection offset/size/binding drift and emitted WGSL alignment/binding/entry drift. |
| `./scripts/build linux-clang-debug` | Passed. |
| `./scripts/test linux-clang-debug` | 29 registered tests, zero failures; 2 live Wayland tests skipped. |
| `./scripts/build linux-clang-development` | Passed with warnings as errors and the probe enabled. |
| `./scripts/test linux-clang-development` | 30 registered tests, zero failures; 2 live Wayland tests skipped; both probe CTests passed. |
| `./scripts/build linux-clang-asan-ubsan` | Passed with warnings as errors and the probe enabled. |
| `./scripts/test linux-clang-asan-ubsan` | 27 registered tests, zero failures; 2 live Wayland tests skipped; probe pixel and contract tests passed, with no ASan/UBSan/LSan findings. |
| `LUDUS_SHADER_PROBE_SOFTWARE=1 node tools/shader-probe/browser-test.mjs out/shader-probe` | Passed: pinned Chromium/SwiftShader, eight readbacks plus animated presentation, no compilation/validation errors. |
| `node tools/shader-probe/browser-test.mjs out/shader-probe out/shader-probe-browser-default` | Failed: no WebGPU adapter without flags; hardware/flag-free gate remains open. |
| `./scripts/install-sdk linux-clang-development` | Refused the existing assertion-policy-v1 prefix, preserving it. Clean-prefix install and external consumer passed as described below. |
| `./scripts/check linux-clang-development --all` | Passed: repository formatting, include boundary, and clang-tidy 18 after disabling cached time tracing. |
| Final `cmake --build ... --target ludus_shader_probe -j2` and `ctest --test-dir ... -R ludus_shader_probe --output-on-failure` in Development and ASan/UBSan | Passed after final array-index type adjustments: 2/2 tests in each tree. |
| `git diff --check` | Passed. |

Initial sandboxed test runs failed because socket/process inspection was denied;
LeakSanitizer could not inspect threads. The successful native suites and SDK
consumer were rerun outside those sandbox restrictions, without disabling leak
checks. Browser execution also needed localhost sockets and a browser process
outside the sandbox. Initial repository tidy rejected the pre-existing cached
`-ftime-trace` option; reconfigure with `-DLUDUS_ENABLE_TIME_TRACE=OFF` for analysis.
The tools-specific check removes only Ninja's module response-file argument and
the compiler profiling flag while preserving all warning/exception/include flags.

To preserve the older SDK, validation used this separate prefix and consumer:

```bash
out/host-tools/venv/bin/cmake --install out/build/linux-clang-development \
  --prefix "$PWD/out/install/shader-probe-development"
out/host-tools/venv/bin/cmake -S tests/sdk_consumer \
  -B out/build/shader-probe-sdk-consumer -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$PWD/out/host-tools/venv/bin/ninja" \
  -DCMAKE_CXX_COMPILER="$PWD/out/host-tools/bin/clang++" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$PWD/out/install/shader-probe-development;$PWD/out/conan/linux-clang-development"
out/host-tools/venv/bin/cmake --build out/build/shader-probe-sdk-consumer -j2
out/build/shader-probe-sdk-consumer/ludus_sdk_consumer
```

The existing `engine.verify_sdk_install` check confirmed installed files,
manifest/policy, header privacy, and absence of source/build paths in exports.
Additional inspection found no shader-probe files/targets or Slang dependencies
in the installed SDK. The consumer reported Development policy version 2 and
passed its installed Input/RHI checks. The probe executable's runtime dependency
list contains no shader compiler libraries.
