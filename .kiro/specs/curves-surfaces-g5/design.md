# Curves and surfaces G5 — design

This design turns the G5 requirements into concrete modules, headers, record
layouts, codec flow and editor adapters, reusing the existing Content, Geometry
and editor owners. It follows ADR 0024's frozen vocabulary and the precedents in
`modules/content/scene_format`, `modules/content/render_graph_format`,
`modules/content/scene_cook`, `modules/content`, and `apps/editor`.

## Overview and module layout

Three deliverable units, matching the ADR 0024 boundary table:

| Unit | Location | Target | Depends on |
| --- | --- | --- | --- |
| Cooked format | `modules/content/geometry_format` | `Ludus::GeometryFormat` (header-only INTERFACE) | FoundationBase only |
| Content codec | `modules/geometry/content` | `Ludus::GeometryContent` (static library) | Content, FoundationGeometry, GeometryFormat |
| Editor adapters | `apps/editor/src[/internal]` | `ludus_editor_*` object library + tests | GeometryContent, FoundationGeometry, DocumentHistory |

Data flow (mirrors the architecture's ownership diagram):

```
JSON source  --ParseGeometrySource-->  GeometrySource (validated, owned)
GeometrySource --CookGeometry-->        cooked blob bytes (GeometryFormat layout)
cooked blob  --LoadGeometry-->          GeometryLoad { PreparedPath|PreparedTrack|PreparedSurface }
PreparedX    --borrow view-->           allocation-free sampling (G2-G4)

editor: CurveDocument / SurfaceDocument (DocumentHistory<T>)
     -> debounce -> GeometryPrepareRequest (identity) -> GeometryPrepareJob {Pending,Ready,Failed}
     -> GeometryPreview (transactional candidate publication, stale indicator, reload policy)
```

A cooker never links the renderer; a runtime loader never links a cooker; both
include `Ludus::GeometryFormat`.

---

## R1 — `Ludus::GeometryFormat`

Header-only INTERFACE target at `modules/content/geometry_format`, modelled on
`modules/content/scene_format`. Namespace `ludus::content::geometry_format`.
Depends only on `Ludus::FoundationBase` (uses `ludus/foundation/base/types.h`,
and in the codec consumers `byte_order.hpp`).

### Public headers

- `limits.h` — admission limits and capacity constants, one owner per policy.
- `geometry_format.h` — magic, version ladder, enum tags, record structs, the
  blob-header layout, `static_assert`ed sizes and width constants, and the
  `@file` wire-layout contract.

### Magic, versions and metadata

```
inline constexpr std::string_view MAGIC{"LUGEO01\0", 8};   // 8 bytes incl. NUL
inline constexpr uint32 FORMAT_VERSION_1 = 1;              // first cooked layout
inline constexpr uint32 FORMAT_VERSION = FORMAT_VERSION_1; // current
inline constexpr usize DIGEST_BYTES = 32;                  // SHA-256 (content::Digest)
```

The cooker version is a separate `uint32 COOKER_VERSION` (bumped when the cook
output changes for the same source). Both go in the header; `content-resources.md`
cache keys use schema version, cooker version and digests.

### Serialized enum tags (explicit 32-bit)

All serialized enums use `: uint32` (with the `performance-enum-size` NOLINT the
render-graph format uses):

- `GeometryKindTag { Path = 0, Track = 1, Surface = 2 }` — which record sections
  are present. One cooked blob holds exactly one kind.
- `SpanKindTag { Line = 0, Cubic = 1 }` — maps to `geometry::SpanKind`.
- `TrackInterpolationTag { Step = 0, Linear = 1, Hermite = 2 }` — maps to
  `geometry::TrackInterpolation`.
- `PatchSideTag { U0, U1, V0, V1 }`, `PrecisionProfileTag { Strict = 0, Visual = 1 }`.
- `BoundaryPolicyTag`, `SpanSideTag` for the declared evaluation policy.

Each tag has a free `TryDecode*`/`Encode*` that maps to/from the frozen
`geometry::` enum and rejects unknown values with `false` (readers turn that into
a diagnostic, never an assertion).

### Blob layout (`@file` documented, version 1)

Little-endian, field by field, no host struct bytes copied. Order:

1. **Header** — fixed-width fields, `HEADER_BYTES` = field schema width +
   `3 * DIGEST_BYTES`:
   - `Magic[8]`
   - `uint32 FormatVersion`, `uint32 CookerVersion`
   - `uint32 KindTag` (GeometryKindTag)
   - `uint32 PrecisionTag`
   - `uint32 SpanCount`, `uint32 JointCount` (path)
   - `uint32 KeyCount` (track)
   - `uint32 PatchCount`, `uint32 AdjacencyCount` (surface)
   - `uint32 SourceIdentityCount`
   - `uint32 Flags` (bit 0 IsClosed)
   - `float64 Origin[3]` (chunk origin)
   - `float64 AssetScale`
   - `uint64 Revision`
   - `ToleranceProfile` fields (12 × float64) + the resolved
     `PositionFloor`/`EffectivePositionError` (float64)
   - `uint32 SectionCount`, then `SectionCount` × `SectionEntry { uint32 Id;
     uint32 Offset; uint32 Count; uint32 ElementBytes }` — the checked
     offset/count directory. `Offset` is from the blob start; sections do not
     overlap and lie within the blob.
   - `Digest SourceDigest`, `Digest DependencyDigest`, `Digest PayloadDigest`
     (each `DIGEST_BYTES`).
2. **Payload sections**, each at its directory `Offset`, `Count` × `ElementBytes`:
   - Path: `PathSpanRecord` (SpanKindTag + 4 × float32[3] controls + float64
     DomainWidth + uint32 SourceElement), then `JointRecord` (4 × float64 +
     4 × uint8 continuity flags + 3 bytes pad to 4).
   - Track: `TrackKeyRecord` (float64 Time + 3 × float32 + uint32
     InterpolationTag).
   - Surface: `PatchRecord` (uint64 Id + 16 × float32[3] controls), then
     `AdjacencyRecord` (uint32 FirstPatch, uint32 FirstSideTag, uint32
     SecondPatch, uint32 SecondSideTag, uint32 IsReversed).
   - `SourceIdentityRecord` (uint64 AuthorId + uint32 SourceElement) — maps a
     prepared element back to a stable author ID for inspection/diagnostics.

Every record struct carries a `static_assert(sizeof(X) == X_BYTES)` and, where
alignment matters, `offsetof` asserts. `ElementBytes` in the directory is the
schema width constant, not `sizeof` of a padded host struct — the codec writes
field by field, so the directory width equals the bytes actually written.

### Limits (`limits.h`, one owner per policy)

- Encoded input: `MAX_BLOB_BYTES` (decoded-payload admission ceiling);
  `MAX_SOURCE_BYTES` (JSON source admission).
- Decoded payload counts: `MAX_SPANS`, `MAX_JOINTS`, `MAX_KEYS`, `MAX_PATCHES`,
  `MAX_ADJACENCY`, `MAX_SOURCE_IDENTITIES`, `MAX_SECTIONS`.
- These are the single owner reused at read, validation, cook and publication.
  Peak memory is a distinct `WorkLimits` budget passed to the FoundationGeometry
  owners; it is not one of these admission counts.

`GetKindSections(GeometryKindTag)` names which section Ids a kind requires, so a
reader rejects a blob that declares one kind but carries another's sections.

---

## R2 — `Ludus::GeometryContent`

Static library at `modules/geometry/content`, namespace `ludus::geometry::content`.
Links `Ludus::Content` (Status/Diagnostic/JSON/Bytes/Digest/Catalog),
`Ludus::FoundationGeometry` (prepared owners + GeometryStatus) and
`Ludus::GeometryFormat`. It does NOT link the renderer.

Public header `geometry_content.h`; private helpers under `src/internal`.

### Diagnostics

Reuse the frozen `geometry::GeometryDiagnostic` for the geometry-specific fields,
wrapped with the content transport status and a repair suggestion:

```
enum class GeometryContentStatus : uint8 { ... }   // NOT needed: reuse content::Status + GeometryStatus
struct GeometryDiagnosticReport final {
    content::Status Transport = content::Status::Ok;   // Ok/Invalid/Limit/OutOfMemory/Conflict/Cancelled/Unsupported
    geometry::GeometryDiagnostic Detail{};             // code, source element, property, tolerances, range
    const char* Repair = "";                           // static suggested repair text (R3.6 / AGENTS "errors offer solutions")
    MessageText Message;                               // bounded human-readable wording
};
```

A `Fail*` helper (as `scene_cook`'s `Failure`) sets these and returns `false`;
nothing asserts on bad content.

### Source model (owned, validated)

```
enum class GeometrySourceMode : uint8 { CubicPath, HermitePath, CatmullRomPath, ScalarTrack, PatchSurface };

struct GeometrySource final {        // move-only owner; bounded Arrays
    GeometrySourceMode Mode;
    uint64 AssetId; uint64 Revision;
    math::Vector3d Origin; float64 AssetScale; bool IsClosed;
    geometry::ToleranceProfile Tolerance; geometry::WorkLimits Limits;
    geometry::PrecisionProfile Precision;
    // mode-specific owned arrays (author IDs attached per element):
    Array<AuthoredSpan>  Spans;       // CubicPath
    Array<AuthoredWaypoint> Waypoints;// Hermite/CatmullRom
    Array<AuthoredKey>   Keys;        // ScalarTrack
    Array<AuthoredPatch> Patches; Array<SurfaceAdjacencySource> Adjacency; // PatchSurface
};
```

Each authored element has a stable `uint64 AuthorId`. Handle mode is recorded on
authored waypoints (`HandleMode { Automatic, Manual }`) so an edit that moves an
automatic handle can convert it visibly (R3.1).

### Source JSON schema

`{ "version": 1, "mode": "...", "units": "...", "convention": "right-handed-y-up",
   "assetId": "...", "origin": [x,y,z], "assetScale": s, "closed": bool,
   "tolerance": { ... 12 fields ... }, "limits": { ... }, "precision": "strict|visual",
   "spans"|"waypoints"|"keys"|"patches"+"adjacency": [ ... ] }`

- `ParseGeometrySource(json, GeometrySource& out, GeometryDiagnosticReport& outDiag)`:
  validates via `JsonValue::Fields`, known enum strings, duplicate author IDs,
  monotone key times, finite numbers, and count ceilings from `GeometryFormat`
  limits. Unknown top-level fields, unknown `version`, duplicate IDs and invalid
  constraints → `content::Status::Invalid` + a diagnostic. (Fields unknown to the
  schema are rejected, matching ADR 0024's "unknown fields return diagnostics".)
- `WriteGeometrySource(const GeometrySource&, Bytes& out)`: deterministic
  canonical JSON through `content::JsonWriter`, fixed field order, reproducible
  numeric round-tripping. Round-trips `Parse∘Write∘Parse`.

### Cooking

```
[[nodiscard]] content::Status CookGeometry(
    const GeometrySource& source, Array<uint8>& outBlob,
    GeometryDiagnosticReport& outDiag,
    const Cancellation* cancelOrNull = nullptr) noexcept;
```

- Validate the source again (defence in depth); poll `cancelOrNull` between
  bounded units → `content::Status::Cancelled`, no partial output.
- For `*Path`/`Surface`, dry-run the conversion+preparation to confirm it is
  cookable (so a cook never emits a blob that cannot load): build the
  FoundationGeometry source inputs and call the matching `TryPrepare*`; a
  preparation failure maps its `GeometryStatus` into the diagnostic.
- Serialize field by field into a private `ByteWriter` (like scene_cook's), in
  the GeometryFormat layout: header first with placeholder digests/offsets, then
  sections, then back-patch the section directory and compute
  `PayloadDigest = Hash(payload)` and `SourceDigest = Hash(canonical source)`.
- `DependencyDigest` covers referenced sources (empty for a self-contained asset,
  hashed to the canonical empty digest).
- Determinism: identical source bytes → identical blob bytes.

### Loading / readers

```
struct GeometryLoad final {          // move-only; holds exactly one prepared owner
    GeometryKindTag Kind;
    geometry::PreparedPath Path;      // valid iff Kind == Path
    geometry::PreparedTrack Track;    // valid iff Kind == Track
    geometry::PreparedSurface Surface;// valid iff Kind == Surface
    GeometryCacheKey Key;             // digests, versions, precision, tolerances
};

[[nodiscard]] content::Status LoadGeometry(
    std::span<const uint8> blob, const geometry::WorkLimits& limits,
    GeometryLoad& outLoad, GeometryDiagnosticReport& outDiag) noexcept;
```

A private `BlobReader` (built on `foundation::core::TryReadLittleEndian` +
`math::IsFinite`, as scene.cpp's `Reader`) validates, in order, BEFORE building
any source input or exposing a view:

1. Size ≥ `HEADER_BYTES`; `≤ MAX_BLOB_BYTES`.
2. Magic match; `FormatVersion` supported; `CookerVersion` recorded.
3. `KindTag`/`PrecisionTag` are known enum values.
4. Each count ≤ its `GeometryFormat` limit; `SectionCount ≤ MAX_SECTIONS`.
5. **Count products / byte accounting**: for each section, `Count * ElementBytes`
   is computed with `TryIntegerCast`/checked multiply; the section
   `[Offset, Offset + Count*ElementBytes)` lies within the blob, is 4-byte
   aligned, and does not overlap any other section; the sections a kind requires
   are all present and no foreign section is present.
6. `ToleranceProfile` passes `geometry::TryValidateToleranceProfile`; monotone
   (strictly increasing) track-key times; finite origin/scale/controls/tolerances.
7. **Cache consistency**: recompute `Hash(payload)` and compare to
   `PayloadDigest`; the digests/versions/precision form the returned
   `GeometryCacheKey`.

Any failure → structured diagnostic, outputs unchanged. On success it builds the
borrowed source views and calls `TryPreparePath` / `TryPrepareTrack` /
`TryPrepareSurface`, mapping any returned `GeometryStatus` into the diagnostic and
leaving `outLoad` unchanged on failure.

### Cache key (R2.6)

```
struct GeometryCacheKey final {
    content::Digest SourceDigest; content::Digest DependencyDigest;
    uint32 FormatVersion; uint32 CookerVersion; uint32 GeometrySchemaVersion;
    geometry::PrecisionProfile Precision;
    geometry::ToleranceProfile Tolerance;      // all tolerances
    content::Digest TopologyDigest;            // surface adjacency / path closure / key domain
    // frame seed / seam policy are reserved fields for the framed/sweep caches (G3), zero here
};
```

### Catalog registration (versioned extension, R2.5)

Edit `modules/content`:
- `content.h`: add `Kind::CookedGeometry` to the enum (documented Doxygen).
- `catalog.cpp`: accept catalog `version` 1 or 2; the `"cooked-geometry"` string
  maps to `Kind::CookedGeometry` only when `version >= 2` (version 1 rejects it as
  `Invalid`, preserving old-catalog behaviour). Extend `Put` and `Write` mappings.
  `Write` emits `"version": 2` only when a `CookedGeometry` entry exists, else
  keeps `1` (so existing catalogs round-trip unchanged).
- `MAX_RESOURCES` and the limit owners are unchanged.

This is the minimal, backward-compatible "versioned extension" the task requires;
the catalog remains a closed enum (there is no dynamic registry, per the
precedent).

---

## R3 — Editor-private authoring adapters

New files under `apps/editor/src` (impl) and `apps/editor/src/internal` (private
headers), compiled into a model-level OBJECT library (mirroring
`ludus_editor_render_graph_model`), linking `Ludus::GeometryContent`,
`Ludus::FoundationGeometry`, `Ludus::Content` and the existing editor identity.
The geometry/document logic is Qt-free and headless-testable; GPU presentation is
out of scope for G5 (G6 integrates rendering).

### Document + undo (R3.1)

`CurveDocument` / `SurfaceDocument` embed `editor::DocumentHistory<State, N>`
exactly as `WorldDocument` embeds it:

- `State` is a small, equality-comparable, exception-free-copy value holding the
  authored source (controls, handle modes, keys/patches, author IDs). It is the
  editor's own representation that converts to a `geometry::content::GeometrySource`.
- Every mutator is one validated transaction routed through a private
  `bool commit(const State& candidate, GeometryDiagnosticReport& outDiag)`:
  validate the whole candidate (convert to `GeometrySource`, run
  `ParseGeometrySource`-equivalent checks), and only on success call
  `History_.Commit(before, after)` and swap the draft. Failure leaves draft,
  history and outputs unchanged — the candidate stays visible in the UI layer but
  is not published.
- Semantic edits: `MoveControl`, `SetHandleMode` (automatic→manual visible
  conversion), `InsertWaypoint`, `RemoveWaypoint`, `SetKey`, `MovePatchControl`,
  `SetClosed`, etc. Group edits (e.g. a drag) coalesce via `Commit(..., merge=true)`.
- `Undo`/`Redo`/`CanUndo`/`CanRedo` forward to `History_`; `Revision()` is the
  monotonic document revision keyed by the prepare/publish layer.
- Author IDs are minted through the existing `editor_identity.h`
  `TryGenerateUuidText` path (mapped to the `uint64 AuthorId` via a stable
  per-document counter + UUID label); no new identity source.

### Debounced live preview (R3.2)

`GeometryLiveBuild` mirrors `RenderGraphLiveBuild`: a debounce/supersession owner
that owns no documents. `Schedule(identity, hasDebouncedSource)` /
`MarkRequested(identity)` / `SetEnabled` / `SetBusy` / `Reset`, and a
`BuildRequested` signal. `ComputeGeometryBuildIdentity(const CurveDocument&,
profile)` returns a stable semantic digest (author-visible geometry + profile),
excluding cosmetic/layout-only state so those do not trigger rebuilds.

### Async prepare request + late-completion rejection (R3.3)

```
struct GeometryPrepareRequest final {
    uint64 Request;        // monotonic serial
    uint64 DocumentId;     // session/document identity
    uint64 Revision;       // document revision at dispatch
    uint64 Generation;     // activation serial
    geometry::PrecisionProfile Profile;
    content::Digest InputIdentity;   // semantic source+profile digest
    Array<uint8> SourceBytes;        // immutable snapshot cooked/prepared from
};
struct GeometryPrepareResult final {
    GeometryPrepareRequest Request;  // echoed identity
    bool IsReady; bool IsCancelled;
    GeometryLoad Load;               // valid iff IsReady
    GeometryDiagnosticReport Diag;   // on failure
};
```

`GeometryPrepareJob` runs the cook+load on an immutable `SourceBytes` snapshot
with a `{Pending, Ready, Failed}` pump (`enum class GeometryPrepareStatus`),
modelled on `PreviewGpuGeneration::Prepare()`: advance one bounded stage per call,
retain exclusive output through completion, and thread the `Cancellation` token so
superseded work stops and reports `Cancelled`. The owner records the dispatched
`InputIdentity`/`Generation`/`Revision`; on completion it publishes ONLY if the
result's identity still matches the current document identity — a late completion
(stale generation/revision) is rejected and does not publish. (FoundationThreading
`JobSystem` is available if off-thread execution is wanted later; G5 pumps on the
owner thread, consistent with the current preview adapters.)

### Transactional candidate publication + stale indicator (R3.4)

`GeometryPreview` mirrors `RenderGraphPreview`: it owns the active published
`GeometryLoad` and at most one candidate. `Tick(frameBoundary)` publishes a Ready
candidate at the declared frame/tick boundary; a Pending/Failed/cancelled
candidate or an identity mismatch keeps the active load and raises a stale flag
(`GetDisplayStatus() -> {Active, StalePending, StaleFailed}`). A failed
replacement never erases the active load. `GetPresentedRevision()` /
`GetPresentedGeneration()` carry the published identity. Peak memory is bounded to
active + one candidate (R4 test).

### Cursor reload policy (R3.5)

New explicit enum (nothing to mirror; it is introduced here):

```
enum class CursorReloadPolicy : uint8 { RetainParameter, RetainAbsoluteDistance,
                                        RetainNormalizedDistance, ProjectWorldPosition };
struct GeometryCursor final { uint64 Revision; geometry::SegmentParameter Parameter;
                              geometry::PathDistance Distance; math::Vector3d WorldPosition; };
[[nodiscard]] geometry::GeometryStatus ReloadCursor(
    const geometry::PathView& newPath, CursorReloadPolicy policy,
    const GeometryCursor& previous, GeometryCursor& outCursor) noexcept;
```

On publication the preview resets cursors and recomputes placement under the
consumer-chosen policy; a cursor whose `Revision` does not match the published
view is `StaleRevision`. The policy is a required argument, never implicit.
`RetainAbsoluteDistance`/`RetainNormalizedDistance` use the distance cache
(`TryResolveDistance`); `ProjectWorldPosition` uses `TryProjectPoint`;
`RetainParameter` clamps the span index/u into the new span range.

### Repairs (R3.6)

Each `GeometryDiagnosticReport` on a failure path sets a static `Repair` string,
e.g.:
- coincident waypoints → "Merge coincident waypoints (edit → merge)".
- `UnsupportedPrecision` below floor → "Split this chunk or relax PositionError to
  the Certified floor".
- `BudgetExceeded` → "Raise MaxLeaves/MaxDepth, or relax DistanceError".
- `InvalidTopology` → "Fix the adjacency join or remove the partial edge".

---

## R4 — Tests

### Format/codec tests (`modules/geometry/content/tests`, Catch2)

Links `Ludus::GeometryContent`, `Catch2::Catch2WithMain`,
`ludus_geometry_reference_fixtures` (G0 corpus). A standalone Catch2 case and a
separate `*_alloc_tests` for the allocation seam, mirroring
`ludus_geometry_alloc_tests`.

- **Round-trip**: build a `GeometrySource` from the G0 corpus cubic spans /
  tracks / patches / waypoints / far-origin fixtures; `WriteGeometrySource` then
  `ParseGeometrySource` equal; `CookGeometry` then `LoadGeometry` yields a
  prepared owner that evaluates to the fixture references within the G2/G4 Rounded
  tolerances; re-cook is byte-identical.
- **Corrupt/overflow fuzz**: a deterministic fuzzer mutates header bytes, section
  directory entries and counts — truncation, bad magic/version, overflowing
  `Count*ElementBytes`, out-of-range/overlapping/misaligned offsets, unknown enum
  tags, nonmonotone key times, nonfinite controls, mismatched `PayloadDigest`.
  Each returns a structured diagnostic (specific `GeometryStatus`/`content::Status`),
  never asserts/UB, and leaves `outLoad` unchanged. Count arithmetic is checked
  with `TryIntegerCast`.
- **Source validation**: unknown fields, unknown `version`, duplicate author IDs,
  invalid constraints (nonmonotone times, bad enum, coincident waypoints) →
  diagnostics.
- **Catalog**: a version-2 catalog admits `cooked-geometry`; a version-1 catalog
  rejects it; `Write` round-trips version and kind.
- **Allocation failure**: inject failure at each fallible allocation in
  `ParseGeometrySource`, `CookGeometry`, `LoadGeometry` via the existing seam; each
  returns `OutOfMemory`/`Invalid`-with-`OutOfMemory`-transport and preserves the
  active owner; evaluation after load allocates nothing.

### Editor adapter tests (`apps/editor/tests`, offscreen Catch2 + Qt6::Test)

Added to the offscreen test executable (and/or a standalone one like
`render_graph_live_build_tests`). No GPU/display.

- **Undo/redo**: semantic edits, coalesced drag, checked-head rejection, revision
  monotonicity.
- **Failed prepare**: a candidate that fails load keeps the active preview and
  sets `StaleFailed`.
- **Cancelled prepare**: cancelling the job yields `Cancelled` and keeps the
  active preview.
- **Late completion**: a result whose `Generation`/`Revision` no longer match is
  rejected and does not publish.
- **Stale cursor**: after publication, `ReloadCursor` under each policy produces
  the documented placement; a mismatched-revision cursor is `StaleRevision`.
- **Peak memory**: the preview holds at most active + one candidate; a probe
  asserts the peak against the declared budget.

---

## R5 — Documentation

- Doxygen on every new public declaration in `geometry_format.h`, `limits.h`,
  `geometry_content.h`, and the editor-private headers (private headers are not
  in the public SDK inventory but still documented per the reference rule).
- `docs/architecture/curves-surfaces.md`: update the status line, the
  "Repository fit" boundary table (mark GeometryFormat/GeometryContent and the
  editor adapters implemented), and the G5 gate row; add a short "Implemented
  content and editor (G5)" section linking the evidence ledger.
- `docs/architecture/math-evidence/curves-surfaces.md`: a G5 section with the
  gate evidence table, numerical/limit observations, departures, and a validation
  record listing what ran locally and what is deferred to CI (the pinned Clang 23
  toolchain is unavailable in the implementation sandbox; CI runs the warning-clean
  build, clang-tidy-23, ASan/UBSan, Wasm and SDK consumer).
- A canonical usage page `docs/architecture/.../geometry-content.md` (or under the
  module guide) only if setup/workflow instructions are warranted — the geometry
  source schema, cook and load workflow — registered in the index and wiki.
- `python scripts/check-docs --fix-index`; strict MkDocs; `scripts/build-api
  --bootstrap`; `scripts/check-wiki` — run where the toolchain allows, else
  recorded as deferred.
- Reference comments at the top of each new source/header pointing to
  `docs/architecture/curves-surfaces.md` (the design owner), ADR 0024 (frozen
  contracts), and the precedent modules that informed the layout
  (`scene_format`/`render_graph_format` for the wire format, `scene_cook` for the
  cook/digest flow, `RenderGraphPreview`/`PreviewGpuGeneration`/`DocumentHistory`
  for the editor adapters).

## Design decisions and reuse

| Decision | Reason |
| --- | --- |
| Header-only `GeometryFormat` depending only on FoundationBase | ADR 0024 + repository rule for tool/runtime-shared formats; mirrors `scene_format` |
| One cooked blob holds exactly one geometry kind, tagged | Avoids guessing source mode from counts; keeps the directory simple |
| Section directory with `{Id, Offset, Count, ElementBytes}` | Explicit checked offset/count sections; matches the architecture's "checked offset/count sections" and the render-graph count-prefixed sections |
| Serialized widths derived from field schema, not `sizeof` | AGENTS.md representation-constant rule; `static_assert(sizeof==_BYTES)` guards it |
| Reuse `content::Catalog` closed enum + schema version 2 | No dynamic registry exists; the versioned extension is the minimal, backward-compatible change |
| Cook dry-runs the preparation | A cook never emits a blob that cannot load; the loader re-validates independently |
| Editor documents embed `DocumentHistory<State>` like `WorldDocument` | ADR 0023 "one history per document"; no parallel undo system |
| Prepare job is an owner-thread `{Pending,Ready,Failed}` pump | Mirrors `PreviewGpuGeneration`; immutable inputs, exclusive output, identity-checked late-completion rejection |
| `CursorReloadPolicy` is a required explicit enum | Architecture: "must not be chosen implicitly" |
