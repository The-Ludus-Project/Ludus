# Graphics debugging architecture and implementation plan

Status: proposed implementation contract, 2026-10-20. The phases below are not
implemented or qualified by this document. The first delivery is shared GPU
names/scopes followed by native Metal and RenderDoc capture control.

Shader and renderer developers should be able to run the selected project in
Ludus, capture the GPU work responsible for its image, and inspect that capture
in an external graphics debugger. Ludus supplies meaningful names, correct
capture boundaries, process selection and reproducible artifacts. External tools
own resource inspection, pixel history, shader stepping and hardware analysis.

## Existing owners and baseline

The [RHI contract](rhi-gdi.md) owns device, command, resource and completion
lifetime; the [public rendering guide](../development/fullscreen-rendering.md)
records implemented rendering profiles. Native backends are Vulkan on Linux and
Metal on macOS; browsers use WebGPU or WebGL 2. Windows/D3D12 and mobile capture
integration require their platform backends and qualification first.

The [reconciled profiling architecture](profiling-final.md) owns CPU traces,
counters and proposed GPU timestamp collection. This design owns external
graphics-debugger integration. GPU annotation must work without CPU trace
recording or timestamp queries; a CPU scope around a draw does not measure GPU
execution time. Existing reference-image capture is a rendering product, not an
external debugger capture.

The [developer tools design](developer-tools.md) owns external process launching
and local tool preferences. The [editor GUI systems contract](editor-gui-systems.md)
owns hosted producer/consumer images and viewport publication. Capture adapters
must preserve those owners rather than introduce another resource registry,
process supervisor or viewport publication transaction.

At this baseline, graph pass descriptions already carry authored diagnostic
names. Broad native object naming, nested GPU annotations, a graphics-capture
request service and editor capture controls remain work in this plan. Finding an
installed tool or a single existing Metal label does not establish support.

## Decisions and initial scope

- Implement backend-neutral GPU object names and command scopes first. Carry
  authored pass identity through graph compilation and pass merging.
- Deliver Metal capture on macOS and RenderDoc capture on Linux/Vulkan first.
  They share request/status semantics but retain distinct native adapters.
- Separate capture enablement at process startup from a later capture request.
  Missing injection or capture configuration can require restarting the target.
- Keep frame capture, timing traces and GPU crash dumps separate capabilities.
  No universal capture-file format or universal begin/end API is assumed.
- Keep tool installation optional. Opening projects, running doctor and normal
  builds must not install debuggers, elevate privileges or launch viewers.
- Preserve rendering when tooling is unavailable or capture fails. Capture does
  not change the selected backend or enable a different renderer silently.

The first release covers a single submitted raster frame, including its uploads
and compute work where supported by the selected backend. Shader source display
is a qualification target; original Slang source stepping, ray-tracing debugging,
multi-frame traces and hosted shared-image replay are separately qualified.

## Responsibility boundaries

| Owner | Responsibility |
| --- | --- |
| GraphicsRhi | Resource names; command/queue scopes; native frame boundaries; private capture hooks tied to the live device session. |
| GraphicsRenderer | Meaningful pass, view, material and shader identities; graph-to-executed-work correspondence. |
| Graphics capture service | Bounded requests, session identity, state transitions, adapter selection and artifact metadata; no draw/resource lifetime ownership. |
| Tooling and editor launch services | Tool discovery, startup environment/injection, target process selection, local preferences and opening a completed artifact. |
| Existing editor/game transport | Versioned capture requests and replies to the rendering process; no native GPU handles crossing IPC. |
| FoundationProfiling | Optional correlation IDs and GPU timestamps under its existing design; no vendor capture dependency. |

Implement the capture service initially as a narrow responsibility beside the
RHI session, with private platform translation units. Do not create a new module
or general diagnostics framework just for dispatch. Expose only the minimal
Ludus-owned request/capability/status vocabulary needed by SDK consumers. Native
handles, RenderDoc/PIX headers, Objective-C objects and loader details remain
private. Confirm dependency direction before implementation; FoundationBase and
FoundationProfiling must not acquire a graphics or tool-installation dependency.

