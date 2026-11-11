# RHI and Graphics Device Interface Reference Review

**Status:** Design evidence, 2026-10-04. The initial
[RHI and GDI architecture](rhi-gdi.md) was saved before opening
[the article map](../../references/game-dev-gems-toc.md). This review selected
relevant articles, read their actual text, cross-checked modern API constraints,
and updated the architecture. That historical review did not implement or
benchmark a renderer. Implementation status and the research gate were reconciled
on 2026-10-11; the [architecture implementation record](rhi-gdi.md#implementation-phases-and-acceptance)
owns the shipped baseline and its evidence. The planned
[R6–R17 follow-up milestones](rhi-gdi.md#follow-up-milestones-r6r17), assigned
on 2026-10-12, are maintained in that architecture; this review does not duplicate
their scopes or acceptance gates. The 2026-10-12 API review recorded beside that
roadmap separates binding arrays from descriptor indexing and native ray queries
from dedicated pipelines; it does not add a research-trial adoption.

The durable contribution of these references is explicit ownership and data
flow, with optimizations selected by workload. The final Ludus proposal retains
a narrow device interface, an ordered pass compiler, and portable raster
variants, while improving allocation, staging, binding, and pipeline contracts.

## Reading scope and evidence

The map was searched for rendering architecture, resource/shader management,
command recording, GPU memory, synchronization, abstraction, and batching.
Five complete chapters were read from local PDF text. Selected opening pages,
attachment guidance, staging lifetime diagram, and uniform framework diagram
were also rendered and inspected. No companion code/CD was inspected. Other
mapped titles remain candidates, not evidence from unread articles.

PDF positions below are **one-based file pages** in this repository's reference
copies, distinct from the books' printed page numbers. These ignored local PDFs
require the user's reference collection; the tracked review remains usable as
a decision record without them.

| Source actually read | Printed pages | Local PDF pages | Useful architectural evidence |
| --- | --- | --- | --- |
| Sebastien Schertenleib, Game Engine Gems 1 chapter 9, [A Multithreaded 3D Renderer](../../references/Game%20Engine%20Gems%201.pdf#page=213) | 185-195 | 213-223 | Batch data locality, parallel recording granularity, synchronization, buffering latency |
| Jeremy Moore, Game Engine Gems 1 chapter 11, [A GPU-Managed Memory Pool](../../references/Game%20Engine%20Gems%201.pdf#page=247) | 219-231 | 247-259 | Streaming staging ring, fence-based reuse, application eviction policy, bounded relocation work |
| Patrick Cozzi, Game Engine Gems 2 chapter 5, [Delaying OpenGL Calls](../../references/Game%20Engine%20Gems%202.pdf#page=87) | 71-81 | 87-97 | Private selector state, dirty updates, avoiding redundant calls, early bulk transfers |
| Patrick Cozzi, Game Engine Gems 2 chapter 6, [A Framework for GLSL Engine Uniforms](../../references/Game%20Engine%20Gems%202.pdf#page=99) | 83-92 | 99-108 | Explicit scene/object state, uniform meaning separated from API state, cached derived values |
| Arseny Kapoulkine, GPU Zen 2, 3D Engine Design article 4, [Writing an Efficient Vulkan Renderer](../../references/GPU%20Zen%202%20Advanced%20Rendering%20Techniques.pdf#page=227) | 215-247 | 227-259 | Allocation, descriptors, commands, barriers, render graphs, attachment bandwidth, pipeline enumeration/prewarming |

## Vulkan allocation and resource bindings

Kapoulkine compares allocation strategies and binding models, emphasizing their
hardware and maintenance tradeoffs. He discusses suballocation, dedicated
requirements, persistent host mapping, VMA, descriptor pools, dynamic uniform
offsets, grouping by update frequency, and the cost of binding changes. Bindless
can reduce CPU submission work but requires suitable limits and increases shader
indirection and renderer complexity. It is not uniformly optimal.

**Ludus refinement:** Select a private pinned VMA adapter for general Vulkan
allocation. Query memory requirements, flush/invalidate rules and actual budgets;
do not inherit historical heap sizes or vendor-specific assumptions. Make
View/Material/Draw grouping a useful convention, use immutable binding snapshots,
and allocate small dynamic data from aligned rings. Native descriptor pools retire
by completion; persistent material sets have their own ownership. Portable
materials remain functional without bindless.

**Excluded:** A public native heap/descriptor API, fixed desktop binding counts,
per-draw descriptor recreation, and immediate patching of a material binding
still referenced by old packets. A shader package must validate the target layout;
a conceptual group does not prove an identical binding implementation everywhere.

## Predictable dependency planning and attachment bandwidth

Kapoulkine's synchronization discussion connects barrier precision to knowledge
of future resource use. It contrasts just-in-time tracking with declared graph
dependencies, notes the author's preference for explicit final pass order, and
warns that putting all streaming work in a frame graph can increase complexity.
The render-pass section explains load/store intent and attachment resolves as
ways to avoid unnecessary memory traffic, particularly on tile-based GPUs.

**Ludus refinement:** Begin with an authored ordered pass list compiled into a
dependency graph. Track resource versions and RAW/WAR/WAW hazards, reject illegal
feedback and undefined loads, and keep a cross-execution import ledger. Lower
semantic dependencies in each backend, with exact consumer stages. Upload
scheduling remains separate and publishes import dependencies. Describe
Clear/Load/Discard, Store/Discard and resolve intent explicitly; add pass merging
or native subpass optimization only when measured.

**Excluded:** Heuristic pass reordering as the initial default, public Vulkan
subpasses, blanket all-stage barriers, and treating GPU queue order alone as
sufficient memory visibility. The article's historical statement about missing
barrier validation is not today's tooling contract: use current synchronization
validation, while still testing cases that validators cannot establish.

## Pipeline variants and compilation latency

Kapoulkine separates just-in-time pipeline lookup, driver cache persistence,
prewarming from observed combinations, and ahead-of-time knowledge of technique
state. Including complete state in technique definitions improves predictability,
but permutations multiply and need deliberate control. Native driver compilation
still occurs; source compilation and pipeline readiness are separate facts.

**Ludus refinement:** Have content/settings enumerate required canonical
technique variants and request them before use. Draw packets retain ready
pipeline handles. Record missing variants and pending/failed status, provide a
ready compatible fallback, and separate portable manifests from driver cache
blobs. Prewarm with budgeted workers or verified asynchronous backend support;
no cache miss creates a pipeline inside draw encoding.

**Excluded:** Pointer-derived persistent keys, raw padded state hashing,
unbounded permutation generation, and a promise that shipping SPIR-V/WGSL
eliminates driver compilation. The article motivates the policy; the exact cache
protocol and safe publication design are Ludus choices.

## Staging and GPU completion

Moore's console streaming pool uses GPU copies and a staging ring whose entries
are reusable only after their copy fence completes. He separates resource data
from CPU headers, leaves eviction priority to application logic, and describes
copy budgets and fragmentation problems. Platform-specific sections make clear
that the implementation depends on the console memory/API model.

**Ludus refinement:** Distinguish logical resource identity, retained CPU records,
physical allocation, source consumption, queued upload and destination readiness.
Return upload/readback tickets and apply backpressure to bounded rings. Preserve
staging until the actual backend consumption/completion obligation ends. Keep
eviction/residency policy with content, and show pending retirement in memory
reports. Future external compute needs the same explicit ownership discipline.

**Excluded:** General live GPU-pool compaction, direct CPU patching of moved native
resource addresses, console alignment constants, and a copy-every-frame policy.
Moore's ordered console stream does not prove Vulkan cross-queue synchronization
or WebGPU mapping rules. New Vulkan objects/bindings and verified dependencies
are required for any future relocation; native allocations cannot be treated as
freely movable raw console memory.

## WebGL state and uniform meaning

Cozzi's two chapters offer complementary lessons. Delaying state updates until
use can remove selector errors and redundant API calls, while delaying a large
buffer/texture transfer until drawing can hurt CPU/GPU overlap. Explicit state
objects separate scene/object meaning from global GL state; derived values can
be recomputed only when their inputs change.

**Ludus refinement:** Give WebGL one private context state cache and indexed
revisions/dirty ranges. Apply complete pipeline/binding snapshots at draws and
schedule bulk uploads earlier. Reset the cache on recreation and invalidate it
at any deliberate foreign-call boundary. At renderer level, prepare explicit
View/Material/Draw data; generate numeric layout mappings offline and cache
derived matrices in the data owner.

**Excluded:** Virtual observer/factory objects per uniform, a global string-to-
factory registry, runtime shader-source parsing, and uniform name lookup during
draws. The modern uniform-buffer and reflected-binding path carries the useful
meaning/state separation with less object and lifetime machinery.

## Recording granularity and latency

Schertenleib treats a batch as the data and state required to render an object,
shows how scattered scene/library access can inhibit throughput, and discusses
parallel display-list generation and CPU/GPU synchronization. His buffering
examples expose a latency/memory tradeoff. Kapoulkine adds Vulkan-specific pool
ownership and cautions against very small command buffers and excess submits.

**Ludus refinement:** Extract compact immutable packets, set execution order
before recording jobs, assign private completion-scoped pools to native workers,
and submit through one owner. Keep small passes serial and batch compatible
work. Measure input-to-display latency, memory, worker join cost, and CPU/GPU
frame time when selecting concurrency and buffering.

**Excluded:** Automatic one-thread-per-subsystem partitioning, one submission
per draw, guaranteed speedup from more worker threads, and a hardcoded historical
submit/millisecond budget. Browser WebGL still uses its context owner; a native
parallel-recording path does not establish browser thread support.

## Current primary source cross-check

These sources were consulted for constraints and design comparison. They do not
prove Ludus implementation support or transfer performance results. Latest
specifications/documentation may describe features beyond the pinned SDK;
actual enablement requires a version check and backend conformance.

| Primary source | Constraint or comparison read | Resulting Ludus decision |
| --- | --- | --- |
| [WebGPU specification](https://www.w3.org/TR/webgpu/), synchronization, buffer mapping, queue and pipeline sections | Render-pass usage scopes include bound resources; mapping and queue completion are distinct; asynchronous pipeline creation has explicit readiness | Validate whole scopes, keep mapping state separate, and use generation-checked asynchronous status supported by the pinned C API |
| [WebGL 2 specification](https://registry.khronos.org/webgl/specs/latest/2.0/) | Core resource/draw/copy operations and short client-wait limits | Restrict portable operations and poll completion across event-loop turns |
| [WebGL feedback rules](https://registry.khronos.org/webgl/specs/latest/1.0/) | Conflicting texture/framebuffer read/write produces an error | Reject feedback in the declared plan before executing |
| [Vulkan synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html) | Access/stage/layout dependencies, including execution-only WAR cases | Maintain a small tested semantic mapper rather than expose native flags |
| [Vulkan present retirement](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html) | Submit fences do not establish presentation completion; maintenance1 provides presentation fences | Preserve separate render and presentation retirement |
| [VMA quick start](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/quick_start.html) | Private single-implementation integration, explicit errors, no C++ exception requirement | Use a pinned backend adapter while keeping allocation metadata out of public headers |
| [Epic RDG](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine), debugging/validation | Deferred execution can obscure setup locations; individual optimizations and parallelism can be disabled | Add setup-context diagnostics and serial/no-alias/no-cull investigation controls |
| [D3D12 allocator reset](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandallocator-reset) and [Metal synchronization](https://developer.apple.com/documentation/metal/resource-synchronization) | Reuse/synchronization obligations depend on GPU work and the selected native model | Keep native details private and require common lifetime/dependency conformance |
| [CUDA interoperability](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html) | Device identity matching, external memory and synchronization protocols | Plan a separate compute executor and explicit native bridge; no implicit cross-API sharing |

The WebGPU specification exceeded the web reader's size limit, so the relevant
sections were fetched directly from the official W3C document and read as text.
Apple's synchronization discussion was read through its official Markdown
representation because the HTML page requires JavaScript.
The NVIDIA skill catalog was checked for the CUDA boundary; this broad engine
architecture did not require installing a product-specific skill. No skills,
repository dependencies, or toolchain settings were changed.

## Other mapped candidates

The map also identifies Camera-Centric Engine Design for Multithreaded Rendering,
A Generic Multiview Rendering Engine Architecture, A Generic Handle-Based Resource
Manager, resource-management chapters, shader interfaces, and cross-platform
rendering-thread articles. Their full text was not read in this task, and no
specific mechanism is attributed to them. They are useful next candidates when
implementing multiview, identity integration, streaming or a dedicated render
thread. Existing Ludus identity/memory decisions were checked directly for
compatibility, rather than presenting earlier reviews as new chapter reading.

## Resulting design and remaining proof

The reviewed design makes allocation and ownership more concrete without adding
a universal GPU runtime. Its first implementation remains one device, one owner,
one physical queue, reflected immutable resources/bindings, completion-based
reuse, and a serial ordered pass compiler. CUDA/other compute domains and native
advanced rendering stay explicit extensions.

The books do not prove correctness of Ludus's handle retention, graph compiler,
callback publication, external-state ledger, or cross-API bridge. The bounded
R0–R5 implementation now has adversarial tests, native/browser
rendering and installed-SDK acceptance evidence, recorded in the architecture.
That evidence does not establish a future cross-API bridge, broader capabilities
or unmeasured target-device performance; each extension keeps its own proof gate.

## Research after the initial implementation

**Status:** Queued reading and improvement trials, reconciled 2026-10-11.
The initial bounded [R0–R5 profiles](rhi-gdi.md#implementation-phases-and-acceptance)
are implemented. The architecture's [baseline and evidence record](rhi-gdi.md#baseline-and-evidence-record)
pins the accepted implementation, supported profile owners, diagnostic scenes,
reproduction commands and measurement limits. Its
[remaining work](rhi-gdi.md#remaining-work-and-decision-gates) separates consumer
extensions from optional research; completing every extension is not a
prerequisite for a selected trial.

Use that recorded baseline when evaluating a change. Pin the trial's revision,
backend/profile and workload, retain a control path, and report unavailable
GPU/latency/backing measurements explicitly. Adopt a technique only after its
correctness, relevant benefit and maintenance cost are established. Required
API/specification checks and the references already adopted above remain normal
implementation inputs. The [conference review below](#synchronization-attachments-and-schedules)
records design refinements from the expanded reference index.

The initial shortlist came from venue programs, session descriptions and
available abstracts. The reading status in each row identifies the material
consulted and links to any resulting decision.

### Venues to follow

| Venue | Priority after the baseline | Topics and relevant Ludus work |
| --- | --- | --- |
| [Rendering Engine Architecture Conference (REAC)](https://enginearchitecture.org/2025.htm) | First | Production frame graphs, shader bindings, instance data, modularity and permutation control; R1-R3 and measured renderer integration |
| [Vulkanised](https://www.vulkan.org/events/vulkanised-2026) | First | Frames in flight, completion, synchronization, resource reuse and profiling; R2/R3 correctness and R5 measurements |
| [SIGGRAPH Advances in Real-Time Rendering](https://advances.realtimerendering.com/s2023/) | First | Low-level API/backend design and practical rendering systems; R0-R3 contracts and portable resource integration |
| [GDC Programming / GDC Vault](https://www.gdcvault.com/play/1024612/FrameGraph-) | First | Production pass/resource architecture and job-based rendering; R3 graph design and optional R5 recording |
| [Shading Languages Symposium](https://www.khronos.org/events/shading-languages-symposium-2026) | First | Slang reflection, target layouts, shader compilation and WGSL constraints; R1 shader packages and R2 pipeline services |
| [High-Performance Graphics (HPG)](https://highperformancegraphics.org/2026/schedule/) | Selective | GPU scheduling, efficient data structures and profiling; R4 compute consumers and R5 experiments |
| [Journal of Computer Graphics Techniques (JCGT)](https://jcgt.org/about.html) | Practical techniques | Peer-reviewed implementation techniques and author-provided code; select concrete resource, renderer or validation problems |
| [Computer Graphics Forum / Eurographics Symposium on Rendering](https://www.eg.org/wp/eurographics-publications/cgf/) | Selective | Rendering research, practical developments and reviews; evaluate future consumers and their resource/pass requirements |
| [ACM Transactions on Graphics / SIGGRAPH technical papers](https://s2026.siggraph.org/program/technical-papers/) | Selective | Research algorithms and their assumptions; evaluate capability-specific renderer paths after the common infrastructure is validated |

These links identify archives or publication entry points, not an attendance
schedule. A venue's inclusion does not establish support in Ludus's pinned SDKs
or prove that an advanced native technique fits its portable backend contract.

### Initial reading order

| ID | Source and attribution | Reading status | Question and possible trial after full review |
| --- | --- | --- | --- |
| RG-01 | Sebastian Aaltonen, **HypeHype Mobile Rendering Architecture**, SIGGRAPH Advances in Real-Time Rendering, 2023. [Course and materials](https://advances.realtimerendering.com/s2023/) | Relevant slides and notes reviewed | [Attachment lifetime and pass groups](rhi-gdi.md#attachment-lifetime-and-pass-groups) |
| RG-02 | Charles Giessen, **Frames in Flight Demystified**, Vulkanised 2026, LunarG. [Program, slides and recording](https://www.vulkan.org/events/vulkanised-2026#wednesday) | Queued; program screened | Does a different frame-storage/reuse policy improve tail latency or memory? Compare the same workload with slow-GPU, resize and destruction journeys; prove reuse against actual completion. |
| RG-03 | Yuriy O'Donnell, **FrameGraph: Extensible Rendering Architecture in Frostbite**, GDC 2017, Frostbite / Electronic Arts. [Session](https://www.gdcvault.com/play/1024612/FrameGraph-) | Queued; session description screened | Which pass/resource abstractions simplify our ordered compiler? Compare a bounded compiler change with the reference plan, including history roots, imported uploads, feedback and undefined loads. |
| RG-04 | Nicolas Lopez and Michel Bouchard, **Anvil Rendering Architecture**, REAC 2025, Ubisoft. [Abstract, slides and recording](https://enginearchitecture.org/2025.htm) | Frame Graph slides and notes reviewed | [Schedule comparison for R15](rhi-gdi.md#schedule-comparison-for-r15) |
| RG-05 | Nia Bickford, **Upgrading from GLSL to Slang in the Vulkan Nvpro-Samples**, Shading Languages Symposium 2026; Dan Sinclair, **WGSL: Past, Present and Future**, same symposium. [Program and materials](https://www.khronos.org/events/shading-languages-symposium-2026) | Queued; program and available abstracts screened | Which reflection, layout and compilation practices improve our packages? Trial a verified package/schema change against target packing, interface mismatches and pipeline readiness on all supported backends. |

Continue by screening these candidates when a concrete baseline limitation
justifies the additional reading:

- Max Bukhalov and Egor Orachev, **Geometry rendering and shaders infrastructure
  in Warhammer 40000: Space Marine 2**, REAC 2025, Saber Interactive.
  [Abstract and materials](https://enginearchitecture.org/2025.htm). Queued;
  abstract screened. Investigate instance-data organization and shader
  permutation control against pipeline counts, compile stalls and memory.
- Lucas Miguel Antunes da Silva, **Solving All Synchronisation Problems with
  Timeline Semaphores**, Vulkanised 2026, DevSH Graphics Programming.
  [Program and materials](https://www.vulkan.org/events/vulkanised-2026#wednesday).
  Queued; program screened. Investigate completion/dependency simplification
  while preserving our negotiated Vulkan paths and separate presentation retirement.
- Natalya Tatarchuk, **Destiny's Multithreaded Rendering Architecture**, GDC 2015,
  Bungie. [Session](https://www.gdcvault.com/play/1021926/Destiny-s-Multithreaded-Rendering).
  Queued; session description screened. Investigate packet/job boundaries and
  recording granularity; measure join cost, frame latency and pool ownership.
- Karl Sassie, Johannes Hanika, Lucas Alber, Reiner Dolp and Carsten Dachsbacher,
  **Optimizing Vulkan Dispatch Schedules for Real-Time U-Net Denoising**, HPG 2026.
  [Program](https://highperformancegraphics.org/2026/schedule/). Queued; program
  screened. Investigate scheduling only when a comparable compute workload exists;
  compare execution time, synchronization, memory and correctness.

### Synchronization, attachments and schedules

On 2026-11-11, the expanded table of contents directed this review to GDC,
SIGGRAPH and REAC material. The comparison used Ludus `253c2f61a` on `develop`
and the primary slide sections below, including their diagrams and available
speaker notes. The decisions specify design and acceptance criteria. Runtime
trials and hardware measurements remain open.

| Source and scope read | Observation in Ludus | Decision |
| --- | --- | --- |
| Matt Pettineo, **Breaking Down Barriers: An Introduction to GPU Synchronization**, GDC 2019, Ready at Dawn. Slides “GPU Thread Barriers”, “GPU Cache Barriers”, “GPU Compression Barriers”, “D3D12 Barriers”, “Vulkan Barriers” and the multi-queue recap distinguish execution, visibility and layout costs. [Primary slides](https://media.gdcvault.com/gdc2019/presentations/Pettineo_Matt_Breaking%20Down%20Barriers.pdf) | `GraphReport` records semantic dependencies. Backend helpers emit synchronization during encoding and transfers. The report cannot show which native stage, access or range an optimization changed. | Observe actual Vulkan lowering before narrowing buffer barriers in R13a. Keep semantic edges distinct from observed native actions, including Direct3D 12 same-state UAV ordering. [Design](rhi-gdi.md#synchronization-evidence), [Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html), [Microsoft resource barriers](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12) |
| Sebastian Aaltonen, **HypeHype Mobile Rendering Architecture**, SIGGRAPH Advances in Real-Time Rendering, 2023. Slides “Mainstream Phones vs Xbox 360 (and Xbox One)”, “GPU-Driven Rendering on Mainstream Phones?” and “Our Solution: Minimal Platform Abstraction” discuss on-chip attachments, bandwidth and API scope. [Primary slides and notes](https://advances.realtimerendering.com/s2023/AaltonenHypeHypeAdvances2023.pdf) | R5 can merge compatible consecutive draws. The current merge predicate requires both depth stores to be `Store`. Its published Metal comparison measured CPU cost. | Trial a physical group that honors an authored final depth discard. Qualify it using pixel agreement, initialization and target-GPU bandwidth evidence. Framebuffer fetch and broad implicit store elision need their own contracts and evidence. [Design](rhi-gdi.md#attachment-lifetime-and-pass-groups) |
| Nicolas Lopez and Michel Bouchard, **Anvil Rendering Architecture**, REAC 2025, Ubisoft. The “Frame Graph” sequence, including “GetInputOutput” and “Render”, covers the explicit schedule, resource lifetime and platform-specific schedules. [Primary slides and notes](https://enginearchitecture.org/downloads/REAC_2025_Anvil.pdf) | Ludus already separates declared uses from packet execution and has an authored order. Its current whole-object pool retires by completion. Anvil's automatic producer insertion and heap aliasing require additional scheduling and lifetime machinery. | Preserve the authored list as the control for R15 schedule experiments. Make queue ownership, waits and retirement part of the comparison. Automatic producer insertion and heap aliasing remain outside this trial. [Design](rhi-gdi.md#schedule-comparison-for-r15) |

### Review and trial record

Use **Queued**, **Reading**, **Reviewed**, **Trial planned**, **Trial complete**,
**Adopted**, **Deferred** or **Rejected** for each source. A title/abstract screen
is not a full reading, and a completed reading is not a measured improvement.
Update the existing row and append the detailed record here as work progresses.

For each completed review or experiment, record:

1. Source ID, exact author/title/year, primary URL or DOI, review date and the
   pages, sections or talk timestamps actually consulted.
2. The relevant idea, assumptions, limitations and differences from Ludus;
   distinguish design inspiration from adapted code.
3. The affected RHI/GDI phase/module and a testable improvement hypothesis.
4. A bounded trial, baseline revision, workloads and adversarial failure cases.
5. Reproduction commands, toolchain/backend/device versions and links to the
   test, benchmark or PR; keep generated captures/results in ignored `out/`.
6. Before/after image and synchronization correctness, CPU/GPU frame-time
   distributions, input-to-display latency, compilation stalls, allocations,
   memory/retirement high-water marks and relevant traffic/submit counts.
7. An adopt/defer/reject decision, its reason, and links to the updated owning
   architecture/ADR and implementation attribution when a change is accepted.

Apply the architecture's existing backend, SDK and adversarial acceptance gates
to every trial. Report unavailable GPU/presentation measurements as unavailable.
Keep native advanced paths capability-specific and preserve the supported
fullscreen facade and portable raster behavior. A successful trial must show
correctness and an explainable benefit on its declared workload before adoption.
Preserve the completed Gems review above; extend its adopted evidence only after
the full source has actually informed an accepted change.
