# Windows and Direct3D 12 backend

Ludus runs on Windows through a Win32 Platform backend and a Direct3D 12 RHI
backend. This page owns that port: the decision, the contracts of the slices
that exist, the [port record](../development/platform-porting.md#reusable-record-format)
and the staged plan. The work is at the **buildable** admission gate for the
D1 and D2 closure described below (both verified on WARP), with D3a resource
implementation awaiting Windows execution; it is not yet a runnable or qualified engine
deployment. The remaining implementation adopts **MSVC as the Windows reference
compiler**, with CMake generating Ninja builds for VS Code/CLI/the Ludus editor
and Visual Studio solutions built by MSBuild. The migration has started with
the isolated [D0 primitive probe](#d0-primitive-portability-probe); the engine's
compiler gate remains unchanged and the recorded D1/D2 evidence still belongs
to Clang. Linux, macOS
and browser compiler profiles remain unchanged.

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

The current build uses the toolchain that the
[Windows threading lane](threading.md#milestones) qualifies: pinned Clang 23
with the Microsoft STL, dynamic CRT and Windows SDK. Targeting the MSVC ABI
is not evidence of compilation with Microsoft's `cl.exe`. FoundationBase still
rejects non-Clang compilers; D0 below owns that migration.

The target Windows toolchain is a pinned **MSVC `cl.exe` toolset**, Microsoft
linker, Microsoft STL, dynamic CRT and Windows SDK. D0 must choose and record
exact versions after C++23 compatibility tests; no MSVC version is qualified by
this plan alone. Keep LLVM 23 for repository formatting/static analysis and
existing non-Windows compiler profiles. Windows Clang may remain an optional
portability check after MSVC acceptance, with its own evidence and SDK identity.

Shaders continue through pinned Slang/DXC to DXIL regardless of C++ compiler or
build generator ([DXIL artifacts](#dxil-artifacts)). The D1/D2 closure adds no
third-party runtime dependency; later runtime/editor dependencies require their
own audit and packaging. COM objects remain owned by `UniquePtr<T, ComRelease>`,
a private release protocol over the Ludus owner.

## Port record

| Field | Value |
| --- | --- |
| Target | D1/D2: Windows 10 version 1703 or later, x86_64, little endian, 64-bit pointers; D0/D7 must revalidate the minimum OS against the full runtime/editor dependency set |
| Host and tools | Current: Windows host, LLVM 23.1.3, CMake/Ninja/Conan. Target: pinned MSVC toolset/SDK, Ninja and MSBuild; exact new pins and setup delivered in D0 |
| Target SDK | Current: Windows 10/11 SDK supplied by the build image, Microsoft STL and dynamic CRT. D0 pins the SDK and matching dependency/CRT configuration for MSVC |
| Runtime graphics | Direct3D 12 at feature level 11_0 or higher on a hardware adapter, otherwise WARP |
| Unsupported variants | 32-bit x86, ARM64 and ARM64EC; UWP/GDK; Windows before 10 version 1703 (per-monitor DPI v2) |
| Dependency pins | No runtime dependencies; build-time Slang 2026.1.2 (Windows x86_64) and DXC 1.9.2609 in `config/shader_toolchain.json`; Catch2 for tests through the existing Windows Conan profile |
| Validation | Existing Clang D1/D2 lane: `.github/workflows/windows-direct3d12.yml` on `windows-2022` (WARP; no GPU). MSVC, MSBuild and physical-GPU acceptance pending |
| Owner and branch | D1: `codex/windows-direct3d12-rhi` (merged); D2: `codex/windows-d3d12-dxil-draw`; D3a: `codex/windows-integration-d3a` |
| Last audited base | Plan/toolchain audit: `develop` at `94bc3e3c` (2026-10-26); historical D1/D2 runtime evidence remains at the commits recorded below |

## Developer workflow contract (planned)

CMake owns targets, dependencies, compiler options, shader recipes and generated
files for every supported environment. Generate `.sln`/`.vcxproj` into ignored
build directories; maintain project structure in CMake, not the generated
projects. Use separate build/cache/output directories for Ninja and MSBuild.
No second handwritten MSBuild project graph is required.

| Environment | Configuration and execution | Acceptance |
| --- | --- | --- |
| VS Code | Shared CMake presets, CMake Tools, Ninja, MSVC | Configure/build/test/debug the engine checkout and an external SDK project; navigation and Problems diagnostics match the selected configuration |
| Visual Studio | CMake Visual Studio generator, generated solution/projects, MSBuild, MSVC | Generate/open/build/test/debug; switch configurations; rebuild shaders and regenerate after CMake changes |
| CLI / other source editors | Same documented CMake configure/build/test presets, Ninja, MSVC | Build and launch without an IDE or Ludus editor; report missing tools with actionable setup diagnostics |
| Ludus editor | Shared project setup and CMake presets; Ninja/MSVC by default | Build/play/reload and resolve configuration-specific artifacts through the CMake File API; support a project's admitted MSBuild preset too |
| Windows CI | MSVC/Ninja main validation plus MSBuild workflow coverage | Validate each admitted closure as it grows, including generated shaders, tests and installed consumers; D7 records full profile qualification |

Both build generators must use the same pinned MSVC toolset, SDK, CRT policy
and matching dependency binaries. Pin tool inputs in the existing configuration
owners and preserve local machine paths in ignored user settings. Do not assume
Clang-produced SDKs or game DLLs are interchangeable with MSVC artifacts;
validate their compatibility through the existing SDK/game ABI checks.

Visual Studio is a multi-configuration generator. D0 must map Ludus Debug,
Development, Profile and Release flavors consistently to configurations,
assertions, flags, dependency variants, PDBs, generated shaders, output paths
and install/export metadata. Audit `CMAKE_BUILD_TYPE` assumptions and fixed
artifact paths; build, CTest and install must select the intended configuration.
The same checks apply to SDK project setup and repair. Today
`cmake/EngineBuildFlavor.cmake` explicitly rejects multi-configuration generators
and generates one flavor-specific SDK variant per build tree. D0 must replace
that limitation with configuration-correct policy headers, generated outputs
and exports. Development and Profile both currently use `RelWithDebInfo` but
have different engine policies; a configuration mapping must preserve that
distinction. Removing the guard alone is not Visual Studio support.

Embedded shader/script editing and external source editors share the owning
compiler/runtime validation and publication operations. Cover includes, source
locations, Unicode/spaced paths, file replacement on save and concurrent edits.
Report conflicts without overwriting unsaved document edits. Engine C++ changes
use the selected CMake build; shader/script edits use only their supported reload
contracts. Failed or stale candidates must not replace the last accepted
runtime state or drawable viewport.

See the existing [project setup](../development/project-sdk-workflow.md),
[live game editing](../development/project-live-reload.md) and
[shader authoring](../development/render-graph-authoring.md) owners for shared
contracts. Windows support must extend those workflows rather than duplicate
their project model or introduce a separate compiler pipeline per editor.

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

D3a adds the resource portion described below. Raster draws/passes, compute and
lifetime transfers (`raster_unsupported.cpp`) remain unavailable. Offscreen-only
frames (`ShouldPresentToSurface = false`), GPU timing/pass scopes, native interop,
ray queries and spatial upscaling remain unavailable. The renderer, editor, SDK
install and game host are not part of the Windows build yet.

### Raster resources and bindings (D3a)

The private `raster_d3d12.cpp` owner shares D1's device, direct queue and fence.
The effective profile exposes resource limits but zero draws, color attachments
and vertex stride. `PortableRaster`, ordered graphs and compute remain disabled;
requiring them still fails startup with `RequirementsUnsatisfied`. Applications
can query `GetRasterCapabilities` and prepare resources on an otherwise ready
explicit device. D3b must supply and qualify drawing before admitting the complete
portable profile. This does not complete the MSVC baseline in D0.

- **Buffers.** Immutable vertex, index and uniform buffers use upload heaps in
  `GENERIC_READ`, with complete initial bytes copied before return. Native padding
  is zeroed and rounded to the 256-byte CBV alignment. Uniform slices retain their
  logical byte bounds; descriptor sizes round up within that backing. Storage
  buffers remain unavailable with the compute profile.
- **Textures.** Original-format sampled RGBA8/sRGB, R8, RG8, RGBA16F and BC1/3/5
  allocations support complete mip chains in D2, arrays, cubes and volumes when
  `CheckFeatureSupport` admits the native format/dimension. Native copyable
  footprints determine aligned row/slice placement; caller padding is preserved
  in the borrowed upload contract, without conversion. Creation returns `Pending`;
  `GetStatus` or ordinary frame collection publishes `Ready` after the queue fence.
  Compressed base extents must be block-aligned. Staging bytes consume the
  resource budget in addition to logical texture bytes
  and are released at completion. Attachments and deferred initialization remain
  unavailable. Format queries do not advertise drawing or transfer uses.
- **Views and samplers.** Color SRVs select bounded mip/layer ranges, retaining
  the source allocation. D2 views of an array/cube layer report `Unsupported`
  because a D3D12 array SRV cannot be bound as a scalar Texture2D. Depth/stencil
  and writable views wait for D3b. Samplers implement nearest/linear texel and
  mip filtering and clamp/repeat addressing; no comparison or anisotropy.
- **Bindings.** Scalar uniform, sampled-color and sampler layouts create a root
  signature with one table per binding at the same native register in space zero.
  Each immutable set owns separate shader-visible resource and sampler heaps.
  The shared registry budgets every descriptor and retains layout/resources
  through snapshot retirement. Dynamic offsets, descriptor arrays, runtime arrays
  and partial binding remain explicitly unavailable. Heap creation is transactional;
  a failed second heap does not publish a partially usable set.
- **Lifetime.** Texture upload backing and its allocator/list remain owned until
  the exact creation fence completes, including logical destruction while pending.
  Submission bookkeeping reserves a configured completion cell before encoding and
  maps accepted ordinals to the existing queue fence. Reset drains the queue before
  physical destruction. A failed signal retains submitted creation backing for
  shutdown rather than treating it as completed work.
- **Shaders.** Raster DXIL is reflected independently at build time, including
  native registers, uniform member offsets and vertex inputs. Native creation
  checks stage/entry and the complete PSV0 resource table against the copied scalar
  layout: missing/extra bindings, wrong spaces, ranges, categories and dimensions
  reject. Bytecode is copied into budgeted storage. Pipelines and draws wait for D3b.

The Windows lane builds `d3d12_resource_tests.cpp` beside D1/D2: partial-profile
admission, descriptor/resource exhaustion and reuse, retained snapshots, padded
mip/array/cube/volume creation across native formats, pending-upload destruction
and restart, and real raster DXIL interface rejection. Portable PSV reader tests
and resource-only facade admission run with the common lifecycle suite on every
host. Cross-compilation verifies Windows syntax and the backend closure; WARP,
MSVC and physical-adapter execution must be recorded separately before claiming
Windows qualification. These resource tests do not establish rendered image
correctness; D3b owns shared raster/image conformance.

### DXIL artifacts

`ludus_compile_shader` compiles DXIL **only when CMake runs on a Windows host
and targets Windows**: it runs Slang's `dxil` target, which has the pinned DXC
(`dxcompiler.dll`) compile shader model 6.0 and its validator (`dxil.dll`)
sign the result. Retail Direct3D 12 runtimes reject unsigned DXIL, and the
signing library ships only as a Windows binary, so Linux and macOS builds never
need DXC and cross-compiled Windows configurations stop with a setup error.
Fullscreen and scalar `RASTER` vertex/fragment artifacts are supported.
`COMPUTE`, storage bindings and descriptor arrays wait for later Direct3D 12
profiles. Raster headers carry target-specific bindings, uniform members and
vertex inputs; the fullscreen header contract remains unchanged.

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

This section describes the existing Clang D1/D2 closure. D0 adds the MSVC
commands and presets only after they work; these examples do not qualify MSVC.

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
| D2 (WARP verified) | DXIL shader artifacts from the Slang toolchain (pinned DXC), `ShaderDescription` DXIL fields, root signatures, and the fullscreen compatibility draw | Fullscreen pixel tests passed on the WARP lane for PR The-Ludus-Project/Ludus#586 ([run 40102938249](https://github.com/The-Ludus-Project/Ludus/actions/runs/40102938249), `002969cf`); hardware-adapter run still to be recorded here |
| D0 (next; MSVC baseline) | Pin MSVC/SDK/CRT/dependencies; enable `cl.exe`, Ninja and generated Visual Studio/MSBuild projects; reconcile compiler policy and presets | D1/D2 and Foundation tests pass on MSVC; both generators configure/build/test; physical-GPU clear/fullscreen evidence recorded |
| D3a (implemented; Windows execution pending) | Raster resources, bindings and DXIL interfaces | Resource/binding suites; bounded allocation/descriptor failure; safe retirement while work is in flight |
| D3b | Complete portable raster profile | Shared raster/image suites on WARP and hardware; required raster profile admitted |
| D4a | Transfers, readback and lifetime | Shared transfer/lifetime suites; offscreen rendering, cancellation and failure preservation |
| D4b | Compute and ordered render graphs | Compute/graph suites, mixed graphics/compute/transfer journeys; required compute profile admitted |
| D5a | Windows host services and Foundation validation | Native input, entropy, files/persistence/watch tests; diagnostics and Foundation runtime evidence |
| D5b | GPU measurement and debugging | Timestamp/pass-scope tests; PIX capture/markers and DRED failure diagnostics documented |
| D6a | Renderer, content and playable runtime | Shared authored project renders, accepts input, loads/saves and shuts down on a physical Windows GPU |
| D6b | Installed SDK, projects and IDE journeys | Clean installed consumer; VS Code/Ninja and Visual Studio/MSBuild configure/build/test/debug; setup repair journeys |
| D6c | Shader/script authoring, worker and live reload | Embedded/external edit-save-compile-reload; diagnostics, conflicts and failed replacement tests |
| D6d | Interactive Windows editor and hosted viewport | Current project World/camera, navigation/edit/resize; GPU producer/consumer ownership and pending/failure publication tests |
| D7 | Profile qualification and continuous validation | All required coverage rows verified at one candidate commit; hardware, lifecycle, performance and both build workflows recorded |
| D8 | Release packaging and distribution | Clean-machine install/launch, upgrades/data preservation, dependency/license inventory and matching symbols |

D1 and D2 remain historical completed slices under Clang/WARP, not MSVC exit
evidence. D0 precedes all new Windows implementation. D3a -> D3b -> D4a -> D4b
is the graphics dependency chain; D5a can progress after D0 alongside graphics.
D5b requires the submission/pass foundation from D4. D6a requires D4b and D5a;
D6b and D6c build on D6a, and D6d integrates them with hosted presentation.
D6a establishes a runnable sample for its recorded scope; D7 establishes the
qualified profile and D8 the distributable artifact under the shared
[admission gates](../development/platform-porting.md#admission-gates). Shared
code must retain Linux/macOS/browser regression coverage throughout.

### D0: establish MSVC and both build generators

- Declare the x86_64 baseline's required runtime/editor/renderer profiles,
  physical-GPU support matrix and minimum OS/dependency requirements. Assign
  subsystem owners and distinguish required functionality from optional
  feature-parity extensions before selecting implementation scope.
- Audit C++23 language/library support, compiler detection, intrinsics, alignment,
  calling conventions, assertions/debug breaks and export boundaries. Keep the
  exception-free engine and test-only exception boundary, strict language mode,
  warnings-as-errors, source policy and build-time budgets. Add equivalent MSVC
  warning controls; accepting `cl.exe` must not silently remove warning coverage.
- Pin the tested toolset, Windows SDK and CRT/dependency variants, including
  Conan profiles and Qt/Slang host-tool compatibility where applicable. Reconcile
  [ADR 0025](../decisions/0025-llvm-toolchain-pin.md), contributor rules, setup,
  SDK identity and Behavior compiler admission in the implementation change;
  retain LLVM formatting/tidy and other platforms' compiler profiles.
- Add selectable configure/build/test presets and Windows setup/repair for
  Ninja/MSVC and Visual Studio/MSBuild. Make flavor/configuration, generated
  shader dependencies and File API artifact resolution work for both. Validate
  a single-file edit and shader/include edit trigger the correct incremental
  rebuild. Discover developer tools without requiring one particular IDE.
- Port/re-run the existing Foundation, Win32, lifecycle and DXIL fullscreen tests
  under MSVC on WARP and physical hardware. Existing Clang checks remain until
  equivalent MSVC coverage is established. Later milestones extend both build
  workflows; the current limited module closure must not be advertised as a
  complete installed SDK.

#### D0 primitive portability probe

The first implementation slice removes LLVM overflow builtins from
FoundationBase's public checked-integer helpers. They check the representable
bounds before evaluating arithmetic, retaining constexpr evaluation, noexcept,
output preservation and operand/output aliasing. No `<limits>` or other new
standard-library facility enters the public header. The
[numeric boundary owner](primitive-types.md#final-boundary-contracts) records
the shared contract and test coverage.

`tests/build_contract/primitive_portability` is an isolated CMake project. It
builds the production type representation checks, checked-integer and byte-order
tests, plus a compiler abstraction test. It does not configure the engine or
export an SDK. The Windows workflow adds a separate native `cl.exe` job using
the MSVC 14.44 candidate, SDK 10.0.26100.0 and dynamic CRT, with MSVC-built Catch2
from the locked recipe. It exercises Ninja Debug/RelWithDebInfo builds and both
configurations in one generated Visual Studio 2022 solution. Candidate compiler,
toolset and SDK identities and CTest logs are uploaded as
`windows-msvc-primitive-probe`.

The probe's host CMake is separately pinned to 3.31.10 in
`config/tool_versions.json` (`windows_msvc_probe.cmake`). The common managed
3.29.6 build cannot select the explicit 14.44 minor version of v143; the newer
generator recognizes it. This leaves the existing platform tool pins unchanged
while the D0 toolchain audit continues.

The candidate uses `/std:c++23preview`, `/permissive-`, `/Zc:preprocessor`,
`/Zc:__cplusplus`, `/W4` and `/WX`. CMake's default mapping of C++23 to
`/std:c++latest` would also admit later draft features, so only this isolated
probe overrides that mapping. Microsoft's
[/std reference](https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version)
describes the C++23-specific preview mode. This is compatibility evidence for
the tested primitives, not full C++23 conformance or an engine toolset pin.

To reproduce on Windows, enter an x64 Visual Studio 2022 developer command
prompt selecting toolset 14.44 and SDK 10.0.26100.0. Install the probe's CMake
and the managed Ninja/Conan pins from `config/tool_versions.json`, export
`third_party/catch2`, and install
the locked `catch2/3.4.0` with
`config/conan/profiles/windows-msvc-probe-x86_64` for host and build. Generate
`CMakeDeps` into one dependency directory for Debug and RelWithDebInfo. Configure
the probe with `-DCMAKE_PREFIX_PATH=<dependency-directory>`, then build and run
CTest with `--config`/`-C` selecting the intended configuration. Use separate
build trees for Ninja and Visual Studio; pass `-A x64`, the selected full
toolset version with `-T`, and `-DCMAKE_SYSTEM_VERSION=10.0.26100.0` for Visual
Studio. The workflow contains the complete commands.

D0 remains in progress. The production FoundationBase compiler gate, flavor
headers/SDK identity, Behavior admission, full engine MSVC warning policy,
selectable engine presets, setup/repair, WARP runtime tests and physical GPU
acceptance are unchanged. The candidate's actual patch versions must be reviewed
and pinned after compatibility results; the 14.44 family selection is not that
final pin. A successful primitive probe does not qualify the engine's
multi-configuration build or complete the D0 acceptance criteria above.

### D3 and D4: complete the portable graphics contract

D3a implements buffers, textures, views, samplers, descriptor heaps, binding
layouts/sets, truthful format/capability queries and raster DXIL reflection.
Establish submission fences and descriptor/resource retirement before general
use. D3b adds vertex/index input, pipelines, depth/stencil/blending, render
attachments/passes and transitions for the declared renderer profiles. Share
conformance fixtures with Vulkan/Metal, reject unsupported descriptions and
record image tolerances and debug-layer results.

D4a adds staged buffer/texture upload/update, asynchronous readback, completion
polling and shutdown/cancellation, including `ShouldPresentToSurface = false`.
Test multiple frames in flight, output preservation on failure and resource
reuse after completion. D4b adds compute DXIL/pipelines, storage bindings,
dispatch, barriers and ordered graph execution. Test compute-to-raster,
raster-to-readback and mixed transfer paths through ordinary public contracts.
Only advertise a required profile after its suite passes; keep unavailable
hooks explicitly `Unsupported`.

### D5: close Windows platform and diagnostics gaps

D5a delivers Win32 key mapping/delivery, focus-loss recovery, system entropy,
filesystem mounts/reads/writes, durable publication and required watch behavior.
Audit the existing Windows async filesystem implementation before replacing
anything. Verify Unicode/spaced paths, permissions, interrupted saves, corrupt
packs, cancellation and bounded failures. Revalidate FoundationBase diagnostics,
memory/alignment/allocation failure, containers, time/threading, logging and
profiling under MSVC; prior Clang evidence remains separately identified.

D5b delivers GPU frame/pass timestamps with valid frequency/completion/failure
handling as required by the public contracts, PIX markers/capture and DRED
collection on device failure. Record real capture and diagnostic artifacts,
including behavior when optional capture/debug tooling is absent. Follow the
[graphics debugging owner](graphics-debugging.md).

### D6: deliver runtime and developer workflows

- **D6a (runtime):** expand the top-level Windows module closure in dependency
  order, admitting the renderer/graph, content/resources, World/camera, text/UI
  and game host. Audit gameplay/physics, audio, networking and behavior/scripting
  separately; verify portable code and implement necessary OS boundaries for
  each required profile. Use the same cooked authored sample and inputs as other
  platforms; do not invent lights or substitute a fixed demonstration scene.
- **D6b (SDK/IDEs):** install/export the SDK and shader helpers, then build an
  external project in VS Code/Ninja and a generated Visual Studio/MSBuild
  solution. Exercise selectable presets, code navigation, diagnostics,
  breakpoint/step debugging, launch, test, install and configuration switching.
  Cover fresh clones, missing presets, moved tools/SDKs and repeated explicit
  repair; preserve custom settings and keep project loading read-only.
- **D6c (authoring):** port the shader worker's process/IPC/deployment and
  Slang/DXC dependencies, file watching, game DLL loading/reload, process-tree
  cleanup and matching symbol validation. Qualify script compilation/runtime,
  supported reload and exposed debugging features. From embedded shader/script
  editors and external editors, test successful edits, syntax/interface errors,
  include changes, replacement-on-save, concurrent edits, worker timeout/crash,
  cancellation and stale responses. Share diagnostics and publication paths;
  failed replacements preserve accepted state and the last drawable viewport.
- **D6d (editor):** port Qt host integration and hosted-window transport plus
  D3D12 native interop/shared presentation with explicit handle ownership,
  adapter identity, producer/consumer synchronization and teardown fencing.
  Validate DPI/display changes, focus, dialogs, paths and process cleanup.
  The viewport must render the selected project's current World/camera and
  respond to navigation, authored edits and resize. Test pending, failure and
  successful publication while retaining the last drawable view until Ready.
  CPU readback is diagnostic evidence, not interactive GPU presentation
  acceptance; optional path tracing cannot be required for raster navigation.

### D7 and D8: qualify and distribute

D7 extends MSVC/Ninja batch validation to all required modules, SDK consumers,
editor, worker and runtime tests, with MSBuild coverage of the admitted closure
and generator-sensitive journeys. Keep integration PRs targeting `develop`;
only the existing batch/promotion process qualifies and advances `main`.
Record one candidate commit, exact toolchain/SDK/OS/driver versions, physical
adapters from the declared support matrix, required profiles, feature limits
and reproducible tests. WARP coverage supplements physical-GPU evidence.
Exercise restart, resize/minimize, device loss/failure recovery, resource and
performance budgets, debugger/symbol journeys and available sanitizer tooling;
report unavailable instrumentation explicitly. Every required subsystem row
must have an owner, implementation PR and accepted evidence before qualification.

D8 produces Release SDK/game/editor packages as scoped, with redistributable
runtime/Qt dependencies, manifests, assets, shader artifacts, licenses and
matching symbols. Validate clean-machine installation and launch, update/data
preservation, uninstall and failure diagnostics; validate signing for the
chosen distribution channel. Ship canonical Markdown and checked HTML together
for offline use through the existing documentation packaging workflow.

### Optional feature parity

Track DXR/ray queries, wider native interop and Windows spatial-upscaling
providers as separately scoped extensions with their own capability, shader,
lifetime and physical-device suites. Editor presentation interop is required
by D6d; optional interop extensions cannot substitute for it. An extension is
mandatory when an admitted renderer/project profile promises it. Keep unsupported
features explicit and do not call the baseline port full feature parity while
these rows remain pending.

## Subsystem coverage

| Subsystem / required profile | Owner | Status | Blocker / next action | PR | Evidence / commit |
| --- | --- | --- | --- | --- | --- |
| FoundationBase / target ABI | threading T3; D0 pending | verified (Clang only) | Revalidate with MSVC in D0 | threading T3 | [threading evidence](../development/threading-evidence.md#platform-completion-t3) |
| Toolchain / build configuration (Clang) | D1, D2 | verified (WARP) | Retain historical scope; MSVC migration tracked separately | PR #573; PR #586 | Windows lane, PR #573 (`7167c8a2`) |
| MSVC / Ninja / MSBuild / configuration mapping | unassigned | needs port | D0; pins, compiler gates, presets and policy reconciliation | pending | none |
| Memory / containers / diagnostics | unassigned | unreviewed | D0 baseline and D5a runtime/failure audit under MSVC | pending | none |
| Time / threading | threading T3; D0 pending | verified (Clang only) | Re-run MSVC suites in D0/D5a | threading T3 | [threading evidence](../development/threading-evidence.md#platform-completion-t3) |
| Logging / profiling | unassigned | needs port | D5a; closure builds, Windows suites still needed | pending | none |
| Filesystem / persistence / watches | unassigned | needs port | D5a; audit existing Windows async support | pending | none |
| Platform / host (Win32 windows) | D1; D0 pending | verified (Clang/WARP) | Revalidate in D0; hosted channel in D6d | PR #573 | Windows lane, PR #573 (`7167c8a2`) |
| Input / system entropy | unassigned | needs port | Win32 key delivery/focus recovery and entropy in D5a | pending | none |
| RHI / graphics (clear/present) | D1; D0 pending | verified (Clang/WARP) | MSVC and hardware-adapter evidence in D0 | PR #573 | Windows lane, PR #573 (`7167c8a2`) |
| RHI / graphics (DXIL/fullscreen) | D2; D0 pending | verified (Clang/WARP) | MSVC and hardware-adapter evidence in D0 | PR #586 | [Windows lane](https://github.com/The-Ludus-Project/Ludus/actions/runs/40102938249) |
| RHI / raster resources and bindings | D3a | implemented; execution pending | Run the resource/binding suite on Windows; MSVC baseline still pending | pending | Local cross-compilation and portable tests only |
| RHI / raster rendering | D3b | needs port | Pipelines, passes and shared raster images | pending | none |
| RHI / transfers, lifetime, compute and graph | unassigned | needs port | D4a/D4b | pending | none |
| GPU timing / PIX / DRED | unassigned | needs port | D5b | pending | none |
| Renderer / graph | unassigned | unreviewed | D6a after D4b/D5a | pending | none |
| Text / localization / UI | unassigned | unreviewed | D6a required-profile audit and sample | pending | none |
| Content / resources | unassigned | unreviewed | D6a cook/load/error journeys | pending | none |
| Runtime / game host | unassigned | unreviewed | D6a boot/shutdown; D6c DLL reload and symbols | pending | none |
| Behavior / scripting | unassigned | unreviewed | D0 compiler admission; D6a runtime; D6c authoring/reload | pending | none |
| Gameplay / physics | unassigned | unreviewed | D6a required-profile audit | pending | none |
| Audio | unassigned | unreviewed | D6a playback/device/failure evidence for required profiles | pending | none |
| Networking | unassigned | unreviewed | D6a transport/reconnect evidence for required profiles | pending | none |
| SDK / project setup / VS Code / Visual Studio | unassigned | needs port | D0 generator foundation; D6b installed consumers and IDE journeys | pending | none |
| Embedded/external shader/script authoring | unassigned | needs port | D6c worker/IPC/watch/reload and diagnostic acceptance | pending | none |
| Editor / hosted viewport | unassigned | needs port | D6d GPU sharing, transport and interactive publication | pending | none |
| Optional ray / interop extensions / upscaling | unassigned | unreviewed | Separate feature-parity profiles and hardware acceptance | pending | none |
| Qualification / CI / documentation | D1, D2; D7 pending | in progress | Clang lane gates promotion today; D7 qualifies MSVC profiles | PR #573; PR #586 | Historical D1/D2 evidence above |
| Packaging / distribution | unassigned | unreviewed | D8 after D7 | pending | none |

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
- Thanks to Kitware, [Visual Studio generator](https://cmake.org/cmake/help/v3.29/generator/Visual%20Studio%2017%202022.html),
  [CMake build-system model](https://cmake.org/cmake/help/v3.29/manual/cmake.1.html#introduction-to-cmake-buildsystems)
  and [configuration types](https://cmake.org/cmake/help/v3.29/variable/CMAKE_CONFIGURATION_TYPES.html):
  the planned shared CMake graph, generated MSBuild projects and explicit
  multi-configuration acceptance follow these contracts.
