# Multi-worker scene-import extra-CPU attribution (read-only analysis)

Status: **static / read-only.** No profile was captured — the analysis host is Linux
x86_64 (Amazon Linux 2023, Clang 15) with no macOS `sample`/Instruments, no reference
scenes (`~/Ludus/references/resources`), and no timing harness. The task's "measure
first" step and all `sample`/Instruments evidence must still be gathered on the macOS
host. What follows is a code-grounded hypothesis that identifies *where* the extra CPU is
structurally spent and *why*, with file:line citations, so the Mac profile has concrete
predictions to confirm or refute.

All line numbers are against `origin/develop` at `cd9909df`.

## TL;DR

The dominant structural source of extra CPU at 4 workers is **per-batch
submit/wait/wake churn on a single shared gate, with no cross-batch pipelining**. The
texture decode queue reads a whole batch on the owner, submits it, then **blocks the owner
in `JobSystem::Wait` until the slowest decode in that batch finishes** before the next
batch's file reads begin. Between batches every worker is woken by a `pthread_cond_broadcast`,
contends for one mutex, finds little or no work, and re-parks — a thundering-herd that is
charged as CPU on every worker and repeats once per batch for hundreds of batches.

Ranked suspects:

1. **TextureDecodeQueue batch barrier + owner-serial work between batches — PRIMARY.**
2. **JobSystem wake/park model (single gate, broadcast-to-all) — amplifier.**
3. **ParallelTasks — same submit/wait shape, secondary, bounded by band count.**
4. **stb bounded allocator — ruled out as a contention source.**

---

## 1. TextureDecodeQueue batch barrier (PRIMARY)

`modules/content/scene_cook/src/importers/texture_decode_queue.cpp`

Consumer: `CookTextures` in `best_effort_import.cpp:1053-1071` constructs one
`TextureDecodeQueue` and then walks planned materials **one at a time**, calling
`decodes.Take(planned.Texture, decode)` in sequence (line 1071). Each `Take` that crosses a
batch boundary drives `takeDecode` -> `fillBatch`.

`fillBatch` (texture_decode_queue.cpp:181-210) does, per batch:

1. Reads **every** file in the batch on the owner thread
   (`readSlot` loop, lines 192-202) — I/O stays off job callbacks, which is correct per
   `docs/architecture/threading.md`, but it is a serial section with all workers idle.
2. `decodeBatchInJobs(count)` (line 204) submits the batch and **blocks**:
   `mJobsOrNull->Wait(outcome)` at line 257. The owner does not return from `Take` until the
   **slowest** decode in the batch completes.
3. Only after `Wait` returns does the consumer in `CookTextures` run the per-texture
   owner-side work — `ConvertTexels`, `ApplyAlphaMask`, swatch creation
   (best_effort_import.cpp:1101, 1158, 1166) — again with all workers parked, before the
   next `Take` starts the next batch's reads.

Batch size is `min(workers + 1, MAX_TEXTURE_DECODE_BATCH=8)` (lines 42-44). At 4 workers a
batch is 5 images. So for N planned textures there are ~N/5 batches, and **between every
batch** the pipeline fully drains: workers go idle during (1) the owner's reads and (3) the
owner's post-decode work, and the owner goes idle during (2) waiting on the slowest decode.

Why this shows up as CPU, not just wall time:

- Each batch is a submit + broadcast + N-worker wake + N-worker re-park cycle (see §2). That
  wake/contend/re-park overhead is CPU spent by workers that did little or no useful work in
  that batch, and it recurs once per batch.
- The slowest-decode barrier means the batch's wall time is `max(decode_i)` while its useful
  CPU is `sum(decode_i)`; the gap (workers finishing early and parking) is idle, but the
  **re-wake for the next batch** pays the herd cost again. With image sizes varying widely in
  Sponza, `max` >> `mean`, so many workers idle-then-rewake repeatedly.

Prediction for the Mac profile: a large fraction of worker CPU in
`pthread_cond_wait`/`pthread_cond_broadcast`/mutex lock wakeups and in `JobSystem::Wait` ->
`ExecuteOneJob` misses, correlated with batch count (~N_textures/5), **not** in
`DecodeImage`/stb. The owner thread should show serial time in `readSlot`
(`detail::ReadFileBounded`) and in `ConvertTexels`/`ApplyAlphaMask` with workers idle.

## 2. JobSystem wake/park model (AMPLIFIER)

`modules/foundation/threading/src/jobs.cpp`, `.../src/native_posix.cpp`

Verified mechanism (no spin, no futex, no work-stealing — a single mutex + condvar gate):

- Owner `Wait` loop, jobs.cpp:714-719: help-run one ready job under the gate, otherwise
  `system.Gate.Wait()` == `pthread_cond_wait`. A ready-queue miss goes straight to a blocking
  wait (no spin/yield).
- `Submit` wakes workers with `system.Gate.WakeAll()` (jobs.cpp:673) ==
  `pthread_cond_broadcast` (native_posix.cpp). `CompleteJob` also broadcasts when work becomes
  ready or the graph finishes (jobs.cpp:~188).
- Worker loop (jobs.cpp:~299-304) is the same shape: run a ready job or `pthread_cond_wait`.
- Single shared ready ring under one gate; every `ExecuteOneJob` and `CompleteJob` takes that
  gate.

