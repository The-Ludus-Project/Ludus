# Ludus Randomness: Gems Review and Design Revisions

**Status:** Literature review informing the proposed
[randomness architecture](randomness.md). No runtime implementation or Ludus
performance result is supplied by this review. The architecture owns the current
[implementation milestones and later research backlog](randomness.md#delivery-plan-and-alternatives);
this review preserves the evidence used to establish the initial design.

The initial design preserved Ludus's PCG32 v1 stream, added Philox addressed
samples for parallel work, separated sampling from gameplay policy, and versioned
replay contracts. Only after recording that baseline did the detailed chapter
review revise the design. Discovery used
[game-dev-gems-toc.md](../../references/game-dev-gems-toc.md), searching for
randomness, sampling, distributions, procedural generation, and replay.

## Evidence and resulting changes

The books are local, ignored files under `references/`. Physical PDF pages below
are one-based and verified against chapter headings; they are not inferred from
printed pagination. Gems 1, 3, and 4 required OCR. Rendered pages confirmed the
Predictable Random Numbers and Fast Base-2 chapter identities and the projectile
chapter's incorrect axis-bias statement. Gems 2 and 7 and Ray Tracing Gems II
provided extractable text. GPU Gems 3 was read on the official publisher-hosted
chapter page. No chapter code or book file is added to the engine.

| Question | Initial design | Revision after reading |
| --- | --- | --- |
| Procedural worlds | Stable addresses regenerate independent chunks | Persist generation recipes and revisions; preserve nested property identity; store world mutations separately |
| Replay isolation | Separate logical domains | Require regressions under rendering/audio changes, debug draws, and checkpoint restoration |
| Random distributions | Small named sampling layer | Give true normal, bounded bell-like, disk, cone, and radial profiles distinct semantics; reject the Gaussian axis-bias claim |
| AI uncertainty | Sample decisions at stable events | Let planners query immutable outcome models without advancing live RNG; isolate rollout domains |
| Repeated weighted draws | Start with simple integer cumulative weights | Add an optional cooked O(n)-built integer alias table when workloads repay it |
| Streaming candidates | No specialized selector initially | Reserve reservoir sampling for demonstrated one-pass streams, with explicit traversal/reduction semantics |
| GPU generation | Future addressed sampler port | Generate within the consumer kernel; qualify integer parity and measure register/multiply costs |
| Bounds and entropy | Checked rejection; external entropy | Retain those choices and reject historical modulo, float scaling, and ad hoc entropy collection |

Changes listed here are engineering adaptations of the ideas read. None of the
books specifies the complete PCG/Philox design, and historical speed results are
not Ludus measurements.

## Procedural generation

**Source:** Guy W. Lecky-Thompson, “Predictable Random Numbers,” §2.0,
*Game Programming Gems 1*. Reviewed PDF pages 129-136, printed pages 133-140.
[Local chapter](../../references/Game%20Programming%20Gems%201.pdf#page=129);
[TOC entry](../../references/game-dev-gems-toc.md#L6713).

The chapter develops reproducible on-demand generation for nested universe
objects, relating object properties and history to seed selection. This supports
regenerating a procedural base rather than storing every generated attribute.
Its examples also expose limitations: global `srand`/`rand`, coordinate arithmetic
that can alias, modulo range mapping, and advancing through earlier draws to
reach a location are unsuitable as modern engine contracts.

**Adopt:** stable hierarchical identity and regeneration. Ludus extends that into
a versioned generation recipe plus explicit mutation overrides. **Replace:**
sequential traversal with direct Philox addresses and collision-checked identities.
Changing a generation recipe must not silently change the base underneath an
old save. The book's apparent uniqueness of arithmetic seed combinations is not
accepted as proof.

## Generator choice and quality

**Source:** Chris Lomont, “Random Number Generation,” §2.1,
*Game Programming Gems 7*. Reviewed PDF pages 146-158, printed pages 113-125.
[Local chapter](../../references/Game%20Programming%20Gems%207.pdf#page=146);
[TOC entry](../../references/game-dev-gems-toc.md#L7186).

The chapter compares generator families, state/period tradeoffs, testing, seed
quality, and simulation versus security uses. Its useful lesson is to choose for
the workload and test the generator, not equate a huge period with adequate
quality. Its WELL recommendation reflects the available alternatives of its era.

**Retain:** explicit seeds, caller-owned state, known answers, offline statistical
qualification, and a separate security boundary. **Do not adopt:** WELL solely
on the historical recommendation, or the chapter's float-scaled inclusive integer
example; that example can produce an upper-bound violation and does not establish
an exact unbiased integer mapping. Existing PCG v1 remains stable; new Philox is
qualified against its own published reference, not asserted superior by this Gem.

## Bounded integer generation

**Source:** James McNeill, “Fast Base-2 Functions for Logarithms and Random
Number Generation,” §2.1, *Game Programming Gems 3*. Reviewed PDF pages 156-158,
printed pages 157-159.
[Local chapter](../../references/Game%20Programming%20Gems%203.pdf#page=156);
[TOC entry](../../references/game-dev-gems-toc.md#L6892).

The chapter demonstrates modulo bias and a power-of-two bit-mask/rejection
solution. Its rejection approach is sound under its uniform-source assumptions,
and expected small loop counts do not imply bounded latency. Returning zero for
an invalid/very small range in its example is not Ludus's checked failure contract.

**Retain:** validation and rejection instead of naive `% bound`. **Modern
adaptation:** use multiply-high rejection for new addressed bounds and benchmark
prepared thresholds. Preserve the existing stream mapper exactly. Bit-mask
rejection remains a valid benchmark candidate, rather than another public RNG
mode. See [Lemire's primary paper](https://arxiv.org/html/1805.10941v4).

## Replay isolation

**Source:** Bruce Dawson, “Game Input Recording and Playback,” §1.16,
*Game Programming Gems 2*. Chapter at PDF pages 102-108, printed pages 105-111;
reviewed its predictability, initial-state, and playback discussion.
[Local chapter](../../references/Game%20Programming%20Gems%202.pdf#page=102);
[TOC entry](../../references/game-dev-gems-toc.md#L6801).

The chapter gives a concrete replay failure: rendering and simulation consume
the same RNG, so rendering cadence changes gameplay. It also discusses task
timing, sound completion, known initial state, and code/data changes.

**Adopt:** explicit regression scenarios that vary rendering, audio completion,
debug instrumentation, and job scheduling while comparing authoritative state.
Saving a seed is insufficient without matching the rest of the initial state
and deterministic consumption. Keep current same-build replay guarantees honest;
these tests do not establish cross-platform floating-point lockstep.

## Entropy boundary

**Source:** Pete Isensee, “Genuine Random Number Generation,” §1.19,
*Game Programming Gems 2*. Chapter at PDF pages 124-129, printed pages 127-132;
reviewed entropy sources, mixing, limitations, and the seeding use case.
[Local chapter](../../references/Game%20Programming%20Gems%202.pdf#page=124);
[TOC entry](../../references/game-dev-gems-toc.md#L6804).

The chapter distinguishes repeatable simulation values from unpredictability,
and recommends using its expensive collection process for initial seeds rather
than ordinary draws. That boundary is useful; its collection examples and dated
cryptographic mixers are not a suitable engine implementation today.

**Retain:** a separate checked Platform secure-byte API and one recorded root
seed per setup. **Reject:** reading stack/arbitrary memory, treating identifiers
or clock values as sufficient secret entropy, and writing a custom mixer. Use
maintained OS/browser cryptographic services; browser semantics are specified by
[Web Cryptography](https://www.w3.org/TR/webcrypto/#Crypto-method-getRandomValues).

## Decision-making

**Source:** Karén Pivazyan, “NPC Decision Making: Dealing with Randomness,” §4.3,
*Game Programming Gems 4*. Reviewed PDF pages 327-337, printed pages 325-335.
[Local chapter](../../references/Game%20Programming%20Gems%204.pdf#page=327);
[TOC entry](../../references/game-dev-gems-toc.md#L6998).

This is an article about planning in stochastic environments, not a better
PRNG or a shuffle-bag technique. It uses action-outcome probability tables and
dynamic programming to reason about expected future costs; merely selecting a
path assuming the most likely outcome can miss substantial risk.

**Adopt:** make authored outcome models readable by planning code without taking
a real simulation draw. Planning and execution share the same versioned model;
randomized rollouts have a separate domain and scenario identity. **Defer:** a
generic dynamic-programming solver and learning system. Decision persistence,
cooldowns, and pity counters in the architecture are Ludus policy choices, not
claims attributed to this chapter.

## Projectile spread

**Source:** Steve Rabin, “Using Gaussian Randomness to Realistically Vary
Projectile Paths,” §2.8, *Game Programming Gems 7*. Reviewed PDF pages 232-237,
printed pages 199-204.
[Local chapter](../../references/Game%20Programming%20Gems%207.pdf#page=232);
[TOC entry](../../references/game-dev-gems-toc.md#L7206).

The chapter motivates concentrated spread and inexpensive sums of uniforms.
That is useful for authoring, but a finite sum is a bounded approximation, not
an exact normal. Its absolute-normal radius with uniform angle is a separate
radial distribution. On printed page 203 it incorrectly claims that independent
normal X/Y coordinates favor axes over diagonals.

For independent centered X/Y with equal variance, their product density is
`exp(-(x*x+y*y)/(2*sigma*sigma)) / (2*pi*sigma*sigma)`. Equal-radius points have
equal density. This correction is a mathematical derivation, also consistent
with the [PBRT normal-distribution treatment](https://www.pbr-book.org/4ed/Sampling_Algorithms/Sampling_1D_Functions).

**Adopt:** distinct named/authored spread profiles and explicit support, draw
count, and truncation policy. **Reject:** the axis-bias claim and a universal
cache-performance verdict for lookup-based normal generators. A future Ziggurat
implementation requires a workload benchmark and a separate sampling contract.

## Weighted selection

**Source:** Chris Wyman, “The Alias Method for Sampling Discrete Distributions,”
Chapter 21, *Ray Tracing Gems II*. Reviewed PDF pages 379-383, printed pages
339-343.
[Local chapter](../../references/Ray%20Tracing%20Gems%20II.pdf#page=379);
[TOC entry](../../references/game-dev-gems-toc.md#L14366).

The chapter explains the alias table's uniform column plus threshold decision,
why precomputation helps repeated draws, and why rebuilding for rapidly changing
weights can be unattractive. Its introductory construction sorts remaining
weights repeatedly; it explicitly points to Vose for an O(n) construction.

**Adopt conditionally:** immutable cooked alias tables for demonstrated repeated
weighted workloads. Start with simpler integer cumulative weights. Ludus's
integer cutoff/ticket formulation, overflow checks, stable cooking order, and
exact ticket tests are adaptations. An O(1) table lookup does not bound rejection
time in the random-variate generation that precedes it.

## Streaming selection

**Source:** Chris Wyman, “Weighted Reservoir Sampling: Randomly Sampling
Streams,” Chapter 22, *Ray Tracing Gems II*. Reviewed PDF pages 384-388, printed
pages 345-349.
[Local chapter](../../references/Ray%20Tracing%20Gems%20II.pdf#page=384);
[TOC entry](../../references/game-dev-gems-toc.md#L14373).

The chapter treats selecting a small weighted subset while seeing candidates
once, and distinguishes replacement semantics. It offers a useful alternative
when collecting and storing all candidates would waste resources.

**Reserve for a real consumer:** explicit traversal, validated weights/totals,
and reproducible reservoir state. Do not infer order-independent outcomes from
an addressable RNG, or quietly treat with-replacement samples as distinct choices.
If parallel reduction or order-independent selection is required, specify and
verify that algorithm separately. Zero/empty totals are checked explicitly;
the engine must not rely on a NaN comparison to mask an invalid division.

**Adopted** for the first such consumer, the
[many-area-light resampling estimator](many-area-light-sampling-reference-review.md):
FoundationMath's [sequential weighted reservoir](randomness.md#sequential-weighted-reservoir)
implements the chapter's single-sample, with-replacement update over an explicit
order, with bit-level validation and an explicit empty result. The minimum
weight, subnormal flushing and contribution-weight helper are Ludus additions for
CPU/GPU parity and RIS.

## GPU generation and consumption

**Source:** Lee Howes and David Thomas, “Efficient Random Number Generation
and Application Using CUDA,” Chapter 37, *GPU Gems 3*.
[Official chapter](https://developer.nvidia.com/gpugems/gpugems3/part-vi-gpu-computing/chapter-37-efficient-random-number-generation-and-application);
[TOC entry](../../references/game-dev-gems-toc.md#L8742).

The chapter relates GPU generator choice to state storage, divergence, and
generation inside the consuming simulation. Those system-level costs remain
useful. Its hybrid Tausworthe/LCG and historical CUDA timings do not determine
the best implementation for Ludus's WebGPU/WGSL path.

**Adopt:** in-kernel generation for a future GPU consumer, measured register/
arithmetic cost, and exact integer reference qualification. **Replace:** the
historical generator with the selected Philox contract where simulation parity
is required. WGSL high-word multiplication must be implemented and validated;
no CPU/GPU performance or parity result is claimed here.

## Modern references and deliberate exclusions

- [PCG reference](https://www.pcg-random.org/using-pcg-c-basic.html): confirms the
  existing algorithm, seeding convention, and known answers.
- [Random123](https://github.com/DEShawResearch/random123) and its
  [original paper](https://www.thesalmons.org/john/random123/papers/random123sc11.pdf):
  support direct counter/key evaluation and the Philox choice. New code must pin
  the reference revision and preserve its license requirements.
- [Lemire](https://arxiv.org/html/1805.10941v4): supports multiply-high rejection.
  [Batched ranged generation](https://arxiv.org/abs/2408.06213) is a newer
  optimization candidate, deferred until consumption/versioning and workload
  evidence justify its extra complexity.
- [Blackman/Vigna's generator reference](https://prng.di.unimi.it/): xoshiro256++
  is a legitimate sequential benchmark alternative; it does not justify replacing
  Ludus's frozen PCG contract without measurements.
- [PBRT sampling interface](https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Sampling_Interface):
  renderer dimension organization and sampling quality remain renderer concerns.

The TOC also lists Perlin/noise, importance sampling, blue-noise, audio synthesis,
and random-rotation articles. They are specialist consumer techniques, not
evidence for replacing the core simulation architecture. They were not used as
unread support for specific algorithms or performance claims. Security standards
and platform APIs take precedence over dated entropy-collection examples.

## Review validation

Documentation links, local anchors, code-fence balance, and whitespace were
checked. Temporary arithmetic models matched the three published Philox4x32-10
vectors, exhaustively checked multiply-high acceptance counts for all 255
nonzero 8-bit bounds, checked 243 scope/event/dimension/attempt boundary
combinations for packing aliases, and verified exact alias-table mass for 1,359
small nonempty-weight fixtures (one to five entries, weights zero through three).

These checks substantiate the proposed arithmetic and cited fixtures. They are
not tests of a Ludus implementation, a full statistical battery, a performance
measurement, or native/web/GPU qualification. No engine code changed and no
build, unit-test, sanitizer, or tidy result is claimed by this documentation task.
