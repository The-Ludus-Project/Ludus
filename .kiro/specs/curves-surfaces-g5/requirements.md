# Curves and surfaces G5 — requirements

Phase G5 of the curves and surfaces architecture: the first phase that touches
Content and the editor. It adds the shared cooked geometry format, a
GeometryContent codec/cooker/loader over the existing `Ludus::FoundationGeometry`
owners, and editor-private authoring adapters built on the existing document,
undo, preview and publication owners.

Authorities, read before changing any of this:

- Architecture: [docs/architecture/curves-surfaces.md](../../../docs/architecture/curves-surfaces.md),
  especially "Editing, cache invalidation and publication", "Content and failure
  contracts", "API shape and ordinary debugging" and the G5 gate row.
- Frozen contracts: [ADR 0024](../../../docs/decisions/0024-curves-surfaces-contracts.md)
  (GeometryStatus, guarantee classes, tolerance fields, domains, the format
  boundaries, and the targets `Ludus::GeometryFormat` and `Ludus::GeometryContent`).
- Content resources and limit ownership:
  [docs/architecture/content-resources.md](../../../docs/architecture/content-resources.md).
- Editor document/undo owner: [ADR 0023](../../../docs/decisions/0023-editor-presentation-and-document-core.md).
- Existing precedents to mirror, not re-invent: `modules/content/scene_format`,
  `modules/content/render_graph_format`, `modules/content/scene_cook`,
  `modules/content` (Catalog/JSON/Bytes/Digest), `apps/editor` document history,
  preview and async job owners.

The repository standards in `AGENTS.md` and `.kiro/steering/coding-standards.md`
apply throughout: no C++ exceptions; Ludus fixed-width primitive aliases; Ludus
containers/views; the naming rules; no magic numbers; explicit types; `noexcept`;
`[[nodiscard]]` on fallible operations; Doxygen on all new public API.

## R1 — `Ludus::GeometryFormat` cooked record format

The shared cooked geometry format, at `modules/content/geometry_format`.

1.1. A header-only `INTERFACE` CMake target `Ludus::GeometryFormat` that depends
only on `Ludus::FoundationBase` (and, where a fixed-width unsigned codec concept
is needed, the Base `byte_order.hpp` / `checked_integer.hpp` it already surfaces).
It installs its public headers through a `FILE_SET`, mirroring
`modules/content/scene_format`.

1.2. It holds the versioned cooked record layouts, limits and constants, with
`static_assert`ed `sizeof`/`offsetof` for every record and `*_BYTES` width
constants derived from the explicit field schema, never from host padding.

1.3. Fields are fixed-width and little-endian, serialized field by field:
`uint32` indices and counts, `float32` controls, `float64` origins, domains and
tolerances. Scoped enums that are serialized use an explicit 32-bit underlying
type. The format NEVER encodes `usize`, pointers, native class layouts, SIMD
registers or FoundationMath values.

1.4. The blob layout has: a header carrying the magic, format schema version and
cooker version; source and dependency digests; precision and profile metadata;
and checked offset/count sections for each kind of record (path spans, joints,
surface patches, adjacency, tracks/keys, source identity table). The format
documents the layout in a `@file` comment, the "serialize field by field, never
copy host struct bytes" contract, and its per-version append history.

1.5. The format is owned by neither tools nor runtime: the cooker and the loader
both include it; a cooker never links the renderer and a runtime loader never
links a cooker.

## R2 — `Ludus::GeometryContent` codec, cooking, loading, publication

At `modules/geometry/content`, target `Ludus::GeometryContent`, depending on
`Ludus::Content`, `Ludus::FoundationGeometry` and `Ludus::GeometryFormat` (per
the ADR 0024 boundary table).

2.1. **Source codecs.** A JSON source schema read through the existing checked
Content JSON facilities (`content::JsonDocument`/`JsonValue`). A source record
carries units, coordinate convention, source mode (line/cubic spans, Hermite
waypoints, centripetal Catmull-Rom waypoints, scalar track keys, bicubic
patches + adjacency), points/handles, domains, topology, closed/continuity
declarations, stable author IDs and a preparation profile. The source round-trips
through a deterministic writer.

