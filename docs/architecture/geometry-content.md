# Geometry content: authoring, cooking and loading

This page is the workflow guide for cooked curves and surfaces: the
`Ludus::GeometryFormat` cooked layout, the `Ludus::GeometryContent` codecs and
the editor-private authoring adapters added in phase G5 of the
[curves and surfaces architecture](curves-surfaces.md). The architecture page
owns the rationale and the full contract; [ADR 0024](../decisions/0024-curves-surfaces-contracts.md)
owns the frozen domains, statuses and format boundary. This page shows how the
pieces fit together for an author and a runtime.

## Pipeline

```
JSON source  -- ParseGeometrySource -->  GeometrySource (validated, owned)
GeometrySource -- CookGeometry -------->  LUGEO blob bytes (GeometryFormat layout)
LUGEO blob   -- LoadGeometry ---------->  GeometryLoad { PreparedPath | PreparedTrack | PreparedSurface }
PreparedX    -- borrow a view --------->  allocation-free sampling (FoundationGeometry)
```

A cooker never links the renderer, and a runtime loader never links a cooker;
both include the header-only `Ludus::GeometryFormat` contract.

## Source schema

A geometry source is hand-readable JSON, parsed through the checked Content JSON
facilities. Version 1 holds one geometry kind. The shared members are:

- `version` — the source schema version; `1`.
- `mode` — `cubic-path`, `hermite-path`, `catmull-rom-path`, `scalar-track` or
  `patch-surface`. The waypoint modes are authored through the editor adapter,
  which converts them to cubic spans before cooking.
- `revision` — owner-assigned snapshot revision copied into the prepared owner.
- `origin` — the float64 chunk origin subtracted before narrowing to float32.
- `assetScale` — the declared asset scale for `relativePositionError`.
- `closed` — whether the path or surface boundary is closed (enables `Wrap`).
- `precision` — `strict` or `visual`.
- `tolerance` — every ADR 0024 `ToleranceProfile` field (units in that record).
- `limits` — the preparation `WorkLimits` (`maxSpans`, `maxLeaves`, `maxDepth`,
  `maxSubdivisions`, `maxIterations`, `maxPreparedBytes`, `maxPatches`).

A `cubic-path` adds `spans`, each `{ id, kind: line|cubic, domainWidth, control:
[[x,y,z] x 4] }`. A `scalar-track` adds `keys`, each `{ id, time, value, inSlope,
outSlope, interpolation: step|linear|hermite }` with strictly increasing times.
Author IDs are unique within a source.

Unknown fields or versions, duplicate IDs and invalid constraints (coincident
waypoints, non-monotone key times, out-of-range tolerances) return a structured
diagnostic with a concrete repair suggestion. They are never assertions.

## Cooked blob

`CookGeometry` validates and dry-runs the FoundationGeometry preparation, so a
cook never emits a blob that cannot load, then serialises the LUGEO blob field by
field in little-endian order and records three SHA-256 digests (source,
dependency and payload). Identical source bytes produce identical blob bytes. The
cook is cooperatively cancellable through the `Cancellation` hook; cancellation
yields `Cancelled` and leaves no partial output.

The blob header carries the cache key the architecture lists: the source and
dependency digests, the format and cooker versions, the schema version, the
precision profile and every tolerance. `LoadGeometry` validates the magic, both
versions, the enum tags, every count against the format limits, each section's
`count x element bytes` byte product, alignment, offset range and non-overlap, the
tolerance profile, strictly increasing track-key times, control finiteness and
the recomputed payload digest before it exposes any prepared owner. Every failure
leaves the output unchanged.

## Catalog registration

Cooked geometry is a `cooked-geometry` resource kind in the content catalog. The
catalog schema version is extended to `2`; a version-1 catalog still parses and
rejects the new kind, and a catalog without geometry round-trips as version 1.

## Editor authoring

The editor-private adapters are Qt-free and build on the existing owners. A
`CurveDocument` records semantic edits as undoable commands on the shared
`DocumentHistory`: moving an automatic handle visibly converts it to manual, a
drag coalesces into one undo step, and the monotonic revision keys the preview.
A debounced live build supersedes stale requests by a semantic identity that
excludes cosmetic state. An async prepare job pumps `{Pending, Ready, Failed}`
over an immutable cooked snapshot carrying the document, revision, generation and
profile identity, with cancellation; the transactional preview publishes a Ready
candidate at a declared frame or tick boundary, keeps the last valid view active
with a stale indicator while an edit is invalid or preparing, never erases the
active view on a failed replacement, rejects a late completion whose identity no
longer matches, and holds at most the active load plus one candidate. On
publication a cursor reloads under a consumer-chosen `CursorReloadPolicy` (retain
parameter, retain absolute distance, retain normalized distance, or project world
position); the choice is explicit, never implicit.

## Where this lives

- `modules/content/geometry_format` — `Ludus::GeometryFormat` (header-only).
- `modules/geometry/content` — `Ludus::GeometryContent`.
- `apps/editor` `ludus_editor_geometry_model` — the editor-private adapters.

See the [evidence ledger](math-evidence/curves-surfaces.md#g5-cooked-geometry-content-and-editor-adapters)
for the G5 gate results and validation record.
