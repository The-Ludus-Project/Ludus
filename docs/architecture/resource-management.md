# Resource manager architecture

Status: Stage 1 AudioContent integration and the Stage 2 fixed-capacity native
coordinator, Stage 3 immutable CPU mesh consumer, and Stage 4 required DAGs and
explicit replacement transactions are implemented. Cooking, GPU residency adapters
and streaming below remain proposals. The initial
review used repository HEAD `144a708` on 2026-10-04. Stage 2 implementation and
bounded acceptance are described below; no performance improvement is claimed.

Use **one explicit coordinator for resource metadata and loading, immutable
resident versions, typed handles for identity, and explicit leases for lifetime**.
Keep audio, graphics, and other resource preparation and physical retirement in
their owning subsystems. Compile expensive content work in tools, acquire bytes
asynchronously, and publish complete candidates at controlled boundaries.

This is a design recommendation for Ludus, not a claim of universal optimality.
It favors a small inspectable implementation while retaining the contracts needed
for large games. The [reference review](resource-management-reference-review.md)
records the later article review and resulting refinements.

## Scope and existing contracts

The manager owns the lifecycle of reusable content such as meshes, textures,
materials, animations, sound definitions, and level descriptions. Mutable worlds,
voices, playback decoders, render targets, and gameplay instances keep their own
owners. Resource identity does not mean every engine allocation becomes an asset.

Preserve [Content version 1](content-resources.md), the
[handle and UUID design](object-handles-uuid.md),
[memory contracts](memory-management.md), and
[module lifecycle](module-lifecycle.md). Follow [AGENTS.md](../../AGENTS.md) and
[ADR 0003](../decisions/0003-standard-library-usage-policy.md): explicit failure,
exception-free operations, Ludus primitive types, bounded storage, narrow public
headers, and no reverse dependency into FoundationBase.

The first implementation must continue to load existing audio catalogs and retain
AudioSystem's serialized control owner and acknowledged retirement. It must not
require UUID migration, a universal VFS, reflection, an editor, a database, or a
general job system to load a sound.

The first useful runtime consists of five private tables, a request/result queue,
a small dependency walker, one budget ledger, and an audio adapter. Add only the
records and stages needed by its actual consumers. Cookers, chunk streaming,
alternate eviction scores, mounts, and spatial prefetch are later extensions.
Every extension must preserve four invariants: lookup does not load, a live lease
pins one exact version, publication exposes complete bindings, and freeing requires
proof that all users have finished.

| Inspected implementation | Consequence |
| --- | --- |
| [Content API](../../modules/content/include/ludus/content/content.h) | Version 1 has bounded string IDs, three audio kinds, owned bytes, SHA-256 digests, transactional catalog reads, and descriptor-bound native file readers. Preserve its formats and error behavior. |
| [AudioContent API](../../modules/audio/content/include/ludus/audio/content/loader.h) and [loader](../../modules/audio/content/src/loader.cpp) | One control owner, one pending acquisition, 64 revision slots, prepared clip sharing, and separate music playback instances. Cancel/destruction join its native worker; RequestCancel signals and Poll drains without joining. Leases carry loader provenance. One pending acquisition remains; concurrent acquisition service is future work. |
| [RHI resource API](../../modules/graphics/rhi/include/ludus/graphics/rhi/render.h) | The public slice exposes bounded shader, uniform, and pipeline resources. Mesh/texture residency and general GPU completion tickets must be implemented before those adapters can make their promised guarantees. |
| [Level loading](level-data.md) and [frame updates](frame-update.md) | Keep candidate-world activation and explicit frame/tick boundaries. The manager supplies resource readiness; the session commits game state. |

The native [audio content evidence](../development/audio-content-evidence.md)
describes the implemented subset and its limitations. Browser acquisition and
general graphics streaming below are contracts for future implementation, not
claims of browser or texture-streaming acceptance.

## Architecture and ownership

```mermaid
flowchart LR
    Source[Authored files and stable IDs] --> Cook[Validation and cooking tools]
    Cook --> Catalog[Immutable catalog and artifacts]
    Scope[Game session and resource scopes] --> Manager[Resource coordinator]
    Catalog --> Manager
    Manager --> Store[Asynchronous byte storage]
    Store --> Prepare[Private candidate preparation]
    Prepare --> Adapters[Typed subsystem adapters]
    Adapters --> Publish[Boundary publication]
    Publish --> Versions[Immutable resident versions]
    Versions --> Users[Leases and submitted uses]
    Users --> Retire[Subsystem retirement acknowledgments]
    Retire --> Manager
```

| Owner | Responsibility |
| --- | --- |
| Content tools | Import, validate, declare dependencies, cook, and create immutable release artifacts |
| Catalog snapshot | Exact identity, type, variant, dependency graph, artifact location, and optional source/debug metadata |
| Byte store | Read bounded ranges into owned storage; report completion and cancellation |
| Resource coordinator | Deduplicate loads, schedule stages, account budgets, manage requests and leases, publish revisions |
| Type adapter | Validate its payload, prepare its subsystem objects, state readiness, and retire them on the correct owner |
| Resource scope | Own a named set of requests and leases for one level, session, preview, or tool operation |
| Game and presentation code | Choose required content, optional fallbacks, quality targets, and activation boundaries |

Register a small static set of function-table descriptors explicitly at the
application composition root. Avoid static registration constructors, service
locators, a resource subclass hierarchy, and automatic global initialization.
An adapter contains context and stage functions; the coordinator never casts
audio objects into graphics objects or calls device APIs on worker threads.

Keep the dependency structure concrete:

| Proposed boundary | Dependencies and public surface |
| --- | --- |
| Existing `Ludus::Content`, `modules/content/` | FoundationBase and private parser/file/hash implementation. Owns catalogs and bytes; knows nothing about Audio, RHI, World, or Qt. |
| Implemented Stage 2 `Ludus::Resources`, `modules/runtime/resources/` | Content and the required Foundation facilities. Owns generic coordination through private tables and explicit function descriptors. |
| Existing `Ludus::AudioContent` | Retains its current Content/Audio dependencies and compatibility API. Do not make Audio depend on the coordinator. |
| Composition adapters, initially private to the consuming app | Depend on Resources plus AudioContent or RHI. Move to `modules/runtime/resource_adapters/` only when multiple consumers need them. |
| Cooker and inspection tools | Depend on source decoders and artifact schemas. Runtime does not link authoring tool libraries. Qt remains private to the editor. |

The application owns the coordinator, stores, adapters, and subsystems explicitly.
Any provider needed for preparation or destruction outlives its consumers. A
standalone tool can use Content without constructing the runtime coordinator.

Put registry tables, queues, hashing, threading, filesystem facilities, and
profiling formatting behind `.cpp`/private-header boundaries. Public headers
contain small values, spans, views, and thin typed wrappers erasing to non-template
operations. Share handle mechanics with the proposed identity work when available;
do not require a new identity module before using an existing compatible value.

## Implemented Stage 2 profile

[`Ludus::Resources`](../../modules/runtime/resources/include/ludus/runtime/resources/manager.h)
provides explicit fallible initialization, typed 16-byte identity tokens, copied
version-1 catalog metadata, exact linear ID/type lookup, scopes, durable per-caller
requests, shared operations, move-only leases, epoch-validated borrowing and
nonblocking service/close/shutdown. All calls and borrowing use one serialized
control owner. Adapter function tables are copied at composition; their contexts
and code outlive the manager. No static registration or Audio/RHI dependency enters
the coordinator. Its public declaration comments own the exact API contracts.

The original Stage 2 profile fixed catalog metadata and adapters for an
incarnation; Stage 4 extends it with immutable snapshots. A load key includes its
exact asset, snapshot, adapter epoch and direct child-version bindings; equal requests share one operation
and peak reservation. Resolve performs no I/O. Even a resident hit creates a
request. Ready results hold their exact version until claimed, cancelled or closed;
failed/cancelled results remain observable until EndRequest or scope close. A copied
raw token creates no hold. Release/Cancel/CloseScope/Shutdown need no normal queue
slot. Closing never revokes externally claimed leases; GetScope copies their named
scope and blocker counts. Release the holders and poll close again.

