# Windows and Direct3D 12 backend

Ludus runs on Windows through a Win32 Platform backend and a Direct3D 12 RHI
backend. This page owns that port: the decision, the contracts of the slices
that exist, the [port record](../development/platform-porting.md#reusable-record-format)
and the staged plan. The work is at the **buildable** admission gate for the
D1 and D2 closure described below; it is not yet a runnable or qualified engine
deployment, and nothing here changes the Linux and macOS profiles.

## Decision

Windows renders with **Direct3D 12**, not Vulkan. Direct3D 12 is the platform's
native, always-installed graphics API; it needs no loader or ICD on the player's
machine, PIX and the debug layer are its first-class tools, and the
[graphics debugging plan](graphics-debugging.md) already names PIX and DRED as
the Windows capture and crash-analysis path. Vulkan on Windows would duplicate
the Linux backend without removing the need for D3D12-specific integration.

The backend follows the existing RHI shape: one private backend translation unit
(`modules/graphics/rhi/src/rhi_d3d12.cpp`) implements `internal/backend.h` and
the resource hooks, and the public API gains only `Backend::Direct3D12` and
`BackendAvailability::HasDirect3D12`. Native Auto selects it on Windows, as it
selects Vulkan on Linux and Metal on macOS. No COM, Win32 or D3D12 type appears
in a public header; Win32 handles cross the Platform/RHI boundary as borrowed
`void*` in `NativeWindowInfo`.

The toolchain is the one the [Windows threading lane](threading.md#milestones)
already qualifies: the pinned upstream Clang 23 driver targeting
`x86_64-pc-windows-msvc`, the Microsoft STL and dynamic CRT, and the Windows SDK
headers and import libraries for `d3d12`, `dxgi`, `dxguid` and `user32`. No
third-party runtime dependency is added; shaders are compiled at build time by
the pinned Slang and DXC ([DXIL artifacts](#dxil-artifacts)). COM objects are owned by
`UniquePtr<T, ComRelease>`, a private release protocol over the Ludus owner,
rather than WRL `ComPtr`.

## Port record

| Field | Value |
| --- | --- |
| Target | Windows 10 version 1703 or later, x86_64, little endian, 64-bit pointers |
| Host and tools | Windows host; pinned LLVM 23.1.3 Windows installer, CMake/Ninja/Conan from `config/tool_versions.json` |
| Target SDK | Windows 10/11 SDK installed with the build image; Microsoft STL and dynamic CRT |
| Runtime graphics | Direct3D 12 at feature level 11_0 or higher on a hardware adapter, otherwise WARP |
| Unsupported variants | 32-bit x86, ARM64 and ARM64EC; UWP/GDK; Windows before 10 version 1703 (per-monitor DPI v2) |
| Dependency pins | No runtime dependencies; build-time Slang 2026.1.2 (Windows x86_64) and DXC 1.9.2609 in `config/shader_toolchain.json`; Catch2 for tests through the existing Windows Conan profile |
| Validation | `.github/workflows/windows-direct3d12.yml` on `windows-2022` (WARP; no GPU) |
| Owner and branch | D1: `codex/windows-direct3d12-rhi` (merged); D2: `codex/windows-d3d12-dxil-draw` |
| Last audited base | `develop` at `a372af74` (2026-10-24) |

## Win32 Platform backend

`WindowManager` selects the Win32 backend (`LUDUS_PLATFORM_WIN32`) on Windows.
`Initialize` registers one window class and asks for per-monitor DPI awareness
(version 2); a manifest or an earlier call that already fixed the process DPI
mode wins, and initialization is repeatable.

Create, pump and destroy a window on one thread: Win32 delivers a window's
messages only to its creating thread. `HandleEvent` from another thread returns
false and destroying a window off its thread is a contract violation.
`HandleEvent` drains the whole thread queue, so it also dispatches messages for
that thread's other windows; a `WM_QUIT` is left for the application's loop.

Titles are copied from valid UTF-8 of at most 4096 bytes. The requested width
and height are logical units (96 DPI) from 1 through 16384, scaled by the
window's monitor DPI, matching Cocoa's logical points. `NativeWindowInfo`
reports `WindowSystem::Win32`, the borrowed `Win32WindowOrNull` HWND, client-area
pixels and `Scale` as DPI over 96. Minimizing publishes a zero extent and
`WindowVisibility::Minimized`; a hidden window reports `Occluded`. Win32 has no
portable occlusion signal, so a covered window still reports `Visible`. Closing
the window (`WM_CLOSE`) destroys it, clears the handle and extent, reports
`Occluded`, and makes `HandleEvent` return false. Keyboard sinks are retained
but not yet fed; physical keys, text input and the Win32 key map are a later
slice.

Platform's hosted window channel uses POSIX sockets and Mach ports and stays
unavailable on Windows; system entropy also reports unavailable until an audited
Windows source is added.

## Direct3D 12 backend (D1)

D1 starts a device and clears and presents frames through the ordinary RHI
lifecycle (`Start`, `SetFrameTarget`, `BeginFrameStatus`, `EndFrameStatus`,
`Shutdown`) and the legacy synchronous entry points.

- **Adapter selection.** DXGI enumerates adapters by high-performance
  preference, skips software adapters and creates the device on the first
  hardware adapter that supports feature level 11_0. If none does, the backend
  uses WARP, so GPU-less hosts and CI run the same code path.
  `GetMeasurementAdapter` reports the adapter name, vendor and device IDs, and
  whether it is the software adapter.
- **Windows.** `Start` accepts a headless window or a live Win32 window owned by
  the calling thread. Hosted windows report `InvalidWindow`.
- **Frames.** One direct queue, one fence and two frame slots, each with its own
  command allocator; `BeginFrameStatus` waits only for the slot it reuses. A
  Win32 window gets a two-buffer flip-discard swap chain in BGRA8 that DXGI
  stretches to the client area; Alt+Enter fullscreen is disabled. Headless
  sessions clear an offscreen BGRA8 texture. A changed frame target resizes the
  swap chain or texture after draining the queue. Presentation waits for
  vertical blank.
- **Skipped frames.** A zero-size target, or a minimized or hidden window,
  returns `Skipped` without acquiring a buffer.
- **Failure.** A removed, reset or hung device, or a fence wait longer than
  10 seconds, fails the session with `DeviceLost`. Other command, resize or
  `Present` failures report `RenderingUnavailable`. The facade then shuts the
  backend down.
- **Debugging.** Debug and Development builds enable the Direct3D 12 debug
  layer and the DXGI debug factory when the Windows *Graphics Tools* optional
  feature is installed, and continue without them otherwise.
- **Limits.** `MaxFrameDimension2D` is 16384 and the uniform limit is the 64 KiB
  constant-buffer view, clamped by the facade to 16384 bytes.

## Fullscreen compatibility draw (D2)

D2 implements the fullscreen resource API of `render.h` (`CreateShader`,
`CreateUniform`, `CreatePipeline`, `UpdateUniform`, `DrawFullscreen` and the
matching `Destroy` calls) on Direct3D 12, through the legacy facade and the
explicit device path alike.

- **Shaders.** `ShaderDescription::Dxil` carries a signed DXIL container and
  `DxilEntry` its entry name. The facade accepts a Direct3D 12 shader only when
  the bytes start with a `DXBC` header whose recorded size matches, and the entry
  is a valid name; otherwise `InvalidDescription`. The backend then reads the
  container's part table: the `DXIL` program must be of the requested stage, and
  the pipeline state validation part (`PSV0`, runtime info version 3 or later)
  must name the same entry and stage, or creation reports `Failed`. The reader
  (`src/dxil_container.cpp`) is portable and unit-tested on every host. The
  bytecode is copied, so the description is borrowed only for the call.
- **Root signature.** One root signature per device holds a single root constant
  buffer view at register `b0`, space 0, visible to every stage, with the hull,
  domain and geometry stages denied. There are no descriptor tables, samplers or
  input layout.
- **Uniforms.** Each uniform owns one persistently mapped upload-heap buffer per
  frame slot, rounded up to the 256-byte root CBV alignment. `UpdateUniform`
  snapshots the bytes on the CPU; `DrawFullscreen` copies them into the open
  slot's buffer, which that slot's fence wait has already released, so a frame in
  flight never sees a later update.
- **Pipelines.** A graphics pipeline state object targets the BGRA8 surface with
  blending off, culling off, depth clip on and no depth-stencil target: the
  fullscreen draw neither tests nor writes depth on any backend, and Metal's
  depth attachment belongs to its raster pass rather than this contract.
  Shaders can be destroyed once a pipeline exists. Destroying a pipeline or a
  uniform drains the queue first, because a submitted frame may still use it.
- **Draw.** `DrawFullscreen` binds the root signature, pipeline and the slot's
  uniform address, sets a viewport and scissor covering the acquired target and
  issues one 3-vertex triangle list draw. Pixel origins are top-left, matching
  the Vulkan and Metal pixel tests.

The raster, compute and lifetime hooks (`raster_unsupported.cpp`) still report
zero capabilities and `Unsupported`, so a session that requires a raster or
compute profile fails at startup with `RequirementsUnsatisfied`, and
`CreateRasterShader` accepts a DXIL artifact only to return the backend's
`Unsupported`. Offscreen-only frames (`ShouldPresentToSurface = false`), GPU
timing and pass scopes, native interop, ray queries and spatial upscaling are
unavailable. The renderer, editor, SDK install and game host are not part of
the Windows build yet.

### DXIL artifacts

`ludus_compile_shader` compiles DXIL **only when CMake runs on a Windows host
and targets Windows**: it runs Slang's `dxil` target, which has the pinned DXC
(`dxcompiler.dll`) compile shader model 6.0 and its validator (`dxil.dll`)
sign the result. Retail Direct3D 12 runtimes reject unsigned DXIL, and the
signing library ships only as a Windows binary, so Linux and macOS builds never
need DXC and cross-compiled Windows configurations stop with a setup error.
Only fullscreen `VERTEX`/`FRAGMENT` stages are supported; `RASTER` and
`COMPUTE` wait for the Direct3D 12 raster profile (D3/D4).

`config/shader_toolchain.json` pins the Slang Windows archive and the DXC
release archive by SHA-256, plus the two DLLs themselves. On the Windows host:

```bash
python scripts/shader-probe bootstrap
cmake ... -DLUDUS_SLANG_COMPILER=out/shader-tools/slang/bin/slangc.exe \
  -DLUDUS_DXC_DIRECTORY=out/shader-tools/dxc
```

The build driver (`cmake/shaders/compile_shader.py --dxil`) checks the DLL
hashes, defines `LUDUS_DXIL=1`, reflects the uniform independently (one
constant buffer at `b0`, space 0, at most 16384 bytes), rejects a container
whose hash is zero or a constant bypass value, and writes a header with
4-byte-aligned `Dxil` arrays and `DxilEntry` names. Slang passes
`SV_VulkanVertexID` to HLSL unchanged, so shaders spell the fullscreen vertex
index `LUDUS_SHADER_VERTEX_ID` from the RHI's `raster_bindings.slang`, which
selects `SV_VertexID` for DXIL. DXC is licensed under NCSA and MIT terms;
`dxil.dll` is covered by Microsoft's DirectX Shader Compiler license, and the
tools are used at build time only, never installed with an SDK.

## Build and test

The Windows configure builds the threading closure plus Identity, Math, Logging,
Input, Platform and the RHI (top-level `CMakeLists.txt`). The
[Windows Direct3D 12 lane](../development/continuous-integration.md#windows-direct3d-12-lane)
builds them warning-clean and runs:

- `ludus_platform_win32_tests`: window creation, DPI-scaled extent, minimize and
  restore, close, invalid requests and destruction.
- `ludus_rhi_d3d12_tests`: startup policy, headless clear and pixel readback,
  repeated target resizes across both frame slots, explicit limits, legacy
  restart while recording, and a Win32 swap chain that resizes with its window
  and skips minimized frames. With the shader tools configured (`LUDUS_TEST_DXIL`)
  it also compiles `tests/sdk_consumer/shaders/diagnostic.slang` to DXIL and
  checks entry/stage rejection, the fullscreen image pixel by pixel across
  resizes, a 40-frame burst over both frame slots, a 16384-byte uniform, restart
  with a recorded but unsubmitted draw, and drawing into a Win32 swap chain.
- `ludus_rhi_lifecycle_tests`: the backend-independent facade suite, including
  the DXIL container reader on synthetic containers.

Contributors without a Windows host can compile the closure with the pinned
Clang and the mingw-w64 headers, which carry `d3d12.h` and `dxgi1_6.h`. This
checks syntax and the backend symbol closure only; the MSVC STL, the Windows SDK,
DXIL compilation and execution are qualified by the CI lane. The configure needs
a CMake toolchain file that sets these values (the engine refuses a configure
without `CMAKE_TOOLCHAIN_FILE`), with llvm-ar/llvm-ranlib and `-fuse-ld=lld`.

```bash
brew install mingw-w64
cmake -S . -B out/build/windows-mingw-check -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
  -DCMAKE_CXX_COMPILER="$(brew --prefix llvm@23)/bin/clang++" \
  -DCMAKE_CXX_COMPILER_TARGET=x86_64-w64-mingw32 \
  -DCMAKE_SYSROOT="$(brew --prefix mingw-w64)/toolchain-x86_64" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLUDUS_BUILD_FLAVOR=Development \
  -DLUDUS_BUILD_TESTS=OFF -DLUDUS_WARNINGS_AS_ERRORS=ON -DLUDUS_ENABLE_CCACHE=OFF
cmake --build out/build/windows-mingw-check --target ludus_platform ludus_graphics_rhi
```

## Milestones

| Milestone | Scope | Exit evidence |
| --- | --- | --- |
| D1 (WARP verified) | Windows closure for Platform and RHI; Win32 windows; Direct3D 12 device, swap chain, clear, present, resize and readback | Windows lane passed on PR The-Ludus-Project/Ludus#573 ([run 40043576093](https://github.com/The-Ludus-Project/Ludus/actions/runs/40043576093)), merged as `7167c8a2`; presented frames on a hardware adapter still to be recorded here |
| D2 (in progress) | DXIL shader artifacts from the Slang toolchain (pinned DXC), `ShaderDescription` DXIL fields, root signatures, and the fullscreen compatibility draw | Fullscreen pixel tests on the WARP lane; hardware-adapter run still to be recorded here |
| D3 | Portable raster profile: buffers, textures, views, samplers, descriptor heaps, passes and barriers | Raster conformance suite shared with Vulkan and Metal |
| D4 | Lifetime transfers, readback, ordered graph and buffer compute profile | Lifetime, graph and compute suites; `RequiredProfile` startup passes |
| D5 | Win32 keyboard input and key map, system entropy, filesystem, timestamps, PIX markers and DRED | Input, entropy and filesystem suites on Windows; captures documented |
| D6 | Renderer, installed SDK, selectable Windows presets, game host and editor | Installed consumer, preset listing/configure/build/test, sample project |

Each milestone extends the backend behind the existing contracts. A slice that
cannot implement a hook reports `Unsupported` explicitly instead of approximating
another backend's behavior.

## Subsystem coverage

| Subsystem / required profile | Owner | Status | Blocker / next action | PR | Evidence / commit |
| --- | --- | --- | --- | --- | --- |
| FoundationBase / target ABI | threading T3 | verified | none for x86_64 MSVC ABI | threading T3 | [threading evidence](../development/threading-evidence.md#platform-completion-t3) |
| Toolchain / build configuration | D1, D2 | verified (WARP) | DXIL build-time tools added in D2 | PR #573; D2 PR | Windows lane, PR #573 (`7167c8a2`) |
| Time / threading | threading T3 | verified | none | threading T3 | [threading evidence](../development/threading-evidence.md#platform-completion-t3) |
| Logging / profiling | D1 | needs port | Builds in the closure; run the logging suites on Windows | pending | none |
| Filesystem | unassigned | needs port | Windows backend for mounts and persistence (D5) | pending | none |
| Platform / host (Win32 windows) | D1 | verified (WARP) | Keyboard (D5) and hosted window later | PR #573 | Windows lane, PR #573 (`7167c8a2`) |
| Input | unassigned | needs port | Win32 key map and keyboard delivery (D5) | pending | none |
| RHI / graphics (clear/present) | D1 | verified (WARP) | Hardware-adapter evidence | PR #573 | Windows lane, PR #573 (`7167c8a2`) |
| RHI / graphics (DXIL shaders, fullscreen draw) | D2 | in progress | Windows lane on the D2 PR; hardware-adapter evidence | D2 PR | pending |
| RHI / graphics (raster, compute, lifetime) | unassigned | needs port | D3-D4 | pending | none |
| Renderer / graph, text, content, runtime, audio, networking | unassigned | unreviewed | After D4 | pending | none |
| SDK / projects / editor tooling | unassigned | unreviewed | D6 | pending | none |
| CI / documentation | D1, D2 | in progress | Lane gates batch promotion; D2 adds the shader tool bootstrap | PR #573; D2 PR | Windows lane, PR #573 (`7167c8a2`) |

## References

- Microsoft, [Creating a basic Direct3D 12 component](https://learn.microsoft.com/windows/win32/direct3d12/creating-a-basic-direct3d-12-component)
  and [Synchronization and multi-engine](https://learn.microsoft.com/windows/win32/direct3d12/user-mode-heap-synchronization):
  the queue, fence and per-frame command allocator sequence.
- Microsoft, [ID3D12CommandAllocator::Reset](https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12commandallocator-reset):
  allocators are reset only after their GPU work completes.
- Microsoft, [DXGI flip model](https://learn.microsoft.com/windows/win32/direct3ddxgi/dxgi-flip-model):
  flip-discard swap chains and buffer resize rules.
- Microsoft, [Windows Advanced Rasterization Platform (WARP) guide](https://learn.microsoft.com/windows/win32/direct3darticles/directx-warp):
  the software adapter used on GPU-less hosts.
- Microsoft, [Using a root signature](https://learn.microsoft.com/windows/win32/direct3d12/using-a-root-signature)
  and [Using descriptors directly in the root signature](https://learn.microsoft.com/windows/win32/direct3d12/using-descriptors-directly-in-the-root-signature):
  the single root constant buffer view of the fullscreen pipeline.
- Microsoft and the DirectX Shader Compiler contributors, `DxilContainer.h` and
  `DxilPipelineStateValidation.h` in [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler):
  the container, program header and PSV0 layouts read by `dxil_container.cpp`.
- The Slang team, [Supported Compilation Targets](https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/09-targets.html):
  DXIL generation through DXC.
- Microsoft, [Managing application state](https://learn.microsoft.com/windows/win32/learnwin32/managing-application-state-)
  and [WM_DPICHANGED](https://learn.microsoft.com/windows/win32/hidpi/wm-dpichanged):
  per-window state and DPI-change resizing in the Win32 backend.
