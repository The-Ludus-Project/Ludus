# Runtime UI validation

Implementation: U0, `Ludus::Ui`. Full renderer/text/IME/accessibility and authoring
remain U1-U4; see [architecture](ui.md). Validation uses isolated clones so it
does not modify concurrent user work in either checkout.

## CPU measurement

A host microbenchmark submits 16, 128 and 1024 solid buttons, resolves a 1280 x
1536 logical viewport, then routes focus-next, activation, pointer-down and
pointer-up. It warms up 100 iterations and records 2000 samples. Clang 18.1.3,
C++23, Development core (`-O2`, assertions enabled), ordinary Linux host under
concurrent build load; no target hardware or total-frame claim.

| Elements | Median | p95 |
| --- | --- | --- |
| 16 | 1.73 us | 2.35 us |
| 128 | 13.53 us | 16.23 us |
| 1024 | 110.00 us | 135.53 us |

The source is `modules/ui/tests/benchmark.cpp`. Reproduce against an installed
Development SDK (paths below are local build outputs, not committed presets):

```sh
out/host-tools/bin/clang++ -std=c++23 -O2 \
  -I out/install/linux-clang-development/include \
  modules/ui/tests/benchmark.cpp \
  out/install/linux-clang-development/lib/libludus_ui.a \
  out/install/linux-clang-development/lib/libludus_foundation_containers.a \
  out/install/linux-clang-development/lib/libludus_foundation_base.a \
  -ldl -pthread -o /tmp/ludus-ui-benchmark
/tmp/ludus-ui-benchmark
```

These figures
measure CPU resolution plus four events, not font shaping, GPU composition,
script execution or platform input. Workload contains no text, image or modal
controls. Do not extrapolate to AAA screens.

The separate allocation regression verifies zero new/delete calls across 1000
64-element document/event iterations after initialization. It also injects
failure at each of ten initialization allocations and verifies successful retry.
Sanitizer builds run the ordinary behavior tests; replacement allocators are
kept in a separate non-sanitized test executable.

## Verification record

Local verification with the pinned toolchains:

- Warning-clean Development engine build and web Development build, including
  browser smoke apps and the installed Ui archive/header.
- Full Development CTest: 49 tests, zero failures; two live Wayland tests skipped
  because there is no compositor. Debug Ui behavior/allocation tests passed.
- Ui ASan/UBSan behavior tests passed, including LeakSanitizer. The host sandbox
  prohibits ptrace and Unix-socket IPC, so these and the existing IPC tests ran
  outside it. No physical GPU/runtime-native window acceptance is claimed.
- Project formatting and foundational include checks passed. Full clang-tidy
  analysis found one swappable test-helper parameter pair; after using a typed
  parent index, the affected translation unit passed re-analysis. All other
  translation units passed the full analysis run.
- Installed native SDK consumer compiled, linked and ran. Native and web
  consumer configure checks accepted Ui, tolerated an unknown optional
  component and rejected an unknown required component with a precise message.
- Sandbox native build and all four CTests passed. Its repaired local setup
  exposed selectable configure/build/test presets and configured native and
  both web profiles. Web Development rendered matching HUD pixels on WebGPU
  and WebGL 2 at 960 x 540 (1x) and 390 x 844 (2x); screenshots were inspected.

The PR checks supply full Debug/sanitizer, optional-editor, PCH, build-budget
and packaged Release browser validation on CI. Browser screenshots exercise
Chromium SwiftShader; software GPU checks do not establish physical GPU/mobile
behavior or interactive frame-rate performance.

## U1 render adapter

Scope: the [UiRender contract](ui.md#u1-render-adapter) and the issue #1219
clip/hit qualification. UiText, atlas exhaustion, device loss and the text
sweeps are separate U1 work and are not covered here.

The qualification document has a gray decoration panel at logical (4, 4) with
size 30.25 x 20 that clips its children, so its right clip edge is 34.25. That
edge lands on the centre of physical pixel 68 at scale 2. Button A is the
panel's child at (20, 8) with size 20 x 10, so it overflows the clip. Button B
is a root sibling at (24, 12) with size 8 x 16, painted after A, overlapping A
inside the clip and extending below the panel. Button C is a 6 x 6 root button
anchored bottom-right. Paint produces four quads in three runs: panel (viewport
scissor), A (panel scissor), then B and C (viewport scissor). B's run therefore
follows a scissor change.

| Case | Scale | Physical extent | Logical viewport |
| --- | --- | --- | --- |
| 1 | 1 | 48 x 32 | 48 x 32 |
| 2 | 2 | 96 x 64 | 48 x 32 |
| 3 (resize) | 2 | 80 x 60 | 40 x 30 |

Each case checks the colour of every pixel against the snapped paint list
(one byte of tolerance, opaque fills) and routes a press at every pixel centre
through `Ui::Route`. The activated button must be the topmost snapped button at
that pixel. Six logical probes name the acceptance points: inside the clip, the
last pixel inside the half-open edge, the edge itself, the overflow outside the
clip, the sibling over A, and the sibling below the panel.

### Recorded results

| Backend | Environment | Cases | Result |
| --- | --- | --- | --- |
| Metal (`Backend::Metal`) | Apple silicon Mac, installed Debug SDK, offscreen RGBA8 target | 1, 2, 3 | Every case: 4 quads in 3 runs, 0 colour, 0 hit and 0 probe mismatches (2026-11-19) |
| Vulkan | Linux SDK job in batch CI | 1, 2, 3 | Pending: runs in the Linux renderer SDK step |
| WebGPU, WebGL 2 | Pinned Chromium SwiftShader, renderer-baseline browser suite | 1, 2, 3 | Pending: runs in batch CI; also requires byte-identical readbacks across both |

The Metal readback hashes (FNV-1a 64) were 15348001186566893997,
2367401795274365253 and 11905181414727121221 for cases 1-3. The browser suite
compares WebGPU and WebGL 2 with each other, not with these native values.

Mutation checks show the gates can fail. Treating a pixel-centre tie as
outside the left edge fails four CPU test cases. Removing the downward
correction of the float64 estimate fails only the exhaustive 1260-combination
property test. Shrinking every scissor by one pixel makes the Metal fixture
report 16 colour mismatches and a failed half-open edge probe in case 1.

Limits: the browser runs use a software GPU, and this record has no physical
GPU, mobile or frame-time evidence. Solid fills are opaque, so the parity
comparison does not cover translucent blending; the
[overlay reuse fixture](../../tests/sdk_consumer/overlay_reuse.cpp) owns that.