All engine operations use explicit status results and `noexcept` where
applicable. Public declarations require meaningful Doxygen contracts. Use
existing bounded text/views, containers, checked numeric conversions and process
facilities; no STL container, formatting or exception-based convenience layer.

## Shared GPU names and scopes

### Naming and identity

Name devices/queues, command buffers, buffers, images/views, shaders and
pipelines where the backend supports it. Names identify an engine role and a
stable diagnostic ID; native addresses are not persistent identity. Examples
include `World/MainView/Opaque`, `Material/<identity>/BaseColor` and
`Frame/<sequence>/Upload`. Avoid constructing per-draw strings by default.

Reuse existing graph pass-name ownership and bounds. Introduce a single owner
for any additional bounded diagnostic text type, with documented byte encoding,
termination and overflow behavior. Copy borrowed names before their storage can
expire. Reject invalid text at the public boundary; when native display limits
require shortening, retain the distinguishing ID and report shortening outside
the recording hot path. Renaming must not alter resource identity or lifetime.

Renderer identity generation happens at resource creation, graph preparation or
an explicit rename. Recording reuses prepared names, performs no engine heap
allocation and does no file I/O. Measure enabled/disabled overhead on the same
workload; native tool overhead is additional and must not be described as free.

### Scope placement and pairing

Record frame, graph, physical pass, logical pass, upload and dispatch scopes at
the command-recording sites that execute the work. Include queue and frame
identity for correlation. Keep physical pass groups and authored logical pass
names distinguishable after merging; culled passes belong in graph diagnostics,
not as fictitious GPU execution. Backend encoder constraints determine legal
nesting and may require several native regions for one logical operation.

Pair begin/end on the same recording context and owner thread. Do not span
coroutine suspension, command-buffer reset, submission or device shutdown with
an RAII scope. Early failures must unwind the recorded scope stack. Bound nesting
and diagnose mismatches without corrupting commands or changing rendering.

| Backend | Native mapping |
| --- | --- |
| Vulkan | Optional `VK_EXT_debug_utils`; object names, command labels and queue labels through loaded extension entry points. Missing extension support disables annotations only. |
| Metal | Object `label` properties and legal command-buffer/encoder debug groups or signposts. Labels do not extend borrowed resource lifetime. |
| WebGPU | Object labels and encoder/pass debug groups where available through the pinned port. |
| WebGL 2 | Capability-checked annotation facilities or tool-side metadata. Never require a desktop GL extension. |
| Future D3D12 | Object names plus PIX command-list/queue events; CPU PIX events are a separate instrumentation choice. |

Name/scope capability is independent of debugger presence, capture ability,
timestamps and validation. Annotation enablement must not require recording a
CPU trace. Validation layers and shader validation have separate controls and
their overhead must be recorded in capture metadata.

## Capture request and lifetime contract

Capabilities are queried for a particular process, device session, backend and
tool activity. Distinguish installed tooling, runtime attachment/enablement,
frame capture, explicit-region capture, timing, offscreen workloads and viewer
availability. An installed viewer does not imply a capturable live session.

Each request has a monotonically changing session generation and request ID,
an explicit rendering target, capture kind, bounded destination and frame
selection. The initial policy admits one active capture per process and rejects
competing tools/requests with Busy; do not compose capture layers without a
separately tested combination. Metadata records the native capture scope when a
device-wide capture includes more work than the selected view.

| State | Meaning and permitted next transition |
| --- | --- |
| Idle | No request owns the capture adapter; admit or reject a request. |
| Armed | Validated and waiting for an eligible frame; move to Capturing, Cancelled or Failed. |
| Capturing | Native capture has started; complete recording, or perform adapter cleanup after cancellation/failure. |
| Finalizing | Native stop was issued; verify completion and collect artifacts before releasing request ownership. |
| Complete | Capture artifact and its manifest are usable; opening the viewer is a separate operation. |
| Failed or Cancelled | Terminal outcome with a reason and any explicitly identified partial artifact; never report a previous capture as this request's success. |

