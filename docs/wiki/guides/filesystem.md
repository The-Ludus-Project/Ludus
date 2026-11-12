# Read and publish files through the filesystem

Link `Ludus::FoundationFilesystem` and include the API you use: `filesystem.hpp`
for native directories/files, `namespace.hpp` for logical roots and mounts,
`pack.hpp` for shipping packs, `async.hpp` for bounded background reads,
`persistence.hpp` for atomic publication, and `watch.hpp` for cooked-file hints.
Native directory reads work on Linux, macOS and Windows. Memory and pack
providers work on every target. AsyncReader has a dedicated blocking pool on
Linux, macOS and Windows. Browser initialization returns Unsupported. Browser hosts use
synchronous memory/pack reads and own any fetch adapter themselves.

## Publish a read namespace

Create a directory provider for your trusted authoring root, or copy entries
into a memory provider. For shipping, run `scripts/pack SOURCE OUTPUT.pack` and
create a pack provider using the directory provider that holds the archive.
Create a `MountSnapshot` with explicit Root, provider, priority and stable nonzero
mount ID, then resolve a validated `VirtualPath` through `OpenRead`. Higher
priority wins; the smaller mount ID breaks ties. Only NotFound falls back to the
next mount. Pack corruption or access errors remain visible.

Use a new snapshot generation when publishing a new mount table. Opened files
retain their prior provider revision after unmount or reload. Publish replacement
asset handles in Content at your controlled synchronization boundary. Filesystem
reads bytes; it does not choose resource identity or replace live game objects.

## Submit bounded background reads

Initialize an `AsyncReader` during host setup with an explicit worker count,
request capacity and byte budget. Use its own workers for blocking storage;
ordinary job callbacks must not block on I/O. Submit an already-open `VirtualFile`
with an offset and a caller-owned destination. The scheduler retains that file
without reopening its pathname. Keep the destination alive and exclusively
writable until Poll returns its terminal completion. Live destinations must not
overlap. File ownership can be released immediately after successful admission.

For example, host-owned storage can persist across frame boundaries:

```cpp
#include <ludus/foundation/filesystem/async.hpp>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;

struct AssetRead
{
    uint8 Bytes[4096]{}; // Storage outlives Reader, including destructor drain.
    RequestHandle Request;
    AsyncReader Reader;
};

// Host setup, with explicit overload and initialization handling.
AsyncStatus BeginRead(AssetRead& pending, const VirtualFile& file) noexcept
{
    const auto initialized = pending.Reader.Initialize({32, 2, 1024 * 1024, 5'000'000, 0});
    if (initialized != AsyncStatus::Ok) { return initialized; }
    return pending.Reader.Submit(file, 0, pending.Bytes, pending.Request);
}

// Call from your host update loop outside render/audio critical sections.
// True means the destination can now be reused, even when the read failed.
bool FinishRead(AssetRead& pending, ReadCompletion& completion) noexcept
{
    return pending.Reader.Poll(completion);
}
```

Poll delivers each accepted completion once. For `ReadDisposition::Read`, inspect
`completion.Read.Outcome` and `BytesRead`; successful EOF can mean fewer bytes than
the destination length. Changed invalidates potentially touched bytes. For
Cancelled, discard the buffer: a running read may already have written it.
DeadlineExpired means I/O never started. Rejected Submit calls preserve both
output handle and destination, and produce no completion.

CapacityExceeded includes completions you have not collected. ByteBudgetExceeded
charges full destination sizes through collection, even for reads at EOF. Poll
regularly and handle overload by deferring admission or choosing synchronous I/O
where measured small cached reads justify it. File releases during collection can
close a final provider revision, so polling belongs outside critical sections.

## Cancellation, deadlines and shutdown

Cancel records intent; it never permits immediate buffer reuse. A queued read
skips I/O. An active blocking read must return before its Cancelled completion can
be collected. Cancellation after terminal publication keeps the read result.
Stale handles cannot cancel a subsequent request that reuses the slot.

ReadOptions can carry a Tag, priority and an absolute latest-start deadline in
AsyncNow nanoseconds. Earlier deadlines, then higher priority, then FIFO select
work until the oldest request reaches the starvation threshold. Age then wins.
Deadlines do not interrupt active reads and are not a hard completion guarantee.

