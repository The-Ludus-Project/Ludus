# Profiling Subsystem — Final Architecture (Reconciled)

> Status: **Reconciled design and staged implementation contract.**
> [Current module usage](../modules/foundation/profiling.md) records the delivered
> CPU tracing scope; later phases below are proposals. This document supersedes the independent
> [baseline](profiling.md) after deliberate reconciliation against the
> [historical literature review](profiling-literature-review.md) (Rabin, Evertt,
> Hjelstrom & Garrabrant, Lung). The baseline was treated as a proposal to be
> revised, not defended; the references were treated as evidence, not requirements.
>
> **Provenance legend** used throughout so the origin of every decision is explicit:
> - **[FP]** — survives from first-principles baseline reasoning, unchanged.
> - **[FP+R]** — baseline reasoning, *sharpened* by the references (evidence improved it).
> - **[R→adopt]** — an idea the references contributed that changed the design.
> - **[R→reject]** — an idea from the references deliberately rejected, with reason.
> - **[?meas]** — remains uncertain; a measurement gate decides it (see §12).
>
> Audience: engine engineers. Prerequisites as in the baseline (ADR 0003/0004/0005,
> assertions design, build-flavor system).

---

## 0. Executive summary of what changed

The baseline was **substantially right** and survives largely intact. The references did
not overturn its structure; they **hardened its semantics** in four places and forced two
outright corrections. Net changes from baseline:

| # | Change | Class | Driver (evidence) |
| --- | --- | --- | --- |
| C1 | **Event/timeline stream is the ground truth; the hierarchical tree is a *derived* off-line view.** Baseline already leaned this way; now it is a firm, load-bearing rule. | **[FP+R]** | H's shared-`CurrentNode` tree and R's parent-inference are aggregations that discard chronology and don't survive concurrency (review §4.4, §7.1–7.2). |
| C2 | **Invocation counts (calls/frame) and explicit inclusive-vs-exclusive semantics become first-class**, not optional presentation. | **[R→adopt]** | R & H: counts distinguish *more work* from *slower work* and expose behavioral bugs; ambiguous "time" misleads (review §2.3, §4.3). |
| C3 | **Instrumentation identity is split into three orthogonal axes: site, caller-context, and category** — never one identifier standing for all. | **[R→adopt]** | H (caller context), E (category/group), R/H (site). Review §7.1: "One identifier cannot safely stand for all four." Rejects raw-pointer identity. |
| C4 | **Memory profiling is fully severed from Trace's transport and loss policy.** It gets provenance-at-free, its own lossless/snapshot contract, and lives with the allocator. Corrects baseline §10's "feeds the *same* capture infrastructure." | **[R→adopt]** | L: a lost free permanently corrupts live-byte accounting; frees cross threads and outlive scopes/captures (review §5, §6). |
| C5 | **Corrected the baseline §4 claim that "RAII lexical scope enforces same-thread begin/end."** It does not under coroutine suspension / job migration. Same-thread pairing is now an explicit *contract*, and cross-context work uses flow events, not one spanning scope. | **[R→adopt]** | Review §7.2, §8: "a coroutine can suspend with an RAII object alive and resume elsewhere." |
| C6 | **Statistical outputs must name their window and denominator; smoothed display is separated from exact retained hitch evidence.** Decaying extrema are banned as generic min/max. | **[R→adopt]** | R's decaying extrema "can conceal an earlier hitch"; percentages shift when the denominator changes (review §2.4, §8). |
| C7 | **Clock correctness precedes speed, made explicit as a gate.** Keep `steady_clock` ns; TSC only after a measured ordering/frequency/migration contract. | **[FP+R]** | E's/H's raw-tick + CPU-MHz calibration is not a sufficient modern contract (review §3.4, §4.2). |

Everything else in the baseline — three separate subsystems, cheap-to-include header,
per-thread chunked lock-free buffers, off-path collector reusing the logging `AsyncBackend`
discipline, Perfetto-first export, deferring GPU until an RHI and memory until an allocator
— is **kept**. No historical *mechanism* (global name arrays, FourCC, binary patching,
single-precision seconds, shared current-node, draw-time reset) was adopted.

---

## 1. Goals and non-goals (kept, with two sharpened goals)