The fixed physical tables hold at most 64 catalog entries, 16 scopes, 32 requests,
8 operations, 16 candidates/residents/retiring versions and 64 lease records.
Config can lower admitted capacities without shrinking the physical allocation.
Slot generations saturate permanently and manager owner identities are never
reused. Service rotates through at most the requested number of operation/version
records (including transaction records in Stage 4), capped at one full table traversal; callbacks may perform bounded cold
installation/cleanup work, so this is not an audio callback or a measured time
budget. There is no concurrent-producer command queue in this profile.

Initialization checks a fixed metadata byte limit before allocating the PIMPL.
Admission reserves adapter-declared preparation and resident peaks with checked
remaining-capacity arithmetic. Deduplicated work is charged once. Preparation
reservation drops after successful publication; resident reservation survives
leases, submitted audio voices and delayed retirement. Failed/obsolete work retains
both reservations until the adapter acknowledges cleanup. Inspect copies these
conservative reservations and table counts, rather than measured process memory.
Allocator bookkeeping, opaque decoder/driver heaps, separately owned AudioSystem
control storage and music playback are outside these counters. A strict complete
heap/device budget still needs adapter allocation-domain evidence; these limits
must not be described as a hard process-memory cap.

Idle versions immediately enter retirement when their final request/lease hold
ends. A request arriving after that point creates a new operation; it cannot
resurrect retiring storage. Persistent idle caching/LRU, failure caching, priorities,
and cooked artifact revision keys remain later work. Stage 4 adds replacement and
required dependencies as described below. Keep loose-file roots immutable during an incarnation: copied
catalog metadata pins names/paths, not changing source-file contents. Loader's
optional catalog snapshot copies those metadata before worker dispatch and avoids
reopening catalog.json, while retaining descriptor-bound reads of individual files. The added optional
Begin parameter preserves source call sites but changes its native symbol; rebuild
SDK consumers with the updated AudioContent library.

The private [audio sample adapter](../../examples/audio-content/resource_adapter.cpp)
demonstrates two scoped callers for one Sound, cancelling preview while game
acquisition succeeds. It retains AudioContent's one worker and deliberately permits
only one candidate/resident sound at a time. Its AudioSystem must have no other
resident-clip owner; independently owned music streams remain supported. Retirement
polls Loader/AudioSystem and requires zero resident PCM, conservatively covering
partial installation failures and active voices. It cannot finance another sound
from retiring bytes. Its 128 MiB preparation and 64 MiB resident reservations are
sample policy allowances, not proven decoder heap bounds. A reusable multi-resident
adapter needs finer acknowledged retirement/accounting tickets before relaxing
this limitation. The adapter and sample are native Linux/macOS consumers; browser
acquisition is still Unsupported.

Typical owner flow is Initialize, OpenScope, Resolve, BeginAcquire, then Service
and GetRequest/TakeLease while continuing the frame loop. Borrow only during the
lease interval. CloseScope returns Pending while an external lease remains; release
it and poll CloseScope. Shutdown closes all scopes and rejects new admission;
continue Service and release external leases until Shutdown returns Ok before
reinitializing or destroying the manager/context. A backend unable to prove drain
keeps returning Pending/error and retains ownership; the caller must retain it or
use the host's unsafe-stop policy rather than destroying live state.

The [audio sample guide](../examples/audio-content.md) records SDK usage. Deterministic
core tests cover independent cancellation before/during/after publication, durable
outcomes under table pressure, cache hits, partial failure/retry, epoch loss,
output preservation, stale/foreign/reused handles, generation exhaustion, restart
identity, overflow admission and delayed cleanup. Native audio tests withhold worker
exit while proving shared installation/cancellation and retain budget until an
actual submitted voice reaches terminal acknowledgment. These are correctness
acceptance tests, not cold/warm latency or workload benchmarks.

## Implemented Stage 3 profile

[`MeshDefinitionAdapter`](../../modules/runtime/resources/include/ludus/runtime/resources/mesh.h),
exported by the existing `Ludus::Resources` component, prepares immutable CPU
triangle geometry for a second consumer. Content catalog kind `mesh` keeps
exact ID/path/type matching. Audio and mesh readiness, cancellation and
retirement remain adapter-owned.

Compose at most 16 `MeshSource` records explicitly. Initialize validates topology,
finite positions/UVs and finite nonzero optional normals, copies source metadata,
and borrows the arrays. Keep those arrays immutable and alive until adapter
destruction. Limits are 4096 vertices and 12288 triangle-list indices per source,
with at most 16 candidate/resident slots. No file decoder, mesh binary schema,
PBRT/FBX/glTF importer or filesystem worker is implied. Tools or application
composition supply prepared arrays; source paths are labels, not runtime reads.

Resolve and BeginAcquire remain free of adapter preparation. Owner-side Service
allocates candidate arrays explicitly at Begin, then Poll copies at most 128
vertices and 384 indices per visit. Complete arrays publish together. Equal
requests share one copy; cancellation drops only its caller. The largest source's
array byte size is reserved per operation, conservatively overcharging smaller
meshes. Borrowed source memory, adapter control storage (reported by ControlBytes),
allocator bookkeeping and renderer/GPU copies are outside that reservation.
Allocation can occupy a cold owner turn; record bounds are not measured time bounds.

Borrow with `content::Kind::Mesh`, then pass the `MeshDefinition` arrays to
`Renderer::CreateMesh` while holding the lease. The renderer copies them before
returning Ready/Pending. Release the CPU lease and service its retirement; renderer
mesh/snapshot/view and submitted RHI uses retain independently owned GPU versions.
CPU reservation release is therefore proof of CPU-copy cleanup only, never GPU
completion. GPU readiness still uses Renderer::GetStatus. Release renderer owners
and let RHI prove accepted-use retirement through its existing completion services.

A native mixed-adapter regression holds the sound worker before its read while a
mesh becomes ready through the same manager, then lets sound acquisition finish.
The public-only installed-SDK renderer fixture creates its quad and triangle through
resource leases, destroys CPU resource ownership before drawing, and preserves its
existing image oracle. Production renderer/RHI tests delay queue completion after
CPU teardown. The portable in-memory mesh adapter has no native worker dependency;
browser acceptance uses the same SDK fixture.