On shutdown, stop admission with Shutdown, collect all remaining completions,
then destroy the reader and destinations. Shutdown drains active reads and can
block until a provider returns. A provider calling Shutdown on its own reader
receives InsideRead without stopping admission. Keep destruction with the host,
outside active provider reads. Shutdown cancels queued work and preserves all terminal
records for the host. The destructor also drains, but abandons records you did not
collect. Initialize/destruction require exclusive owner access; other operations
are concurrent while the reader lives. Serialize Shutdown calls with each other.

## Inspect metrics and traces

Metrics reports current queue, active reads, delivery backlog, byte/request peaks,
rejections and terminal counts. ReadCompletion records queue/service/delivery and
cancellation latency in nanoseconds. Enable a finite TraceCapacity to collect
ReadTrace records via PollTrace; provide a valid VirtualPath in TracePath when
logical names are useful. The key is copied for diagnostics and does not resolve
the file. Traces include mount/generation, offset, requested bytes and native
errors. Overflow discards oldest records and increments DroppedTraces. No logging
or callbacks run inside the scheduler.

These budgets cover destinations and scheduler records. Set resource residency,
pack decode scratch and GPU upload budgets separately. Select a write capability
explicitly when publication is required.

## Publish through an explicit write root

Open a `WriteDirectory` with the native root selected by your host. Supply a
validated relative path, a complete byte buffer and `WriteOptions`; parent
directories must already exist. For optimistic admission, select Missing or
MatchingStamp (obtain metadata with `Directory::Observe`). These are cooperative
checks, so retain Content digest validation when stronger integrity is required.

```cpp
#include <ludus/foundation/filesystem/persistence.hpp>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;

PublicationResult SaveCooked(const WriteDirectory& root,
                             std::span<const uint8> bytes) noexcept
{
    WriteOptions options;
    options.MaxBytes = 8 * 1024 * 1024;
    options.Sync = SyncPolicy::FileAndDirectory;
    return root.Publish("cooked/model.bin", bytes, options);
}
```

Inspect Published independently from Outcome: a directory-sync failure can occur
after the new file is visible. FileSynced/DirectorySynced report native sync
acknowledgments; Cleanup reports temporary or close failures. Handle those
results at the host, including recovery when cleanup leaves a temporary. The
[architecture contract](../../architecture/filesystem.md#f5-persistence-and-cooked-file-hints)
defines cooperative locking, platform support and sync limitations.

## Observe cooked-path hints and recover overflow

Initialize a `Watcher` with fixed MaxPaths, HintCapacity and debounce nanoseconds.
Register final cooked paths from your catalog with Add, keeping the returned
handles. On its owner thread, call Advance with your monotonic timestamp and a
bounded slot budget, then Poll hints. Treat Changed as a prompt to reopen and
validate bytes; it never selects a new reader revision. Require atomic publication
from producers and perform Content rebinding at your host boundary.

When NeedsRescan becomes true (or Poll returns RescanRequired), call BeginRescan.
Across host updates, call NextRescan with a bounded number of calls. Revalidate
each RescanEntry, including unchanged paths. A successful empty poll completes the
pass. If NeedsRescan remains true, inspect per-entry errors and retry after
recovery. While rescanning, registration mutation and incremental polling are
refused. Update registrations explicitly when the catalog gains or loses paths.
Polling misses transient/metadata-preserving edits and is not a completion or
integrity guarantee. See the architecture link above for the full contract.

## Continue improving the completed baseline

F1-F5 provide the current filesystem contracts. Use the architecture's
[post-baseline research and improvement plan](../../architecture/filesystem.md#post-baseline-research-and-improvement-plan)
to select a scoped follow-up: publication recovery, representative streaming,
conditional backend/pack optimization, or Content hint recovery. That page owns
the references, priorities and acceptance criteria. Record evaluated experiments
in the [filesystem measurements](../../development/filesystem-benchmark.md).

For proposed pipeline and platform backend work, see the
[asynchronous I/O evolution decision](../../architecture/filesystem.md#asynchronous-io-evolution-decision).
It preserves the current SDK contracts and records the Gems review and adoption gates.