Execute native transitions on the thread owning the RHI session. Marshal UI/IPC
requests to that owner rather than calling vendor APIs from arbitrary threads.
Invalidate stale replies/callbacks using session and request identity. Shutdown
must stop or abandon a capture using the adapter's documented cleanup before
releasing the native objects it borrows. The adapter must not stop a capture
owned by an external user/tool session.

An eligible frame has an acquired drawable/offscreen target and reaches the
normal recording/submission path. Hidden, zero-size, skipped or unready frames
do not consume an armed request. Apply a bounded deadline so a permanently
hidden target cannot remain armed indefinitely. Report which frame was actually
captured, separately from the frame displayed when the request was made.

Start before creating/recording the commands to include; stop after their
submission at the adapter's required boundary. Metal requires command buffers
to be created and committed inside the capture interval. Capture-specific
draining, if required by a tool, is explicit and recorded; do not add a normal
per-frame GPU wait or synchronous timestamp readback.

Errors distinguish UnsupportedBackend, ToolUnavailable, RestartRequired,
Busy, InvalidTarget, InvalidDestination, DeadlineExceeded, DeviceLost and
NativeCaptureFailed, with native details in bounded diagnostics. Names are
design vocabulary, not declarations of an existing SDK API. Failed/cancelled
captures preserve the last drawable viewport and do not publish a pending or
failed replacement view.

Write into a fresh request-owned output directory under ignored `out/` by
default. Never overwrite captures or unrelated files. Publish a manifest only
after the adapter's completion condition and artifact verification; a directory
entry alone is insufficient. If the vendor format has no semantic completion
query, document the native stop/file checks used and retain viewer-open acceptance
as a separate qualification gate. Preserve the capture if manifest export fails,
and report that outcome explicitly.

The versioned manifest records engine revision/build profile, tool/runtime
identity, OS/GPU/driver, process role, backend, device session, requested/actual
frame IDs, world/view identity, renderer profile, shader artifact hashes,
validation/debug/optimization settings and relative artifact locations. Copy
only the relevant launch settings; do not serialize credentials or the process
environment wholesale. Packaging and deletion are explicit user actions.

## Initial native adapters

### Metal on macOS

Use `MTLCaptureManager` and `MTLCaptureDescriptor` to capture the selected device
or queue to `MTLCaptureDestinationGPUTraceDocument`, after checking destination
support. Save a `.gputrace` and open it in Xcode only on request. Use the
Objective-C status/`NSError` path; do not translate Apple's Swift exception
examples into C++ exceptions.

Enable programmatic capture through the development app's `MetalCaptureEnabled`
property, or `MTL_CAPTURE_ENABLED=1` on macOS 14 and later. Supply startup
configuration before Metal initialization. Keep final distribution enablement
an explicit build choice. A process already being captured is Busy; do not stop
another capture. Check the complete create/commit interval for uploads, render
and compute command buffers, including work without a presentation call.

File capture does not require an open Xcode GUI. Xcode provides the supported
interactive inspection workflow; feature availability depends on the installed
Xcode/macOS/GPU combination. New Apple command-line capture/debugging tools may
be exposed later after availability checks, without replacing this adapter.

### RenderDoc on Linux and Vulkan

Launch the intended rendering executable under the selected RenderDoc capture
environment before Vulkan initialization. Detect its already loaded runtime and
negotiate `RENDERDOC_GetAPI` once, early and on the owning thread. Pin the
reviewed application header/API minimum in the integration; record the actual
runtime version. Do not assume loading a library into an already initialized
Vulkan process retroactively intercepts its device.

Use `StartFrameCapture`/`EndFrameCapture` for explicit RHI frame intervals,
including offscreen frames. Supply the documented Vulkan device-pointer form
and relevant window, rather than assuming every native handle can be cast to
the application's generic device pointer. Use the API's capture result/path
queries to identify the new `.rdc`; never scan for the newest unrelated file.

