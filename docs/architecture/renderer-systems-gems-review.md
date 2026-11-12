# Low-level renderer systems: reference review and research plan

Status: Design evidence, October 7, 2026, with graph/shader authoring follow-up
October 12, 2026; this review supplies no implementation or benchmark.
The [architecture](renderer-systems.md) was saved before searching the article map.
This review records the subsequent source-driven changes. Detailed device,
camera, resource and text contracts remain in their existing owners.

## Selection and evidence

Searched the local `references/game-dev-gems-toc.md` for renderer architecture,
submission, viewport, materials/shaders, culling, lighting, shadows, atlases and
fonts, then ray/path tracing for the follow-up. Selected six initial articles and
three ray/path-tracing chapters for actual text inspection. Read the relevant
sections listed below, including limitations and tradeoffs; this is not a claim
to have read every chapter/book or inspected companion code. The atlas and font
chapters were inspected through their conclusions/references. Some source text
has extraction artifacts; author/title/page details and the key diagrams were
cross-checked against rendered PDF pages.

PDF positions are one-based file pages in the user's local copies. Those ignored
PDFs and extraction/rendering scratch files are not distributed in the repository.
This record and the architecture are usable without them. Titles are discovery
evidence until actual article text is inspected; unrelated catalog entries did
not alter the design merely because their names sounded relevant.

