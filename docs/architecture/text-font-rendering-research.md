# Text/font rendering research and decisions

L2 now implements the shared [GraphicsText atlas and Renderer adapter](renderer-systems.md#implemented-l2-mutable-versions-and-overlays), including native CPU Text preparation and portable R8 sampling. Browser CPU shaping/source bootstrap and the standalone F0–F6 acceptance gates remain pending.

Research date: 2026-10-01. Implementation baseline: `origin/main` at `fa0fc7f`.
This document records the design research, including local PDF findings, so Kiro
can implement without access to `references/` or this conversation. The
[requirements](../../.kiro/specs/text-font-rendering/requirements.md) and
[design](../../.kiro/specs/text-font-rendering/design.md) define the deliverable.
Performance and pixel-quality claims remain hypotheses until measured.

## Architecture selected

Use HarfBuzz for Unicode run shaping, FreeType for size-specific grayscale
glyphs, a bounded append-only R8 atlas, and painter-ordered instanced quads through
a small RHI coverage-quad interface. Share font bytes, shaping, rasterization,
atlas rules, instance format, and fixtures across Linux and web. Keep Vulkan and
WebGPU resource/synchronization code private to RHI.

There is no universally fastest font renderer. For the engine's first debug/UI
text, this choice balances small-size quality, implementation size, predictable
memory, visible intermediate data, and established libraries. This is a design
judgment, not a benchmark result. Large transformed text may justify another
raster backend later without replacing shaping or the application interface.

## Contemporary primary sources

| Source actually inspected | Finding used here | Decision |
| --- | --- | --- |
| [HarfBuzz overview](https://harfbuzz.github.io/what-is-harfbuzz.html) and [FreeType integration](https://harfbuzz.github.io/integration-freetype.html) | Shaping chooses/positions glyphs; `hb-ft` connects those results to a sized FreeType face. | Shape first, rasterize glyph IDs; keep size/load flags synchronized. |
| [HarfBuzz clusters](https://harfbuzz.github.io/working-with-harfbuzz-clusters.html) | Output clusters can merge or reorder input. | Preserve UTF-8 byte clusters; do not promise code-point-based caret positions. |
| [HarfBuzz scope](https://harfbuzz.github.io/what-harfbuzz-doesnt-do.html) | Unicode bidi processing belongs outside run shaping. | Explicit script/direction runs first; no false claim of full mixed-direction paragraphs. |
| [FreeType bitmap conventions](https://freetype.org/freetype2/docs/glyphs/glyphs-7.html) | Pitch is signed and distinct from width; bitmap bearings use font coordinates. | Normalize rows; make baseline/sign conversion a tested boundary. |
| [msdfgen](https://github.com/Chlumsky/msdfgen) and [msdf-atlas-gen](https://github.com/Chlumsky/msdf-atlas-gen) | Multichannel fields preserve corners and require range/scale-aware reconstruction. | A strong future option for broadly scaled text; not the initial UI raster path. |
| [HarfBuzz GPU API](https://harfbuzz.github.io/harfbuzz-hb-gpu.html) and [raster API](https://harfbuzz.github.io/harfbuzz-hb-raster.html) | New APIs offer analytic GPU outlines and CPU rasterization. The GPU API includes WGSL as well as GLSL support. | Evaluate them honestly as current alternatives; keep them disabled in this milestone. |
| [HarfBuzz releases](https://github.com/harfbuzz/harfbuzz/releases/tag/14.5.1) | Release 14.5.1 fixes issues in 14.5.0; release notes describe the new rendering libraries as experimental. | Pin 14.5.1 for shaping; use mature FreeType rasterization. |
| [FreeType releases](https://freetype.org/index.html) and [2.14.3 build configuration](https://github.com/freetype/freetype/blob/VER-2-14-3/CMakeLists.txt) | 2.14.3 is a maintenance release; optional dependency detection can vary builds. | Pin 2.14.3 and explicit options on both targets. |
| [Khronos glslang 15.4.0](https://github.com/KhronosGroup/glslang/releases/tag/15.4.0) | A released GLSL-to-SPIR-V compiler is available. | Use a pinned host build for the two small Vulkan shaders; no runtime compiler. |

HarfBuzz GPU rendering is a credible state-of-the-art candidate, rather than
an option rejected because WebGPU lacks shaders. Its outline encoding and curve
evaluation would add a different GPU data model and validation surface to an RHI
that currently has no public drawing API. Start with a one-channel texture sample
and instrument the workload. Revisit analytic rendering or MSDF for measured
large-size/transform needs, including wasm size, cold cost, GPU time, small-size
quality, and both backend implementations.

The atlas design does not need to cache Unicode characters or ship a giant
prebaked alphabet. Cache only glyphs actually returned by shaping, at the sizes
actually used. No LCD rendering: monitor subpixel order and browser compositing
are unsuitable assumptions for a shared renderer.

### Fractional glyph positioning

Nicolas P. Rougier shows why rounding an advance can change the length of a
line. Ludus takes the signed advances and offsets from HarfBuzz, which already
accounts for kerning. CPU measurement and GraphicsText accumulate the pen in
26.6 units and convert at the pixel boundary. The paper's horizontal RGB filter
assumes a known display subpixel order. Ludus uses grayscale coverage for
composited targets, and the Qt editor HUD uses Qt's own font metrics and painter.
Figure 7's fractional-origin sweep is a way to evaluate the rendered result in
an installed app. Compare dark-on-light and light-on-dark text at the same
positions and record the target encoding before considering a coverage change.
[Rougier, *Higher Quality 2D Text Rendering*, JCGT 2(1), §§2.3–2.5 and Fig. 7](https://jcgt.org/published/0002/01/04/paper.pdf#page=6)
· [U1 installed-app qualification](https://github.com/The-Ludus-Project/Ludus/issues/1219)

## Local Gems readings

The local `references/game-dev-gems-toc.md` was searched for font, text,
localization, atlas, smoothing, and distance-field topics. The following two
chapters were read from the locally available PDFs. Relevant figures were also
visually inspected. These are paraphrases and applications, not copied code.

### Aurelio Reis — “Fast Font Rendering with Instancing”

*Game Programming Gems 8* (2010), chapter 1.1, printed pp. 3–11; local PDF pages
18–26, counting the first PDF page as 1.

The chapter explains bitmap glyph quads and atlas lookup, then identifies the
cost of repeatedly uploading per-glyph geometry. It separates fixed quad corners
from per-glyph placement/UV information and batches compatible glyphs. Texture
or render-state changes break a batch. It also discusses resolution dependence,
GPU stalls, order/layering, and measuring hardware-dependent instancing costs.

Apply the durable ideas: six generated corners, one compact instance per glyph,
reserved reusable buffers, adjacent compatible batches, and no overwrite of
in-flight data. Rasterize at the real framebuffer size. Preserve alpha painter
order. Expose uploaded bytes, batch count, and timings before adding compression.

Do not transplant the historical Shader Model 3 constant-register method,
85-quad limit, 4-byte packed vertices, D3D9 lock/discard API, console-specific
performance assumptions, or readability tradeoffs into Vulkan/WebGPU. Modern
instanced vertex attributes are a simpler portable implementation. The book's
CD demo is unnecessary to implement this design.

### Manny Ko — “A Fast and High-Quality Texture Atlasing Algorithm”

*Game Engine Gems 3* (2016), chapter 9, printed pp. 111–120; local PDF pages
106–115. This chapter addresses irregular mesh/lightmap charts, not a ready-made
font atlas implementation.

Its useful decomposition is candidate placement, fit testing, and a goodness
metric. It reserves borders before packing, describes a simple descending
first-fit heuristic, measures utilization, and considers more elaborate search
when large shapes or very small canvases make greedy packing poor.

Apply the simplicity and measurement lessons to rectangular glyphs: deterministic
shelves, transparent reserved gutters, checked fit/size arithmetic, bounded
pages, and visible occupancy. Retain glyph positions until a deliberate reset.
Replace the private packer only if measured fragmentation warrants it.

The chapter's atlas-aware diffusion follows mesh topology across chart seams.
Independent glyph masks have no such shared topology: do not diffuse one letter
into another, build microedges, implement mesh segmentation, or copy the
irregular bitmask/annealing machinery. Transparent coverage gutters and no
mipmaps solve the initial font filtering problem.

### Additional catalog entries and availability

The TOC also lists ShaderX's “Font Smoothing” and Philip Rideout's “2D Distance
Field Generation with the GPU” in GPU Pro 360. Their source chapters were not
available/read for this task. They are discovery pointers only; this design
does not attribute techniques or conclusions to their unseen contents. The
current primary sources above provide the distance-field comparison.

## Repository audit and prerequisites

- Main already contains keyboard input M0–M5 and WebGPU work W4–W6. Both changes
  must survive this implementation. Inspect current code if the baseline moves.
- Public `modules/graphics/rhi/include/ludus/graphics/rhi/rhi.h` provides startup,
  backend/status reporting, and frame operations. It does **not** yet provide a
  public texture, buffer, sampler, or draw interface.
- `rhi_vulkan.cpp` owns native swapchain/submission; the native smoke renderer
  currently only begins/ends frames. Native text requires a real graphics
  pipeline and render-attachment recording, not reuse of a nonexistent draw API.
- `rhi_webgpu.cpp` handles asynchronous device startup and frame lifetime. The
  web smoke triangle uses `src/internal/webgpu_probe.h` with borrowed handles.
  This probe interface must not become a public dependency of GraphicsText.
- Existing surfaces use unorm formats. Choosing `SRGB_NONLINEAR` as the Vulkan
  surface color space alone does not make an unorm attachment blend in linear
  space. Text needs the explicit attachment-view/pipeline format work in F3.
- CMake currently declares CXX only. FreeType is C; enable C deliberately and
  keep C++-specific project flags from leaking onto C dependencies.
- Native dependency resolution uses Conan; web bootstrap bypasses native Conan
  and pins Emscripten 4.0.23/EmDawn in `config/web_toolchain.json`.
- The inspected 4.0.23 SDK port sources pin HarfBuzz **3.2.0** and FreeType
  **2.13.3**. Therefore bare `USE_HARFBUZZ`/`USE_FREETYPE` port flags do not provide
  the chosen common versions. Build the locked sources for wasm separately.
- ADR 0008 is a proposed memory design; there is no implemented FoundationMemory
  to assume. Use existing fallible container/allocation seams and library C
  allocation hooks; do not implement that separate subsystem for this feature.
- W5/W6 automated probes include mocks around real wasm/C-API paths. They do not
  establish actual rendered glyph quality. Hardware/compositor gates remain
  necessary, on both platforms.

## Tradeoffs and revisit triggers

| Decision now | Cost accepted | Revisit only with evidence |
| --- | --- | --- |
| Size-specific R8 masks | New glyph cache entries after size/DPR changes | Large/rotated/world-space text needs MSDF or analytic outlines |
| Explicit Unicode runs | Caller supplies paragraph composition | A real mixed-direction/wrapping consumer needs Unicode bidi, itemization, and line-breaking work |
| One face per run, no automatic fallback | `.notdef` for absent characters | Product language coverage needs a defined fallback policy that preserves shaping context |
| Fixed append-only atlas | Size churn can reach a visible limit | Measured long-session pressure justifies page retirement/LRU with GPU lifetime tests |
| Deterministic shelves | Some unused atlas area | Occupancy/capacity measurements justify skyline packing |
| Main-thread CPU preparation | Cold work can consume frame time | Profiled spikes justify prewarming or later budgeted/background preparation |
| Narrow coverage-quad RHI | A specialized first drawing facility | A second real graphics consumer justifies a more general resource API |

Do not call this architecture optimal without evidence. Its intended strengths
are small contracts, mature shaping/raster libraries, explicit failure, visible
intermediates, and a bounded warm path. F5 measures those claims.