The editor/CLI launcher owns injection and viewer launch. The runtime adapter
owns only capture control and borrowed API pointers. Missing runtime attachment
returns RestartRequired with the external-launch remedy. Negotiate only calls
available in the selected API version; capture/replay of extensions, ray queries
and shared external memory requires explicit feature qualification.

## Editor, hosted game and SDK workflow

Offer explicit target roles: editor scene viewport, hosted game producer and
editor UI/compositor. Show backend, selected tool and capability before arming.
Route a hosted-game request over the existing versioned transport to the game;
the editor's capture of a sampled shared texture cannot reveal the producer's
draws. Include producer frame sequence and surface generation in correlation
metadata. Do not imply one process capture includes the other process's work.

Qualify hosted external-memory capture and replay independently on each native
backend. A tool limitation must have a visible reason and a standalone launch
of the same project/world/renderer as the fallback. Retain shared-image
producer/consumer fencing and leases while capturing. Resize, project switch,
process exit and device loss must cancel or finish requests without releasing a
surface still used by the other process.

The editor exposes launch with capture support, capture next frame, cancel,
capture status, reveal artifacts and open in viewer. Persist tool paths and
machine choices only in local ignored settings. Reuse existing process argv,
environment and working-directory handling; never assemble a shell command from
project data. Explicit invalid preferences fail instead of selecting another
tool. SDK applications and future CLI controls use the same request contract;
this proposal does not claim new commands already exist.

## Shader artifacts and performance interpretation

Keep generated MSL/SPIR-V/WGSL, entry points, source/include identity, compiler
versions/options, reflection and hashes with the existing shader artifact owner.
Capture manifests reference that identity instead of creating a competing shader
cache. Preserve required source/debug sidecars for the lifetime of a saved
capture, with explicit packaging and pruning behavior.

Shader-debug mode preserves source/debug information and uses the appropriate
optimization policy; performance mode retains representative optimization and
useful names/symbols. Record the mode and any unavailable source mapping.
Generated MSL display does not prove original Slang stepping. Verify vertex,
fragment and compute source visibility, then qualify stepping separately.

Frame debuggers can perturb scheduling and replay workloads. Reproduce an image
defect there; validate performance conclusions on representative optimized runs
with appropriate timing/profiling tools. Shared names connect those workflows
without conflating their data or completion conditions.

## Mac developer setup