The [graphics debugging architecture](graphics-debugging.md) owns the proposed
GPU annotation and external capture integration. Annotation enablement is
independent of CPU trace recording and GPU timestamp collection. The
[basic GPU timing contract](#basic-gpu-timing-for-perfetto) below stages frame
duration export before pass timing and correlated GPU lanes. The external
debugger still owns capture visualization and shader/resource analysis.

Baseline goals stand **[FP]**. Sharpened by the references:

- **[R→adopt]** Add explicit goal: *distinguish workload change (calls/frame) from cost
  change (time/call)*. R and H both show this is often the fastest route from symptom to
  cause and can reveal work that runs twice or never (review §2.1, §4.3).
- **[FP+R]** Reframe the "historical inspection" goal: retain **temporal evidence around
  an unexpected hitch**, because every historical summary model (running totals, reset
  windows, decaying stats) *discards the sequence that produced a peak* (review §4.4). This
  is exactly what the baseline's ring + pre/post-trigger capture (§12) provides, and the
  references make it the decisive advantage of an event model over an aggregate model.

Non-goals unchanged **[FP]**: not a sampling profiler, not an in-engine flame-chart UI,
not a GPU capture tool, not a telemetry platform. The review **reinforces** these: modern
Perfetto/Tracy/perf/heaptrack/RenderDoc cover visualization, sampling, and graphics
capture, narrowing the reason to build engine code to *semantic context tools can't know*
(review §7.5).

---

## Basic GPU timing for Perfetto

Delivery update, 2026-10-20: this is a proposed extension to the implemented CPU
tracer. The RHI already provides `EnableGpuTiming`, `ArmGpuTiming` and
`GetGpuTiming` in `measurement.h`, and editor viewport Stats already polls and
re-arms its single frame probe. The probe reports elapsed device-clock duration
without CPU/display clock correlation. CPU tracing, viewport Stats and benchmark
timing do not yet establish a combined editor Perfetto recording.

Start with coarse frame-duration samples in the existing Chrome/Perfetto JSON
export, then add timed physical passes, then calibrated execution tracks. Reuse
the existing probe and measurement semantics instead of building a second GPU
timer. The [editor workflow and P0–P3 plan](graphics-debugging.md#perfetto-recording-in-the-editor)
own recording controls, modes and delivery order. No vendor capture tool is
required for this path.

### Duration samples before execution slices

P1 exports a named `GPU/FrameDurationNanoseconds` counter on a clearly named GPU
measurement track, with device/queue and capture identity. Its value measures
the native probe interval, including applicable GPU stalls/idle gaps; it is
not utilization, display latency or a sum of busy cycles. If the selected native
probe covers one command buffer, identify that scope rather than claiming it
covers independent uploads or other queues.

Each sample retains device-session generation, submitted frame sequence, queue,
sampling method, validity/precision, duration and CPU submission/collection
timestamps. Associate the counter with the recorded CPU submission marker and
label that timestamp as attribution, not GPU execution start. Collection time
is separate. A numeric duration alone must never be drawn as an execution slice
beginning at submission or ending when a later poll returns. Keep current
`GpuTiming` compatibility; add the missing frame association in the owning
sampler/session boundary before exporting samples.

Use a transport-neutral bounded sample record accepted by the compiled exporter;
FoundationProfiling must not include RHI/native types or borrow query storage.
The existing `SampleCounter` stamps emission time and accepts only an unsigned
value, so polling it later is insufficient for originating-frame attribution.
Extend the export/sample interface explicitly for stored timestamps, identity,
units and validity. Validate finite/nonnegative durations and checked conversion
to the encoded unit; preserve a valid quantized zero. Round once with a stated
policy rather than silently truncating or overflowing a backend floating value.

CPU scopes and GPU measurements have separate recording gates and storage.
CPU-only mode enables the existing CPU recorder; GPU-only mode records typed GPU
samples plus minimal frame/session metadata without admitting ordinary CPU
scopes. Both admits both sources. The exporter composes their immutable snapshots
into one trace, including a valid GPU-only trace with no CPU events. Preserve
the existing no-data result for a truly empty capture. Gate the new Perfetto
recording path with the profiling build policy and report unavailable sources
explicitly; do not silently fall back from a requested GPU source to CPU timing.

### Bounded sampling and deferred results

Begin P1 with at most the current probe's single outstanding sample per device
session. Poll completion without waiting and arm the next eligible submission
after the previous sample is consumed. This intentionally samples some frames;
it does not promise a measurement for every presented frame. Share one owner
between viewport Stats and capture, fan out each result once, and exclude a
Stats sample submitted before the recording's admission boundary. Independent
consumers must not race to re-arm or replace the same terminal result.

Count eligible, sampled, unavailable, disjoint/failed, capacity-skipped and
unresolved frames over the recording interval. Include finalization outcomes
separately. Never turn Pending/Unavailable/Disjoint into a zero measurement,
duplicate a result across polls, or retain a stale duration after device restart.
Report the actual sample window and count beside any mean/percentile; perform
aggregation off the recording path. Quantization and sparse sampling limit
interpretation even when the displayed number has many digits.

RHI owns query/command-buffer lifetime and completion checks. A query slot cannot
be reset/reused before its submission completes and its result is consumed or
explicitly discarded under the request-generation contract. Existing probes
have narrow allocation behavior, including deferred native allocation: audit it
before promising a zero-allocation recording path. Move reservation/preparation
before Recording where needed, and separately bound setup cost. Reuse rather
than re-create native query resources per sample. No wait-for-idle, blocking
query flags or synchronous readback may be added for profiling.

Stopping closes admission while already submitted results drain under a finite
deadline. Freeze CPU events and resolved GPU samples under one coordinator;
late results cannot enter a new capture. Timed-out results are marked incomplete,
then native storage retires through the existing completion owner even if the
export is already finalized. Do not destroy in-flight queries to meet a UI
deadline. Output failure retains the frozen recording for retry/discard.

### Backend precision and later pass timing

Metal initially reuses completion-only command-buffer GPU duration; that is not
per-pass timing. P2 investigates capability-checked counter sampling at legal
encoder/pass boundaries before exposing named pass durations. Never split a
command buffer or encoder just to obtain a prettier timing breakdown without
measuring and declaring the resulting execution change.

Vulkan reuses timestamp queries with the actual queue's support, valid bit count,
tick period, wrap/discontinuity handling and availability/completion rules.
P2 adds bounded query pairs around executed physical passes and documents the
stage interval. Logical passes merged into one physical pass retain annotation
identity but share the physical timing; do not fabricate independent logical
durations or sum nested/overlapping intervals as a frame total. Query pressure
skips samples with coverage information instead of serializing work.

WebGPU/WebGL timing retains backend feature/privacy/disjoint constraints and is
qualified separately. A CPU wall-clock timer around submission is never a GPU
fallback. Multiple graphics/compute queues have separate measurement tracks.
The existing benchmark timing and image checks remain independent controls.

P3 adds real execution slices only when a backend clock mapping to the CPU trace
domain is validated and its uncertainty/drift recorded. Vulkan calibrated
timestamps are one capability-dependent route; Metal needs its own documented
mapping. Recalibrate at bounded intervals and invalidate the mapping after a
clock/session discontinuity. If unavailable or too uncertain, retain duration
counters. Hosted-process clocks need a separate offset/error contract.

Use distinct process/track identities and legal nesting for Chrome `B`/`E` or
`X` slices. Queue overlap belongs on separate tracks; collection-thread order
must not invent parentage or serialize overlapping GPU work. Submission/frame
IDs enable correlation even in duration-only recordings, without implying
cross-clock execution alignment. Keep native Perfetto protobuf export a later
format decision rather than a prerequisite for the coarse feature.

### Timing and editor acceptance

Validate CPU, GPU and Both modes in the real editor and installed-SDK path.
Check exported units, originating-frame association, explicit source filtering,
GPU-only nonempty export, repeated start/stop, unexported snapshot protection,
Stats visibility changes, Busy and profiling-disabled builds. Force delayed,
skipped, quantized-zero, disjoint, device-lost and exhausted-query outcomes; check
coverage and finalization without GPU waits or stale samples. Verify viewport
navigation, edits and resize remain interactive throughout recording/export.

Compare an optimized control against each recording mode on the same machine,
scene, compiler and settings; report CPU recording cost, query cost, result lag
and coverage separately. Inspect a real exported trace in Perfetto and compare
durations against the existing RHI probe/benchmark for a controlled workload.
Counters need no invented clock alignment; execution slices require a separate
measured calibration and overlap/nesting acceptance. A Linux pass does not
qualify Metal and a Mac pass does not qualify browsers or hosted processes.

Thanks to Khronos Group, [Timestamp queries](https://docs.vulkan.org/samples/latest/samples/api/timestamp_queries/README.html)
and [Queries](https://docs.vulkan.org/spec/latest/chapters/queries.html), for the
query support, units and availability contract; to Apple, [GPUStartTime](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime)
and [GPUEndTime](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpuendtime),
for completion-only command-buffer timing; and to Perfetto contributors,
[Visualizing external trace formats](https://perfetto.dev/docs/getting-started/other-formats),
for JSON counter/duration conventions and slice nesting. Duration-only placement,
coverage policy and the staged delivery are Ludus decisions. Retain relevant
reference comments beside future sampler/exporter changes.

## 2. Step 1 — Re-evaluation of every major baseline decision

| # | Baseline decision (profiling.md) | Verdict | Reasoning |
| --- | --- | --- | --- |
| D1 | Three independent subsystems (Trace / Counters / GPU+Memory), not one framework (§2) | **Keep** **[FP+R]** | Review §6 independently reaches the same separation for memory ("different completion rules are stronger evidence for separate ownership"). Strengthened, not weakened. |
| D2 | Cheap-to-include public header; type-erase heavy logic to `.cpp` (§18) | **Keep** **[FP+R]** | All four articles omit build-cost analysis (review §2.4, §3.4, §4.4, §5.4); the review repeatedly warns not to put containers/UI/platform deps in a ubiquitous header. Confirms our constraint. |
| D3 | Compile-time-gated RAII scope; disabled = zero code (§3–4) | **Keep** **[FP+R]** | Universally endorsed (R/E/H all converge on RAII + compile-out). Review §8 asks only to *verify* true compile-out and early-return pairing — a validation gate, not a design change. |
| D4 | Event stream as primary representation; hierarchy derived (§3, §4-hierarchy) | **Refine → make firm rule (C1)** **[FP+R]** | Baseline said "reconstructed off-line"; the review's dissection of H's tree and R's parent-inference proves the tree must *never* be the producer-side ground truth. Elevated to an invariant (§6 here). |
| D5 | Per-thread chunked ring, lock-free chunk handoff, off-path collector (§6, §23) | **Keep** **[FP+R]** | Review §7.2/§7.3: TLS partitions attribution but "does not establish scalability"; per-thread buffers + bounded preallocation are the right primitives, to be *measured* not assumed. Matches baseline's own [?meas] posture. |
| D6 | Compile-time FNV-1a name hash → `ZoneRegistry`; id in events (§13–14) | **Refine (C3)** **[FP+R]** | Keep hashed *site* identity; **add** a separate caller-context axis and category axis. Review §7.1 + rejection of raw-pointer identity (H/L). Baseline conflated site with context. |
| D7 | Frame = boundary markers + epoch, not a container (§8) | **Keep** **[FP+R]** | Review §7.2 independently: "not every quantity is frame-owned or frame-resettable"; begin-frame attribution for boundary-crossing scopes is the right call. Add a gate comparing begin-frame vs clip-to-frame (§12). |
| D8 | Counters/gauges as a separate scalar subsystem (§11) | **Keep** **[FP+R]** | Review §8 explicitly warns *not* to equate scalar counters with timed scopes just because Evertt calls both "counters." Our separation is correct; we make the distinction sharper (§3 here). |
| D9 | Hitch detection = ring + pre/post-trigger capture, evaluated on collector (§12) | **Keep** **[FP+R]** | This is the modern extension the historical summary models *lack* (review §4.4, §8). Add a gate: pre-trigger data must survive overload and export must not induce a second hitch. |
| D10 | Visualization external-first (Perfetto), tiny overlay only (§16–17) | **Keep** **[FP+R]** | Review §7.5 documents that Perfetto track events already provide nested slices/counters/flows; an engine tree viewer is no longer justified. Reinforced. |
| D11 | Statistics as off-line derivation (§1, §11, §16) | **Refine (C6)** **[R→adopt]** | Add hard rule: every statistic names its window + denominator; smoothed UI is separate from exact hitch evidence; no decaying extrema masquerading as min/max. |
| D12 | Clock = `steady_clock` ns now; `rdtscp` later behind an abstraction (§5) | **Keep, elevate to gate (C7)** **[FP+R]** | Review §3.4/§4.2 dismantles raw-tick+MHz calibration. Correctness (ordering, frequency contract, migration) is a *precondition* for any TSC backend, not an optimization detail. |
| D13 | Same-thread scope pairing "enforced by RAII lexical scope" (§4) | **Replace (C5)** **[R→adopt]** | Factually wrong for coroutines/migration (review §7.2). Replace with an explicit contract + flow events for cross-context work. |
| D14 | GPU trace design, build later (§9) | **Keep / Stage** **[FP+R]** | CPU scopes do not measure GPU execution. Existing RHI frame probes enable the proposed P1 Perfetto duration export; P2 pass queries and P3 calibrated execution lanes remain separate gates in the basic GPU timing contract above. |
| D15 | Memory profiling deferred, tiered, allocator-owned (§10) | **Refine (C4)** **[R→adopt]** | Keep deferral and tiers; **correct** the "feeds the same capture infrastructure" wording — memory needs provenance-at-free and its own loss/snapshot contract, not Trace's drop policy. |
| D16 | `JobId` field reserved now, correlation logic later (§7) | **Keep** **[FP+R]** | Review §7.2 confirms cross-thread/task correlation "must be modeled separately"; a reserved field + flow events is the right seam. |
| D17 | Dynamic-name runtime interner, opt-in, bounded (§14) | **Keep** **[FP]** | Uncontroversial; the references' dynamic naming needs are covered. |
| D18 | Fixed-size POD `TraceEvent`, side-channel for variable payloads (§13) | **Keep, add counts (C2)** **[FP+R]** | Add per-event fields/derivation for invocation counting; keep the record small. |

Nothing is classified **Remove** outright — the baseline contained no dead weight. The only
**Replace** is the incorrect same-thread claim (D13). GPU delivery (D14) now has a
coarse-to-correlated plan using existing RHI probes; memory tiers (D15) retain
their own staged implementation and ownership.

---

## 3. Step 2 — Every useful reference idea, evaluated

Applying the 8-question test (problem / do we solve it / is it better / runtime cost /
build cost / complexity / external-tool coverage / unique-semantics justification). Only
favorable tradeoffs are adopted.

### Adopted **[R→adopt]**

| Idea (source) | Problem | Already solved? | Better? | Runtime cost | Build cost | Complexity | External tool? | Unique semantics? | Decision |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **Invocation counts / calls-per-frame** (R, H) | Is it more work or slower work? Behavioral bugs (2×/0× execution). | Partially (durations only). | Yes — orthogonal signal. | ~free (increment during off-line aggregation, or one field). | none. | Low. | Perfetto shows slice counts; but the *engine* names the scope. | Yes. | **Adopt** as first-class (C2). |
| **Inclusive vs exclusive, explicitly defined** (R child-subtraction, H) | "Time" is ambiguous; nested work double-counts. | Baseline implied it; not defined. | Yes. | Derived off-line (no hot-path cost). | none. | Low–Med (define union-of-children on one context). | Perfetto renders slices; *definitions* are ours. | Yes. | **Adopt** as analysis contract (C1, §5 here). |
| **Caller-context ≠ site identity** (H) | Same routine under load vs frame-update differs. | No (baseline hashed site only). | Yes for attribution. | Off-line reconstruction from begin/end order; no hot-path cost. | none. | Med (context is derived, not stored per node). | Perfetto flows/tracks help; semantics ours. | Yes. | **Adopt** (C3). |
| **Explicit "unlogged remainder"** (H) | Makes incomplete instrumentation visible. | No. | Yes — cheap honesty. | none (analysis-time). | none. | Low. | n/a. | Yes. | **Adopt** in analysis output. |
| **Provenance-at-free for memory** (L) | Attribute a free to its allocation origin across threads/scopes. | No (memory deferred). | Yes — the core memory insight. | Allocator-side metadata read at free. | n/a (allocator). | Med. | heaptrack does allocation capture; engine tags/domains are ours. | Yes. | **Adopt** into memory design (C4, §7 here). |
| **Live-usage vs allocation-churn as distinct metrics** (L) | Stable live bytes can hide heavy alloc/free traffic. | No. | Yes. | Counters. | n/a. | Low. | heaptrack partially. | Yes (engine tags). | **Adopt** into memory design. |
| **Statistical-window discipline** (R decaying extrema counter-example; E windows) | Misleading stats hide hitches / shift with denominator. | No (baseline said "moving median" loosely). | Yes — correctness. | none. | none. | Low. | n/a. | n/a. | **Adopt** as rule (C6, D11). |
| **Category/group as capture-and-display axis, separate from site** (E) | Teams need focused views without coordinating every probe. | Baseline had optional category metadata. | Yes — separate *capture* enable from *display* filter. | none hot-path (metadata). | none. | Low. | Perfetto category selection. | Partial. | **Adopt**: category is metadata + capture policy, distinct from display filter. |
| **Clock-correctness-before-speed** (E/H negative example) | Wrong tick semantics corrupt durations. | Baseline chose steady_clock. | Yes — make it a gate. | n/a. | n/a. | Low. | n/a. | n/a. | **Adopt** as gate (C7, D12). |
| **Same-thread pairing is a contract, not a guarantee; use flows for migration** (L TLS + cross-thread free; review §7.2) | Coroutine/job migration breaks lexical pairing. | No (baseline over-claimed). | Yes — correctness. | none (design). | none. | Low now; Med when jobs land. | Perfetto flow events. | Yes. | **Adopt** (C5). |
| **Remote inspection for headless/devkit** (L) | Inspect without a large in-game viewer; headless targets. | Baseline: external-first export. | Complementary. | Bounded transport. | none in core. | Med (out of core). | Tracy is exactly this. | Partial. | **Adopt** posture: prefer Tracy/remote over in-engine UI (already baseline direction). |

### Rejected **[R→reject]** (copied verbatim would harm us)

| Rejected mechanism (source) | Why rejected |
| --- | --- |
| Global name-array scan + parent inference from start times (R) | O(active names) per begin/end; merges caller contexts; no concurrency model. Superseded by hashed sites + off-line nesting from explicit begin/end order. |
| Single-precision seconds absolute timestamps (R) | Subtracting large rounded floats loses short-interval precision as uptime grows. We keep integer ticks, convert durations only for display. |
| Decaying extrema presented as generic min/max (R) | Can conceal an earlier hitch and implies statistics not computed. Banned (C6). |
| FourCC as the organizing namespace (E) | Manual tiny namespace, encoding pitfalls; no benefit over hashed descriptors + categories. |
| Name registration/lookup on every timed use (E) | Makes probe cost scale with registry/string work; E itself cautions against it. We register once, store the id. |
| `RDTSC` + CPU-MHz/sleep calibration as the clock (E, H) | Not a sufficient modern contract (ordering, frequency meaning, migration, portability). Gate C7. |
| Resetting measurement state inside `DrawCounters` / draw-time reset (E) | Collection semantics must not depend on rendering being visible/present. Frames are markers, not resets (D7). |
| One shared `CurrentNode` (H) | Concurrent execution interleaves unrelated stacks; a lock can't make them one meaningful stack. Per-thread buffers + derived trees instead. |
| Hot-path child-list search + first-encounter node allocation (H) | Cost grows with sibling count; allocates exactly when a new scene first runs (cold-path disturbance). Producer records fixed POD events only. |
| Raw string-literal *pointer* as universal semantic identity (H, L) | Static lifetime ≠ unique/canonical identity across sites/modules; raw pointers aren't persistent capture identifiers. We use hashed site ids + separate context. |
| Hand-written x86 binary patching / trampolines as the allocator seam (L) | Makes allocator correctness depend on code relocation and executable-memory handling; poor coverage/portability. Use explicit allocator hooks (we own the allocator). |
| Embedding memory records/loss in the CPU Trace stream (implied by naive sharing) | A lost free permanently corrupts live-byte reconstruction; Trace's drop policy is unsafe for memory (C4). |
| Treating a shared CPU/memory scope tree as one runtime subsystem (H+L blend) | Memory state outlives threads/captures; CPU interval state has different completion/loss rules (review §5.3, §6). |
| Borrowed "under 5% enabled / <1% disabled" acceptance targets (E) | Historical goals aren't evidence of acceptable perturbation at modern event rates/frame deadlines. We define our own gates by measurement (§12). |

---

## 4. Step 3 — Layering discipline (protect against inflation)

Firm boundaries; a capability lives in exactly one layer and does **not** migrate inward
just because it shares a UI or a clock with another. **[FP+R]**

```text
┌ INSTRUMENTATION (must be trivially cheap, ubiquitous, header-only-cheap) ─────────────┐
│   CPU scopes · counters · GPU markers (later) · allocation tag/site (allocator-side)   │
│   → hashed site id + category; RAII; compile-out; NO storage/UI/platform in the header │
├ RECORDING (per-thread, lock-free, fixed POD) ─────────────────────────────────────────┤
│   Trace: per-thread chunked ring of TraceEvent (event stream = ground truth)           │
│   Counters: per-thread scalar shards                                                   │
│   Memory: allocator-owned provenance metadata + activity records (SEPARATE contract)   │
├ CAPTURE / AGGREGATION (off hot path; one collector; AsyncBackend lifetime discipline) ─┤
│   drain chunks · own string table · rolling window · hitch trigger · counter snapshots │
├ ANALYSIS (off-line / off-process; convenience over hot-path speed) ────────────────────┤
│   derive hierarchy tree · inclusive/exclusive · calls/frame · unlogged remainder ·     │
│   caller-context grouping · statistical windows (named) · critical-path (later)        │
├ VISUALIZATION (mostly external; tiny in-engine overlay only) ──────────────────────────┤
│   Perfetto/Chrome JSON · Tracy (live) · tiny counters+top-N overlay                     │
└ SPECIALIZED PROFILERS (own data models; may share transport/UI, never ownership) ──────┘
    Memory profiler (allocator)   ·   GPU profiler (RHI)   ·   (future) asset-streaming
```

Rules that prevent inflation:
- **Analysis never runs on the hot path.** Trees, inclusive/exclusive, counts-per-frame,
  and stats are computed in the collector/exporter/viewer, from the event stream.
- **Sharing a viewer or a clock does not mean sharing buffers, backpressure, or exactness
  guarantees** (review §6). Memory's lossless needs never leak into Trace's drop policy,
  and vice versa.
- **The instrumentation header stays under the CI build budget** (ADR 0005); specialized
  profilers pull their heavy deps into their own `.cpp`/modules.

---

## 5. Step 4 — In-game vs external visualization (final position)

**Decision: external-first, with a deliberately tiny in-engine overlay. [FP+R]** The
historical emphasis on in-game visualization was a product of its era (no good external
tools, a serial frame loop, a text renderer at hand). Modern workflows invalidate it as a
*default*.

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| **In-game overlay** | Zero context switch; works on devkit; good for QA/designers (R's insight). | Requires the renderer/UI under test; limited screen area; high maintenance for rich views; can perturb the very frame it measures (R's own text-render-in-graphics-interval pitfall). | **Tiny only**: counters + top-N scopes as text/HUD. No interactive timeline. |
| **Editor recording controls and summary** | Capture while editing, choose CPU/GPU/Both and inspect coverage without switching applications. | Must preserve responsiveness, process ownership and data after export failure. | **Adopt** in P0/P1; detailed timelines remain in Perfetto. |
| **Editor timeline/flame-chart implementation** | Contextual inspection. | Duplicates a mature viewer and enlarges the renderer-dependent UI surface. | **Defer**; recording controls do not require an embedded viewer. |
| **Standalone profiler app** | Full control. | We'd rebuild Perfetto/Tracy; huge cost. | **Reject** (build-vs-buy, review §7.5). |
| **External trace viewer (Perfetto)** | Free, powerful, universal, multi-monitor, remote-friendly, no engine UI code; opens exported captures anywhere. | Post-hoc (unless Tracy-live); needs a schema/export. | **Primary.** |
| **Live external (Tracy)** | Real-time, purpose-built for this instrumentation style, remote, low integration. | External dependency; must validate cost. | **Adopt as optional live backend** (Phase 3). |
| **Hybrid (overlay + export + optional Tracy)** | Immediate glanceable feedback *and* deep off-line/live analysis; matches remote/headless/automated capture. | Two consumers to keep coherent (mitigated: both read the same collector output). | **Chosen.** |

Modern-workflow drivers (all favor external/hybrid): multi-monitor and remote dev make an
external viewer *more* convenient than an overlay; console/devkit testing needs remote
capture (Tracy/exported files); **headless** workloads (already a Ludus platform backend)
have no screen at all — the overlay is meaningless there, and only export/remote works;
automated captures (CI hitch triggers) must produce *files*, not pixels. Therefore: **the
runtime engine renders only a minimal counters/top-N overlay; all timelines, flame charts,
memory graphs, and comparisons live in external tools.**

---

## 6. Step 5 — Hierarchical profiling: a derived view, not the record (C1)

**Ground truth = a per-thread stream of timestamped begin/end/instant/flow events. The
hierarchical tree is reconstructed off-line. [FP+R]** The references make this decisive:

- H's mutable runtime tree with a shared `CurrentNode` "retains where a sample occurred in
  instrumented context, not the temporal order of all invocations" and cannot represent
  concurrent stacks (review §4.4). R's flat-array parent inference from start-time ordering
  is unreliable under ties/precision and merges callers (review §2.4).
- For modern workloads the tree is **one view, not the ground truth**: across workers,
  summing elapsed durations can exceed frame wall time; subtracting async worker time from
  a dispatching scope is not valid exclusive time; the **critical path requires
  scheduling/dependency evidence a tree summary does not carry** (review §7.2).

Representation decisions:
- **Per-execution-context nesting** is reconstructed from each thread's begin/end ordering
  (matched by lexical pairing *within a thread*). This yields the familiar tree **per
  thread**, off-line, at no hot-path cost.
- **Caller-context sensitivity** (H's genuine contribution) is available as an analysis
  grouping (path = sequence of enclosing site ids), computed in the viewer/collector — not
  stored as nodes on the hot path (C3).
- **Cross-thread / job / async / GPU-queue relationships** are represented by **flow
  events** (a `flow_id` linking a producer event to a consumer event), *not* by a single
  scope spanning contexts. This is the seam for the reserved `JobId` (D16) and for GPU
  queues (D14). A tree is not forced where a DAG/timeline is the truth.
- **Inclusive vs exclusive** are defined precisely (C2): *inclusive* = entry→exit interval;
  *exclusive* = inclusive minus the union of nested synchronous child intervals **on the
  same context**. Neither equals scheduled CPU time (waits/preemption remain) — stated
  explicitly in output so no one misreads elapsed as CPU time (review §7.2).
- **Incomplete/malformed nesting** (crash, dropped chunk, coroutine): the builder tolerates
  unmatched begin/end, and **marks synthesized closures as such** — a fabricated
  chunk-boundary end is labeled "incomplete," never reported as a measured duration
  (review §8). "Unlogged remainder" is shown to expose instrumentation gaps.

---

## 7. Step 6 — Memory profiling ownership (C4)

The proposed [memory profiling architecture](memory-profiling.md) develops this
boundary under [ADR 0008](../decisions/0008-memory-management.md), including
optional side metadata, proven domain baselines, quiescent controls, and
explicit loss. Its [article review](memory-profiling-gems-review.md) records the
later reference reading. These links describe a design, not implemented features.

**The allocator owns allocation correctness and provenance; a separate memory-profiler
module owns rich capture/analysis; neither is the CPU Trace. [R→adopt]** This corrects the
baseline §10 wording ("feeds the *same* capture infrastructure").

Originates **in the allocator** (foundation, when it exists):
- Allocation identity, size, alignment, allocator/**tag** (compile-time-hashed, same scheme
  as zones), and a compact **origin token** (return address and/or current context id)
  stored as allocation metadata so **free can charge the origin** across threads and after
  the allocating thread exits (L's core insight). Aligned-payload and full-size contracts
  are the allocator's responsibility (review §5.4).
- Foundational totals + high-water marks (a few atomics) — cheap enough to keep in **all**
  builds incl. Release (baseline §20 tier).

Owned by the **memory-profiler module** (optional, dev/capture-only):
- **Four separate questions**, per review §6, each with its own records — never conflated:
  1. **Capacity** (requested/live vs reserved vs committed vs resident — different metrics).
  2. **Activity** (allocs/frees per interval, sizes, short-lived churn) — distinct from live
     totals; 1,000 alloc+free in a frame ends at zero live but is real activity.
  3. **Lifetime & attribution** (origin, current owner/tag, thread/task, capture boundary;
     pointer reuse must not conflate lifetimes).
  4. **Allocator efficiency** (fragmentation, free-space layout) — needs allocator-specific
     evidence beyond live bytes.
- **Loss semantics are lossless-or-explicitly-marked**, *not* Trace's silent drop: a lost
  free permanently corrupts live-byte reconstruction, so a bounded memory stream must, on
  overflow, **mark the affected analysis incomplete and re-establish a known snapshot** (or
  switch to a defined sampled mode). This is the single most important divergence from
  Trace (review §6).

Explicitly **not** adopted: binary patching/trampolines (we own the allocator → explicit
hooks), embedding a `pointer+int` header exactly as printed (alignment/failure/lifetime
unhandled), one-critical-section-per-origin as a scalability claim (measure contention).

Capture-mode overhead reality (review §5.4): per-allocation records + prefix metadata are
expensive (illustratively, 16 B metadata × 1M live allocs = 16 MB before tree/rounding) →
**tag totals always-on; per-alloc capture is opt-in capture-only; call-stack symbolization
is off-line, never on the hot path.** Delegate broad heap analysis to **heaptrack** where
it suffices; the engine adds only what needs its tags/domains (review §7.5).

**No duplicate tracking system:** there is exactly one allocation-tracking source (the
allocator); the profiler is a *consumer* of allocator events/metadata.

---

## 8. Step 7 — Revised target architecture

Differences from baseline are tagged inline. The layered flow (§4) is the structure; this
adds thread interactions, ownership, and the derived-view rule.

```text
INSTRUMENTATION (public header ≤ log.hpp build budget)
  LUDUS_PROFILE_SCOPE/FUNCTION/FRAME/COUNT/GAUGE/MARK   [+ GPU_SCOPE later]
  → constexpr {siteId (FNV-1a), category} descriptor + non-template RAII ScopedZone
  → optional flow_id begin/end for cross-context links            ◄── C3, C5 (flows)
        │  NowTicks()  (shared steady_clock ns; TSC only past gate C7)
        ▼
RECORDING  (per producer thread, TLS; single-writer, no atomics on push)
  Trace: chunked ring of fixed POD TraceEvent  ── EVENT STREAM IS GROUND TRUTH  ◄── C1
         (Kind: Begin|End|Instant|FrameMark|FlowOut|FlowIn|GpuBegin|GpuEnd)
  Counters: per-thread scalar shards
  [Memory: allocator-owned provenance metadata + activity records — SEPARATE, lossless ◄── C4]
  full chunk / frame-flip ──► DV-style bounded MPSC chunk queue (reused from logging)
        ▼
CAPTURE / AGGREGATION  (one collector; AsyncBackend stop/join discipline; §21)
  drain chunks · own authoritative string table · rolling window (pre/post-trigger) ·
  hitch trigger (collector-side, no producer cost) · per-frame counter snapshots
        ▼
ANALYSIS  (off-line / off-process; convenience > speed)          ◄── C1, C2, C6
  per-thread nesting → tree (DERIVED) · inclusive/exclusive (defined) ·
  calls/frame · caller-context grouping · unlogged remainder ·
  named statistical windows (no decaying extrema) · flows → job/GPU correlation
        ▼
VISUALIZATION
  Perfetto/Chrome JSON (primary) · Tracy live (optional) · tiny overlay (counters + top-N)
```

- **Hot-path behavior [FP]:** timestamp + relaxed TLS stores into the current chunk; no
  lock/alloc/string/tree.
- **Thread interactions [FP]:** producers ↔ collector only via the lock-free chunk queue;
  memory metadata is allocator-local (charged at free via origin token, C4).
- **Ownership [FP+R]:** thread owns its buffer; collector owns shared/export state;
  allocator owns allocation metadata. No shared `CurrentNode` **[R→reject]**.
- **Memory model [FP]:** fixed, pre-reserved; Trace drops-with-count on overflow; **memory
  stream marks-incomplete instead of silently dropping [R→adopt C4]**.
- **Identity [R→adopt C3]:** three axes — site (hashed id), caller-context (derived path),
  category (metadata + capture policy). Collision-checked in Development; widen to 64-bit if
  it ever bites (event field is the only change).

---

## 9. Step 8 — Final instrumentation API

Compact, orthogonal. Names illustrative. For each: enabled behavior / disabled behavior /
expression evaluation / event cost / metadata cost / compile-time cost.

```cpp
#include <ludus/profiling/profiling.hpp>   // ≤ log.hpp build budget (ADR 0005)

// 1. CPU scope — RAII; compile-time site id from the identifier token
LUDUS_PROFILE_SCOPE(RenderScene);
LUDUS_PROFILE_SCOPE(RenderScene, LUDUS_PROFILE_CAT(Render));   // optional category (C3)

// 2. Function scope — name derived from source_location
LUDUS_PROFILE_FUNCTION();

// 3. Frame marker — boundary + advances frame epoch (not a reset) (D7)
LUDUS_PROFILE_FRAME();

// 4. Counter / gauge — scalar; Count accumulates within frame, Gauge sets instantaneous (D8)
LUDUS_PROFILE_COUNT(DrawCalls, n);
LUDUS_PROFILE_GAUGE(PendingJobs, pending);

// 5. Instant marker — zero-duration annotation
LUDUS_PROFILE_MARK(LevelStreamedIn);

// 6. Flow — link work across threads/jobs/queues (the cross-context primitive) (C5, C1)
uint64 f = LUDUS_PROFILE_FLOW_OUT(AnimJob);   // at dispatch
LUDUS_PROFILE_FLOW_IN(f);                      // on the worker that runs it

// (later) GPU pass scope; P1 starts with the existing frame-duration probe (D14)
LUDUS_PROFILE_GPU_SCOPE(cmd, ShadowPass);
```

| API | Enabled build | Disabled build | Expression eval | Event cost | Metadata cost | Compile-time cost |
| --- | --- | --- | --- | --- | --- | --- |
| `PROFILE_SCOPE` | ctor Begin + dtor End into TLS chunk | expands to `((void)0)`; name token **not** evaluated | name is a *token* (stringized once), not evaluated at runtime | 2× `NowTicks` + ~6–8 stores + 1 predicted branch | one `constexpr` descriptor per **site** (not per hit) | 1 constexpr hash + 1 POD; no template instantiation, no heavy include |
| `PROFILE_FUNCTION` | as SCOPE, name from `source_location` | `((void)0)` | none | same as SCOPE | descriptor uses compile-time `source_location` | same |
| `PROFILE_FRAME` | emit FrameMark + bump epoch | `((void)0)` | none | 1 event | none | trivial |
| `PROFILE_COUNT` | 1 relaxed atomic add to a shard | `((void)0)`; **value arg not evaluated** | value **is** evaluated only when enabled | ~few ns | one counter handle | trivial |
| `PROFILE_GAUGE` | 1 relaxed store to a shard | `((void)0)`; value not evaluated | value evaluated only when enabled | ~few ns | one handle | trivial |
| `PROFILE_MARK` | 1 Instant event | `((void)0)` | none | 1 event | one descriptor | trivial |
| `PROFILE_FLOW_OUT/IN` | emit Flow event with id | `((void)0)`; returns 0 | none | 1 event | none | trivial |
| `PROFILE_GPU_SCOPE` | 2 query writes into cmd + 1 CPU pending entry | `((void)0)` | `cmd` evaluated only when enabled | no GPU stall; deferred readback | descriptor per pass | trivial (impl in `.cpp`) |

Six core concepts (scope, frame, counter/gauge, mark, flow, gpu) — deliberately **not**
twenty macros. Category, color, and dynamic name are optional arguments/overloads, not new
macros. **[FP]** with flow added **[R→adopt C5]**.

Design rule enforced by a gate (§12): the disabled expansion must evaluate **no argument**
(so a `PROFILE_COUNT(DrawCalls, ExpensiveRecount())` costs nothing when disabled) and emit
**zero instructions** — verified on the generated code, echoing R's/E's compile-out intent
but held to a measured standard, not a borrowed percentage **[R→reject borrowed targets]**.

---

## 10. Step 9 — Runtime event representation (data format)

Optimize for cheap writes; analysis convenience is secondary. **[FP]**, with count/flow
fields **[R→adopt]**.

```cpp
struct TraceEvent            // POD, trivially copyable; target 16–24 B (gate §12)
{
    uint64 Ticks;            // monotonic steady_clock ns (§/C7); integer, never float
    uint32 SiteId;           // compile-time FNV-1a of the site → ZoneRegistry (C3)
    uint16 Kind;             // Begin|End|Instant|FrameMark|FlowOut|FlowIn|GpuBegin|GpuEnd
    uint16 Flags;            // has-flow, has-category, incomplete(synthesized), queue bits
    // Variable / optional payloads live in a PARALLEL side-channel stream keyed by index,
    // so the common event stays small and cache-hot:
    //   FlowId (uint64)  — for FlowOut/In and job/GPU correlation (C5, D16)
    //   CategoryId       — when Flags say so (C3)
};
```

- **Event types [FP+R]:** Begin/End (paired *within a thread*, C5), Instant, FrameMark,
  Flow (cross-context link — the modern replacement for a spanning scope, C1), GPU begin/end
  (later). No "tree node" type — the tree is derived (C1).
- **Timestamps [FP+R→reject float]:** integer `steady_clock` ns; durations computed by
  subtraction; wall-clock derived off-line via the logging `SessionClockAnchor`. Never
  single-precision seconds (R's precision-loss failure).
- **Name IDs [R→adopt C3]:** events carry `SiteId` only; names live once in `ZoneRegistry`.
  **Caller-context** and **category** are *separate* axes, not folded into the site id.
- **Thread IDs [FP]:** reuse logging's `uint32 ThreadId` + `SetCurrentThreadName`; the
  buffer partition *is* the thread, so the id is metadata on the chunk, not per event.
- **Payloads [FP]:** side-channel parallel stream for optional/variable data; keeps the hot
  record fixed-size and branch-free to append.
- **String table [FP]:** collector owns the authoritative id→{name,file,line,category}
  table; serialized once per export. No strings copied at the site (contrast R's 256-char
  inline names, **[R→reject]**).
- **Alignment [FP+R]:** 8-byte aligned, power-of-two record; chunk-aligned ring so a push
  touches the producer's own cache lines only.
- **Buffer ownership [FP]:** TLS per thread; chunks pre-allocated (≥2 for double-buffering);
  handed to the collector via the DV-MPSC chunk queue.
- **Serialization [FP+R]:** off the hot path in the collector/exporter (Perfetto JSON
  first). **Memory records are a different schema in a different stream** (C4), never
  interleaved into this one.

---

## 11. Step 10 — Final feature boundary

| Capability | Build now | Later | External tool | Reject | Rationale |
| --- | --- | --- | --- | --- | --- |
| **CPU scope profiler** | ✅ | | | | Core value; only the engine names scopes. **[FP]** |
| **Hierarchical timing** | ✅ (derived view) | | | | Ground truth is the event stream; tree reconstructed off-line (C1). Producer-side tree **rejected**. |
| **Invocation counts / calls-per-frame** | ✅ | | | | Distinguishes work amount vs cost; cheap (C2). **[R→adopt]** |
| **Inclusive/exclusive (defined)** | ✅ (analysis) | | | | Prevents double-counting; defined, not ambiguous (C2). **[R→adopt]** |
| **Counters / gauges** | ✅ (Phase 2) | | | | Cheap always-on scalars; separate from timed scopes (D8). **[FP+R]** |
| **Timeline / flame-chart viewer** | | | ✅ Perfetto/Tracy | | Solved externally; multi-monitor/remote/headless friendly (§5). **[FP+R]** |
| **Tiny in-engine overlay (counters+top-N)** | ✅ (Phase 2) | | | | Glanceable dev feedback; not an interactive timeline (§5). **[FP]** |
| **Hitch capture (ring + pre/post trigger)** | ✅ (Phase 3) | | | | Retains temporal evidence historical summaries lack (D9). **[FP+R]** |
| **Perfetto/Chrome export** | ✅ (Phase 1) | | | | Universal; primary viewer path. **[FP+R]** |
| **Tracy integration (live)** | | ✅ (Phase 3) | ✅ | | Best live UX; validate cost first (§5, [?meas]). **[FP+R]** |
| **Flow events (job/async correlation)** | ✅ (primitive now) | ✅ (JobSystem dispatch flows, [threading T2](threading.md#task-identity-and-execution-traces)) | | | Cross-context truth is a DAG, not a spanning scope (C1, C5, D16). **[R→adopt]** |
| **GPU profiling (timestamp queries)** | | ⏳ P1–P3 Perfetto integration; RHI frame probe exists | RenderDoc/PIX/Nsight for capture | | Duration counters first, then pass timings and calibrated lanes; tools retain per-draw/pixel inspection (D14). **[FP+R]** |
| **PIX / vendor debug markers** | | ✅ planned independently of recording mode | ✅ | | Annotates captures with pass names; enabled/disabled overhead must be measured. **[FP]** |
| **Memory tracking (tag totals)** | ⏳ (needs allocator; always-on tier) | | | | Cheap atomics; keep in all builds (C4, §7). **[FP+R]** |
| **Memory provenance / live-vs-churn / activity** | | ⏳ (memory-profiler module) | heaptrack for broad heap | | Provenance-at-free is L's core insight; own tags/domains (C4). **[R→adopt]** |
| **Allocation call stacks** | | ⏳ (opt-in capture-only, off-line symbolized) | heaptrack | | Expensive; never hot-path symbolization (§7). **[R→adopt bounded]** |
| **Remote profiler** | | ✅ (via Tracy/exported files) | ✅ | | Headless/devkit need it; don't build bespoke transport (§5). **[FP+R]** |
| **Capture files** | ✅ (Phase 1, Perfetto JSON) | ✅ (compact binary if volume demands) | | | Files are the automation/CI artifact. **[FP]** |
| **Statistical sampling** | | | ✅ perf/VTune/Superluminal | | Sees inlined/library frames we can't; complements our scopes (§1, review §7.5). **[FP+R]** |
| **In-engine interactive timeline UI** | | | | ❌ | Requires renderer/UI under test; duplicates Perfetto; huge maintenance (§5). **[R-era assumption rejected]** |
| **Producer-side mutable call tree (shared CurrentNode)** | | | | ❌ | No concurrency model; hot-path alloc/search (C1). **[R→reject]** |
| **Binary-patch allocator interception** | | | | ❌ | We own the allocator → explicit hooks; patching is unportable/unsafe (§7). **[R→reject]** |

---

## 12. Step 11 — Final staged roadmap

Each phase delivers usable optimization capability; no phase requires "the whole profiler"
to exist first. **[FP]**, resequenced so the reference-driven semantics (counts,
inclusive/exclusive, honest stats) land *with* the first CPU trace, not after.

| Phase | Delivers | Depends on | Value | Complexity | Perf risk | Gate to pass (see §13) |
| --- | --- | --- | --- | --- | --- | --- |
| **0. Shared clock** | Promote `NowTicks()` to foundation; ticks→ns; monotonicity/ordering contract | — | Enables all | Low | Low | Clock read cost; monotonic & ordered across threads (C7) |
| **1. CPU Trace MVP + Perfetto export** | `SCOPE/FUNCTION/FRAME`, TLS chunk ring, `ZoneRegistry`, collector, **derived tree**, **inclusive/exclusive + calls/frame + unlogged remainder**, Perfetto JSON | Phase 0, MPSC queue (exists) | **Highest** — "where did the frame go," honestly | Med | Med (hot-path push) | Scope overhead; disabled = 0 codegen; multi-thread flood; tree/incl-excl correctness; **captures load in ui.perfetto.dev**; header build budget |
| **2. Counters + tiny overlay** | `COUNT/GAUGE/MARK`, per-frame snapshot, named windows, text HUD | Phase 1 | High — cheap always-on trends | Low–Med | Low | Counter contention; **every stat names window+denominator (C6)**; snapshot correctness under overlapping frame flip |
| **3. Hitch capture (+ optional Tracy)** | Rolling ring, trigger policy, auto-export; optional Tracy live backend | Phase 1 | High — catch unexpected spikes | Med | Low (off hot path) | Pre-trigger survives overload; **export doesn't induce a second hitch**; Tracy cost measured before adoption ([?meas]) |
| **4. Flow correlation** | Scheduler stamps `flow_id`; job/async DAG view | **Job system ([threading T2](threading.md#task-identity-and-execution-traces) stamps dispatch flows)** | High once jobs exist | Med | Low | Flow linkage correctness across migration; no hot-path cost added |
| **5. GPU Trace** | P1 frame-duration counters, P2 pass queries, P3 calibrated queue lanes; share G0 identities without requiring vendor capture | Existing RHI probes and P0 editor recording | High | Incremental | Measured per mode | Basic GPU timing contract: coverage, units/frame identity, no GPU wait, CPU/GPU/Both filtering and real Perfetto acceptance |
| **6. Memory profiler** | Allocator provenance + tag totals (all builds); activity/live/lifetime (dev); per-alloc capture (opt-in, off-line symbols) | **Allocator (Milestone ?)** | High for mem bugs | High | Med (capture mode) | Lossless-or-marked semantics (C4); cross-thread free accounting; capture-mode overhead bounded |

Ship **0→1** first: that alone makes the engine meaningfully easier to optimize (semantic
frame attribution + honest inclusive/exclusive/counts + a universal viewer). Phases 4–6 are
gated on subsystems that do not yet exist and are co-designed with them.

---

## 13. Step 12 — Validation gates (what to measure; decisions that depend on it)

No borrowed numbers **[R→reject E's targets]**; each gate states *what* to benchmark and
*which architectural choice it decides*.

| Gate | Measure | Decision it drives |
| --- | --- | --- |
| **G1 Disabled codegen** | Assembly of a disabled `SCOPE`/`COUNT`; confirm zero instructions and **no argument evaluation** | If not zero → fix the macro before anything ships (§9). |
| **G2 Enabled scope overhead** | ns/scope (begin+end), identical optimized workload, idle vs recording | Sets whether `steady_clock` dominates → whether to pursue TSC (C7/G7). |
| **G3 Clock** | `NowTicks` cost; monotonicity & cross-thread ordering; resolution | Gate C7: TSC backend only if G2 shows clock dominates **and** ordering/frequency/migration contract holds. |
| **G4 Throughput / saturation** | events/sec across N threads; per-thread buffer throughput; drop rate at saturation; TSan-clean | Chunk size, ring depth, collector topology (dedicated thread vs piggy-back logging worker) [?meas]. |
| **G5 Memory footprint** | Per-thread buffer + collector rolling-window bytes at default config; retained-window cost at realistic event rates (the review's 240 MB/s arithmetic is a warning, not a default) | Default window size; aggregate-vs-event retention tradeoff. |
| **G6 Capture size & load** | Perfetto JSON size for a representative capture; loads in `ui.perfetto.dev`; hierarchy/incl-excl/counts match expectation | When (if) a compact binary format is worth building (D18). |
| **G7 Counter contention** | Hot counter (e.g. DrawCalls) single-atomic vs per-thread shard | Whether counters need sharding (D8). |
| **G8 Build-time** | `profiling.hpp` self-parse under CI `-ftime-trace`; must sit under its budget line | Fails the CI gate if a heavy include sneaks in (ADR 0005, D2). |
| **G9 Code-size** | `.text`/metadata growth per 1000 sites; Release binary delta (near-zero when compiled out) | Confirms descriptor-per-site (not per-hit) and clean compile-out. |
| **G10 Observer effect (whole-system)** | Instrumentation off / idle / recording / recording+export / recording+overlay on the *same* optimized workload; tail frame times, not just a micro-loop | Whether recording/collector/overlay perturbs the very thing measured (R/E warning; review §7.4). |
| **G11 GPU query overhead** (Phase 5) | Query write cost; confirm **no** synchronous readback / stall | Query-pool sizing; keeps "no GPU stall for profiling" true. |
| **G12 Allocator tracking cost** (Phase 6) | tag-total atomics on alloc/free; per-alloc capture-mode overhead; cross-thread-free contention | Which memory tiers are always-on vs capture-only (C4, §7). |
| **G13 Incomplete-capture honesty** | Force drops/crash mid-scope; verify synthesized closures are **labeled incomplete** and memory gaps **mark analysis incomplete** (never silent) | Validates C1/C4 loss semantics. |

Open questions explicitly left to measurement **[?meas]**: TSC vs steady_clock (G2/G3);
event size vs richness with flow/category inline vs side-channel (G4/G5); collector topology
(G4); counter sharding (G7); Tracy backend cost (Phase 3); compact binary threshold (G6).

---

## Final deliverable — provenance summary

**What came from first-principles reasoning [FP], retained:** three independent subsystems;
cheap-to-include header with type-erased `.cpp`; compile-time-gated RAII scopes with
zero-code disabled path; per-thread chunked lock-free buffers + off-path collector reusing
the logging `AsyncBackend` lifetime discipline and DV-MPSC queue; frame markers (not
resets); event stream as the record; Perfetto-first external visualization with a tiny
overlay; deferring GPU (RHI) and memory (allocator); reserved `JobId`/flow seam;
fixed-size POD events with a side-channel for payloads; `steady_clock` ns now.

**What was improved by the historical references [R→adopt / FP+R]:** (C1) hierarchy is
firmly a *derived* off-line view, cross-context work uses **flow events** not spanning
scopes; (C2) **invocation counts** and **explicitly defined inclusive/exclusive** become
first-class; (C3) identity split into **site / caller-context / category**; (C4) memory
profiling severed from Trace's transport and drop policy, given **provenance-at-free**, a
**lossless/marked-incomplete** contract, and four separate metric families, owned with the
allocator; (C5) the false "RAII enforces same-thread" claim **corrected** to a contract +
flows; (C6) **statistical windows/denominators named**, decaying extrema banned, smoothed
UI separated from exact hitch evidence; (C7) **clock correctness elevated to a gate** before
any TSC work; plus the "unlogged remainder" honesty output and the whole-system observer-
effect gate.

**What the references offered but we deliberately rejected [R→reject]:** global name-array
scans and start-time parent inference (R); single-precision-seconds timestamps (R); decaying
extrema as generic min/max (R); FourCC namespace and per-use name lookup (E); RDTSC+CPU-MHz/
sleep calibration as the clock (E/H); draw-time reset of measurement state (E); one shared
`CurrentNode` and hot-path tree allocation/search (H); raw string-literal-pointer as
universal identity (H/L); hand-written binary patching/trampolines as the allocator seam and
the printed `pointer+int` header (L); embedding memory records in the CPU trace stream with
Trace's drop policy (L blend); and borrowed fixed overhead-percentage acceptance targets (E).

**What remains uncertain and requires measurement [?meas]:** TSC vs `steady_clock` (G2/G3);
event size vs richness / inline vs side-channel for flow & category (G4/G5); collector
topology — dedicated thread vs piggy-backing the logging worker (G4); counter sharding (G7);
Tracy live-backend cost before adoption (Phase 3); the compact-binary export threshold (G6);
and the true whole-system observer effect (G10). Architectural commitments in these areas
are deferred until the corresponding gate produces evidence.

This architecture is optimized for **Ludus** — its `-fno-exceptions`/no-RTTI foundation,
its logging-proven lock-free patterns, its build-time budgets, its build flavors, and its
current absence of a job system, renderer, and allocator — not for reproducing any of the
four historical systems. The references sharpened its *semantics and honesty*; they did not
change its *shape*.
```

Now let me verify the document and reconcile it against the frozen baseline to make sure my change-tracking is accurate.


<invoke name="execute_bash">
<parameter name="command">cd /projects/sandbox/Ludus && wc -l docs/architecture/profiling-final.md && echo "=== headings ===" && grep -nE "^#{1,3} " docs/architecture/profiling-final.md && echo "=== provenance tag counts ===" && for t in "FP+R" "R→adopt" "R→reject" "?meas" "\[FP\]"; do printf "%-10s " "$t"; grep -oE "$t" docs/architecture/profiling-final.md | wc -l; done && echo "=== check all 12 steps present ===" && grep -oE "Step [0-9]+" docs/architecture/profiling-final.md | sort -u