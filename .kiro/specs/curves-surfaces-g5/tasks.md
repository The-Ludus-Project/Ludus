# Curves and surfaces G5 — tasks

Checkboxes are kept current as work proceeds. Toolchain note: the pinned Clang 23
build/format/tidy/ASan and the Wasm/SDK/doc tooling are unavailable in the
implementation sandbox (Amazon Linux 2023, Clang 15, CMake 3.22); those run in
batch CI. Local verification is close reading against the C++23 contracts plus
`clang++ -fsyntax-only -std=c++23` syntax checks where includes resolve.

## 1. `Ludus::GeometryFormat` (R1)

- [x] 1.1 Create `modules/content/geometry_format/` with the header-only INTERFACE
  CMake target, `FILE_SET` public headers, FoundationBase-only link, install rule,
  and a Catch2 test target. Add `add_subdirectory` in the root `CMakeLists.txt`
  next to `scene_format`.
- [x] 1.2 `include/ludus/content/geometry_format/limits.h`: admission limits and
  capacity constants (one owner per policy), with Doxygen.
- [x] 1.3 `include/ludus/content/geometry_format/geometry_format.h`: magic, version
  ladder, cooker version, serialized enum tags (`: uint32`) with predicates,
  the header + record structs, the section directory entry, `*_BYTES` width
  constants, `static_assert` on sizes/offsets, and the `@file` wire-layout and
  field-by-field contract. Reference comments to the architecture, ADR 0024,
  `scene_format` and `render_graph_format`.
- [x] 1.4 Format unit tests: size/width static_assert coverage, enum tag
  decode/reject, section element widths, header arithmetic.

## 2. `Ludus::GeometryContent` (R2)

- [x] 2.1 Create `modules/geometry/content/` static-library target (public header
  `geometry_content.h`, `src/` impl, `src/internal/` private helpers, tests).
  Add `add_subdirectory(modules/geometry/content)` in the root `CMakeLists.txt`.
- [x] 2.2 Source model: `GeometrySource` owner, authored element structs, handle
  modes, `GeometryDiagnosticReport` + `Fail*` helpers (no assertions).
- [x] 2.3 `ParseGeometrySource` (JSON → owned source; unknown fields/versions,
  duplicate IDs, invalid constraints → diagnostics) and `WriteGeometrySource`
  (deterministic canonical JSON); round-trip.
- [x] 2.4 `CookGeometry`: validate, dry-run prepare, serialize field by field via a
  private `ByteWriter`, back-patch directory, compute digests, cancellation →
  `Cancelled` with no partial output, determinism.
- [x] 2.5 `LoadGeometry` + private `ByteReader`: full validation order (size,
  magic, versions, enums, count products/byte accounting, offsets/alignment/
  overlap, tolerance profile, monotone domains, finiteness, payload digest), then
  build source views and call the FoundationGeometry `TryPrepare*`; structured
  diagnostics; outputs unchanged on failure. `GeometryCacheKey` assembled.
- [x] 2.6 Catalog versioned extension: `Kind::CookedGeometry` in `content.h`;
  `catalog.cpp` accepts version 1 or 2, maps `"cooked-geometry"` for version ≥ 2,
  extends `Put`/`Write`; keep version-1 catalogs round-tripping.

## 3. Editor-private adapters (R3)

- [ ] 3.1 `CurveDocument`/`SurfaceDocument` with `DocumentHistory<State>`, validated
  transactional mutators, semantic edit commands, handle-mode conversion, stable
  author IDs via `editor_identity.h`, undo/redo forwarding and monotonic revision.
- [ ] 3.2 `GeometryLiveBuild` debounce/supersession owner +
  `ComputeGeometryBuildIdentity`.
- [ ] 3.3 `GeometryPrepareRequest`/`Result`/`GeometryPrepareJob` async pump
  (`{Pending,Ready,Failed}`), immutable input snapshot, cancellation, identity
  carriage and late-completion rejection.
- [ ] 3.4 `GeometryPreview` transactional candidate publication, stale indicator,
  failed-replacement retention, publication at a frame/tick boundary,
  active+candidate peak-memory bound, presented revision/generation.
- [ ] 3.5 `CursorReloadPolicy` + `ReloadCursor` (four policies) and
  `StaleRevision` on cursor mismatch.
- [ ] 3.6 Repair suggestions on every failure path.
- [ ] 3.7 Wire the new editor files into `apps/editor/CMakeLists.txt`
  (model-level OBJECT library + links).

## 4. Tests (R4)

- [x] 4.1 Format/codec round-trip (source + blob + determinism) using the G0 corpus.
- [x] 4.2 Corrupt/overflowing-blob fuzz (sections + count arithmetic), unknown
  fields/versions, duplicate IDs, invalid constraints.
- [x] 4.3 Injected allocation-failure suite (`*_alloc_tests`).
- [x] 4.4 Catalog version-2 admit / version-1 reject / round-trip.
- [ ] 4.5 Editor tests: undo/redo, failed prepare, cancelled prepare, late
  completion, stale cursor, active+candidate peak memory (offscreen, headless).

## 5. Documentation (R5)

- [ ] 5.1 Doxygen on all new public API.
- [ ] 5.2 Update `docs/architecture/curves-surfaces.md` (status, boundary table,
  G5 gate row, short G5 section).
- [ ] 5.3 Add the G5 section + validation record to
  `docs/architecture/math-evidence/curves-surfaces.md`.
- [ ] 5.4 Canonical usage/workflow page under `docs/` (geometry content authoring,
  cook, load), registered in index/wiki, only if warranted.
- [ ] 5.5 `python scripts/check-docs --fix-index`; strict MkDocs; `scripts/build-api
  --bootstrap`; `scripts/check-wiki` — run where possible, record deferrals.
- [ ] 5.6 Reference comments wherever a source informed the design.

## 6. Validation before PR (run what the toolchain allows; defer the rest to CI)

- [ ] 6.1 Build, warning-clean (CI — pinned Clang 23).
- [ ] 6.2 Unit tests (CI).
- [ ] 6.3 `./scripts/check --format` (CI — clang-format-23).
- [ ] 6.4 `python3 scripts/python/source_policy.py --base <merge-base>` (local if
  Python deps resolve, else CI).
- [ ] 6.5 `./scripts/check <preset> --tidy-changed --base origin/develop` (CI).
- [ ] 6.6 check-docs, strict MkDocs, `scripts/build-api --bootstrap`,
  `scripts/check-wiki` (CI/local as available).
- [ ] 6.7 Local `clang++ -fsyntax-only -std=c++23` syntax check of new TUs where
  includes resolve.
- [ ] 6.8 Open the PR against `develop`; list exactly what ran and what didn't;
  never add `ci:full`; never merge/enqueue; report branch and worktree path.
