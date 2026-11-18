# Viewport input pacing baseline

November 18, 2026. Scope: the native-viewport acceptance case
"Continuous camera input does not postpone viewport frames" in
`apps/editor/tests/viewport_navigation_tests.cpp` (issue #1338). These samples set
that case's thresholds. They describe one shared Mac and are not a display-latency
or scanout guarantee. The current acceptance rule is in
[native viewport scheduling acceptance](continuous-integration.md#native-viewport-scheduling-acceptance).

## Method

- **Observation point.** `PreviewViewport::ImagePresented` fires when the native
  viewport hands Qt a new image, from Qt Quick `updatePaintNode` (or from the
  readback canvas on the offscreen path). The point precedes Qt's draw and display
  scanout. Each tick is a `foundation::time::NowTicks()` reading in nanoseconds.
  In every recorded window, the number of ticks equals the `PresentedFrames()`
  advance.
- **Windows.** After startup and one unrecorded 1 s warm-up, the case samples ten
  1 s windows. Idle and input windows alternate, starting with idle. During input
  windows, a 2 ms precise timer calls `RenderPreview::Look`. Each window records its
  begin and end ticks, the camera updates it delivered, and every new-image tick
  inside it. Its longest gap includes the open gaps from the window start to the
  first image and from the last image to the window end. A gap that crosses a
  window boundary therefore appears as a lower bound. Each window also records
  whether the native window was exposed and the application active.
- **Configurations.** `macos-clang-debug` and `macos-clang-asan-ubsan` were built
  on develop `46f03beb7` with this change. The last green candidate before the
  failure (`61064d344`) was built under the sanitizer preset, with the same
  instrumentation applied as a local patch. Its viewport has no Qt signals, so a
  callback was placed at the same observation point.
- **Host.** Apple M5, macOS 26.6, 60 Hz built-in display. Other builds ran on the
  host throughout, at 1-minute load averages of 25–43 on 10 cores. In the
  **loaded** rows, eight extra `yes` processes added CPU contention. The three
  configurations ran round-robin so that each round shared host conditions.
- **Raw data.** [windows.csv](viewport-pacing-evidence/windows.csv) holds one row
  per window: configuration, run, exit status, load average, load workers, window
  bounds, input count, image count, the `PresentedFrames()` delta, exposure and
  activity, summaries, the first-image offset and every interval in microseconds.

## Results

The table counts only runs whose window stayed exposed for all ten windows.
Covering the window stops presentation on macOS (zero images), so the unexposed
runs are kept in the CSV but excluded from the summary. Image counts are
min/median/max per window. Interval columns are median/p95/max in milliseconds,
pooled over all windows of a kind. The ratio column gives the lowest per-run
ratio of input median images to idle median images. The last column counts runs
that the former 0.75 single-window ratio would have failed.

| Configuration | Runs | Exposed | Load | Idle images | Input images | Inputs/window | Idle intervals | Input intervals | Lowest ratio | Fail at 0.75 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| debug, develop | 12 | 10 | 26–41 | 32/60/61 | 33/58/61 | 179–493 | 16.7/17.7/217.7 | 16.7/33.3/104.7 | 0.90 | 0 |
| ASan, develop | 12 | 9 | 25–43 | 54/60/61 | 25/55/61 | 136–449 | 16.7/17.7/116.7 | 16.7/35.1/239.4 | 0.75 | 0 |
| ASan, 61064d34 | 12 | 10 | 25–40 | 57/60/61 | 32/57.5/61 | 119–488 | 16.7/17.7/50.6 | 16.7/34.7/202.8 | 0.75 | 0 |
| debug, develop, loaded | 10 | 4 | 32–54 | 56/59/60 | 32/38/50 | 156–261 | 16.7/17.7/50.6 | 16.7/61.1/117.3 | 0.64 | 3 |
| ASan, develop, loaded | 10 | 8 | 32–59 | 49/58/61 | 21/35.5/55 | 89–343 | 16.7/17.7/150.8 | 16.7/66.8/185.0 | 0.53 | 6 |
| ASan, 61064d34, loaded | 10 | 5 | 33–57 | 56/59/61 | 29/39/59 | 128–394 | 16.7/17.7/51.0 | 16.7/56.5/146.1 | 0.59 | 4 |

Findings:

- Under CPU contention, sustained input lowers new-image throughput to 0.53–0.65
  of the idle rate. Idle presentation stays near the display rate. Develop and
  the last green candidate behave alike. The batch failure (28 input images
  against 48 idle, ratio 0.58) falls inside this range, so host contention
  explains it without a scheduling regression.
- The idle cadence holds at the 16.7 ms display interval with p95 17.7 ms. Input
  raises the p95 interval to 33–35 ms on the busy host and to 57–67 ms under
  extra load. The median longest gap of a run's input windows never exceeded
  112 ms. Single windows reached 296 ms.
- The former progress check (more than two images per second) would accept a
  regression to 3 images per second. The former 0.75 ratio rejects 13 of the 17
  exposed loaded runs.

## Chosen criterion

A run passes when all of the following hold:

1. Every input window delivered more than 20 camera updates and presented more
   than two new images.
2. If the idle median clears 20 images, the input median keeps at least one
   third of the idle median. The lowest observed ratio, 0.53, leaves a margin
   of 1.6×.
3. The median longest gap of the input windows stays within the larger of
   250 ms and four times the idle median longest gap. The worst observed value
   is 112 ms, 0.45 of the limit.

Replayed over every exposed run in the CSV, the criterion fails none. The
deterministic cases in `apps/editor/tests/viewport_pacing_tests.cpp` show that it
rejects input limited to 3 images per second (ratio and gap both fail). They
also show it rejects a controlled delayed presentation: each input window keeps
the idle image count but holds half of it back behind a 400 ms stall (gap check).
Using medians over five windows of each kind tolerates a single slow window. A
regression that affects every input window still fails.