Consequence for small, frequent batches: each Submit broadcasts to **all** `WorkerCount`
workers regardless of how many jobs (5) became ready. Woken workers all contend for the one
gate (lock convoy), most find `ReadyCount==0` after a few pops, and re-park. Multiply by
hundreds of batches. This is the amplifier that turns the §1 batch barrier into measurable
CPU rather than mere latency. It is a pre-existing engine design choice; the cheapest fix is
to **submit fewer, larger graphs** (i.e. fix §1) rather than change the scheduler.

## 3. ParallelTasks (SECONDARY)

`modules/content/scene_cook/src/importers/parallel_tasks.cpp`

Same submit-then-`Wait` shape (`submitTasks`, lines 61-101; `Wait` at line 95). Used for
surface-map banding and halving via `GetImageBandCount` = `min((workers+1) *
IMAGE_BANDS_PER_THREAD, MAX_PARALLEL_TASKS)` clamped to rows. Each `ParallelTasks::Run` is one
submit/wait/wake cycle. The number of such cycles is bounded by the number of surface maps and
halving passes, which is far smaller than the texture-decode batch count, so this is a smaller
contributor — but it shares the same per-submit herd cost from §2. It is correctly serial when
`jobsOrNull == nullptr` or `count <= 1` (lines 22-29).

## 4. stb bounded allocator (RULED OUT)

`third_party/stb/stb_image_allocator.c`

The live-byte cap is a `_Thread_local` counter (`ludus_stbi_live_bytes`) and the allocator
delegates to plain `malloc`/`realloc`/`free`. There is **no shared global lock or custom
arena**, and blocks are freed on the allocating thread. It therefore adds no cross-thread
contention of its own; any allocator cost under parallel decode is whatever the system
`malloc` does, which is not specific to this code. Not a plausible source of the ~18 s gap.

---

## Implemented fix (explicitly UNVERIFIED — must be built/profiled on macOS before merge)

`texture_decode_queue.cpp` now **pipelines the next sequential batch's file reads with the
current batch's in-flight decode jobs**. `fillBatch` was split into `readBatch` /
`submitBatchJobs` / `prefetchNextBatch` / `waitBatchJobs` / `finishBatch`: after submitting
the current batch's decodes and **before** blocking in `Wait`, the owner reads the next
sequential batch (`mBatchEnd`) into a second slot buffer (`mPrefetchSlots`). The next
`fillBatch` for that sequential `first` swaps the prefetched buffer in instead of re-reading.
This removes the owner's idle time in `Wait` (it now does useful I/O there) and removes the
"workers idle while the owner reads the next batch" gap, so the submit/wake herd cost of §2
is paid while the owner is also busy rather than back-to-back with an idle stall.

Byte-identity argument (why outputs cannot change):

- `GetFileReads` is populated only in `Take`, from the *taken* decode — never by the
  read-ahead. Prefetching moves only *when* bytes are read, not *which* decode is taken or
  recorded.
- Decode is a pure function of the slot's bytes; jobs still touch only their own slot. The
  read-ahead writes a disjoint buffer (`mPrefetchSlots`), never the in-flight `mSlots`.
- A plan skip past `mBatchEnd` (the `found != mPrefetchFirst` case) drops the prefetch and
  reads the new `first` itself — identical to the old single-buffer path.
- Cancellation is still polled exactly once per `Take` (in `takeDecode`); the read-ahead does
  no cancellation polling and no recording, so poll counts are unchanged.
- The busy / refused-graph path (`submitBatchJobs` returns false) skips the read-ahead and
  falls back to inline decode in `finishBatch`, as before.

Known tradeoff a reviewer must weigh on real scenes: the read-ahead holds **one extra batch
of encoded source bytes** (bounded by `MAX_SOURCE_IMAGE_BYTES` per slot). This raises peak
memory versus the single-buffer design. It must be checked against Main Sponza's real image
sizes on the Mac.

Tests: `texture_decode_tests.cpp` gains
`"Texture decode queue read-ahead matches inline decoding across many batches"`, which forces
many sequential batch boundaries (exercising prefetch-then-reuse) interleaved with a forward
skip and a backward revisit (exercising prefetch-then-drop), and asserts identical decodes,
file-read order and digests against the `nullptr` serial oracle at 0/1/3/7 workers. The
existing `RequireWorkerCountIndependence` suites (0/1/3/7 vs `nullptr`) remain the primary
byte-identity gate.

## What still MUST be done on the macOS host (not possible in this analysis environment)

1. Build `macos-clang-debug` and `macos-clang-asan-ubsan`; run the scene_cook suites.
2. `./scripts/check --format`, `./scripts/check --tidy-changed --base origin/develop`,
   `python3 scripts/python/source_policy.py --base origin/develop`.
3. `scripts/check-docs`, a strict MkDocs build, `scripts/check-wiki --documentation-only`
   for this doc.
4. Capture the `sample`/Instruments profile to CONFIRM the §1 attribution is the dominant
   cost (and that the fix moves it), then the import-timing-harness before/after wall + CPU
   medians and ranges on Main Sponza at 4 workers and serial.
5. `ludus-cook` byte-identity: `scene.luscn` + `import-report.json` from the develop binary
   vs the new binary, on Main Sponza and the pbrt scenes (Bistro entry `bistro_cafe.pbrt`),
   at 0, 1 and 4 workers.