| Source inspected | Printed pages and sections | Local PDF pages |
| --- | --- | --- |
| Donald Revie, “Designing a Data-Driven Renderer,” *GPU Pro 3* (2012), V.4 | pp. 291–318; 4.2–4.8, especially representation/context, frame graph and case-study tradeoffs | 295–322 |
| Markus Billeter, Ola Olsson and Ulf Assarsson, “Tiled Forward Shading,” *GPU Pro 4* (2013), II.4 | pp. 99–114; 4.4–4.8, transparency, MSAA and clustered comparison | 116–131 |
| Takahiro Harada, Jay McKee and Jason C. Yang, “Forward+: A Step Toward Film-Style Shading in Real Time,” *GPU Pro 4* (2013), II.5 | pp. 115–134; 5.2–5.6, light-list construction, culling data and shadow cost | 132–151 |
| Arseny Kapoulkine, “Writing an Efficient Vulkan Renderer,” *GPU Zen 2* (2019), IV.4 | pp. 215–247; revisited 4.4–4.6, ordered dependencies, attachments and pipelines | 227–259; pertinent sections 250–257 |
| Manny Ko, “A Fast and High-Quality Texture Atlasing Algorithm,” *Game Engine Gems 3* (2016), chapter 9 | pp. 111–120; packing decomposition, borders and topology-aware filtering | 106–115 |
| Aurelio Reis, “Fast Font Rendering with Instancing,” *Game Programming Gems 8* (2010), 1.1 | pp. 3–11; quad data, batching, instancing limits and painter order | 18–26 |
| Jakub Boksansky and Adam Marrs, “The Reference Path Tracer,” *Ray Tracing Gems II* (2021), chapter 14; [DOI](https://doi.org/10.1007/978-1-4842-7185-8_14) | pp. 161–187; selected portions of 14.2 and 14.3.1–14.3.7: scene/build lifetime, primary-ray parity, RNG/accumulation, termination and light visibility | 207–233 |
| Anders Lindqvist, “Multiple Importance Sampling 101,” *Ray Tracing Gems II* (2021), chapter 20; [DOI](https://doi.org/10.1007/978-1-4842-7185-8_20) | pp. 327–337; 20.1–20.2, especially density measure conversion and matching sampler/PDF evaluation | 368–378; pertinent 368–377 |
| Carsten Wächter and Nikolaus Binder, “A Fast and Robust Method for Avoiding Self-Intersection,” *Ray Tracing Gems* (2019), chapter 6; [DOI](https://doi.org/10.1007/978-1-4842-4427-2_6) | pp. 77–85; 6.1–6.3 and limitations on p. 85 | 113–121 |

Visual inspection covered *GPU Pro 3* Figure 4.23 (printed p. 313), *GPU Pro 4*
Figure 4.3 (p. 108), and *Game Engine Gems 3* Figure 9.1 (p. 113). The first
is a context/control traversal, not a modern resource-hazard graph; the second
shows why transparent bounds differ; the third demonstrates border reservation.

## Findings and adopted changes

### Representation, context and technique boundaries

Revie separates shared representation from camera/viewport/target context and
describes selectable techniques and ordered passes. The case study also reports
configuration growth, difficult special cases and optimization costs.

**Change:** Make pass inputs complete immutable records, declare depth/shadow/color
variants in the material package, resolve parameter mappings before drawing and
bound configurations. Keep stable views separate from cameras/targets.

**Departure:** No XML object pipeline, global context push/pop, entity requirement
for postprocess/UI, name-searching draw path or universal emulation of unavailable
hardware. Concrete functions and typed data make the useful separation easier
to follow under Ludus's explicit ownership rules.

### Transparent lighting and view-dependent culling

Billeter, Olsson and Assarsson distinguish opaque and transparent depth bounds,
retain ordinary alpha sorting, and discuss both MSAA samples and the additional
constant costs of clustering. Tiled and clustered methods trade workload costs;
the chapter does not establish a universal winner.

**Change:** Begin clustered assignment with conservative view-space cells that
cover transparent geometry. Keep opaque depth/normal pruning separate. Define
the finite lighting range, empty-depth behavior and multisample handling.

**Departure:** No direct transplantation of GLSL buffer-texture layouts, fixed
tile sizes or historical performance numbers. The CPU light path remains useful;
clustered assignment does not solve intersecting transparency order.

### Light-list capacity and shadow cost

Harada, McKee and Yang separate compact culling bounds from lighting properties,
generate lists through local then global storage, discuss depth occupancy masks,
and show that many-light shading does not remove shadow-generation costs.

**Change:** Separate culling/shading arrays, use capped fixed-stride lists first,
validate every append and published count, visualize overflow and independently
budget shadow assignments. A saturated list takes a stable bounded fallback.

**Departure:** Their example increments an attempted append count while guarding
the local store; its subsequent export illustrates why a production design must
bound the exported count as well. Ludus does not assume the demo's capacity
covers all scenes. The 2.5D masks and ray-cast shadow extension remain candidates,
not baseline features or proof of contemporary ray-tracing performance.

### Ordered work, attachments and pipelines

Kapoulkine explains ordered dependency planning, load/store/resolve bandwidth,
pipeline creation/cache costs and ahead-of-time enumeration of material states.
The existing [RHI/GDI review](rhi-gdi-gems-review.md) remains the detailed owner
of allocation, descriptor and command lifetime findings from this source.

**Change:** Make material state enumeration an admission/prewarm requirement,
preserve explicit attachment intent and keep streaming scheduling outside the
scene-pass traversal while importing its readiness dependencies.

**Departure:** No Vulkan-specific graph in public Renderer, no promise that a
driver cache eliminates first-use stalls, no forced newest Vulkan feature set,
and no assumption that tile and immediate GPUs have identical fast paths.

### Atlas borders and chart semantics

Ko decomposes chart packing into placement, fit and quality; reserves borders
before packing; and filters chart seams using original mesh topology. He also
identifies cases where simple greedy packing leaves quality on the table.

**Change:** Give glyph, sprite and lightmap atlases separate contracts. Keep
runtime glyph shelves simple, use offline chart packing for lightmaps, and budget
filtering footprints before placement. Preserve chart adjacency for lightmap
filtering and validate mip-safe gutters or a declared LOD clamp.

**Departure:** No topology-aware diffusion between independent letters or sprites,
no live atlas relocation, and no general annealing/bitmask packer until offline
occupancy measurements justify it. Better packing does not itself make mips safe.

### Glyph submission and historical instancing costs

Reis separates fixed quad corners from per-glyph attributes, describes compatible
batch boundaries and painter order, and notes that the tested instancing methods
have hardware-dependent overhead and constant limits.

**Change:** Retain compact glyph instances, completion-scoped uploads, adjacent-only
batching and ordering. Add tiny-label as well as large-overlay benchmarks.

**Departure:** No D3D9 discard path, Shader Model 3 constant-array packing, fixed
85-quad limit or inference that modern instancing is always faster. Unicode
shaping remains with Text; the chapter is submission evidence, not a paragraph
layout or font-quality specification.

## Changes to the initial design

| Initial decision | Refinement now in the architecture | Gate added |
| --- | --- | --- |
| Explicit passes and material families | Complete pass inputs and cooked depth/shadow/color variants | Variant count and reflected layout validation |
| Conservative clustered lists | View-space range covering transparent geometry; separate opaque pruning | Glass in front of opaque geometry, sky and MSAA cases |
| Bounded lighting | Written-count safety, compact culling arrays, independent shadow budget | Local/global overflow and stable fallback fixtures |
| Texture semantics and atlases | Distinct atlas owners and mip/filter footprints reserved before packing | Lightmap seams, sprite bleed and glyph gutters |
| Instanced glyphs | Small-label and batch-break comparisons, preserved painter order | Pixel parity and upload-byte/CPU/GPU measurements |
| Pipeline prewarming | Enumerate legal material states; expose cold misses | First-use/reload/driver-cache failure behavior |
| Ray tracing previously an optional technique row | Shared ray scene and RT0–RT4 plan: traversal, reference transport, hybrid effects and high-end PT | AS lifetime/pose parity, estimator convergence, reconstruction quality and target-device budgets |

## Ray and path tracing follow-up

The follow-up reads the three chapters above from the catalog's *Ray Tracing
Gems* entries. The [architecture's ray/path section](renderer-systems.md#ray-tracing-and-path-tracing)
now separates intersection infrastructure, independent reference integration,
hybrid lighting and real-time reconstruction. This is additional design work;
no existing RHI implementation or browser capability is implied.

**Boksansky and Marrs:** Their in-engine progressive reference uses the existing
scene and compares primary hits/material attributes against raster output before
building up transport. It distinguishes persistent geometry/structures from
temporary build scratch, handles RNG/linear accumulation, and makes termination
and light visibility explicit. This motivates an early reference mode, exact
resource/pose parity, AOV comparisons and completion-scoped build memory. The
discussion of maximum depth acknowledges truncation bias; the accumulation
discussion permits a highest-resolution texture policy at a performance cost.

**Departure:** Use explicit fixed-mip/filtering metadata rather than claim all
texture models agree; isolate query execution from transport instead of adopting
the sample's DXR ray pipeline and bindless setup. Add immutable build/shading
versions, accumulation keys and incomplete-content statuses under Ludus's current
owners. Reference quality applies only to the declared supported transport model.
No sample code, assets or vendor integration layer was imported.

**Lindqvist:** Light sampling and material sampling favor different configurations.
His MIS construction compares probabilities in a common measure and requires
the separately evaluated light PDF to match the actual selection/sampling scheme.
The architecture therefore requires the light-selection probability, conditional
light density, explicit area/direction conversion and BSDF-hit emission weighting.
These are correctness requirements, not merely a late performance optimization.

**Departure:** The chapter's example uses surface-area densities and the balance
heuristic; Ludus may use directional densities and another verified heuristic.
Analytic delta lights/lobes need explicit handling beyond the finite-area examples.
Normalization and multi-seed convergence fixtures accompany the estimator.

**Wächter and Binder:** They analyze why primitive exclusion, fixed `tmin`,
shading-normal offsets and fixed-length origin offsets fail. Barycentric surface
reconstruction plus adaptive geometric-normal offsets motivates the ray-spawn
contract and diagnostics. Their limitations include very thin crevices and large
instance transforms; this adds thin-geometry/extreme-transform fixtures and a
render-origin rule, not a promise of perfect self-intersection elimination.

**Departure:** Verify the method against the actual backend precision and
instancing path before choosing constants or copying code. Numerical safeguards
must preserve nearby valid intersections as well as remove acne. The architecture
records the idea without reproducing the chapter's implementation.

Current cross-checks consulted Khronos's [ray tracing guide](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html)
for separate query/pipeline capabilities, shared acceleration structures and
build/read/scratch synchronization; Apple's [acceleration-structure documentation](https://developer.apple.com/documentation/metal/ray-tracing-with-acceleration-structures)
was available only as a search summary, with the full page requiring JavaScript.
Metal contracts/toolchain support still require implementation-time verification.
Matt Pharr, Wenzel Jakob and Greg Humphreys's *Physically Based Rendering*, fourth
edition (2023), [section 13.4](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer),
was inspected for MIS emission weights and compensated termination. Its spectral
and wider material model is not silently adopted as the RGB reference scope.

Daqi Lin, Markus Kettunen and Chris Wyman's [“ReSTIR PT Enhanced: Algorithmic
Advances for Faster and More Robust ReSTIR Path Tracing”](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/)
(*I3D / Proceedings of the ACM on Computer Graphics and Interactive Techniques*,
May 2026) was checked through its publication page and abstract. It identifies
reconnection, sample correlation and reuse overhead as contemporary review topics.
The paper's speedup is not a Ludus result. Full estimator review remains necessary
before RT4 adoption; similarly, PBRT chapter 15's overview was inspected only as
a wavefront follow-up source. Denoising/resampling never replaces independent
raw reference samples in correctness comparisons.

## Render graph and shader authoring follow-up

The [graph/shader architecture](render-graph-authoring.md) was first written on
October 12 against `5c44d19`, before this targeted article-text review. It addresses
reusable graph nodes, camera/offscreen outputs, scene coverage, author-owned
permutations, shared layouts and game-module hot reload. The table below records
actual readings selected from `references/game-dev-gems-toc.md`, not conclusions
drawn from titles alone. Existing renderer algorithms and their previous evidence
remain above. No companion code was imported or assessed.

| Source and catalog entry | Sections actually inspected | One-based local PDF pages |
| --- | --- | --- |
| Donald Revie, "Designing a Data-Driven Renderer," *GPU Pro 3*, V.4 ([catalog](../../references/game-dev-gems-toc.md#L9743)) | 4.4-4.8, shared representation/context, technique/pass filters, frame traversal, case-study limitations and debug techniques; printed pp. 307-317 | 311-321 |
| Arseny Kapoulkine, "Writing an Efficient Vulkan Renderer," *GPU Zen 2*, IV.4 ([catalog](../../references/game-dev-gems-toc.md#L10838)) | End of 4.4, 4.5-4.6.3 and conclusion: declared dependency order, attachment intent, complete pipeline states, prewarm and permutation growth; printed pp. 238-247 | 250-259 |
| Curtiss Murphy, "Support Your Local Artist: Adding Shaders to Your Engine," *Game Programming Gems 7*, 7.4 ([catalog](../../references/game-dev-gems-toc.md#L7296)) | Full article including Test Mode, Prototypes, parameters/cloning, groups, future work and references; printed pp. 541-554 | 574-587 |
| Rémi Arnaud, "The Game Asset Pipeline," *Game Engine Gems 1*, chapter 2 ([catalog](../../references/game-dev-gems-toc.md#L5552)) | 2.2-2.3, readable intermediate/cooked ownership, editor/source workflow and pull-model dependencies; printed pp. 24-30 | 52-58 |
| Patrick Cozzi, "A Framework for GLSL Engine Uniforms," *Game Engine Gems 2*, chapter 6 ([catalog](../../references/game-dev-gems-toc.md#L5783)) | Full chapter 6.1-6.6, automatic uniforms, explicit state, implementation and derived-value caching; printed pp. 83-92 | 99-108 |
| Christopher Maughan / NVIDIA, "The Design of FX Composer," *GPU Gems*, chapter 30 ([catalog](../../references/game-dev-gems-toc.md#L8291)) | Full chapter 30.1-30.9, properties/parameter reconciliation, communication/lifetime costs, UI, device-independent compilation and per-device instantiation | 269-276; this local copy does not preserve printed page labels |

The ignored local PDFs remain in the primary reference library and are not copied
into public documentation. Readings used extracted page text; rendered checks
verified Kapoulkine's complete technique example (p. 244), Murphy's parameter
diagram/duplicate-texture discussion (p. 548), and Cozzi's factory diagram (p. 88).
The latter two are historical object models, not designs transplanted into Ludus.

### Refinements to the initial proposal

| Source finding | Refinement and rationale | Departure / acceptance gate |
| --- | --- | --- |
| Revie reports XML/configuration complexity, special-case friction and runtime optimization costs; filters can leave entities without matching passes | Preserve nested source paths and readable generated plans; compile graph interpretation ahead of runtime; check intended scene participation independently of compatibility filters | No context push/pop or global technique state. RG0/RG2 require route diagnostics and mixed draw-item coverage; no unsupported item can disappear inside selection |
| Kapoulkine explicitly includes fixed state and target formats in techniques, then discusses multiplying combinations and alternatives to more permutations | Bound pipeline recipes separately from unique shader programs; review runtime work or separate authored effects before adding static tuples | No automatic permutation from unused output, unrestricted Cartesian product or promise of zero driver work. RG1/RG2 count both programs/recipes and reject cold required pipelines before activation |
| Murphy shares compiled programs between parameterized instances, warns about duplicated texture data and emphasizes seeing the result in the real engine | Clone small values while sharing immutable program/resource versions; add frozen-fixture parity between preview and live package execution | No singleton, virtual parameter per field, scene-state inheritance or direct replacement of executing programs. RG3 verifies retained versions and preview/live parity |
| Arnaud separates source/intermediate/final assets and warns that editing intermediates loses changes on rebuild | Make generated views read-only and attach Open Source/provenance to selected artifacts; retain readable intermediate plans and transitive invalidation | No COLLADA/DCC dependency or second authored generated source. RG1 tests clean rebuild, source-directed navigation and dependency changes |
| Cozzi replaces implicit global uniform state with automatic inputs sourced from explicit state, with derived values cached | Add typed engine-value provider nodes and declared view/draw update frequencies | No string-name factory registry, runtime source parsing or virtual dispatch per uniform. RG1/RG2 check provider schema/dependencies and two views with distinct values |
| Maughan describes parameter reconciliation and compilation independent of device-specific effect instances; the chapter also records ownership/UI complexity and repeated resources across devices | Require stable field/type/semantic migration and visible conflicts; separate document/package/device status; share device-local leases where possible | No COM plug-in framework, device per panel or lazy pipeline creation during draw. RG3 tests changed semantics, missing capabilities, migration rejection and ready admission |

These are refinements of the already saved proposal, not a claim that old APIs
are contemporary best practice. Thanks to the authors for the design ideas and
explicit limitations. The architecture places concise attribution beside the
affected contracts; this review supplements rather than replaces it.

Contemporary primary documentation was also checked: Epic Games' Render Dependency
Graph (resource declarations, Debugging and Validation), PSO Precaching (Global
Shader PSO Precaching), Slang reflection/module specialization, and Khronos fragment
operations. Exact links, scope, departures and pinned-version qualification are in
the [architecture's cross-checks](render-graph-authoring.md#contemporary-cross-checks).
These readings are not benchmarks, full engine-source audits, or evidence of new
Ludus/backend features. No new toolchain version is selected.

### Render pipeline resource management follow-up

The updated local catalog led to these REAC talks. The review checked the
slides against `RenderGraphActivation`, the bilinear pass's program
preparation and RHI's raster request path. The
[resource contract](renderer-systems.md#render-pipeline-and-pass-resources)
defines the proposed changes.

| Primary material reviewed | Ludus decision |
| --- | --- |
| Oleksandr Drazhevskyi, [“Resource Management Architecture in 4A Engine,” REAC 2024](https://enginearchitecture.org/downloads/REAC_2024_4A.pdf), deck slides 9–14 and 30–35 | The talk distinguishes logical resources, views and backend storage, and shows logical textures borrowing cache entries. Apply the ownership distinction to pass definitions, their invocations and device pipeline leases. The source example is texture streaming. |
| Sebastian Aaltonen, [“Modern Mobile Rendering @ HypeHype,” REAC 2023](https://enginearchitecture.org/downloads/reac2023_modern_mobile_rendering_at_hypehype.pdf), PDF pp. 14–15 and 23 | The renderer prepares pipeline state and material bindings before the draw loop. Prewarm the recipes required by Ludus's candidate graph and reuse compatible state across invocations. Scope preparation to the admitted recipes needed by the selected configuration. |
| Nicolas Lopez and Michel Bouchard, [“Anvil Rendering Architecture,” REAC 2025](https://enginearchitecture.org/downloads/REAC_2025_Anvil.pdf), deck slides 15–17 and 43–46 | Anvil reports difficulty controlling and scaling packaged pass registration with runtime resource resolution. Use references resolved by Ludus's existing graph library and package closure, then resolve device compatibility at preparation. |

These sources motivate ownership and preparation boundaries. The proposed
Ludus comparison measures reuse, readiness and lifetime on a current
portable pass.

## Conference and journal research plan

This is a contextualized reading plan for the [proposed renderer](renderer-systems.md),
including Graphics Programming Conference (GPC). The venue ranking and the
questions/gates below are Ludus engineering judgments. Official programs,
session descriptions, publication pages and abstracts were checked; this section
does **not** claim full slide/recording/paper review, companion-code inspection or
validated implementation results. The actual Gems readings above remain separate
evidence. A recommended talk is not an adopted algorithm or a dependency.

### Where each venue helps

Use REAC and GPC first for architecture and production constraints, GDC for
targeted shipping experience, and HPG/JCGT for algorithms and validation. Read
backend-specific material alongside the relevant API specification and Ludus's
pinned profile. Select deeper transport research when an effect reaches its phase;
do not make surveying every venue a prerequisite for the small L0 renderer.

| Venue / publication | Ludus context and review priority |
| --- | --- |
| [Rendering Engine Architecture Conference (REAC), 2025 archive](https://enginearchitecture.org/2025.htm) | First priority for Renderer/GDI boundaries, instance processing, shader permutations and pass organization. Use production constraints to challenge maintainability and ownership choices in L0–L5. |
| [Graphics Programming Conference (GPC), 2025 archive](https://graphicsprogrammingconference.com/archive/2025/) and [2024 archive](https://graphicsprogrammingconference.com/archive/2024/) | First priority alongside REAC. Select backend evolution, debugging, shader infrastructure, portability and path-tracer architecture sessions using the mapping below. |
| [GDC, Frostbite FrameGraph session](https://www.gdcvault.com/play/1024612/FrameGraph-) | Targeted pass/resource planning and shipping-engine reviews. Ask how much automatic dependency planning GDI needs while retaining a readable authored sequence. |
| [High-Performance Graphics (HPG), 2026 program](https://highperformancegraphics.net/2026/index.html) | Algorithm/performance review for L5 geometry and RT3–RT4 scaling. Prioritize animated geometry, BVH storage/traversal and reconstruction candidates after collecting Ludus bottlenecks. |
| [Journal of Computer Graphics Techniques (JCGT)](https://jcgt.org/about.html) | Practical peer-reviewed techniques; author-provided code is available for some papers. Prioritize numerical correctness, sampling, filtering and compact data representations. Check licensing before adapting code. |
| [Vulkanised, 2026 program](https://www.vulkan.org/events/vulkanised-2026) | RHI completion, frame pacing, hazards, shader tooling and native RT integration. Vulkan-specific mechanisms remain private; separately prove Metal and browser behavior or explicit unsupported results. |
| [SIGGRAPH Advances in Real-Time Rendering in Games, 2026 course](https://advances.realtimerendering.com/s2026/index.html) | Production effect reviews for L4/L6 and RT2–RT4: sampling/reconstruction, radiance caching, volumetrics and geometry detail. Keep effects separate from baseline lifecycle requirements. |
| [I3D, 2026 papers](https://i3dsymposium.org/2026/papers.html) | Interactive algorithm reviews, especially RT4 resampling. The ReSTIR PT publication already identified above needs its full estimator reviewed before implementation. |
| [SIGGRAPH / SIGGRAPH Asia and ACM Transactions on Graphics (TOG)](https://www.siggraph.org/inside-siggraph/publications/) | Deeper material, transport and sampling work when specifying a reference profile or new estimator. Distinguish journal papers from conference-paper tracks. |
| [Eurographics Symposium on Rendering (EGSR) / Computer Graphics Forum (CGF), 2025 program](https://conferences.eg.org/egsr2025/programme/) | Targeted shading, filtering and light-transport research for L3/L4 and RT1/RT4. Track each paper's actual publication; EGSR's program includes several publication tracks. |

### GPC readings mapped to Ludus

The 2025 talk names and speakers below are verified against the official archive;
the 2024 Vulkan session is also described on [Khronos's event page](https://www.khronos.org/events/graphics-programming-conference).
The archive links supply the slides/recordings where available. The final two
columns are proposed Ludus review outputs, not findings asserted from unread talks.

| Session and attribution | Owning boundary / phase | Question the full review should resolve | Required evidence before changing the design |
| --- | --- | --- | --- |
| Julien Koenen and Lukas Feller, “Vulkan in Enshrouded,” GPC 2024 | RHI/GDI; L1–L3 | Which abstraction, memory, synchronization and pipeline-compilation lessons expose gaps in our readiness/retirement contracts? | Upload/build/use/retire traces under overlap, allocation pressure and loss; compare cold pipeline stalls and peak memory. Keep native choices out of public Renderer. |
| Sidney Just, “The aircraft of Theseus - Shipping X-Plane for 30 years,” GPC 2025 | Backend and shader-tool migration; L1–L5 | Which incremental migration boundaries and driver limitations should our portable profile and package versions account for? | Native/browser shader-layout fixtures, cache invalidation and supported-feature failure paths; preserve the R1 consumer contract during extensions. |
| Sergey Sharybin, “Blender Cycles: architecture of a unified CPU/GPU path tracer,” GPC 2025 | Renderer transport versus traversal; RT1/RT4 | How should shared transport code, scheduling and interactive cancellation evolve across targets? What measurements justify splitting the initial loop into wavefront queues? | Same-profile raw images across execution variants; divergence/occupancy/dispatch/queue-memory measurements and bounded-queue failures. No immediate Cycles backend or CPU-tracer dependency. |
| Martin Mittring, “ShaderToHuman (S2H) HLSL/GLSL library for debugging shaders,” GPC 2025 | Shader tools and Renderer inspection; L3 onward | Which diagnostic ideas can work with our SPIR-V/WGSL/GLSL/MSL packages and bounded source/view/pass labels? | A deliberately faulty shader identifies its invocation and inputs; overflow is safe, instrumentation is disabled by default, and readback is asynchronous. Library adoption needs a separate license/API/profile review. |
| Dominik Lazarek and Philip Hammer, “Visibility Buffer and Deferred Rendering in DOOM: The Dark Ages,” GPC 2025 | Renderer geometry/material reconstruction; L5/L6 | Does a visibility-buffer variant beat our forward/clustered path for the actual geometry and material suite? | Compare CPU/GPU tails, bandwidth and memory, plus masked geometry, derivatives, MSAA and transparent-path correctness. Preserve the direct reference path. A production example alone does not change the default. |
| Mircea Catana and Peter Bay Bastian, “Bringing Hitman to your pocket,” GPC 2025 | Metal backend, attachment policy and content profiles; L1/L3/L5 | Which tile-GPU constraints require private pass/format choices or authored lower-cost material variants? | Physical Apple-device pass timings and attachment traffic where observable; inspect load/store/resolve intent and image parity. Keep platform adaptation out of scene extraction. |
| Lukas Feller, “Lessons learned from shipping a GPU Particle System,” GPC 2025 | Shader cooking and future particle adapter; L3/L6 | How should compute variants be admitted and prewarmed without unbounded compilation/cache growth? | Variant inventory, cook times, cache size, first-use/reload behavior and explicit failure budgets. GPU particle queues need their own overflow/lifetime plan before adoption. |

For this task the session descriptions support the choice of readings; their
implementation details remain pending. In particular, Cycles' experience is a
reason to scrutinize RT4 scheduling, not proof that a wavefront implementation
wins for Ludus's initial RT1 workload. The DOOM session is a comparison candidate,
not proof that our portable forward baseline is obsolete. X-Plane and Hitman
provide target-specific review cases, not permission to change all API profiles.

### Additional targeted readings

- Max Bukhalov and Egor Orachev, “Geometry rendering and shaders infrastructure
  in Warhammer 40000: Space Marine 2,” and Nicolas Lopez and Michel Bouchard,
  “Anvil Rendering Architecture,” [REAC 2025](https://enginearchitecture.org/2025.htm):
  review the L0/L3/L5 instance, binding and permutation boundaries. Produce an
  explicit packet/material/variant inventory and a measured comparison before
  introducing indirection or GPU work generation.
- Yuriy O'Donnell, [“FrameGraph: Extensible Rendering Architecture in Frostbite,”
  GDC 2017](https://www.gdcvault.com/play/1024612/FrameGraph-): review L1/GDI pass
  declarations. Require inspectable attachment preservation, dependency and
  retirement plans, including shared-target rectangles and async uploads. Do not
  infer native memory aliasing support on WebGPU from a frame-graph abstraction.
- Charles Giessen, “Frames in Flight Demystified,” and Lucas Miguel Antunes da
  Silva, “Solving All Synchronisation Problems with Timeline Semaphores,”
  [Vulkanised 2026](https://www.vulkan.org/events/vulkanised-2026): review RHI's
  accepted-use ledger and L1/L2 latency. Require resize/acquisition failure,
  overlapping resource uses and device-loss traces. CPU completion and arbitrary
  frame counts must not substitute for native retirement evidence.
- Mark Jarzynski and Marc Olano, [“Hash Functions for GPU Rendering,” *JCGT* 9(3),
  pp. 21–38 (2020)](https://jcgt.org/published/0009/03/02/): review RT1 random
  sample addressing and quality. Supplement hash benchmarks with multi-seed
  image convergence and pixel/sample/dimension correlation checks on supported
  shader targets; a fast hash alone does not validate the entire sampler.
- HPG 2026's [animated-geometry and BVH sessions](https://highperformancegraphics.net/2026/index.html)
  are discovery entries for RT3's rebuild/refit and storage decisions. Full-paper
  review must separate custom/software BVH techniques from operations actually
  exposed by hardware acceleration-structure APIs. Do not promise access to
  vendor-controlled BVH node layouts or transfer published timings to Ludus.

### Review order and completion criteria

1. **Architecture and lifetime:** Enshrouded Vulkan, X-Plane, REAC infrastructure,
   Frostbite and Vulkanised. Resolve any L0–L3 ownership/hazard ambiguity first.
2. **Shader iteration and portability:** ShaderToHuman, particle permutations and
   Hitman. Specify diagnostic and cooking budgets before adding new variants.
3. **Independent ray reference:** Cycles architecture and JCGT sampling, alongside
   the actually inspected Gems/PBRT evidence above. Keep raw RT1 accumulation.
4. **Measured advanced paths:** DOOM visibility buffers, HPG geometry/BVH work and
   I3D/Advances transport/reconstruction. Implement only the candidate that answers
   an observed L5/RT3/RT4 bottleneck at the target quality and maintenance budget.

Each completed review records the exact sections/slides consulted, assumptions,
applicable profiles, adopted idea or reason for rejection, and the owning contract
or experiment. Architectural changes go into their existing owner, with this
review linking the evidence. Implementation carries attribution beside affected
code and meaningful regression/quality fixtures. Validate target-device timing,
memory and image/temporal error; distinguish unavailable counters from zero.
Thanks to the named authors and venue organizers for making these materials
available. This plan imports no code/assets and establishes no new dependency.

## Other articles and current research

The catalog also contains Cozzi's “A Framework for GLSL Engine Uniforms” and
“Delaying OpenGL Calls,” Schertenleib's “A Multithreaded 3D Renderer,” and Moore's
“A GPU-Managed Memory Pool.” This task reused their existing
[RHI/GDI review](rhi-gdi-gems-review.md) rather than claim fresh readings.
Camera chapters are covered by the [camera review](camera-systems-gems-review.md);
font alternatives by the [Text research](text-font-rendering-research.md).

Perspective/cached shadow mapping, deferred shading, SSAO and many other mapped
articles remain targeted follow-up readings when an implementation needs the
specific technique. They did not justify replacing the core design. Local PDF
availability is not a reason to accumulate features without a consumer.

The publisher's abstract for Olsson, Billeter and Assarsson's
[“Clustered Deferred and Forward Shading”](https://diglib7.eg.org/items/6342d4d6-5220-4376-a5c6-a153058f4a3c/full)
(*High Performance Graphics*, 2012, pp. 87–96) corroborates grouping by 3D position
and optional normals. The full preprint fetch was unavailable; the actual local
tiled chapter supplies the more detailed lighting evidence used here.

Current API checks used Khronos's
[pipeline cache guide](https://docs.vulkan.org/guide/latest/pipeline_cache.html),
[pipeline management sample](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_cache/README.html),
[render-pass specification](https://docs.vulkan.org/spec/latest/chapters/renderpass.html)
and [synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).
The [WebGPU specification](https://www.w3.org/TR/webgpu/) search excerpt verified
the optional timestamp feature; full-page retrieval exceeded the browsing limit.
Existing RHI source/architecture provides the pinned browser constraints.

Google Filament's
[standard BRDF discussion](https://github.com/google/filament/blob/main/docs/Filament.md.html)
provides a contemporary physically based shading reference. The
[SIGGRAPH 2025 Advances course program](https://advances.realtimerendering.com/s2025/)
was inspected as a discovery source for stochastic lights, ray tracing and
transparency. Program descriptions are not algorithm readings or Ludus benchmarks.
Those paths need their own reviews, capabilities and measurements before adoption.

Thanks to all named authors. The architecture adopts ideas and records departures;
it copies no implementation code, diagrams or companion assets. Implementation
must carry concise references near the affected code, preserve applicable notices
and link this decision record. No book technique or current feature is a measured
performance claim for Ludus until its stated acceptance gate passes.