Install a compatible full [Xcode distribution](https://developer.apple.com/xcode/)
for interactive Metal inspection. Open it once and complete its requested setup
and components. The standalone Command Line Tools package does not include the
Xcode Metal debugger UI. CMake/Ninja and Ludus's pinned compiler policy remain
the build workflow; using Xcode to inspect captures does not require migrating
the engine to an Xcode project or replacing the reference LLVM toolchain.

Inspect the currently selected tools with `xcode-select -p`. To check a specific
Xcode without changing the machine-wide selection, use a per-command developer
directory, adjusting the path to the installed app:

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild -version
```

Tool discovery should report missing Xcode/components and a remedy, without
automatically downloading them or changing `xcode-select`. Once the Metal phase
is delivered, capture from Ludus and explicitly open the generated `.gputrace`
in Xcode. Before then, use Xcode's external executable capture workflow where
applicable; installing Xcode alone does not implement Ludus's capture button.
RenderDoc is the initial Linux/Vulkan workflow, not the debugger for Ludus's
native Metal backend. PIX GPU capture requires Windows/D3D12.

## Phased implementation and acceptance

Every phase is pending. Implement in isolated reviewable changes targeting
`develop`, following [CI qualification](../development/continuous-integration.md).
Acceptance into `develop` is distinct from full batch/platform qualification.

| Phase | Deliverable and dependency | Exit gate |
| --- | --- | --- |
| G0 Shared GPU annotations | RHI names/scopes, authored graph identity, Vulkan/Metal mappings, explicit browser capability behavior. | Native captures show useful resource and logical/physical pass names; disabled/missing annotation paths preserve rendering. |
| G1 Capture contract | Session/request ownership, bounded state machine, capabilities, destinations and manifest publication. Depends on G0 identities. | Production-path tests cover admission, skipped frames, deadlines, cancellation, stale replies, shutdown and output preservation. |
| G2 Metal capture | Programmatic `.gputrace`, startup enablement and standalone SDK fixture. Depends on G1; first local platform adapter. | A real Mac capture opens in Xcode, contains the intended frame and exposes named resources/passes and recorded shader-source capability. |
| G3 RenderDoc capture | Linux/Vulkan launch attachment, version negotiation and explicit frame capture. Depends on G1; can follow the same contract independently of G2. | A real `.rdc` replays in the tested RenderDoc version with named resources/passes, correct offscreen boundaries and explicit feature limits. |
| G4 Editor and hosted target control | Shared launch/request controls, IPC identities, target selection and standalone fallback. Depends on qualified G2/G3 adapters. | Selected-world viewport and hosted-game captures target the correct process through edits/navigation/resize; failed capture retains the last drawable view. |
| G5 Shader investigation and automation | Debug/performance shader artifacts, source mapping evidence, CLI/SDK controls and reproducible capture bundles. | Installed-SDK reproduction opens without the developer's original build tree; source-display/stepping claims and overhead have recorded evidence. |
| G6 Additional platform and vendor adapters | PIX/D3D12, Android, browser tooling, vendor profiling and crash diagnostics. Depends on each platform and feature qualification. | Each adapter has its own tested capability matrix, lifecycle tests, artifact/viewer acceptance and documented limitations. |

### G0 and G1 implementation slices

1. Inventory existing resource records, authored graph names, native command
   boundaries and browser APIs. Define name capacity/ownership and capability
   behavior in the RHI owner; add public comments with their declaration change.
2. Add native naming at creation/rename and nested scopes at real execution
   boundaries. Test merged/culled passes, early failure and resource destruction.
   Verify an external capture manually before extending naming granularity.
3. Add the minimal capture session/request owner with private adapter hooks.
   Exercise it through the real frame/session path, using a controlled adapter
   for deterministic failures; a test-only imitation of the state machine is
   insufficient. Keep tool discovery and file publication off recording paths.
4. Test Busy, external capture ownership, invalid destination, unavailable tool,
   no eligible frame, device loss, restart, stale generation and cancellation
   during finalization. Specify adapter cleanup before adding UI controls.

### Native adapter and editor evidence

For G2/G3, cover onscreen and offscreen frames, uploads and compute where
supported, minimized/zero-size windows, repeated capture, resize and native
capture failure. Inspect the resulting image, event hierarchy, buffers/textures
and shader source in the actual viewer. A nonempty file and passing mock tests
do not establish replay, source stepping or GPU correctness.

For G4, exercise the selected project's current World and camera during edit,
navigation, shader reload, resize and project replacement. Independently test
editor rendering and hosted producer rendering, including surface generation
retirement and process disconnect. Capture failure must preserve the last
drawable view and the existing Pending/Ready publication semantics.

Each implementation PR supplies focused tests, required formatting/source
policy/static analysis, relevant native/browser builds, sanitizer evidence and
installed-SDK acceptance. Record tool/runtime versions, GPU/driver, OS, shader
profile, capture scope and viewer result in the owning validation record when
qualification is performed. Mark unavailable hardware/tool checks pending;
never infer Windows, Android, browser or ray-tracing support from a Mac capture.

## Later adapters and decision gates

| Tool/activity | Planned integration boundary |
| --- | --- |
| PIX | Windows/D3D12 prerequisite. Load the PIX GPU capturer before D3D12 calls; use next-frame or explicit-region APIs and target-window selection. Timing capture has separate library/privilege requirements. WinPixEventRuntime alone does not supply the capturer. |
| Nsight Graphics | Start with external launch and standard annotations. Optional SDK control for graphics capture/GPU traces remains version-gated while the API is beta; initialize before the graphics context and keep external launch available. |
| Radeon tools | Standard labels plus tested Radeon Developer Panel/CLI recipes for RGP, RMV and RRA. Driver and activity support are separate capabilities. |
| Android APA and AGI | Qualify the Android backend, development packaging, device/layer setup, Vulkan/ATrace annotations and artifact retrieval. APA is recommended for system profiling; its experimental frame profiler is not a universal baseline. Retain tested RenderDoc/AGI recipes. |
| Browser tools | WebGPU Inspector for WebGPU and Spector.js for WebGL; optional development-only instrumentation before API creation. Qualify workers/iframes and the actual pinned Emscripten port. |
| Hardware-specific profiling | Arm Performance Studio, Snapdragon Profiler and Intel GPA initially use shared names plus documented supported-device workflows. |
| GPU crash analysis | Optional Aftermath, Radeon GPU Detective and future D3D12 DRED integration under crash-report ownership. Keep matching shader artifacts; do not label a crash dump as a frame capture. |

Before implementation, resolve any missing native capture API version, artifact
completion condition or hosted replay support with a small feasibility fixture.
Record the result in this design's relevant contract or the existing validation
owner. Do not weaken renderer synchronization, disable a supported backend or
add an SDK runtime dependency merely to make one debugger attach.

## References and attribution

Thanks to the following authors and organizations for the capture, annotation
and investigation contracts adapted above. Future implementation must retain
concise attribution beside the relevant native adapter and link back here.

- Khronos Group, [Vulkan Debug Utilities Extension](https://docs.vulkan.org/guide/latest/extensions/VK_EXT_debug_utils.html): object names and command/queue regions.
- Baldur Karlsson and RenderDoc contributors, [In-application API](https://renderdoc.org/docs/in_application_api.html) and [application API header](https://github.com/baldurk/renderdoc/blob/v1.x/renderdoc/api/app/renderdoc_app.h): runtime negotiation, explicit capture boundaries and capture enumeration.
- Apple, [Capturing a Metal workload programmatically](https://developer.apple.com/documentation/xcode/capturing-a-metal-workload-programmatically) and [MTLCaptureManager](https://developer.apple.com/documentation/metal/mtlcapturemanager): enablement, command-buffer intervals and trace destinations.
- Apple, [Metal developer tools](https://developer.apple.com/metal/tools/): Xcode/Metal inspection and platform tool roles.
- Microsoft PIX team, [Programmatic Capture](https://devblogs.microsoft.com/pix/programmatic-capture/): startup loading, GPU/timing separation and target-window selection.
- NVIDIA, [Nsight Graphics SDK User Guide](https://docs.nvidia.com/nsight-graphics/UserGuide/sdk.html): optional beta SDK and context initialization; [Nsight Aftermath](https://developer.nvidia.com/nsight-aftermath): GPU crash dumps.
- AMD GPUOpen, [Radeon Developer Panel](https://github.com/GPUOpen-Tools/radeon_developer_panel) and [RGP User Debug Markers](https://gpuopen.com/manuals/rgp_manual/user_debug_markers/): activity-specific capture and marker interoperability.
- Android Developers, [AGI system profiling](https://developer.android.com/agi/sys-trace/system-profiler) and [APA Frame Profiler](https://developer.android.com/android-performance-analyzer/frame-profiler): system/frame distinction and current experimental scope.
- Brendan Duncan, [WebGPU Inspector](https://github.com/brendan-duncan/webgpu_inspector), and Babylon.js contributors, [Spector.js](https://spector.babylonjs.com/): browser API inspection and capture.
- Arm, [Performance Studio](https://developer.arm.com/tools-and-software/arm-performance-studio); Qualcomm, [Snapdragon Profiler](https://www.qualcomm.com/developer/software/snapdragon-profiler); Intel, [GPA](https://www.intel.com/content/www/us/en/developer/tools/graphics-performance-analyzers/overview.html): optional hardware-specific investigation.
- Microsoft, [Device Removed Extended Data](https://learn.microsoft.com/windows/win32/direct3d12/use-dred), and AMD, [Radeon GPU Detective](https://gpuopen.com/learn/rgd_1_0_tutorial/): later crash-analysis workflows.
