# Runtime UI architecture

Status: U0 implemented in `Ludus::Ui`. U1 is in progress: the
[render adapter](#u1-render-adapter) (`Ludus::UiRender`), its clip/hit
qualification and the [text adapter](#u1-text-adapter) (`Ludus::UiText`) with
atlas exhaustion and device-loss handling are implemented; the glyph raster
sweeps (fractional origins, display-scale raster size, baseline phase) remain.
U2-U4 below are a delivery plan, not implemented capabilities. Sandbox consumes
U0 through the installed SDK.

## Decision and current constraints

Use a small, renderer-independent, bounded document resolver with retained
interaction state and game-owned data. Submit plain value descriptions; resolve
layout once; route ordered events against that exact layout; apply returned
actions; consume paint and semantic geometry. There are no widget inheritance
hierarchies, callbacks into game code, pointers as identities, ECS dependency,
implicit global context, script VM, or platform objects in the core.

The current RHI admits one fullscreen draw with up to 16 KiB of uniforms, no sampled textures, vertex/index
buffers or scissor API. Native `Ludus::Text` owns shaping/rasterization; its web
bootstrap and GPU atlas are pending. The Editor uses Qt. Browser game controls
already use accessible DOM. Those boundaries determine the first delivery:
solid rectangles, layout, focus and pointer ownership, plus a real HUD consumer.
General UI rendering and text input require subsequent capabilities.

A AAA production UI must eventually support shaping/bidi/fallback, IME,
controller navigation, accessibility, localization, virtualized lists, animation,
authoring and inspection. Calling U0 a complete AAA toolkit would be misleading.
Its value is a small tested foundation and a migration seam that does not require
those systems to be invented simultaneously.

## Alternatives considered

| Approach | Strength | Decision for Ludus now |
| --- | --- | --- |
| Qt for editor and DOM for browser forms | Existing native controls, input methods and accessibility | Keep these adapters for their current surfaces. Do not port the Editor to a game HUD toolkit. |
| Dear ImGui | Established debug UI API, game-owned data | Suitable as a future optional developer overlay. Introducing it now still requires rendering capabilities absent from the current RHI; it does not remove the player UI requirements. |
| RmlUi | Established document/style and renderer boundary | Re-evaluate for complex authored screens once the renderer/text adapter exists. Avoid duplicating a complete rich document engine in U0. |
| Fully custom retained widget/object hierarchy | Flexible arbitrary behavior | Reject initially: ownership, callbacks, state synchronization and invalidation increase the maintenance surface. |
| Bounded value documents plus retained focus/capture | Small explicit contracts; reusable across current render/platform constraints | Adopt for U0 HUDs and simple controls, with deliberate feature limits and a measured expansion path. |

## Baseline before the Gems review

1. Application models remain the source of truth. Documents are immutable value
   snapshots, and actions carry stable IDs back to the application.
2. Keep a persistent surface context for focus and capture, rather than retaining
   the application's data or rebuilding interaction state each frame.
3. Separate layout, events, painting, text services and platform bridges. Qt
   continues to serve desktop authoring; DOM continues to serve browser input.
4. Use logical pixels, deterministic flow/anchors and bounded storage. Avoid
   general CSS, global constraint solvers and a new dependency tree initially.
5. Make geometry and commands inspectable and GPU-free to test. Optimize based
   on captures and counters; do not promise performance without measurements.

This API/implementation separation follows the [Dear ImGui author's discussion](https://github.com/ocornut/imgui/wiki/About-the-IMGUI-paradigm):
immediate submission can coexist with persistent internal state. The
[RmlUi render interface](https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/interfaces/render.html)
also demonstrates a useful boundary between UI output and renderer resources.
These support the separation, not an assertion that U0 matches either toolkit's
feature coverage or speed.

## Gems review and resulting changes

The index is a discovery aid, not evidence of an article's contents. The
following chapters were read from the repository PDFs (Gems 3 is scanned;
its pp. 109-116 were rendered and OCR-transcribed).

| Article from `references/game-dev-gems-toc.md` | Useful finding | Change adopted | Limits |
| --- | --- | --- | --- |
| Gero Gerber, *A Flexible User Interface Layout System for Divergent Environments*, Game Programming Gems 8, 4.10, pp. 442-452 ([index](../../references/game-dev-gems-toc.md#L7410)) | Layout variants, safe regions and shared defaults avoid copied screens. Runtime-changing conditions cannot safely use initialization-only caches. | Viewport is an explicit safe-area rectangle; uniform DPI conversion belongs to the adapter. Fractional lengths and anchors coexist with logical fixed sizes. Future theme variants have explicit viewport/locale/input revisions. | Do not copy its XML condition-class hierarchy, arbitrary expression language, platform names, resolution exclusions or stretched virtual screen. U0 has no theme/variant parser. |
| Greg Seegert, *Real-Time Input and UI in 3D Games*, Game Programming Gems 3, 1.13, pp. 109-116 ([index](../../references/game-dev-gems-toc.md#L6886)) | Buffered events preserve brief transitions; authoring data and localization need separation; character input is different from raw gameplay key input. | Ordered event routing with immediate action results, explicit capture/consumption, stable action IDs independent of labels. Future text composition uses platform text events and Text shaping, never key-to-character synthesis. | DirectX8, Windows character APIs and wide strings are historical examples, not portable APIs to adopt. Network prediction is outside this UI change. |
| Hyunwoo Ki, *Optimizing a 3D UI Engine for Mobile Devices*, GPU Pro 1, VI.4, pp. 397-411 ([index](../../references/game-dev-gems-toc.md#L9051)) | Clip invisible work, reduce state changes, batch text, and selectively cache costly composites. | U0 emits only visible clipped rectangles in paint order. Future renderer batches adjacent compatible commands, retains atlas resources, and measures overdraw/cache costs. | Do not reorder translucent commands globally for texture batching. Do not adopt its device timings, 16-bit texture defaults, nearest filtering, manual resource GC or whole-panel caching as universal optimizations. |

The resulting architecture adds safe-area/scale revision ownership, a common
clip contract for rendering and hit testing, release consumption after a widget
is removed, and explicit text/IME milestones. These are engineering adaptations
of the articles, not their original implementations.

## Module boundaries

```
Game model / view descriptions / stable IDs
                  |
                  v
Ludus::Ui -> FoundationBase (public)
         -> FoundationContainers + FoundationMath (private)
                  |
       layout snapshot + paint commands + action result
          /                         \
Platform/DOM semantic adapter      Ludus::UiRender -> Renderer overlays -> RHI
          |                         |
 text composition events      Ludus::UiText -> GraphicsText -> Ludus::Text (native)
```

Ui does not link Platform, Input, Text, RHI, Qt or any third-party UI framework.
Input adapters translate platform records to ordered `Event` values. They route
UI before gameplay and honor `Consumed`; pointer IDs must remain stable for a
gesture. Multiple surfaces use different contexts. UI resolution and event
routing are main-thread operations. Rendering may copy committed commands into
its own frame storage; it cannot retain borrowed spans across document commits.

The game owns domain state and localized text/semantic names. Ui owns geometry,
paint order, focus and a single captured pointer. The first slice exposes button
roles/IDs/geometry, not a full accessibility tree. A DOM adapter should use real
buttons and native keyboard/assistive semantics rather than reimplement them.
A future native accessibility bridge must include names, values, states and
notifications, not infer semantics from color. [W3C modal guidance](https://www.w3.org/WAI/ARIA/apg/patterns/dialog-modal/)
supports scoped focus and restoration for U2; U0 does not implement modals.

## U0 document and lifecycle contract

`TryInitialize(capacity)` allocates two bounded buffers and an ID table once.
Capacity must be 1..65536; allocation failure returns `OutOfMemory`. It cannot
be resized in play. Destroy and recreate a context explicitly to change capacity.
No allocation occurs in document resolution, painting, focus or event routing.

`TrySetDocument(elements, viewport)` copies plain values into staging, checks
IDs, parents, finite geometry, enum ranges and RGBA channels, and resolves each
element once. Successful resolution publishes the complete staging buffer;
failure leaves the previous document, paint and interaction state intact. An
empty document is valid and clears obsolete focus. Input capture ownership stays
until release/cancel even when its target disappears, so a UI gesture cannot
release into gameplay. Identity is a nonzero application-owned 64-bit key;
reusing an ID for a different logical control during an active gesture is a
caller error. Labels, array addresses and submission indices are unsuitable IDs.

Parents are earlier indices in the submitted span. Submission order is paint
order and focus order; the caller emits the desired order explicitly. This
precludes cycles without traversal stacks, recursion or virtual dispatch. There
is no z-index sorter. Children inherit authored visibility and enabled state.
A hidden child consumes no flow space. Disabled controls paint but never focus
or activate. Disabled buttons still consume pointer gestures over their surface.
Decorations are pointer-transparent unless `BlockPointer` is set.

Layout uses logical pixels, top-left origin and +Y down. Fraction lengths use
the parent's content extent, after padding. Row/column flow overrides the child's
alignment on the flow axis; cross-axis alignment and explicit offsets still
apply. Flow does not auto-shrink or wrap. Overflow is clipped by default; explicit
`ClipChildren=false` permits overflow inside the ancestor clip. Viewport clipping
always applies. Hit testing uses half-open rectangles and the same clip as paint,
in reverse paint order. Zero-area boxes cannot focus. Extents/padding/gaps must
be finite and nonnegative; offsets may be negative. Malformed and overflowing
geometry are rejected, not silently converted into huge GPU coordinates.

The adapter converts framebuffer positions to logical coordinates with one
uniform scale and removes platform safe-area insets before resolution. It must
re-resolve before routing events following a viewport/DPI change. U0 does not
read DPI, rotate, infer device breakpoints or store stale viewport caches.

A button captures on down and activates on release inside its current clipped
rectangle; release outside cancels activation. Unrelated pointer releases cannot
activate it. Cancel/focus loss terminates capture. `FocusNext/Previous` wrap in
submission order, skipping hidden/disabled/fully clipped elements; `Activate`
returns the focused button's ID. Platform repeat policy is the adapter's concern.
Applications apply returned actions after `Route`, preventing reentrant mutation.
U0 omits pointer movement/hover, multipointer gestures and keyboard press/release
capture. Those extensions must preserve gesture ownership, including cancellation.

Paint output is a sequence of clipped solid rectangles with straight alpha and
linear RGB. The renderer handles alpha composition and attachment encoding.
There are no resource handles or borrowed strings in the U0 document. Theme
selection is ordinary application data. A future renderer adds explicit typed
image/glyph resources and scissor commands without changing identity or input.

## Maintainability, debugging and optimization

A single implementation file owns validation, resolution and input state. The
public header uses Base and span only. Inspect `GetLayout()` to find the exact
ID, bounds, clip, enabled/visible state; inspect `GetPaintCommands()` to reproduce
paint order; inspect `InputResult` for ownership and action. `GetStats()` exposes
submitted elements, painted elements and layout visits. Add a debug view as a
consumer of this data, not a second layout/input system. Stable IDs connect a
future authored source location to screenshots and event recordings.

Normal resolution is O(capacity + elements), with open-addressed ID checking;
adversarial collisions can make validation O(elements squared). Capacity is
bounded and documents are trusted application values. Input hit testing/focus
lookup is linear in elements, which is appropriate for HUDs. A spatial index,
virtualized list or cached subtree requires an actual measured large-screen
consumer. U0 intentionally resolves full documents; there is no dirty graph or
hash-based change detection whose invalidation must be debugged.

GPU layout state never feeds back into layout. Preserve translucent order;
merge only adjacent compatible paint commands. Cache shaped runs by font face
revision, locale, bidi direction, text and wrap width. Cache layout only with
explicit dependency revisions. Prefer virtualization to clipping thousands of
live list items. Render-target caching requires measured benefit after accounting
for memory, invalidations and overdraw. Do not sample these counters inside a
text-shaping callback or use synchronous GPU queries on the UI path.

The Sandbox's six-command fullscreen uniform adapter is deliberately bounded.
It proves U0 on the current renderer, but per-fragment rectangle loops scale with
screen area and are not the eventual general UI renderer. U1's
[render adapter](#u1-render-adapter) replaces it with scissored overlay quads.

## U1 render adapter

`Ludus::UiRender` (`ludus/graphics/ui_render/ui_render.hpp`) converts committed
`PaintCommand` values into the renderer's existing `OverlayList`. The list and
`OverlayRenderer` already own the indexed quad upload, R8 coverage sampling,
scissor and bounded versioning (see
[renderer overlays](renderer-systems.md#implemented-l2-mutable-versions-and-overlays)),
so UiRender adds no pipeline, shader or GPU resource of its own and Ui still
links no renderer code.

A `SurfaceMapping` names one uniform scale, the physical position of the logical
origin (safe-area insets) and the framebuffer extent. Physical point *p* maps to
logical (*p* - origin) / scale, computed in float64 and rounded once to float32.
`MapPhysicalToLogical` is that mapping; adapters use it for every pointer
`Event`. A physical pixel is painted for a command exactly when its centre maps
inside the command's half-open `Bounds`, with the right and bottom edges summed
in float32 as Ui hit testing sums them. `TryComputePhysicalRectangle` corrects
its float64 estimate against the float32 mapping, so a pixel-centre tie (logical
34.25 at scale 2 lands on physical 68.5) resolves the same way for paint and
routing. Painted coverage and the activatable region therefore agree pixel for
pixel at integer and fractional scales; GPU tie-breaking rules never decide an
edge because every quad edge lies on a pixel boundary.

Each command carries its effective clip (`PaintCommand::Clip`, the same clip hit
testing uses). Its quad is the snapped clipped bounds, and its scissor is the
snapped clip. Bounds never exceed the clip in real arithmetic; if float32
rounding of the clipped right/bottom edge would leave a painted pixel outside the
snapped clip, the scissor grows to include it rather than diverging from routing.
Commands that cover no pixel centre are culled. Adjacent commands with the same
clip merge into one draw; translucent order is never changed. UiText labels
share this scissor contract; image commands in later milestones will too.

`AppendPaint` validates the mapping and every command, then checks the list's
remaining vertices (six per quad) and runs, before appending anything. Every
failure leaves the list and report unchanged and does not count as a dropped
overlay command. `PaintReport` records commands read, quads, culled commands and
new runs; these are the batching counters U1 acceptance measures. The overlay
list's bounds (512 quads, 256 runs per prepared version) are the current limit;
a document needing more must split across prepared versions or wait for a
measured larger list.

Qualification lives in two places. The CPU tests in
`modules/graphics/ui_render/tests` check the pixel-centre rule over 1260 edge,
scale and origin combinations, and route a press at every pixel of the
clip/hit document at scales 1, 1.5 and 2 and after a resize. The installed-SDK
fixture `tests/sdk_consumer/ui_render.cpp` renders that document offscreen
through `OverlayRenderer`, reads the RGBA8 target back, and requires every
pixel's colour and routed press to match the snapped paint list, plus explicit
probes inside the clip, on its half-open edge, in the overflow and in the
sibling that follows the scissor change. Native CI runs it on the Linux
(Vulkan) and macOS (Metal) SDK jobs; the browser suite runs it on WebGPU and
WebGL 2 and requires byte-identical readbacks across both. Results and pending
evidence are in [UI validation](ui-validation.md#u1-render-adapter).

## U1 text adapter

`Ludus::UiText` (`ludus/graphics/ui_text/ui_text.hpp`) draws shaped text on Ui
elements as GraphicsText coverage quads in the runs UiRender emits. Ui still
links neither Text nor the renderer: a `Label` names its element by
`WidgetId`, the identity Ui already requires, and carries glyph quads the caller
prepared. Labels are adapter input beside the document, not document fields, so
`Element` stays a plain value and Ui never sees strings, fonts or atlas UVs.

`AppendUi` walks `GetLayout()` in submission order. For each element it appends
the element's fill exactly as `AppendPaint` would (the shared
`TryComputePaintQuad`), then every label naming the element. A later sibling's
fill therefore covers an earlier element's text, and a transparent element
(which emits no paint command) can still carry a label. A label is positioned
from the element's unclipped layout box: its pen origin and baseline are logical
offsets, mapped with the surface scale and origin and not snapped. Its scissor is
the element's effective clip, the clip its fill and hit region use; when the
element has a fill, the label reuses the fill's overlay style, so the fill and its
text share one draw. Text outside the element's own bounds is drawn where that
clip allows, so size the element to contain its ink or nest it in a clipping
element. Labels of hidden or absent elements append nothing. Each label records
the scale its glyphs were rasterized for, and a mismatch with the mapping is
rejected rather than drawn at the wrong raster size.

The renderer's `CanMergeOverlayStyles` is the one merge rule; both adapters
count runs with it. `AppendUi` validates every label (including hidden ones),
command and glyph, and the list's remaining vertex and run capacity, before
appending anything, so failures change nothing. `UiTextReport` adds glyph,
culled-glyph and text-run counts to the fill counters, plus the pixel centres the
appended quads shade inside their scissors, which divided by the framebuffer area
is the overdraw factor. Glyphs wholly outside their scissor are culled rather than
uploaded. The browser build has all of this; only shaping is native.

### Label cache, atlas exhaustion and device loss

`LabelCache` (`ludus/graphics/ui_text/label_cache.hpp`, native only) holds at
most 32 labels of up to 64 glyphs, one per element, and the GraphicsText atlas
they sample. `SetLabel` copies a shaped run through `PrepareText`; the layout is
borrowed only during the call. The atlas key includes the physical pixel height,
so a run shaped for another display scale uses new cells and stale cells cannot
serve it; returning to an earlier scale reuses its cells without rasterizing.

When the live atlas is full, `SetLabel` compacts automatically: it copies the
coverage of the glyphs live labels use into a second CPU atlas (no rasterizing),
adds the new run there and swaps atlases. Compaction loses no authored data, so
it needs no confirmation (see "Errors offer solutions" in `AGENTS.md`). If the
run still does not fit, `SetLabel` returns `CapacityExceeded`, keeps every
label, including that element's last ready label, and the cells they address,
and reports a `LabelRepair` with the counts: `RemoveLabel` (all slots used),
`ShortenLabel` (more than 64 glyphs) or `ReduceGlyphs` (remove labels, use fewer
pixel sizes or shorten text). An application shows that repair with the counts
where the label was edited.

`GetAtlasRevision()` advances whenever labels may address cells a previously
published texture lacks; publish a new atlas texture then and keep earlier
textures until the overlay versions that sample them are released. The cache owns
no GPU objects. After device loss, reinitialize `OverlayRenderer`, publish
`GetAtlas()` to the new device and rebuild the list from `GetLabels()`; nothing is
shaped or rasterized again, and the application keeps presenting its last
drawable frame until the rebuilt version is ready.

The CPU tests in `modules/graphics/ui_text/tests` check paint-order interleaving,
run merging, parity with `AppendPaint` when no labels are attached, clip culling,
hidden labels, scale/origin mapping and atomic failures, plus Latin, Hangul and
right-to-left Arabic shaping fixtures, display-scale keys, compaction and the
failures that keep the last ready label. The installed-SDK fixture
`tests/sdk_consumer/ui_text.cpp` renders labels on a clipped document offscreen and
checks every pixel against a composite of the snapped fills and atlas coverage;
natively it also rejects an atlas-exhausting label and rebuilds every GPU object
after destroying the device, and both must reproduce the last frame byte for byte.
Results are in [UI validation](ui-validation.md#u1-text-adapter).

## Delivery sequence and acceptance

| Milestone | Deliverable | Acceptance |
| --- | --- | --- |
| U0 (this change) | CPU document layout, clipped rect paint, button routing/focus, SDK export, shared Sandbox HUD | Debug/Development and ASan/UBSan tests; format/tidy; installed native/web consumer; transactional failure/capture regressions; actual browser pixels |
| U1 (UiRender and UiText implemented) | RHI quad/index/texture/scissor capabilities; UiRender and UiText adapters, atlas uploads | Vulkan/WebGPU/WebGL 2 pixel parity; shaping fixtures, atlas exhaustion/device loss; no GPU work in Ui; batching/overdraw measurements |
| U2 | Semantic names/values, native accessibility, text edit/IME, modal scopes, controller direction navigation, scrolling | Screen reader/keyboard/controller journeys; composition/bidi/grapheme tests; focus restore and cancellation under removal/reload |
| U3 | Versioned authoring documents, shared themes/variants, localization, atomic hot reload, inspector | Bounds/ID/source diagnostics; rejected reload preserves live UI; portrait/ultrawide/DPI/safe-area/text-expansion matrix; no user-code callbacks in validation |
| U4 | Virtualized lists, explicit dirty revisions, selective cache/animation | Representative large-menu captures; frame cost p50/p95 and memory before/after; invalidation regressions; reduced-motion support |

Do not claim a universal performance target before defining representative
screens and hardware. Record CPU resolution/input/paint time, allocation count,
capacity/high-water, quads/batches, atlas misses/uploads and overdraw separately.
The initial correctness suite exercises failure rollback, flow/clip consistency,
reorder identity, disabled/hidden ancestors, pointer ownership and focus loss.
Validation results and unfulfilled gates belong in `ui-validation.md`.
