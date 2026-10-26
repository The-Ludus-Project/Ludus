# Primitive types and numeric boundaries

Status: implemented. The initial design was recorded before the Gems review;
review amendments, final contracts and validation evidence follow below.

## Goals and initial design

Keep primitives native and boring: exact fixed-width aliases preserve arithmetic,
layout, C++ overloads, debugger displays, and standard-library interoperability.
No boxed scalar objects, hidden state, allocation, exceptions, or runtime type
registry belongs in FoundationBase. Existing aliases and namespaces remain stable.

1. `types.h` stays the small Band 0 vocabulary: fixed-width signed/unsigned
   integers, native `usize`/`isize`, and IEEE binary32/binary64 floats. Verify
   8-bit bytes, widths, signed ranges, native size/difference compatibility,
   and float representation at compile time. Expensive numeric checks belong
   in one implementation translation unit, not every including translation unit.
2. Add an explicitly included `checked_integer.hpp`: a closed integer concept,
   checked integer-to-integer cast, and checked add/subtract/multiply. All are
   constexpr, noexcept, nodiscard, allocation-free, and preserve the output on
   failure. Bool, plain/Unicode character types, enums, and floating-point conversions
   are excluded. Native integer promotions remain normal C++ behavior.
3. Add an explicitly included `byte_order.hpp`: bounded unsigned integer
   reads/writes in little/big endian order using byte spans. Validate the extent
   before touching data; tolerate unaligned buffers; avoid typed pointer casts,
   object layout serialization, and host endian assumptions. Output/buffer stays
   unchanged on failure. Floats use separately specified codecs owned by their
   format, not an implicit integer cast.
4. Native arithmetic remains the trusted hot-path default. Use checked helpers
   at ingress and allocation/format boundaries. Signed overflow is forbidden;
   unsigned wrap is intentional only where the algorithm requires it (hashes,
   PRNGs, generation counters). No universal saturating arithmetic or epsilon.
5. Keep semantic IDs, handles, physical units, SIMD vectors, fixed point, half
   precision, and GPU layout in their owning modules. Add strong wrappers only
   for demonstrated domain confusion; never replace every scalar with a wrapper.

## Delivery and validation

One cohesive PR can ship the contract, small opt-in headers, boundary tests,
installed SDK coverage, and representative existing boundary integrations.
Review the Gems catalog after this draft, record actual article evidence and
amendments below, then implement. Validate all supported integer widths and
signedness combinations, exact limit values, exhaustive 8-bit arithmetic,
output aliasing and failure preservation, byte vectors and misaligned/truncated
buffers, native and browser toolchains, installed SDK, header isolation,
format/tidy, warning-clean native builds, ASan/UBSan, and CI. Measure header
parse cost rather than claiming a performance gain from alias spelling.

## Evidence review and amendments

The catalog is `references/game-dev-gems-toc.md`; PDFs are local reference
material and are not distributed with the SDK. These are design lessons,
not imported source code. The draft above preceded the review.

| Article actually inspected | Useful evidence | Resulting decision |
| --- | --- | --- |
| Game Programming Gems 2, 2.1, Yossarian King, Floating-Point Tricks: Improving Performance with IEEE Floating Point (PDF pp. 160-172 inspected) | Binary representation, finite conversion domains, and target-dependent timings are central to the proposed tricks. | Verify IEEE binary32/64 exponent and significand properties in a private contract TU. Do not add union-punning or magic-bias float conversions. Do not infer today's costs from Pentium II measurements. |
| Game Programming Gems 3, 1.9, Søren Hannibal, Floating-Point Exception Handling (PDF pp. 71-74, rendered and OCR checked) | Non-finite values can conceal bugs or make loops fail to terminate; libraries can alter FP control state. | Preserve ADR 0010's explicit finite-input checks and module-owned FP policy. FoundationBase must not alter thread FP trap/rounding/FTZ state. Hardware FP exceptions and C++ exceptions are different mechanisms; neither is added here. |
| Game Programming Gems 6, 2.1, Chris Lomont, Floating-Point Tricks (representation sections, PDF pp. 120-123 / printed pp. 121-124, rendered) | Storage representation and execution policy differ; signed zero and subnormals are exceptional values with explicit encodings. | Add compile-time bit-pattern tests for one, negative zero and the smallest subnormal at both float widths; keep FP execution policy outside primitive aliases. |
| Game Engine Gems, Jason Hughes, chapter 1, What to Look for When Evaluating Middleware for Integration, section 1.13 Platform Portability (printed p. 12, text inspected) | File/network streams can hide byte-order assumptions even in otherwise portable middleware. | Keep explicit bounded endian codecs and byte-vector tests at format boundaries. |
| Game Engine Gems 2, 24, Eric Lengyel, Bit Hacks for Games (printed pp. 391-401) | Assumed widths and edge values determine whether branchless formulas are valid; signed-minimum absolute value is a counterexample. | Test every integer width at its limits, including signed minimum times -1, and exhaust the 8-bit input space. Use defined overflow builtins instead of calculating an overflowing signed result and checking afterward. |