2.2. **Validation.** Unknown fields or versions, duplicate IDs and invalid
constraints return structured diagnostics (a `content::Status` + a geometry
diagnostic carrying code, source element, property path, requested/achieved
tolerance and offending range). They are NEVER assertions.

2.3. **Cooking.** Validate and convert a source into a cooked blob. Compute the
source digest and dependency digests; detect a source changed during conversion
(as scene_cook does). Produce the blob field by field, in little-endian order,
into owned bytes. The cook is fallible and cooperatively cancellable through the
existing `Cancellation` token pattern; cancellation yields `Cancelled` and leaves
no partial output.

2.4. **Loading / readers.** Before exposing any view, a reader validates: the
magic and both versions; count products and total byte accounting; section
offsets, alignment and non-overlap; enum values; monotone (strictly increasing)
track-key domains; finiteness of every float control/origin/tolerance; and
cache consistency (the header's declared digests and profile). Any failure
returns a structured diagnostic and leaves outputs unchanged; it is never an
assertion. After validation the loader builds the `PathSource` / `TrackKey` /
`SurfaceSource` inputs and calls the FoundationGeometry `TryPrepare*` owners,
returning the immutable prepared owner.

2.5. **Catalog registration through a versioned extension.** Register a
`CookedGeometry` resource kind with the existing `content::Catalog`. The catalog
schema version is extended (version 2) so that a geometry kind is only admitted
by the new version; version 1 catalogs still parse and reject the new kind
string. Add the `"cooked-geometry"` kind string to the `Read`/`Put`/`Write`
mappings.

2.6. **Cache keys.** The cache key the architecture lists includes the source
digest, converter/cooker version, geometry schema version, precision profile,
all tolerances, frame seed / seam policy, topology and dependency digests. (The
cooked blob header carries exactly the fields that make up this key so a loader
can confirm cache consistency.)

2.7. **Limits have one owner per policy** (`content-resources.md#limit-ownership`):
the GeometryFormat constants are the admission limits for encoded inputs and
decoded payloads; reuse them at read, validation, cook and publication. Keep
encoded-input, decoded-payload and peak-memory budgets distinct.

2.8. All engine APIs are `noexcept`; fallible operations are `[[nodiscard]]`.
Owned builders return a complete candidate only on success; failure leaves the
active owner unchanged.

## R3 — Editor-private authoring adapters

Under `apps/editor`, private (headers in `src/internal`), built on the existing
owners (ADR 0023 document core, `DocumentHistory<T>`, the preview/publication and
async-job owners). No parallel document, undo or catalog system.

3.1. **Semantic undoable edit commands** on the existing document/undo owner,
with stable author IDs (minted through the existing identity path) and handle
modes (automatic handles either preserve their mode or visibly convert to manual).
Each edit is one validated transaction; invalid edits may remain visible in the
source document.

3.2. **Debounced live preview** that coalesces edits on the document owner, as
`RenderGraphLiveBuild` does.

3.3. **Async prepare requests** that carry document/session identity, source
revision, generation and preparation profile. Superseded work is cancelled; late
completions are rejected by comparing the carried identity. A prepare has named
`Pending`/`Ready`/`Failed` outcomes and immutable inputs with an exclusive output
through completion (mirroring `PreviewGpuGeneration`).

3.4. **Transactional candidate publication.** The last valid preview stays active,
with a stale indicator, while an edit is invalid or a prepare is pending or failed.
A failed replacement never erases the active view. Publication happens at a
declared frame/tick boundary.

3.5. **Explicit cursor reload policy.** On publication, cursors reset and
traversal placement recomputes under a consumer-chosen policy: retain parameter,
retain absolute distance, retain normalized distance, or project world position.
The policy is never chosen implicitly.

3.6. **Failures offer a repair** (AGENTS.md "Errors offer solutions"): every
failure path reports a structured diagnostic and a concrete suggested repair
(e.g. "merge coincident waypoints", "split the chunk", "raise MaxLeaves",
"relax the tolerance to its Certified floor").

## R4 — Tests (every G5 gate)

The G5 gate: corrupt and overflowing blobs (fuzz the sections and count
arithmetic), source round-trip, failed and cancelled prepare, undo/redo, late
completion, stale cursor, active+candidate peak memory, and injected allocation
failure.

4.1. **Format/codec tests** (`Ludus::GeometryFormat` and `Ludus::GeometryContent`):
- Blob round-trip: cook a source, load it, confirm the prepared owner matches,
  re-cook and confirm byte-for-byte determinism.
- Source round-trip: parse JSON source, write it back, re-parse, confirm equal.
- Corrupt/overflowing blobs: truncated headers; bad magic/version; count values
  whose products overflow or exceed the limits; overlapping or out-of-range
  offsets; misaligned sections; nonmonotone track domains; nonfinite controls;
  unknown enum values; mismatched digests. Each returns a structured diagnostic,
  never an assertion, and leaves outputs unchanged.
- Unknown JSON fields/versions and duplicate IDs return diagnostics.
- Injected allocation failure through the existing allocation seam: each fallible
  allocation in cook and load returns `OutOfMemory` with the active owner
  preserved; evaluation allocates nothing.
- Reuse the G0 corpus (`ludus_geometry_reference_fixtures`) where it applies
  (cubic spans, patches, waypoints, tracks, far-origin precision). Never change
  an existing corpus reference without amending ADR 0024.

4.2. **Editor adapter tests** (headless, offscreen, mirroring
`document_history_tests.cpp`):
- undo/redo of semantic edits, including coalesced live edits and checked heads;
- failed prepare keeps the active view with a stale indicator;
- cancelled prepare yields `Cancelled` and leaves the active view;
- late completion (a stale generation/revision) is rejected and does not publish;
- stale cursor: after publication the cursor reloads under each reload policy and
  a mismatched cursor revision is `StaleRevision`;
- active+candidate peak memory: the adapter holds at most the active plus one
  candidate, checked against the declared peak-memory budget.

## R5 — Documentation

5.1. Doxygen on all new public API (`GeometryFormat` and `GeometryContent` public
headers), stating purpose, parameters/units, return/status meanings,
failure/output preservation, ownership/lifetime and thread affinity. No `@throws`.

5.2. Update the curves-surfaces architecture page (mark G5 implemented in the
status line, the boundary table and the gate row) and add a G5 section to
`docs/architecture/math-evidence/curves-surfaces.md` with the gate evidence and a
validation record.

5.3. A canonical usage page under `docs/` only if users need setup or workflow
instructions (a geometry content authoring/cook/load page, registered in the
index and wiki).

5.4. Run `python scripts/check-docs --fix-index`; a strict MkDocs build;
`scripts/build-api --bootstrap`; and `scripts/check-wiki` — recording which ran
and which were blocked by the toolchain.

5.5. Reference comments wherever a source informed the design (the architecture
page and the precedent modules).

## Non-functional constraints and environment

- C++23, no exceptions, Ludus primitive aliases, Ludus containers/views, the
  include boundary (Geometry stays out of `core.h`; no public Geometry/Content
  header pulls a heavy STL header). `#pragma once`.
- The pinned toolchain is Clang/LLVM 23 (ADR 0025) with CMake 3.29.6; the batch
  CI runs the warning-clean build, clang-format-23, clang-tidy-23, ASan/UBSan,
  strict MkDocs/Doxygen and the SDK consumer.
- Work in an isolated git worktree on a `codex/` branch cut from a freshly
  fetched `origin/develop`; never commit to `develop`/`main`; open the PR against
  `develop`; never add `ci:full`; never merge or enqueue.