Stage 4 supplies required dependencies and activation. The
[initial Stage 5 profile](#initial-stage-5-storage-and-renderer-profile) adds project
artifacts and a fully resident aggregate scene adapter. Per-asset renderer adapters
and larger measured bounded capacities remain subsequent extensions.
Texture/material resource adapters and GPU residency accounting remain separate
work. Stage 2's 64 assets/16 versions and the renderer's own limits still apply.
Reinitialize an adapter only after all managers using its previous descriptor have
completed shutdown; a new profile needs new copied descriptors and reservations.

## Implemented Stage 4 profile

The existing `Ludus::Resources` component now supports a copied required resident
DAG and explicit replacement transactions. This is coordinator infrastructure:
no new scene file format, importer, material/texture adapter or GPU installation
policy is implied. The native mesh adapter remains a concrete CPU consumer.

Pass `DependencyGraph` to Initialize. Edges use exact parent/child catalog IDs
and an optional bounded authoring property path. Missing references return NotFound
with named provenance; cycles return Cycle with a bounded path and explicit
truncation. Repeated parent/child pairs retain the first property and create one
hold. The initial profile accepts 128 input edges, eight distinct children per
parent, and at most eight distinct nodes in an admitted root closure. All catalog
nodes are cycle-checked, including currently unused ones. Optional, build-only and
package references are not part of this graph. The graph is composition input,
not a change to Content's version-1 catalog serialization.

Admission walks the required closure in child-first order and preflights every
adapter, version/operation slot and combined peak reservation before accepting
anything. Adapter::MaxCandidates declares 1..16 slots for an exact context/type
pair, reserved across retained snapshots until physical cleanup. The entire
closure must fit that capacity as well as the core tables, so a leaf cannot occupy
the last slot its parent needs. Composition reserves those slots exclusively for
this manager; other managers or outside backend owners are not counted. Existing
exact work is charged once, including diamond leaves. Parents
wait on pending counts; child completion wakes their dependent records without
blocking a worker. A parent must provide Adapter::BeginBound. Its temporary
DependencyView array names complete immutable child payloads; retaining those
payload pointers is valid through that parent's physical Retire acknowledgment.
The coordinator retains each direct child once per parent version, independently
of external leases. A failed parent also retains children until its own cleanup
acknowledgment. Zero-byte failed records can briefly remain for propagation after
physical cleanup; Inspect's Versions includes these records, whereas byte counters
already reflect acknowledged release. BorrowDependency exposes only direct,
exact-version children of a valid parent lease.

Replacement follows this owner-side flow:

1. Keep the same catalog ID/type set, create immutable source/adapter profiles and
   assign a nonzero increasing application revision. Paths, profiles and required
   edges may change. Resolve produces logical root handles that survive replacement.
2. Call BeginReplacement with the complete desired root set (one to eight handles,
   duplicates removed). The coordinator copies a private catalog/adapter/graph
   snapshot and admits its whole closure. Service prepares it without changing
   CurrentRevision. OpenTransactionScope optionally gives validation code a private
   scope for candidate acquisition and leases.
3. Observe GetTransaction until Ready, then call Commit at the application's
   reader boundary. Commit validates every root and dependency before publication;
   afterward the binding switch contains no allocation, callback or fallible step.
   The entire root set becomes current together. Ordinary new scopes see it;
   existing scopes keep their previous immutable snapshot and exact bindings.
4. EndTransaction consumes the durable outcome. CancelTransaction drops only the
   attempt's root demand; private scope requests/leases remain owned. Failed,
   cancelled or superseded attempts preserve the current last-good set. Cleanup
   proceeds through Service and adapter retirement acknowledgment.

After valid output/root-token preconditions, a newer desired revision supersedes
older active attempts even if its subsequent graph validation or admission fails.
Older out-of-order completions cannot commit. Retrying the same desired revision
is allowed after failure/cancellation, but never after committing that revision.
The current revision and desired revision are separately observable. A changed
adapter epoch invalidates usability; it does not silently rebind an activated
snapshot to a new subsystem session. Provider contexts and immutable source data
for every admitted snapshot must survive Manager::Shutdown returning Ok; never
reinitialize an adapter still referenced by any snapshot.

Committed roots pin their whole exact closure even with no external lease. Old
activated snapshots remain pinned while old scopes are open, including a closing
scope blocked by a lease. Once those scopes close, root demand drops and parent
retirement precedes child retirement. Ordinary acquisitions outside an activated
root set retain Stage 2's immediate idle retirement. Closing a scope does not
unpublish the current set; Shutdown drops current root pins, closes admission and
requires all external leases and physical retirement to drain. There is no empty
root-set activation or idle cache eviction API in this initial profile.

Fixed storage adds four catalog snapshots and four durable transactions (Config
can lower admission limits); existing 64 assets, 8 operations, 16 versions and
64 leases remain. MetadataBytes defaults to 512 KiB to cover copied paths, DAGs
and failure outcomes; Inspect reports the actual coordinator block size, excluding
allocator overhead and adapter storage. Snapshot reuse resets bookkeeping in place
and overwrites counted entries, avoiding a whole-record temporary on the browser's
64 KiB stack. Reference browser resource builds reject individual stack frames over 16 KiB;
the existing packaged renderer test covers activation under the default stack size.
Reinitialization briefly owns both the
old drained block and the new block. A replacement conservatively prepares its
entire closure in a new snapshot rather than reusing versions across snapshots.
Its peak must fit alongside last-good and retiring versions: retiring bytes never
finance admission before physical acknowledgment. Retained terminal transactions
consume slots until EndTransaction. Saturation never blocks cancellation, lease
release, scope close or shutdown. Service's record count bounds work, not latency.

Deterministic tests cover diamond sharing, duplicate edges, readiness ordering,
missing/cyclic graphs, atomic closure rejection, named failure/retry, multi-root
publication, old-scope coherence, last-good retention, supersession including
failed newer admission, private leases after cancellation, table saturation and
physical parent/mesh cleanup. The public-only installed-SDK renderer fixture
prepares and activates its two mesh roots together, then preserves its image
oracle after complete CPU resource shutdown. Renderer/RHI GPU ownership remains
independent. The private audio sample adapter still supports only one resident
sound, so its one-slot declaration rejects overlapping sound replacement at admission.
A finer multi-resident audio retirement adapter is needed for that use.

New public parameters and Adapter fields preserve existing source call sites with
defaults, but change native ABI/layout; rebuild the SDK and its consumers together.
The [reference review](resource-management-reference-review.md) owns attribution:
Hamaide's pending-count/dependent scheduling informs readiness, while this bounded
single-owner transaction protocol and immutable scoped snapshots are original.

## Identity and lifetime

Keep five distinct values:

1. Durable asset key: existing logical ResourceId in version 1; proposed typed
   AssetId UUID in a versioned future format. Paths and content digests are separate.
2. AssetHandle: typed owner/slot/generation identity for a logical record. It
   stays valid when its payload is evicted or replaced.
3. RequestHandle: one caller's acquisition attempt. Several requests can share
   one underlying load without sharing cancellation or ownership.
4. ResidentVersion: one immutable prepared payload and its exact dependency
   versions. Existing leases continue to refer to it after replacement.
5. ResourceLease: one owned hold of a specific resident version. A handle alone
   does not pin memory, grant access, or imply readiness.

Use the proposed 16-byte owner/slot/generation representation for new public
runtime handles. Validate type, owner, slot bounds, generation, and live state in
release builds, including handles arriving through erased APIs or tools. Never wrap
owner, request, version, or generation counters. Keep existing audio/RHI ABIs until a
separate migration. Cache hashes accelerate lookup; exact keys establish identity.

Resolve durable IDs on the cold path. Borrow typed payload views once within a
protected phase or lease interval. Never provide implicit pointer dereference
that loads content or silently changes the borrowed version during reload.

An asset record contains stable identity, declared type, source metadata, and
current bindings for resolved variants. Removing a record invalidates its logical
handle; evicting a payload does not. Keep removed metadata as a tombstone while
old versions drain. Slots exposed to clients or jobs consume a generation even
when their acquisition fails. Reinitializing a manager obtains a fresh owner token.

The registry keeps separate preallocated pools for logical records, requests,
operations, leases, and resident versions. Hot identity metadata is compact;
payloads, long names, graph diagnostics, and editor annotations live separately.
An exact-ID index is for cold resolution. Fixed capacity and a free list are the
first implementation; grow through stable pages only when catalog scale requires
it. Never move leased payload storage during table growth or cache compaction.

Keep these cache identities separate:

| Cache | Key and validity |
| --- | --- |
| Derived artifact | Source digests, declared build-input digests, canonical settings, importer executable/version, output schema, and target profile |
| Byte chunk | Immutable artifact digest, exact byte range, and codec; an open source revision remains owned until the read finishes |
| Prepared version | Exact logical ID/type, artifact revision, resolved variant, required dependency bindings, preparation profile, and subsystem epoch |

The initial prepared-operation key also includes catalog snapshot identity. This
conservative rule avoids sharing incompatible graphs across snapshots. Deduplicate
across snapshots only after exact dependency/profile equality can be proved.
An ordinary lookup hash never substitutes for full key comparison. Different
logical assets may share immutable byte storage later, but never acquire each
other's mutable state or source provenance by accidental digest equality.

## Public acquisition contract

Expose a small explicit API. The following broader contracts guide the remaining stages; the implemented
Stage 2 subset is specified by its public header and profile above. All operations return bounded
statuses and are `noexcept`. Failed admission acquires no ownership.

Acquisition output parameters must be empty on entry. A nonempty owning output
returns InvalidState and remains intact; an API never overwrites a live lease.
A scope pins the selected catalog snapshot and resolved profile for its requests.
Following a replacement snapshot requires an explicit refresh/transaction, while
existing leases keep their original binding graph.

| Operation | Contract |
| --- | --- |
| `Resolve` | Convert a typed durable reference into an AssetHandle against an owned catalog snapshot. No I/O, preparation, or implicit acquisition. |
| `BeginAcquire` | Add one caller's demand, options, and provenance; return a RequestHandle even for a cache hit. Reserve capacity before accepting ownership. |
| `GetRequest` | Observe Queued, Working, Ready, Failed, or Cancelled, with stage and diagnostic. Always nonblocking. |
| `TakeLease` | On Ready, transfer the request's held version into one typed ResourceLease and consume the request exactly once. Pending transfers nothing. |
| `Cancel` | Drop this caller's demand and unclaimed ready ownership; retain a Cancelled outcome until consumed. Other callers are unaffected. |
| `EndRequest` | Discard the caller's result and invalidate its request handle; cancel first if necessary. Owned operation cleanup continues independently. |
| `Borrow` | Validate a live lease and return a const typed view of that exact version, with its actual coverage. Never acquire or reload. |
| `Release` | Consume one live lease token; mark the version eligible for policy-driven caching or retirement. Never wait for a device. |
| `CloseScope` | Close admission, cancel requests, and release scope-owned leases after their reader phase. Remain Pending while externally owned leases or registered uses still need to finish. |
| `Service` | Apply a bounded completion batch, advance ready stages, publish permitted candidates, and progress retirement on the control owner. |
| `Inspect` | Copy a bounded immutable diagnostic snapshot; no allocation or object mutation on a reader thread. |

A resource scope names the purpose of its accepted requests and registered leases,
such as `level/forest`, `editor/preview`, or `session/ui`. It owns requests and can
store lease wrappers in a bounded collection. A lease claimed into caller-owned
storage remains registered to that scope. Closing cancels requests and releases
the wrappers the scope owns after its borrowers finish; it never revokes an
externally owned lease. Those leases and registered uses are named close blockers
until released or explicitly transferred to another live scope. Close is polled
without blocking, and scope storage remains alive until close completes.

A Ready request pins its result until claimed, cancelled, or its scope closes;
abandoned requests cannot live outside a tracked scope. The manager keeps the
terminal outcome until TakeLease or EndRequest consumes its request. CloseScope
ends all scope-owned requests, including failed/cancelled results. Operations can
finish before a polling call, but never invoke inline client code.

Use a move-only lease wrapper over an owner/slot/generation lease token. Explicit
release clears the wrapper. Its destructor on the control owner only releases the
hold; it never joins work, starts file I/O, or blocks. Raw token copies do not
create holds; a stale or repeated release returns InvalidHandle. Async consumers
receive separately registered use tickets, rather than moving this wrapper to
arbitrary worker destructors. Scopes and leases cannot outlive their manager.

For example, level activation resolves roots, begins scoped requests, polls while
continuing the frame loop, claims all required leases, validates the candidate
world, and commits both the world and its scope at a frame boundary. An unsuccessful
candidate closes its scope while the active level retains its leases. Mutable
spawned instances borrow their definitions without becoming resource records.

Optional presentation code may select an explicit fallback binding. Provide
immutable type-correct fallback assets and report the requested asset's failure.
Do not pretend that fallback texture dimensions, skeleton layout, or sample length
match the requested data. Required gameplay/collision/schema inputs fail admission.

## Loading and concurrency

One control owner mutates the registry, state machines, budgets, and publication
table. Workers perform byte acquisition, decompression, validation, and CPU
preparation using owned inputs. Their completion messages return to that owner.
Graphics and audio adapters marshal installation and retirement to their own
owners. Begin with polling and bounded queues; add worker-pool sophistication
only when real consumers require it.

Stage 1 established the AudioContent integration boundary: RequestCancel signals
without joining, and Poll detaches a completed worker after its final job access.
Its legacy Cancel/destructor still join for cold shutdown. Stage 2's sound adapter
calls Begin, Poll and RequestCancel on AudioSystem's serialized control owner;
worker completion remains durable inside Loader. It retains legacy lease tokens
privately and the facade supplies independent manager/request/lease identities.
Only one acquisition can be pending in Loader, and native Begin remains unsupported
in browsers. The Stage 2 sample also limits its adapter to one resident sound;
other work remains queued. See the implemented profile below for retirement and
budget limitations.

Each adapter explicitly reports its memory estimate and supplies CPU prepare,
begin/poll install, cancel, begin/poll retire, and CPU cleanup functions as needed.
Ready or Failed install attempts may still own objects and remain retireable.
Poll functions perform bounded work or observe durable state; returning Failed
does not mean all acquired ownership disappeared. The coordinator invokes cleanup
on its declared owner and never destroys a type-erased context while its functions
can still execute.

Concurrent producers submit copied commands through bounded admission queues;
they do not mutate registry records. CPU jobs receive immutable catalog/byte owners
and preallocated result slots. Publication uses the queue's release/acquire
synchronization and a reader boundary; an atomic current pointer alone does not
protect its pointee from reclamation. The first implementation permits borrowing
on the control owner, with explicitly pinned immutable inputs handed to workers.
It does not require hazard pointers or a global reader epoch scheme.

Reserve result storage and a durable completion location before job/device
dispatch. A full queue refuses new normal work with QueueFull; it cannot discard
an accepted completion or retirement acknowledgment. Cancellation/release/stop
use reserved control capacity or preallocated per-operation flags, so normal
load pressure cannot prevent cleanup. If an event queue is only a wakeup hint,
its lost wakeup is recoverable by scanning durable result records in bounded
batches. Adapter callbacks execute outside manager locks and cannot reenter it.

Deduplicate by logical key, content revision, resolved variant, import profile,
and subsystem session. Keep one operation with separate caller waiters. Cancelling
one request releases only that waiter's demand. Cancelling the final demand marks
unneeded work for cancellation; it does not free buffers or device objects still
used by workers, GPU commands, or audio callbacks.

Every completion carries manager lifetime, operation generation, catalog snapshot,
target revision, and relevant subsystem epoch. Reject obsolete publication while
retaining ownership long enough to clean up the completed work. Never block a
browser event loop, rendering thread, or audio callback waiting for a resource.

An obsolete completion still owns its candidate buffers and any partially created
subsystem objects. Transfer it to cleanup, acknowledge retirement, then release
its budget. A new request cannot reuse that storage before this protocol finishes.
Retry creates a new operation generation. Cache deterministic validation/type
failures only for their exact immutable inputs and preparation profile, until
explicit retry or input/profile change. QueueFull, BudgetExceeded, cancellation,
and device loss do not permanently poison an asset's cache entry. Transient I/O
failure remains visible on its request; a later acquisition may retry under a
bounded policy with backoff, attempt limits, and one shared operation. Automatic
network retry, when introduced, needs the same bounded backend policy.

Separate operation state from resident state. A current version can remain usable
throughout a replacement's failed or cancelled operation.

```mermaid
stateDiagram-v2
    [*] --> Queued
    Queued --> Reading: Capacity and budget admitted
    Reading --> Preparing: Owned bytes validated
    Preparing --> WaitingDependencies: CPU candidate prepared
    WaitingDependencies --> Installing: Required versions ready
    Installing --> Publishable: Subsystem readiness proven
    Publishable --> Completed: Boundary commit
    Queued --> Cleaning: Cancel or admission failure
    Reading --> Cleaning: Cancel or failure
    Preparing --> Cleaning: Cancel or failure
    WaitingDependencies --> Cleaning: Cancel or dependency failure
    Installing --> Cleaning: Cancel or failure
    Publishable --> Cleaning: Obsolete candidate
    Cleaning --> Terminal: Jobs and partial objects retired
    Completed --> [*]
    Terminal --> [*]
```

Required reads and CPU preparation can run in parallel before dependencies finish;
only stages that actually need those dependencies wait. Deferred admission is
inspectable and bounded. It must not allocate an unaccounted half-built closure.

Readiness has a named contract: validated CPU data, installed subsystem object,
and usable required coverage. A graphics adapter may use a recorded GPU ordering
dependency if the RHI proves it; the first adapter waits for its defined ready
status outside drawing. A music resource means its source/definition is usable;
each playback separately needs its own prepared stream, decoder, ring and prefill.
Ready never means a mutable playback cursor or voice is globally shared.

## Dependencies and publication

Declare required resident dependencies, optional/soft references, and build-only
inputs separately. The required resident graph is acyclic. Validate cycles and
missing references in tools and when mounting a catalog. Load dependencies before
finalizing a parent; its version records exact dependency versions and retains
their ownership until the parent has physically retired.

Prepare replacements privately. A candidate has no externally visible current
binding until validation, dependencies, subsystem readiness, and budget admission
all succeed. Publish at a coordinator boundary; a level or dependent set can
commit through one activation transaction. Failure preserves the last good set.
Old readers and submitted work keep the old version until acknowledgment.

Treat dependency roles as data, rather than one overloaded reference flag:

| Role | Loading and lifetime policy |
| --- | --- |
| Required resident edge | Must reach the requested compatible readiness before parent publication; parent version owns the exact dependency version. |
| Soft runtime reference | Names possible future content without a residency hold. Resolve/acquire explicitly when needed; cycles are allowed. |
| Build input | Invalidates cooking when it changes; need not be shipped or loaded at runtime. |
| Package inclusion | Required in the selected shipping product even when lazily loaded. A soft runtime reference can still be a mandatory package input. |

Store compact adjacency ranges, reverse edges, topological rank, and source
property paths in the validated snapshot. Deduplicate repeated edges. Use checked
pending-dependency counts to make parents runnable; a failed dependency propagates
failure with its chain, rather than disappearing and being considered fulfilled.
Build/package traversal tracks visited nodes, so legal soft-reference cycles do
not loop forever. Never wait on a dependency by occupying a worker thread.

Retain dependencies once per parent resident version, not once per client or
recursive traversal. A diamond graph can share one loaded leaf with distinct
parent holds. Release those holds only after the parent's physical retirement.
Idle parents consequently pin their closure: trimming must consider leaf memory
held by idle parents rather than claiming every zero-client leaf is reclaimable.

A catalog snapshot fixes type, variant and target revision of its dependency
bindings. Device recreation invalidates prepared objects for that device epoch;
CPU artifacts may remain cached. New acquisitions target a supported replacement
epoch. An old device lease reports DeviceLost and retains its cleanup ownership;
it does not automatically turn into a new device object.

### Replacement transactions

File changes trigger a tools-side revision check, import, dependency validation,
and candidate cook. Timestamp events are hints; source digests and saved-document
conflict checks establish the input. Debounce/coalesce changes and retain a
per-asset desired revision so an older successful cook cannot replace a newer one.

Rebuild dependents whose compiled data or prepared bindings change. A material
bound to texture version 4 remains on that version until its replacement binds
the new texture. This intentional policy avoids an old parent silently changing
layout while a reader uses it. For live presentation replacement, stage the affected
reverse dependency set and publish the relevant root bindings as one transaction.
Allow independent compatible assets to publish separately only under explicit
application policy; simulation definitions follow world activation rules.

Preflight publication slots, lease/use records, dependency holds, and budget. Once
preflight succeeds, the owner changes the complete binding set between reader
phases with no allocation, user callback, device call, or fallible step. If immutable
binding snapshots are used, readers retain the old snapshot until their phase/use
ends. Single-file atomic replacement alone is not a multi-asset transaction.

Keep the last good current bindings on any candidate failure. A transaction needing
more peak memory than available returns BudgetExceeded; the application can offer
an explicit unload/loading-screen transition instead. New triggers use the new
audio revision while old voices finish the old one. Preview overrides live in a
private scope/snapshot and never overwrite saved content or leak into shipping.

### Physical retirement

The reclaim condition for a version is zero client/request/parent/reader holds,
no outstanding worker access, no submitted-use ticket, and a completed subsystem
retirement acknowledgment. Caching can delay entering retirement; once retirement
is irreversible, reacquisition starts a new operation rather than resurrecting it.

Before submission, collect distinct versions into a preallocated use batch and
retain them. On successful submission the owning adapter records completion
tickets including device/session epoch and queue serial. Transfer those use holds
to backend retirement tracking; release the original frame/voice setup lease when
appropriate. A definite failed submission drops unsubmitted holds. An uncertain
submission retains them until the backend proves quiescence. Track every relevant
GPU queue and audio terminal acknowledgment; a fixed delay of several frames
does not prove completion.

CPU input, upload staging, payload, and dependency holds can have different safe
release points. The adapter reports each point; the coordinator never guesses
from elapsed time or from a refcount alone.

## Budgets and eviction

Track metadata, encoded input, decoded CPU payloads, decoder scratch, upload
staging, and device memory separately, with active/candidate/idle/retiring states
within each domain. Retiring is a state, not a second charge for the same bytes.
Reserve peak cost before dispatching work. Deduplicated work is charged once.
Replacement requires room for old and new versions and outstanding work together.

Hard limits govern allocations the engine and adapters can account for. An adapter
must provide bounded decoder/backend costs or reserve a conservative proven upper
bound before admission; an unbounded adapter cannot promise a strict-budget profile.
Allocator headers, alignment, backing pages, and fixed control storage are included.
Track process/device measurements separately: a registry counter cannot impose a
hard limit on opaque driver allocations or all OS memory.

Use a simple bounded idle cache first. Evict only versions with no client lease,
parent hold, request hold, borrowed reader, or submitted use. Releasing the last
lease makes a version eligible; it does not force immediate unload. Cache retention
and prefetch are optional policy, while exact lifetime accounting is mandatory.

Budget pressure first cancels speculative work, then trims idle residents, then
reduces optional quality or refuses admission with a structured status. It never
invalidates a lease or reports future reclamation as already freed memory.

For each memory domain enforce:

```text
committed live bytes + reserved additional peak bytes <= domain hard limit
```

Committed bytes include active, idle, candidate, and retiring allocations. A
reservation converts to committed allocation as work progresses; do not count
the same bytes in both terms. Reserve extra scratch/staging still to come, and
release a charge only when its physical allocation is actually freed. Shared
allocation records prevent double charging dependencies; domains describe distinct
allocations even on unified-memory hardware. Also cap concurrent requests, jobs,
edges, descriptors, decoder instances, file handles, and queue records.

As an illustrative accounting check, a 512 MiB domain with 300 MiB active data,
60 MiB retiring data, and a 100 MiB replacement requiring 40 MiB extra scratch
already accounts for 500 MiB. Another 20 MiB load must wait or fail. A retiring
allocation cannot finance a reservation until its acknowledgment arrives. These
numbers illustrate the invariant; they are not proposed platform defaults.

Process domain reservations atomically on the coordinator. Reserve the required
closure for a root transaction using shared operation records before dispatch;
reuse existing admitted work. Do not hold scarce upload/scratch capacity while
waiting for a dependency needing that same capacity. If full-closure reservation
does not fit, queue with no partial reservations or fail; implement staged
admission only with a documented progress guarantee. Oversized indivisible assets
fail explicitly. Fallible allocation still checks every request; limits do not
prove that an OS allocation will succeed.

Use domain soft targets with hysteresis below hard limits. Start with LRU over
eligible idle versions, a minimum retention interval that hard pressure may
override, and bounded trim work per service call. Optional prefetch occupies a
small probationary quota, so a traversal of never-used assets cannot evict the
entire proven working set. Incoming level holds are established before dropping
outgoing holds for shared versions, preserving level-to-level reuse.

The [cache replacement article](resource-management-reference-review.md#cache-replacement)
adds a measured alternative: rank eligible victims using recent actual use,
reload time, reclaimable bytes in the pressured domain, and predicted need. Pure
recency and pure cost can both thrash. Keep the default simple until replay traces
show improvement. Count rendering/audio/CPU uses in batched records; do not mutate
an LRU list or atomic reference counter for every draw or sample. Do not scan all
logical catalog records every frame. Account for a selected parent's closure and
shared leaves when estimating reclaimed bytes.

Allocation belongs to existing/future FoundationMemory domains. There is no
second resource-specific heap, global free hook, or payload-moving defragmenter.
Specialized pools and arena chunks are adapter choices with measured lifetime
benefits and explicit release acknowledgment.

## Project scene import and saved-level workflow

Status: Stage 5 implements the initial fully resident native scene workflow, now integrated into **World** authoring.
The [Editor guide](../development/editor-workspace.md#project-owned-3d-levels-and-reference-import)
owns its user-facing steps and source-profile limits. The standalone staircase2
sample remains historical prototype evidence.

The user-facing acceptance sequence is:

1. Create a project or open an existing project.
2. Create a scene/level or open a saved scene/level in that project.
3. Choose **Import reference scene**, select the source and let the editor validate,
   convert and prepare its assets. Conversion is an internal tool operation.
4. See the imported geometry, materials, textures, lights and camera in the current
   scene's rendered viewport. The scene remains editable and is marked dirty.
5. Save the scene/level. Closing and reopening it restores the imported content
   from project-owned assets without invoking the source importer again.

Project setup and repair follow the existing
[project SDK workflow](project-sdk-workflow.md). Import must not require editing
CMake targets, run arguments, renderer source, asset schemas or machine paths.
An existing project receives the same import operation as a newly created one.

### Durable assets and document ownership

The project owns an asset catalog, immutable cooked artifacts and their dependency
manifests. Their durable location must be included when the project is copied or
packaged; an ignored build directory or disposable derived-data cache is not the
sole owner of a saved level's assets. Preserve attribution/license files with
imported content. Source locations are provenance and optional reimport inputs,
not mandatory inputs for reopening an already cooked scene.

The scene/level document owns persistent object IDs, hierarchy, transforms,
camera and light settings, and stable resource IDs for mesh/material bindings.
The catalog resolves those resource IDs to exact artifact revisions and required
texture/material/mesh dependencies. Never serialize renderer handles, GPU pointers,
leases, request IDs or temporary absolute import paths into the scene document.
Several levels may reference the same immutable assets while retaining independent
object placement and edits.

The editor performs import on bounded workers, validates the complete candidate
closure and reports unsupported features and material approximations. Publish the
candidate into the open document and its renderer bindings at a controlled
boundary; failure or cancellation preserves the current scene and its last-good
rendered version. The resource manager coordinates versions and leases; the
renderer/RHI own GPU preparation and retirement. Saving verifies that the referenced
artifact closure is durable before publishing the level document. A failed save
keeps the document dirty and preserves the previous saved level.

Reopening resolves the saved IDs through the project catalog and loads the cooked
closure through the resource manager and renderer adapters. Runtime decoding and
GPU preparation are normal loading work; source-format parsing/conversion is not.
Only an explicit import/reimport/update operation changes authored asset revisions.
Importing a second instance must support independent scene objects while reusing
compatible existing resources. Importing into an existing scene preserves its
other objects, assets and saved data.

### Implementation admission and acceptance

Stage 5 supplies the project asset/cooking/catalog foundation. Complete the level
asset-reference contract, texture/material/renderer resource adapters and editor
import/viewport/save integration as part of this product milestone. Budget and
validate a profile that admits the reference scene's complete dependency closure;
the small Stage 3/4 acceptance profiles are not an implicit scene-size guarantee.
Mip/LOD/chunk streaming in Stage 6 remains a separate capability and is not required
for a fully resident scene that fits its declared CPU/GPU budgets.

The first complete acceptance test uses staircase2 through the workflow above:
save, close the editor, move/copy the project, remove access to the original external
reference-scene directory and discard temporary build/cook caches, then reopen the
saved level. Verify the hierarchy, asset IDs, transforms, materials, textures,
lights and camera, render it successfully, and assert that the importer was not
invoked. Exercise both new and existing projects and both new and existing levels.
Additional regressions cover shared assets across two levels, duplicate import,
unsupported source features, missing/corrupt cooked dependencies, failed/cancelled
imports, failed saves, and source-edit conflicts while retaining last-good data.

Standalone converter invocations, a special executable for one scene, or a saved
PNG demonstrate isolated mechanisms. They do not satisfy this editor workflow's
exit condition. The [staircase2 prototype guide](../examples/reference-scene.md)
records that limited evidence without claiming product integration.

### Initial Stage 5 storage and renderer profile

The native `Ludus::SceneCook` library owns the loose-file format and is shared by
the `ludus-cook scene` CLI and the native Editor, which runs it in process on a worker
thread. `assets/catalog.json` maps stable typed UUIDs
(`mesh/<uuid>`, `material/<uuid>`, `texture/<uuid>`, and import/license records) to
immutable `assets/objects/<artifact-key>.bin` files. UUIDs identify logical assets;
they are not content digests. New UUIDs are FoundationIdentity UUIDv4 values from
the entropy source the caller passes in `AssetOptions::EntropyOrNull` (the CLI and
Editor pass `ludus::platform::GetSystemEntropySource()`); without one, the operation
fails with `MissingTools` (see [the UUID architecture](object-handles-uuid.md#implementation-sequence)).
Reimporting the same source origin retains its UUID namespace and explicitly
commits new artifact bindings. Separate source origins
retain separate logical ownership even when immutable byte files can be shared.

Artifact keys include source input digests, importer and publisher code digests,
canonical settings, schema/target profile, output digest and exact required child
artifact keys. Required dependency UUIDs and their artifact keys are recorded
before loading. The scene catalog is limited to 64 MiB/4096 records (see
[limit ownership](content-resources.md#limit-ownership)); an admitted scene
closure has at most 64 records, depth eight, and 64 MiB of payloads. Missing,
corrupt or mismatched dependency revisions reject the complete candidate.

A `.lulevel` document stores a UUID, independent object UUIDs, parent relationships,
affine transforms, mesh/material UUIDs, its authored perspective camera and import
references. Instances share immutable artifacts but retain independent placement.
Source locations are optional provenance in import manifests, not level-file
paths or reopen inputs. Original license text is retained in the required closure.

Import writes immutable artifacts privately, prepares a candidate catalog and
validates an interchange packet on a worker. The published catalog stays unchanged
while the renderer prepares the candidate. After its first rendered frame, a
compare-and-publish operation checks the expected catalog digest and source inputs,
then atomically commits the catalog before activating the document/viewport.
Cancellation, renderer admission failure and source/catalog conflicts retain the
previous document and published bindings. Unreachable immutable files may remain;
release publication includes only the selected closure. Writers use nonblocking
native file locks that release on process exit. File synchronization precedes the atomic replacement; directory synchronization
follows it. Level saves additionally compare the previous saved bytes. CLI imports
stage and sync both catalog and level, then replace them inside one exclusive
publication lock. A replacement/sync failure rolls both files back from staged
backups; readers reject an in-flight writer. A rollback failure reports retained
recovery files instead of claiming preservation. This initial pair publication is
an operation-error rollback boundary, not a crash-atomic two-file filesystem
transaction. Abrupt termination/power-loss recovery remains a separate hardening
step.

`Resources::SceneDefinition::Decode` is the Qt-independent bounded runtime codec.
Version-two interchange retains sampler addressing explicitly; version-one
prototype input remains readable. Scalar fields are little-endian integers/IEEE
binary32, not native C++ layouts. The worker assembles exact mesh/material/texture
revisions from project artifacts; it never calls a source importer on reopen.
The control owner obtains an immutable aggregate scene lease through
`SceneDefinitionAdapter`. This initial fully resident composition validates the
per-asset closure in the storage backend and admits one derived scene binding in
the coordinator; it does not pretend the Stage 4 eight-node profile admits a
nineteen-node source graph. The existing small `MeshDefinitionAdapter` remains
unchanged. Per-asset GPU caching and streaming granularity remain later work.

The renderer adapter copies mesh/texture data, prepares lit material bindings and
owns GPU resources/retirement independently of the CPU lease. The initial
(LUSCN versions 1 to 4) profile admits six meshes, eight materials, eight textures
and 128 instances; combined geometry is limited to 500000 vertices, each mesh to
1500000 indices, and textures to 16 MiB each/32 MiB combined. Version 5 admits the
larger import profile (256 meshes, materials and textures, 1,024 instances,
geometry bounded by encoded bundle bytes and 256 MiB of textures; see
[the import profile](content-resources.md#import-profile-and-alpha-masks-luscn-v5)).
Encoded logical dependency closures admit 512 MiB through SceneCook's
`MAX_RESIDENT_CLOSURE_BYTES`, allowing manifests, material JSON and licenses
beside payloads that already fill a 384 MiB assembled bundle. Single artifacts
remain bounded by `MAX_ASSET_BYTES`; assembly still enforces its version's
bundle limit. This encoded-byte admission policy is separate from decoded
residency and peak-memory accounting below.
A bound decoded aggregate is capped at 512 MiB, above the largest 384 MiB bundle; renderer
reference storage has an explicit 128 MiB budget, with RHI storage accounted in its
separate R6 budgets. Worker scratch, retained candidate/last-good CPU definitions,
Qt display images and driver allocations are not misreported as coordinator-owned
resident bytes. The viewport explicitly uses progressive CPU reference traversal,
256 square pixels, 64 samples/pixel, depth four and seed 871. It displays live
renderer output; no saved image substitutes for loading or rendering.

The first importer is the bounded Modern Hall/staircase2 PBRT profile described in
the [prototype guide](../examples/reference-scene.md#conversion-profile-and-limitations).
Static room geometry is grouped into four material meshes, with shared quad/disk
emitter geometry: six meshes, eight materials, three textures and seventeen
independently placed objects. The source's original PLY shape boundaries are not
editable nodes in this initial grouped profile. Other source profiles, transmission,
mips/LOD/chunks and generalized game-play integration are not implied by this import.

The storage regressions explicitly forbid importer invocation on moved-project
reopen. Native offscreen Qt/Metal tests exercise the actual importer/renderer/save
workflow, remove a private copy of source inputs, move the project and verify
identical persisted object/camera/material references and successful rendering.
A native macOS GUI walkthrough on 2026-10-12 additionally created a level,
imported staircase2, renamed an object and saved it. After closing the Editor,
moving the project and removing its private source copy and build cache, reopening
the saved level restored all seventeen objects, the edited name and live rendering.
The saved level bytes remained unchanged, and reopening recreated neither source
inputs nor the project build cache. Loose-file `scene publish` emits a deterministic
selected catalog, artifacts, licenses, level and runtime interchange packet;
independently chunked packs remain optional.

## Cooking and storage

The [common resource-format roadmap](parsing.md#game-resource-formats) records
existing Audio/Text decoding and proposed PNG, OBJ, glTF/GLB, JPEG, and KTX2
profiles. Source readers belong to domain import tools; type adapters prepare
their validated output. Adding a format does not require the shared coordinator
first, and successful source decoding does not establish GPU readiness or a
shipping cooked format. Keep each profile's expanded-data and dependency budgets
in the resource ledger alongside decoder scratch and retained old revisions.

Use authoring formats in tools and immutable versioned artifacts in releases.
Derived keys include source and build-dependency digests, canonical settings,
importer/schema version, and target profile. Keep an ignored derived-data cache;
runtime must not depend on it. Publish artifacts before an atomic catalog pointer.

Start with loose files through the existing validated Content reader. Later pack
files retain independently addressable records and bounded chunks. Validate byte
offsets, lengths, alignment, endianness, codecs, and expanded sizes before access.
Do not serialize native pointers, STL objects, or C++ structure layouts.

The [pipeline and packed-asset chapters](resource-management-reference-review.md#asset-pipeline)
refine this into three independently inspectable products: source/import settings,
rebuildable cooked artifacts, and release catalogs/pack layout. The cooker emits
runtime dependency tables before loading, so the runtime need not read a chain of
payloads to discover its next I/O. A release build starts from explicit roots and
package-inclusion rules; it reports orphaned assets and duplication rather than
shipping everything from a developer directory.

Cook the data that is expensive to process: layout conversion, texture formats,
mesh preparation, animation compression, and schema decoding as their adapters
arrive. Device-specific creation and final runtime binding remain runtime stages.
A local cache directory is the initial derived-data store; distributed cooking,
remote caches, and databases remain separate scaling work. Cache hits validate the
expected artifact metadata/digest before use. Debug metadata retains source file,
property, importer, and settings provenance without bloating the hot registry.

Packed payloads use versioned field encodings and checked offset/length ranges
into an owned immutable block. Validate alignment and integer conversions before
forming a view; copying/decoding scalar fields avoids assuming a C++ object's
lifetime began by mapping bytes. Offset-based read-only views are the default.
Native pointer patching, vtable restoration, and compiler-layout serialization from
older examples are excluded. Memory mapping is a measured native backend option,
not a promise of zero-copy device upload or a portable browser facility.

Keep storage, decompression, and residency granularities explicit. Pack files can
group adjacent requests physically while the catalog names independent resources;
compression chunks bound decode work without making the entire pack resident.
Where multiple resources share an allocation/page, account for the backing page
and its owners: subresource eviction does not free the page until its remaining
users drain. Cook reports show both requested and actually loaded/retained bytes.

Use recorded cold-start/level/streaming traces to cluster co-used content and batch
reads with bounded overfetch. [Access-based file reordering](resource-management-reference-review.md#file-layout)
motivates this measurement; an optical-disk benchmark does not choose an optimal
NVMe or HTTP chunk size. Test chunk sizes, batching, compression, concurrency,
upload cost, and patch amplification together. The store can later support a
platform I/O/decompression accelerator without changing leases or asset identity.

Future mounts select an immutable catalog snapshot with deterministic explicit
priority. Validate compatible type/schema and dependency closure before activation;
old readers retain old mounts and open file revisions. New package files and their
completion manifest exist before changing the active pointer. The current version-1
catalog remains usable; UUID authoring, aliases for legacy IDs, and additional
kinds require a separate versioned migration with reviewable mapping and rollback.

## Streaming and platform profiles

A game/session policy submits required sets, deadlines, and optional quality
targets. The coordinator schedules work; it does not discover player position or
decide that collision can be missing. Begin with explicit required/interactive/
prefetch queues, promotion when a real requester joins prefetch, FIFO tie breaking,
and bounded fairness. Required parents propagate effective urgency to unfinished
dependencies. Stage byte/count/time budgets limit service and upload work; an
individual oversized stage must be chunked or confined to a loading transition.

For streamed types separate immutable logical content revision from its current
coverage: mip range, mesh LOD, animation segment, or audio playback buffer range.
An Acquire specifies minimum usable coverage and observes the actual result.
Keep required coarse coverage held while independently requesting finer chunks;
trim finer coverage through type-specific rules. Publish new immutable binding
views, or use a backend's proven subresource synchronization protocol. The generic
manager never modifies bytes under an in-flight reader or aliases unretired tiles.
Virtual texturing and meshlet residency are specialized future adapters, not
extra states imposed on every simple sound or definition.

Native audio refill is scheduled by the audio owner from decoder/ring state,
with guaranteed service capacity; the general asset queue cannot starve it behind
bulk texture loads. Stateful playback remains per instance. Memory, read bandwidth,
CPU decode time and upload capacity are distinct contention limits, even when
resident bytes fit.

The [sound pack article](resource-management-reference-review.md#sound-packs)
adds an offline occupancy check and measured prefetch lead time. A spatial policy
can start with `lead distance = maximum travel speed * measured end-to-end load
latency + margin`, including queueing, decode, install and competing work. Separate
load/unload thresholds reduce boundary churn. Trace the tail latency and deadline
misses; this formula cannot guarantee readiness through teleportation or unbounded
storage stalls. Such transitions preload explicit roots or show a loading state.
Shared/global sounds receive session holds instead of spatial triggers.

| Profile | Supported design direction |
| --- | --- |
| Initial native | Validated loose files, owned worker results, existing AudioContent preparation, bounded whole-resource loads |
| Later native streaming | Immutable ranged pack reads, bounded chunk decode/upload, typed coverage, backend completion tickets |
| Browser | Event-loop fetch and polling; single-thread fallback with bounded steps, worker preparation only when deployed capabilities allow it |
| Later browser range streaming | Version-bound range requests; validate status, content range, length, cache revision and total downloaded/retained bytes |

A browser server returning a full object to a range request must not bypass the
download cap: reject it or explicitly admit a bounded full-file path. Cancelled
fetch/decode work retains ownership until terminal acknowledgment. Browser audio
still has the existing bounded encoded-input limitation until a separate streaming
implementation proves otherwise. Optional native accelerators are capability
choices; unsupported platforms use the same semantic pipeline with simpler I/O.

## Stop and failure behavior

Stopping closes normal admission, closes child scopes, cancels unneeded operations,
drains owned results, retires resident versions, waits through nonblocking owner
polling for workers/device/audio acknowledgments, and then releases catalogs,
stores and adapter state. Keep provider objects and code loaded through their last
cleanup callback. GameHost code reload cannot unregister an adapter still used by
an operation; initial adapters are statically linked.

Outstanding external scopes/leases are a named stop blocker, never forcibly
invalidated while borrowed memory can still be read. A cancellation deadline is
diagnostic, not evidence of quiescence. If a backend cannot prove retirement, use
the [module lifecycle](module-lifecycle.md) Unsafe/host-restart policy and retain
the uncertain owners. Repeated Start/Stop receives fresh runtime owner identity.
Shutdown progress cannot depend on a logging allocation or a normal-work queue slot.

Return distinct statuses for NotFound, WrongType, UnsupportedVariant, InvalidData,
InvalidState, InvalidHandle, DependencyFailed, Cycle, LimitExceeded, QueueFull,
BudgetExceeded, OutOfMemory, IoError, Conflict, Cancelled, StaleRequest and DeviceLost.
Keep Pending/Deferred distinct from success and failure. Malformed external data is
an ordinary error; assertions diagnose engine invariants. Avoid hidden container growth,
exception-reporting decoders, and allocating diagnostics on failure paths.

## Debugging and inspection

Provide a bounded inspector snapshot answering what an asset is, which revision
is current, who holds each resident version, why loading is waiting, where budget
is spent, and which acknowledgment prevents retirement. Preserve source IDs,
property paths, dependency chains, request provenance, and phase timings. Trace
operations through one correlation identity; observability failure never changes
resource correctness.

Every version shows client scopes, unclaimed Ready requests, parent versions,
worker stage, use tickets, cache policy, allocation domains, and retire blockers.
Every operation shows queued/start/end timestamps, bytes, desired and actual
coverage, catalog/target/device epoch, last failure and retry generation. A numeric
counter alone cannot explain a leak: retain bounded provenance for each hold class
and indicate diagnostic truncation. Release diagnostics cannot depend on debug-only
generation validation. Shipping retains small status/counters; richer tracing is
opt-in and privately implemented.

Provide tool queries equivalent to `Explain(asset)`, `WhyResident(version)`,
`WhyPending(request)`, `Dependencies(asset)`, `Budget(domain)`, and
`Trace(operation)`. For example, an inspector should explain: a forest texture's
old version occupies 8 MiB because a material version still holds it and graphics
queue serial 421 has not completed; its replacement is awaiting 2 MiB of staging
capacity. Names and dependency property paths make this actionable.

Emit bounded events for request, cache hit, stage change, dependency failure,
publication, eviction, and actual free. Keep failure diagnostics in durable result
records even if trace events drop. Include a tools-only artifact/catalog dumper
and dependency/memory report so a bad cook can be inspected without running the game.
Support controlled fault injection for reads, allocation, queues, decode, install,
lost device, and delayed acknowledgments, plus replay of completion schedules.

## Implementation sequence and validation

Implement in stages: preserve existing Content/AudioContent behavior; add explicit
registry/request/lease contracts; add one additional type; then cooking, measured
cache policy, and measured streaming. Test stale completions, concurrent waiters,
partial failures, reload overlap, dependency lifetime, queue exhaustion, and
retirement. Benchmark cold and warm loads, frame impact, memory peaks, and cache
churn before claiming optimization.

Implement and validate this initial architecture before evaluating the additional
conference and journal sources in the
[follow-up research plan](resource-management-reference-review.md#research-after-the-initial-implementation).
Capture a reproducible baseline first, then review and test focused improvements
against it. The shortlist adds no prerequisites to the initial implementation;
its hypotheses do not change the identity, lifetime, or publication contracts.

Implementation files must carry nearby reference comments thanking consulted
authors, identifying the exact work and relevant chapter/pages, and explaining
the adopted idea and significant departures. Include stable URLs when available
and link to the [detailed review](resource-management-reference-review.md).
The review describes design inspiration; it does not supply copied implementation.

| Stage | Deliverable and exit condition |
| --- | --- |
| 1 (implemented) | Audit existing AudioContent request/lease/session boundaries, joining cancellation, and ownership provenance; preserve version-1 fixtures and its cache/retirement behavior. Establish a tested nonblocking integration boundary before exposing the new runtime API. |
| 2 (implemented) | Fixed-capacity coordinator, one owner, exact-ID lookup, scoped requests, deduplicated operations, lease tokens, durable result records, and explicit budgets. Demonstrate two callers acquiring one sound with independent cancellation. |
| 3 (implemented) | Immutable CPU mesh definitions demonstrate a second consumer, bounded staged copies, immediate CPU retirement, mixed native sound/mesh acquisition and an installed-SDK renderer handoff. Renderer/RHI separately own copied GPU data; no graphics-specific branch enters the coordinator. One-pending AudioContent limits remain. |
| 4 | Implemented: transactional replacement and required DAG scheduling. Prove diamond sharing, parent/child replacement coherence, last-good retention, and physical retirement. |
| 5 (initial resident profile implemented) | Project-owned cooked assets/catalogs, dependency manifests, exact artifact keys, dump tooling and deterministic loose-file publication. Complete the [project scene import and saved-level workflow](#project-scene-import-and-saved-level-workflow), including level references and renderer/editor adapters. Reopen a moved saved project without source reimport. Independently chunked packs remain optional; measure against loose files. |
| 6 | Add actual mip/LOD/chunk streaming after RHI capabilities exist; implement trace-driven layout, deadlines, and alternative cache policy only with representative evidence. |

Do not introduce remote caches, custom lock-free tables, universal reflection,
dynamic importer plugins, resource inheritance, a global singleton, per-draw
shared ownership, universal pointer patching, or a general VFS as stage-2
prerequisites. Their absence keeps the control flow readable; narrowly scoped
extensions retain the same explicit identity and lifetime contracts.

Required regressions target behavior:

- Two equal requests cause one prepare/install; one cancellation leaves the other
  successful. Cancelling the final waiter still drains late work safely.
- Forged/wrong-owner/stale handles, duplicate lease release, request-slot reuse,
  generation exhaustion, counter overflow and repeated initialization cannot
  refer to a different incarnation, in both Debug and Release.
- A diamond dependency loads its leaf once; a failed child never marks its parent
  Ready. Hard cycles fail with a named path; legal soft cycles traverse finitely.
- Failed candidate cook/install/publication and out-of-order edits retain the last
  good set. A worker reading an old version and a submitted GPU/audio use survive
  replacement and hold its exact dependency closure until acknowledgment.
- Queue saturation still permits cancel/release/stop and never loses accepted
  results. No worker is blocked waiting for another resource job. Domain admission
  and reservation conversion remain correct across shared work and failure.
- Required coverage, texture/skeleton layout mismatch, DeviceLost, scoped preview,
  and teleport fallback exercise the declared policy instead of hidden success.
- Corrupt packed offsets/counts/expanded sizes, wrong codecs/versions, symlink
  escapes, changed open-source revisions, partial packages, invalid HTTP range
  responses, and source-edit conflicts fail without changing active bindings.
- Shutdown during every stage proves no callback/work/use can reach freed state;
  an unprovable backend drain reports Unsafe rather than freeing it.

Build implementations warning-clean with the pinned toolchains; run unit tests,
ASan/UBSan, format/tidy, public-header/include gates, SDK consumers and applicable
native/browser acceptance. Use TSan for new concurrency paths where supported.
When a change creates or updates a consumer project, verify its actual selectable
configure/build/test presets and setup using the AGENTS.md workflow.

Benchmark a cold startup, warm startup, two levels sharing a large closure,
editor replacement under memory pressure, a working set larger than the cache,
many tiny assets, one oversized asset, and audio refill competing with bulk I/O.
Report platform/build, asset count and bytes, cache/OS-cache conditions, warmup,
distribution of load/stage latencies, queue wait, deadline misses, CPU/frame impact,
actual peak memory by domain, upload cost, cache-hit/churn/overfetch and retained
retirement bytes. Compare the simple baseline against each proposed optimization
on the same captured workload. Capacity arithmetic is testable; latency promises
require platform measurements. This documentation change claims neither.