Modern primary references: [Clang checked arithmetic builtins](https://clang.llvm.org/docs/LanguageExtensions.html#checked-arithmetic-builtins),
[C++ integer comparisons and in_range](https://eel.is/c++draft/utility.intcmp),
and [fundamental types](https://eel.is/c++draft/basic.fundamental). Native Clang
18 and pinned Emscripten are the supported compilers; checked arithmetic uses
Clang's generic overflow builtins, with compile-time tests on both toolchains.
`std::in_range` supplies signedness-aware conversion instead of bespoke range
comparison templates. Helpers restrict inputs to the existing integer aliases;
C++ aliases cannot distinguish `int32` from a native `int` when they are the
same type. Signed/unsigned char remain the underlying 8-bit aliases; plain char,
bool, Unicode characters, enums, and floating-point types are excluded.

## Scalar vocabulary

| Role | Type | Contract |
| --- | --- | --- |
| Fixed-width signed/unsigned arithmetic | int8/16/32/64 and uint8/16/32/64 | Native C++ types; signed overflow is invalid, unsigned wrap is deliberate algorithm policy. Small integer operands still undergo ordinary C++ promotions. |
| In-process sizes, indices, differences | usize, isize | Native size_t/ptrdiff_t ranges; not a portable file or network width. |
| Real-valued arithmetic | float32, float64 | Verified IEEE binary32/binary64 storage, 24/53 significand bits; numerical algorithms own precision/tolerance/rounding policy. |
| Logic | bool | Native logical type; do not add bool8/bool32 convenience aliases. A format can encode canonical 0/1 in uint8 and reject other values explicitly. |
| Encoded text | char and existing text-module vocabulary | Encoding/validation belongs to Text/Content; do not treat a Unicode code point as a generic numeric conversion. |
| Raw byte payloads | uint8 spans | Exact 8-bit byte operations; semantics and field encodings belong to the format. No additional byte wrapper is required by today's callers. |

## Final boundary contracts

- Float aliases promise storage/representation, not deterministic arithmetic,
  current rounding mode, NaN payload preservation, or an automatic exact
  integer-to-float conversion. ADR 0010 owns numerical algorithms and tolerances.
- `usize`/`isize` are native process sizes/differences. Never place them, pointers,
  native bools, C++ enums, or padded structs directly into a stable wire format.
  Choose fixed-width fields, byte order, accepted values and format version.
- Endian helpers consume/write the first `sizeof(T)` bytes of a span; larger
  spans are permitted and trailing bytes are untouched. Truncation is failure.
  They support the unsigned integer aliases; schemas choose uint8/16/32/64
  rather than usize (aliases cannot enforce this spelling). Signed/float encodings require
  a format-specific representation decision. They access bytes individually, so
  no alignment, endian, aliasing, or struct padding assumption is needed.
- Checked arithmetic operands share one integer type, so mixed-width/signedness
  expressions must be converted deliberately. Output can alias either input;
  by-value operands and a temporary result make both success and failure safe.
  Failure returns false without changing output. It never logs, asserts, traps,
  allocates, or saturates. Division and float conversion are not exposed because
  they need additional rounding/domain policy and have no current shared caller.
  Add/subtract/multiply use standard integer bounds checks before evaluating the
  result, so the public helper does not require LLVM overflow builtins. Bounds
  are derived from the closed binary alias set without adding `<limits>` to the
  public header. Signed multiplication checks all sign quadrants without ever
  evaluating signed-minimum divided by -1. Existing exhaustive 8-bit tests and
  additional extrema/aliasing cases run in the isolated
  [Windows D0 probe](windows-direct3d12.md#d0-primitive-portability-probe) as well
  as the ordinary FoundationBase tests; MSVC evidence is limited to that probe.
- Include helpers explicitly. `types.h`/`core.h` do not pull them. No runtime
  primitive registry or redundant short aliases are introduced. Constants and
  signed bounds are not repeated throughout the public API.

Representative integration: protocol framing uses checked queue-size addition
before accepting input (preventing size wrap), checked length conversion, and
explicit little-endian prefixes. Keep its existing byte format and limits.

## Use at boundaries

```cpp
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/base/byte_order.hpp>

// A decoder/allocation caller chooses how to report these failures.
uint32 count{};
usize bytes{};
if (!TryIntegerCast(incomingCount, count) ||
    !TryMultiply(static_cast<usize>(count), sizeof(Element), bytes))
{
    return false;
}
// Serialize the fixed-width count, never the process-native byte count.
return TryWriteLittleEndian(count, destinationBytes);
```

Choose a wider intermediate before multiplication when that is the algorithm's
policy; the helper's output type defines its representable result range. A cast
cannot promise exact integer-to-float conversion: binary32 has 24 significand
bits and binary64 has 53, even though their storage widths are 32 and 64 bits.
Numerical conversion APIs must separately specify range, exactness and rounding.

## Alternatives and maintenance

| Choice | Assessment |
| --- | --- |
| Wrapper class for every scalar | Adds operator/conversion surface and debugging friction while changing source/ABI compatibility. No current domain requirement warrants it. |
| Handwritten signed overflow formulas or a widened 128-bit intermediate | More edge-case code or a nonstandard width. Supported LLVM builtins define every input result and map to native instructions. |
| Implicit saturation/wrap in checked APIs | Would conceal invalid sizes or change algorithms. Checked failure is explicit; PRNG/hash wrap stays local and deliberate. |
| One universal numeric utility header | Would charge all translation units for optional span/range facilities. Preserve the existing include boundary. |
| Reinterpreting a byte buffer as an integer pointer | Alignment, aliasing and host-endian hazards. Bounded byte accesses optimize well without these assumptions. |

Adding a supported backend requires compiling the representation TU and testing
native size aliases, signed boundaries and emitted checked code on that backend.
Adding a new primitive width requires its alias/representation checks and test
matrix deliberately; the closed concepts will not silently admit extended
integers or new float representations. Domain wrappers remain separate work
when actual IDs, units or format contracts justify them.

## Boundary adoption audit

The follow-up inspects allocation sizes, narrowing casts and binary ingress
across Foundation, Runtime/GameApi and GameHost, Content, Audio/AudioContent,
Text, graphics backends and application I/O. Changes target concrete failures
or external numeric admission; already proven bounded casts and intentional
unsigned hash/PRNG/ring arithmetic keep their existing policy.

| Boundary | Finding and implementation |
| --- | --- |
| GameApi checkpoint integer writes | Values too wide for the requested 1..8-byte width previously changed the buffer before returning false. Check fit first, preserving every byte on failure, including nonstandard widths. Keep the small ABI-only byte loop; these widths are format policy beyond the fixed-width Foundation codecs. |
| Checkpoint and authored record readers | Previously published fields before checking record length/IDs/kind. Validate locals first; failure preserves caller output and reader position so retry/diagnostics see the previous complete record. |
| Audio source/session loop conversion | The streaming quotient multiply guard did not protect the final rounded-fraction addition. A shared private checked helper preserves nearest/ties-up rounding for both resident and streaming loops and rejects overflow without changing output. |
| Text rasterization | Negating a signed minimum pitch is undefined; native-width bitmap products also need admission before allocation/pointer offsets. A private allocation-free layout validator widens pitch before negating and checks coverage/source extents against the native pointer-difference range. Keep existing row orientation and rendering policy. |
| Content file ingress | Validate external signed file length with TryIntegerCast before native allocation/cap comparison. File output remains unchanged when admission fails. |
| Foundation diagnostic control protocol | Existing 16-byte header bounds and payload cap already prove safety. Reuse bounded endian codecs to eliminate duplicate fixed-width encodings while keeping the exact wire bytes and emergency-path independence. |
| Existing guarded boundaries | Array capacity/product ceilings, Content document/catalog caps, hash length guards, Audio resident PCM caps and fixed mixer/ring capacities, GameHost schema/queue/allocation caps, UTF-8 decoder caller bounds, graphics device extents and bounded editor queues already establish relevant ranges. Avoid replacing deliberate wrap or adding checks inside trusted sample/render loops. |

The new comments acknowledge sources at the affected file/section boundaries.
The five Gems chapters inform design/edge tests, not copied implementations.
Jason Hughes's author and parent chapter are verified from Game Engine Gems;
the original review's shortened section label is expanded above. Checked
arithmetic and range comparison comments link the consulted LLVM/WG21
documentation. Text's new layout validator additionally follows the FreeType
Project's API Reference, FT_Bitmap fields pitch/width/rows:
[FT_Bitmap](https://freetype.org/freetype2/docs/reference/ft2-basic_types.html#ft_bitmap).
The validator keeps FreeType types private and adds explicit integer admission;
it does not change the library's raster algorithm or import reference code.

## Performance evidence

The original LLVM-builtin implementation's optimized Clang 18.1.3 x86-64 assembly probe (`-std=c++23 -O2
-fno-exceptions`) shows `TryAdd<uint64>` as one `addq` with carry check,
`TryMultiply<int64>` as one `imulq` with overflow check, and a little-endian
`uint32` read as a length check followed by one `movl`. The byte-loop source
therefore does not require byte-at-a-time machine instructions. Failure paths
skip the output store. These are code-generation observations on this target,
not throughput or cross-platform timing claims. Alias spelling itself adds no
runtime optimization; the architecture avoids wrapper overhead and catches
invalid input before unsafe operations.

That historical arithmetic code-generation result does not characterize the
portable bounds-check implementation introduced for Windows D0. No equivalent
MSVC performance or full build-budget qualification is claimed by the primitive
probe; the existing build-budget limits remain unchanged.

The pinned clang-tidy signed-character check mistakes signed int8 numeric
widening for encoded-character conversion, including canonicalized template
instantiations. Three line-local `NOLINTNEXTLINE(bugprone-signed-char-misuse)`
annotations explain why signed value preservation is required: the checked
cast and two widened test-oracle minimum values. Converting through unsigned
char would change negative integers. The repository's checker configuration
and its general character diagnostics are unchanged.

## Boundary adoption validation

Pinned Clang 18.1.3 warnings-as-errors builds and complete Debug (52 CTest
entries), Development (49 entries), and ASan/UBSan (43 entries) suites passed.
Two live Wayland entries in each native suite skipped without a compositor.
All 16 pinned Emscripten Development tests passed, and the installed native
SDK consumer built and ran. An additional isolated wasm32 executable checked
32-bit raster extent rejection, checkpoint failure preservation and Audio's
final rounded-sum overflow; it does not claim a browser Text/Audio backend.
Source formatting and foundational include gates passed. All translation
units passed pinned clang-tidy; the final named-field raster interface and
its two callers were rechecked, and Text tests reran in all three native
profiles plus the wasm32 probe after that interface change.

## Original primitive implementation validation

Pinned Clang 18.1.3 local validation passed the Debug (50 CTest entries),
Development (47 entries), and ASan/UBSan (41 entries) suites. Two live Wayland
entries in each native suite skip when no compositor is available. All 15 pinned
Emscripten development tests passed, including the runtime primitive contract
and standalone public headers. The installed native SDK consumer linked and
ran; installed wasm32 headers passed a separate constexpr consumer compile.
`./scripts/check linux-clang-development --all` passed with the pinned formatter
and clang-tidy. The primitive subset alone passed 788,515 assertions in 37 cases.

An isolated include-only Clang time-trace probe (five compilations per header,
median inclusive `Source` event for the exact header path) measured baseline
`types.h` at 17.8 ms, current `types.h` at 28.6 ms, `checked_integer.hpp` at
191.0 ms and `byte_order.hpp` at 573.7 ms on the shared local host. These local
wall-clock trace durations include scheduling noise and are not a throughput
benchmark or CI calibration. Universal types gain no STL dependency, and the
span/range cost is explicitly opt-in. No build budget is raised; the separate
CI build-budget job remains the authoritative full-graph gate.

## Owned decimal number parsing

Opt in to `ludus/foundation/base/parse_number.hpp`; it is not included by
`types.h` or `core.h`. `ParseUint64` consumes unsigned ASCII decimal digits.
`ParseFloat32` consumes the complete JSON decimal-number grammar, including
fraction and exponent, with no locale, allocation, exceptions or STL numeric
conversion. Both accept at most 1024 bytes and preserve the output on failure.
The API comments define the status and ownership contracts.

Binary32 conversion rounds to nearest, ties to even, including subnormals and
signed zero. Overflow, or a nonzero input that rounds to zero, reports
`OutOfRange`; an exactly zero significand remains zero even with a huge exponent.
A 1024-byte limit makes work and stack storage bounded; callers needing longer
numbers must handle `TooLong` explicitly.

The private implementation builds the exact decimal rational with fixed arrays
of 192 32-bit words. It first rejects nonzero magnitudes outside decimal decades
-46 through 38, then compares that rational against ordered positive binary32
encodings using integer scaling. After locating adjacent encodings, it compares
the exact midpoint and selects the even encoding on a tie. The upper endpoint
models 2^128 solely to detect overflow; it never emits infinity. The maximum
1024-digit significand and denominator scaling, including the binary midpoint
shift, need fewer than 4096 bits, below the 6144-bit storage bound. Conversion
uses no floating arithmetic and does not depend on the active rounding mode.
This favors simple, exact loading-time behavior over a high-throughput parser;
it is not intended for a per-frame text conversion hot path.

Tests cover syntax, complete consumption, uint64 overflow, output preservation,
negative zero, subnormals, normal boundaries, long midpoint-breaking tails,
exact ties and overflow thresholds. A deterministic test-only CRT oracle
provides independent binary32 comparisons. Production does not depend on it.
