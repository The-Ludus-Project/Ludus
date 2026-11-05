# Ludus Shading Language (LUSL)

Status: proposed, November 4, 2026; the design's open questions were resolved the
same day (see [resolved questions](#resolved-questions)). L1a, the preprocessor,
is implemented in `Ludus::ShaderLanguage`; see the
[module guide](../modules/tooling/shader-language.md) for its exact behavior and
limits. The rest of L1 and later phases are not implemented.
[ADR 0027](../decisions/0027-ludus-shading-language.md) records the decision to
build LUSL as a front end that emits Slang, and the alternatives it rejected.

This page owns the LUSL language design, its compiler architecture and its
delivery plan. The [render graph architecture](render-graph-authoring.md) keeps
ownership of shader configurations, schemas, build identity and packages. The
[shared scene shader contract](shared-scene-shader-contract.md) keeps ownership
of the scene shader API. `Ludus::ShaderCompiler` keeps ownership of the pinned
Slang session and target artifacts ([ADR 0010](../decisions/0010-shader-toolchain-feasibility.md),
[ADR 0013](../decisions/0013-browser-glsl-es-shader-toolchain.md)). The
[code editor architecture](code-editor.md) owns language registration and
editor services.

## Goal

LUSL is a C-based shading language whose rules match the Ludus coding
standards. Its compiler checks LUSL source and lowers it to Slang. The existing
pinned Slang chain then produces every portable target: SPIR-V, ray-query SPIR-V,
WGSL and Metal, plus GLSL ES and DXIL through their existing paths. LUSL inherits
those targets, the reflection and layout checks and the package format without
changing them.

Several Ludus rules can only be checked in review today, because
`source_policy.py` is lexical and Slang accepts code that breaks them. LUSL
makes them compile errors:

- no unary `!`: write `condition == false`;
- no implicit conversions;
- one declarator per declaration;
- `out` and `inout` written at the call site, and no aliasing between writable
  arguments;
- named sizes in array declarations;
- no read before a definite write, unless explicitly opted out.

It also adds what the author asked for: named arguments, `= 0` zero-filling any
type, a C preprocessor, and concept-based metaprogramming to select material and
permutation variants.

Non-goals:

- **Not a Slang replacement.** The 451 existing `.slang` files keep working.
  LUSL and Slang code call each other, and files are migrated one at a time
  only when someone chooses to.
- **No new GPU capability.** LUSL cannot express anything the RHI and the
  selected target profile do not already support.
- **No runtime shader generation.** Shipping runtimes consume cooked packages
  and need neither the LUSL nor the Slang compiler.
- **Not a visual shader-expression language.** That stays conditional, as the
  render graph architecture states.

## Name and files

The language is **LUSL**, the Ludus Shading Language.

- `.lusl` files are compilable modules.
- `.luslh` files are include-only declarations, mirroring `.slang`/`.slangh`.
- The `lu` prefix matches the existing `.luscene.json`, `.lushader.json` and
  LUSCN names.
- *LSL* was rejected because it already names the Linden Scripting Language and
  the Lab Streaming Layer.

## Compiler architecture

```text
Immutable source snapshot (.lusl, .luslh, .slang) + selected configuration values
  -> preprocessor        configuration macros only; keeps every token's source position
  -> lexer and parser    bounded, recovers from errors, no exceptions
  -> semantic analysis   names, types, constants, definite assignment, calls, concepts
  -> LUSL IR             typed and target-independent; the seam for later direct backends
  -> Slang emitter       deterministic Slang module plus source map
  -> ShaderCompiler      pinned Slang session, reflection, target artifacts (unchanged)
  -> render-graph build  layout validation, packages, diagnostics (unchanged)
```

| Owner | Responsibility |
| --- | --- |
| `Ludus::ShaderLanguage` (new, `modules/tooling/shader_language`) | Preprocessor, parser, checker, IR, Slang emitter, formatter, source maps and structured diagnostics |
| `Ludus::ShaderCompiler` (existing, `modules/tooling/shader_compiler`) | Accepts `.lusl` files in a request snapshot, lowers them before the Slang session, maps Slang diagnostics back through the source map |
| Render-graph build | Unchanged ownership of configurations, budgets, identity and packages; a program source may name a `.lusl` file |
| `luslc` (new, `tools/luslc`) | Command-line lowering, checking and formatting for the CMake helpers, tests and external projects; uses the same library |
| Code editor | Registry record, syntax grammar and language services ([tooling](#tooling)) |

Rules for the compiler:

- **One compiler core.** Offline builds and live builds use the same library, as
  the render graph architecture requires for Slang. The editor does not get a
  second compiler.
- **Generated Slang is an intermediate.**
  - A `.lusl` source lowers to a generated Slang file at a derived path in the
    request snapshot (`main_shaders/shade.lusl` becomes
    `main_shaders/shade.lusl.slang`). The generated render-graph wrappers
    include that file.
  - The editor shows it read-only, and nobody edits it.
  - The build regenerates it from the LUSL source.
- **Engine implementation rules apply.** The compiler is C++23 with no
  exceptions, STL containers or algorithms. It uses Ludus containers and
  FoundationParsing, returns explicit statuses, and admits only bounded input.
- **No new compiler host.** The library also builds with Emscripten, so the
  browser editor can check and format LUSL. Producing target artifacts in the
  browser follows `ShaderCompiler`'s existing platform support.
- **One owner per limit.**
  - The snapshot's file count and byte limits stay owned by `ShaderCompiler`
    (`MAX_SOURCE_FILES`, `MAX_SOURCE_BYTES`), and the generated Slang must fit
    them.
  - `ShaderLanguage` owns its own limits: tokens, include depth, macro expansion
    depth and count, syntax nodes, specializations and retained diagnostics.
  - Their values are set in L1, measured against built-in content. The
    preprocessor's limits are listed in the
    [module guide](../modules/tooling/shader-language.md#limits).

## Language

The examples below illustrate the proposal. The L1 implementation turns them
into grammar, golden tests and a usage guide.

### Preprocessor

LUSL keeps a C preprocessor, run before parsing:

- **Directives**: `#include "path"`, `#pragma once`, object-like and
  function-like `#define`, `#undef`, `#if`, `#ifdef`, `#ifndef`, `#elif`,
  `#else`, `#endif`, `defined(...)` and `#error`.
- **Includes** take only quoted paths, resolved inside the captured snapshot.
  There is no filesystem fallback.
- **An undefined identifier in `#if` is an error**, not C's silent `0`. This
  catches misspelled option names.
- **External macros come only from configuration values.** A request carries
  the selected render-graph configuration values. Neither the build nor `luslc`
  has a free-form `-D`, keeping the render graph's rule against raw macro
  injection.
- **Unknown configuration names are rejected.** A file that tests a name no
  declared configuration sets is an error.
- **Macro reads join the build key.** Every configuration macro a file reads is
  part of its identity.
- **Diagnostics show the expansion stack** from the macro use to its
  definition.
- **The emitted Slang contains no LUSL macros.** Slang files included through
  [`extern slang`](#slang-interoperability) keep their own preprocessing.
- **Stringification (`#`) and token pasting (`##`)** have their C meanings.
  - A pasted result must form one valid token, or the expansion is an error.
  - Strings exist only in `static_assert` messages, `#error` and attribute
    arguments, so stringification is useful only there.
  - Every check, including naming, runs on the expanded declarations. Its
    diagnostic points at both the macro use and the definition that pasted the
    name.

### Declarations and statements

- Declarations are C-like: `Type name = initializer;`. The grammar allows
  exactly one declarator per declaration, including struct members and loop
  initializers.
- `static const` declares a module constant. Its initializer is a constant
  expression evaluated by the front end.
- Array sizes must be a named constant or a template parameter, never a literal:
  `float32 weights[LOBE_COUNT];`.
- Assignment (`=` and compound forms) and `++`/`--` are **statements, not
  expressions**. `if (a = b)` cannot compile, and `name = value` inside a call is
  unambiguous.
- Removed from C: the comma operator, `goto`, pointers, references, function
  pointers, unions and recursion (rejected from the call graph).
- Control flow: `if`, `else`, `for`, `while`, `do`, `switch`, `break`,
  `continue`, `return` and `discard`.
  - A `switch` case must end in `break`, `return` or an explicit
    `[[fallthrough]]`.
  - Loop and branch attributes such as `[unroll]` and `[loop]` pass through to
    Slang from an allowlist.

### Types

| Kind | Spelling | Slang lowering |
| --- | --- | --- |
| Boolean | `bool` | `bool` |
| Integers | `int32`, `uint32`; `int16`, `uint16`, `int64`, `uint64` when the target profile supports them | `int`, `uint`, `int16_t`, ... |
| Floating point | `float32`; `float16`, `float64` when the target profile supports them | `float`, `half`, `double` |
| Vectors | `float32x3`, `uint32x2`, `boolx4`, ... | `float3`, `uint2`, `bool4`, ... |
| Matrices | `float32x4x4` (rows × columns) | `float4x4`, with the orientation below |
| Arrays | `T name[COUNT]` with a named count | fixed-size array |
| Aggregates | `struct`, `enum class` with an explicit underlying type, `interface` | same |
| Resources and contexts | declared by generated render-graph bindings or [`extern slang`](#slang-interoperability) | unchanged |

- Scalar names match the C++ fixed-width aliases. Using a type the selected
  target profile lacks is a compile error naming the profile.
- `bool` may not appear in a struct that crosses the buffer boundary. Use
  `uint32`, matching the render graph's rejection of accidental host `bool`
  layouts.
- **Slang/HLSL-style aliases are accepted.**
  - The aliases: `float`, `int`, `uint`, `half` and `double`, plus the vector
    and matrix forms built from them (`float3`, `uint2`, `bool4`, `half3`,
    `float4x4`, ...).
  - Each is an exact alias of the fixed-width type with the same width:
    `float3` *is* `float32x3`. Mixing the two spellings is therefore never a
    conversion.
  - Diagnostics name the fixed-width spelling.
  - The formatter keeps whichever spelling the author wrote.

### Matrices and coordinate spaces

- **Same conventions as FoundationMath**:
  - column vectors: `positionClip = WorldToClip * positionWorld`;
  - `A * B` applies `B` first;
  - column-major storage for matrices visible to buffers.
- **`*` is the linear-algebra product** between a matrix and a vector or two
  matrices. The emitter lowers it to Slang `mul` with whichever operand order
  that storage convention needs. The convention is verified on every target by
  the L1 tests.
- **Component-wise products** stay as `*` between vectors.
  `MultiplyComponents(a, b)` spells the rare component-wise matrix product.
- **Space suffixes** (`PositionWorld`, `WorldToClip`) follow the existing naming
  rules. Distinct coordinate-space types are a candidate for L4.

### Conversions and literals

- **No implicit conversions.** That includes integer to float, float to integer,
  `bool` to or from integers, enums to integers, a scalar spread into a vector,
  and a vector shortened to fewer components.
- **Conversions are explicit and use functional syntax**:
  - `float32(count)`, `uint32(index)`;
  - `float32x3(1.0)` to fill a vector from a scalar;
  - swizzles such as `value.xyz` to shorten a vector.

  C cast syntax `(T)x` is not available.
- **Vector-scalar arithmetic** (`direction * distance`) is an operator defined
  for those operand types, not a conversion. Every other binary operator needs
  identical operand types.
- **Literals are untyped constants**, as in Go:
  - An integer literal becomes the target type when its value is exactly
    representable. A floating literal becomes a floating type when it is in
    range, rounded to nearest.
  - `float32 x = 1;` is valid. `uint32 u = -1;` and `int32 i = 1.5;` are errors.
  - Constant expressions are evaluated exactly by the front end, and overflow is
    an error.
- **Boolean operators**: `&&` and `||` take only `bool`, and conditions in `if`,
  `while`, `for` and `?:` must be `bool`. Unary `!` does not exist; write
  `condition == false`.
- **Bare `bool` operands are a style warning.**
  - Ludus style writes `condition == true` instead of a bare boolean operand.
    For Slang and C++ that rule is review-only, because the lexical gate cannot
    see types. The LUSL checker can.
  - Where it fires: a `bool` value used directly as an `if`, `while`, `for` or
    `?:` condition, or as an operand of `&&` or `||`.
  - Examples: `if (isVisible)`, `isA && isB`.
  - Each warning carries an automatic fix that inserts `== true`, and the
    formatter can apply it.
  - The warning follows the [style setting](#style-rules): an error in the
    engine repository.
  - Non-`bool` conditions such as `if (count)` are always errors, because there
    are no implicit conversions.

### Zero initialization

Assigning or initializing **the literal `0`** to a value of any type zero-fills
the whole value.

- **What zero means per type:**
  - integers become `0`;
  - floats become `+0.0`;
  - `bool` becomes `false`;
  - vectors, matrices, arrays and structs are filled recursively.
- **Forms:** `T value = 0;`, `value = 0;`, struct member defaults and `out`
  parameters.
- **Only the literal token `0` triggers it.** `0.0`, or a named constant equal
  to zero, does not.
- **Rejected targets:** resources, interface-typed values and other opaque
  values, which have no zero.
- **Lowering:** the emitter produces an explicit Slang zero construction.
  Readback tests on each target prove the memory is zeroed.

```c
ScenePayload payload = 0;          // every member zero
float32x4 accumulated[TAP_COUNT] = 0;
payload = 0;                       // reset in a loop
```

Declarations without an initializer:

- **Definite assignment.** `T value;` is legal only if the checker proves a
  write before every read on every path. The same applies to `out` parameters
  before each `return`. A possible read before a write is an error that offers
  `= 0` as a fix.
- **`= void` opts out.** `T value = void;` leaves the value uninitialized and
  skips the analysis, as in D. Use it for large arrays filled by loops the
  analysis cannot follow. Reading it before writing it gives an unspecified
  value.

`groupshared` declarations must say `= 0` or `= void`:

- `= 0` makes the emitter insert a cooperative clear at the start of every
  compute entry point that reaches the declaration. Each thread clears a strided
  slice, then a group barrier runs before any user code.
- That needs a constant thread-group size.
- The build report shows the cleared bytes per entry point, so the cost is
  visible.

### Functions, parameters and calls

Parameters are copied: `in` (the default), `out` (written, must be definitely
assigned) and `inout`. There are no references.

Call sites mark writable arguments, positionally or by name:

```c
float32x3 radiance;  // no initializer: the out argument below is its first write
bool hasHit = TraceRadiance(scene, ray, out radiance);
// Equivalent call with named arguments:
// bool hasHit = TraceRadiance(scene = scene, ray = ray, outRadiance = out radiance);
```

- **Aliasing:** one variable may not reach two writable parameters of a call.
  Overlapping parts count, such as `hit` and `hit.NormalWorld`. Copy-in/copy-out
  therefore always matches what a reference would have done.
- **Named arguments** follow Python's call rules:
  - positional arguments come first, then named ones;
  - each parameter is bound once, and every parameter without a default is
    bound;
  - defaults are constant expressions and may appear on any parameter.
- **Evaluation order** is the order written, left to right. When that differs
  from parameter order, the emitter introduces temporaries, so the emitted Slang
  never relies on named-argument support.
- **Parameter names are part of a function's interface.** Renaming one breaks
  named callers, and the diagnostic names the old and new spelling.
- **Overload resolution** considers names as well as types. Named arguments
  work for interface methods and constructors too.

Struct values are built with designated initializers. Omitted members take their
declared default or zero, consistent with the zero rule. Positional brace
initialization is not available:

```c
SurfaceHit hit = { .PositionWorld = positionWorld, .NormalWorld = normalWorld };
```

Function attributes from an allowlist (for example `[noinline]`) pass through
to Slang with their existing target caveats.

### Interfaces, concepts and templates

LUSL uses a hybrid of nominal interfaces and structural concepts:

| | `interface` | `concept` |
| --- | --- | --- |
| Contains | Member functions, and constant members, a type must provide | A compile-time boolean over interface conformance, constants and other concepts |
| A type opts in | Explicitly (`struct X : Material`) or with `extension` | Never; the concept is evaluated |
| Declares callable members | Yes, and only interfaces do | No |
| Lowers to | A Slang `interface`, 1:1 | Nothing; the front end resolves overloads and specializes code |

```c
interface Material
{
    float32x3 Evaluate(in float32x3 normalWorld, in float32x3 viewWorld, in float32x3 lightWorld);
};

interface ClearcoatMaterial : Material
{
    float32x3 EvaluateClearcoat(in float32x3 normalWorld, in float32x3 viewWorld);
};

struct CarPaint : ClearcoatMaterial
{
    static const uint32 LOBE_COUNT = 3;
    // ...
};

extension FoundationGgx : Material { /* existing Slang type */ }

concept Opaque<T> = (T : Material) && (T::LOBE_COUNT == 1);
concept Layered<T> = (T : ClearcoatMaterial) && (T::LOBE_COUNT > 1);

static_assert(Layered<CarPaint> == true, "CarPaint must stay layered");

template <Material M>
float32x3 Shade(in M material, in SurfaceHit hit) { /* general path */ }

template <Layered M> // Layered implies Material, so this overload is more constrained and wins
float32x3 Shade(in M material, in SurfaceHit hit) { /* layered path */ }

template <Material M>
float32x3 ShadeWithFog(in M material, in SurfaceHit hit, in FogParameters fog)
{
    float32x3 color = Shade(material = material, hit = hit);
    if constexpr (Opaque<M> == true)
    {
        color = ApplyFog(color = color, fog = fog);
    }
    return color;
}
```

- **Conformance checks member signatures only.** Value conditions such as
  `LOBE_COUNT > 1` live only in concepts.
- **Template forms:** `template <Concept T>` and
  `template <typename T> requires expression`. Constant integral template
  parameters (`template <uint32 COUNT>`) can size arrays.
- **Overload choice** follows C++20 ordering by constraints over normalized
  atomic constraints. If neither candidate's constraints cover the other's, the
  call is an ambiguity error; the compiler never picks silently.
- **`if constexpr`** checks the syntax of the discarded branch but does not
  type-check or instantiate it.
- **Lowering:**
  - A template constrained only by interfaces, with no value constraint or
    `if constexpr`, lowers to a Slang generic.
  - Otherwise the front end emits one specialized copy per concrete type that
    reaches it, with deterministic names derived from the declaration and type
    names.
- **Specializations are configurations.**
  - The concrete types are closed by the package: material implementations are
    listed in the package closure, as the render graph's
    [compilation choices](render-graph-authoring.md#author-owned-compilation-choices)
    already require.
  - Every specialization counts toward that budget and appears in the build
    report.
  - LUSL never creates a configuration the author did not list.
- **Runtime dispatch** (a `dynamic Material` parameter) is deferred until Slang
  dynamic dispatch is qualified on every target profile.

### Entry points and authored functions

- **Authored functions first.** The first use is the function a render-graph
  pipeline names (raygen, closest-hit, miss and compute callbacks). The
  generated wrappers call it as they call Slang functions today.
- **Compute entry points** take attributes with named constants:
  `[stage(compute), threads(TILE_WIDTH, TILE_HEIGHT, 1)]`. System values are
  parameter semantics, for example
  `in uint32x3 dispatchThread : DispatchThreadId`.
- **Vertex and fragment stages** arrive in L3, with the binding model.
- **Ray tracing** uses the shared scene shader API (`TraceClosest`,
  `TestVisibility`, ...) through its LUSL declarations. Native ray queries keep
  their existing out-of-line requirements on Metal.

### Resources and bindings

L1 adds no binding syntax:

- **Bindings come from the render graph.** Its build already generates binding,
  schema and entry wrappers.
- **LUSL code receives typed contexts.** `SceneContext`, `RayLaunch` and
  generated schema structs arrive through their LUSL declarations.
- **A standalone binding model is an L3 design.** Its constraints are already
  known:
  - emit bindings in ascending slot order, because Slang numbers Metal buffers
    by declaration order;
  - keep the RHI shader boundary's interface annotations;
  - no bindless until the RHI provides it.
- **The schema stays authoritative.** Generated schemas remain the single source
  of host/shader layout. LUSL does not become a second schema owner. The schema
  generator gains a LUSL declaration output instead.

### Slang interoperability

Ludus Slang code shares definitions with `#include`. A LUSL file cannot parse
Slang text, so Slang declarations reach LUSL through `extern slang` blocks in
`.luslh` files:

```c
// ludus/foundation/math.luslh
extern slang "math.slang"
{
    static const float32 PI;
    float32 ComputeLuminance(in float32x3 linearRgb);
};
```

- **Lowering:** the emitter writes `#include "math.slang"` into the generated
  Slang and calls the declarations by their unchanged names.
- **Checking:** the Slang compile of the emitted module verifies each
  declaration. A mismatch is a Slang diagnostic mapped back to the `extern`
  line.
- **SDK declarations:**
  - L1 ships hand-written `.luslh` files for the shared scene API and
    FoundationMath. A test calls every declaration on every target.
  - L3 generates them deterministically from Slang reflection at SDK build time.
- **Slang can use LUSL too.** A `.slang` file can include the generated Slang
  of a LUSL module. Public LUSL declarations keep their names; only overloads
  and specializations get derived names.

### Defined behavior

Targets disagree on several operations. LUSL defines each one once:

| Case | Constant expression | Runtime |
| --- | --- | --- |
| Unsigned overflow | Wraps | Wraps |
| Signed overflow | Error | Two's-complement wrap on every target ([below](#signed-overflow)) |
| Integer division or modulo by zero | Error | Unspecified value; the emitted code never traps |
| Shift count at or above the bit width | Error | Count masked to `width - 1` by the emitter |
| Local or `groupshared` array index out of range | Error | Development builds clamp and latch a failure ([below](#development-bounds-checks)); release builds give an unspecified value |
| NaN and infinity | No literals or sentinel constants | Test finiteness through bit-inspection functions; Metal compiles with fast math |
| Float to integer out of range | Error | Unspecified value |
| Two writable arguments alias | Compile error | — |
| Read before definite assignment | Compile error, unless declared `= void` | — |

#### Signed overflow

Signed `+`, `-` and `*` wrap on every target, as unsigned arithmetic does:
`INT32_MAX + 1` is `INT32_MIN`.

- **Most targets need nothing extra.** SPIR-V, HLSL/DXIL and WGSL already
  define signed arithmetic as wrapping, so the emitter writes the operation
  directly.
- **Metal needs a rewrite.** The Metal Shading Language is based on C++, where
  signed overflow is undefined behavior and the compiler may assume it never
  happens.
  - For the Metal target only, the emitter writes the operation through
    unsigned arithmetic and converts back: `int32(uint32(a) + uint32(b))`.
  - GPUs use the same instruction for signed and unsigned addition, so this is
    expected to cost nothing, while the emitted Metal source gets longer.
- **L1 measures one risk.** The only expected loss is loop optimizations that
  rely on signed counters never overflowing. L1 compares Metal code generation
  and timing for loops with signed counters before and after the rewrite.
  - If the cost is measurable, the design records an explicit exemption, such
    as loop counters, with the measurement.
  - The language rule itself stays "wraps".

#### Development bounds checks

- **Constant indices** into fixed-size local and `groupshared` arrays are
  checked at compile time in every build: `weights[LOBE_COUNT]` on a
  `LOBE_COUNT`-element array is an error.
- **Runtime indices in development builds:**
  - **Check:** the emitter adds a comparison before each such access.
  - **On failure:**
    - the index is clamped, so the access stays inside the array;
    - a per-invocation failure flag is set, with a failure code identifying the
      access.
  - **No signature changes.** The flag is a private per-thread global, so
    function signatures and Slang interoperability are unchanged.
  - **Reporting:** the generated render-graph wrapper combines the flag with the
    existing invocation failure latch of the
    [shared scene shader contract](shared-scene-shader-contract.md#public-shader-operations).
    The invocation shows the error color, and the failure code maps back
    through the source map to the LUSL line.
  - **Opting out:** a development build can turn the checks off. This helps
    when a bug depends on the generated code and the extra comparisons hide it.
- **Release builds emit no checks.** Development and release are already
  separate build keys, so checked and unchecked artifacts never mix.
- **Costs:** development shaders run slower, with an extra comparison and a few
  extra registers per checked access.
- **Buffers are out of scope.** Buffer resources (`StructuredBuffer` and
  similar) are not checked: the graphics APIs define their out-of-range
  behavior, and their sizes are known only at run time.

## Tooling

- **Formatter:** `luslc --format` is a canonical AST formatter that keeps the
  repository's initializer-brace rules. `scripts/check --format` and
  `source_policy.py` cover `.lusl` and `.luslh`. Checks the compiler enforces
  are not repeated lexically.
- **Naming and bare `bool` operands** are [style rules](#style-rules), checked
  by the compiler.
- **Diagnostics are structured.** Each carries file, line, column, stable code
  (`LUSL0001`), message and, where safe, a fix. Examples: `!x` offers
  `x == false`; an implicit conversion offers the explicit constructor; a read
  before assignment offers `= 0`. They feed the render graph's mapped
  diagnostics and the editor's quick fixes.
- **Debugging:** the emitted Slang carries `#line` directives, so Slang
  diagnostics, debug information and frame-debugger source views point at LUSL
  lines. Each target's debug view is qualified separately.
- **Editor:**
  - a language registry record for `.lusl` and `.luslh`;
  - a Ludus-owned tree-sitter grammar under the
    [ADR 0026](../decisions/0026-tree-sitter-syntax-dependency.md) recipe and
    pinning rules;
  - a language-server mode of the same library in L4.

### Style rules

The checker separates style rules from language rules:

- **Style rules:**
  - **Naming**, from the engine's Slang naming rules:
    - PascalCase types, functions and struct members;
    - lowerCamelCase parameters and locals;
    - `outName` out parameters;
    - UPPER_CASE constants;
    - boolean names that read as questions (`isVisible`, `hasHit`).
  - **[Bare `bool` operands](#conversions-and-literals).**
- **Language rules** are everything else:
  - no unary `!`;
  - no implicit conversions;
  - call-site `out`/`inout` marks and the aliasing check;
  - definite assignment;
  - one declarator per declaration;
  - named array sizes.

  They are always errors, everywhere.

Style rules follow a style setting:

| Setting | Effect | Where it applies |
| --- | --- | --- |
| `strict` | Style findings are errors | Always in the engine repository; the default for game projects |
| `warn` | Style findings are warnings | Game projects that choose it |
| `off` | Style findings are not reported | Game projects that choose it |

- **The setting is project-wide.** It lives in the game project's
  configuration; L1 defines the key.
- **No per-file switch.** Code borrowed from another engine or library
  normally stays in Slang or GLSL. It reaches LUSL through
  [`extern slang`](#slang-interoperability) instead of being relaxed in place.
- **Typical relaxed projects:**
  - teams porting shaders that use Unreal-style or `snake_case` names, or `g_`
    globals;
  - studios with an existing convention, such as camelCase functions or
    booleans without question prefixes.

## Determinism and build identity

- **Byte-identical output.** The same snapshot, configuration values and
  compiler version produce the same Slang and source map, on every host.
- **Stable derived names.** Overload and specialization names come from
  declaration order and type names, never from addresses or hash-map order.
- **Build key additions.** The render-graph build key gains the LUSL compiler
  version, the hash of the emitted Slang, and the configuration macros each
  file read. Moving a node or editing runtime values still rebuilds nothing.

## Testing

| Kind | Evidence |
| --- | --- |
| Golden | LUSL source to emitted Slang text, per language feature |
| Diagnostics | One failing source per rule, with expected code, position and fix |
| Targets | Emitted Slang compiled through `ShaderCompiler` for `Spirv`, `SpirvRayQuery`, `Wgsl` and `Metal`, validated by spirv-val and the WGSL validator; DXIL in Windows CI |
| Differential | A ported built-in render-graph shader produces the same pixel hashes as its Slang original |
| Zero rule | GPU readback proves zeroed locals, arrays, structs and cleared `groupshared` memory |
| Signed overflow | GPU readback proves wrapping on every target, including Metal; L1 records the Metal loop code-generation and timing comparison |
| Bounds checks | A development build clamps an out-of-range index and shows the error color with the mapped LUSL line; the release build of the same source contains no checks |
| Style setting | The same sources under `strict`, `warn` and `off` give errors, warnings and no findings, while language-rule errors stay identical |
| Determinism | Two compilations, on two hosts, compare byte for byte |
| Robustness | Bounded fuzzing of the preprocessor and parser under ASan/UBSan: no crash, bounded diagnostics |

## Delivery plan

| Phase | Scope | Acceptance |
| --- | --- | --- |
| L0 | This design and ADR 0027 | Reviewed and accepted into `develop` |
| L1 | `Ludus::ShaderLanguage` and `luslc`: preprocessor (with `#` and `##`), parser, types and their aliases, untyped literals, the zero rule, definite assignment, `in`/`out`/`inout` with call-site marks, named arguments, designated initializers, `extern slang`, signed-overflow lowering, development bounds checks, style rules and setting, Slang emitter, source map, formatter; authored render-graph functions and compute entry points | Golden, diagnostic, target, signed-overflow, bounds-check, style-setting, determinism and fuzz tests; one built-in graph shader ported with identical pixels |
| L2 | Interfaces, concepts, templates, `if constexpr`, constraint ordering, specialization budget report, `groupshared` clear | A material-permutation sample matches its Slang-interface version |
| L3 | Vertex and fragment stages, the standalone binding model, `.luslh` generated from Slang reflection, LUSL output from the schema generator | A raster graph ported with identical pixels |
| L4 | Language server, tree-sitter grammar, uniformity qualifiers, coordinate-space types, debug-view qualification, dynamic dispatch evaluation | Code editor acceptance for LUSL |

## Resolved questions

The design's six open questions were resolved by the author on November 4,
2026:

| Question | Decision | Section |
| --- | --- | --- |
| Accept `float3`-style aliases? | Yes, as exact aliases of the fixed-width types | [Types](#types) |
| Preprocessor `#` and `##`? | Yes, with C semantics | [Preprocessor](#preprocessor) |
| Signed overflow on Metal | Wrap on every target; Metal lowers through unsigned arithmetic, and L1 measures loop code generation | [Signed overflow](#signed-overflow) |
| Warn on bare `bool` operands? | Yes, as a style warning with an automatic `== true` fix; an error in the engine repository | [Conversions and literals](#conversions-and-literals) |
| May game projects relax naming? | Yes, through a project-wide `strict`/`warn`/`off` style setting. The engine is always `strict`, and language rules are never relaxed | [Style rules](#style-rules) |
| Development bounds checks on local arrays? | Yes, for local and `groupshared` arrays: clamp and latch a mapped failure. Release builds emit no checks | [Development bounds checks](#development-bounds-checks) |

## References

These sources informed the design. Code comments will cite them where each idea
is implemented.

- Yong He, Kayvon Fatahalian and Tim Foley, "Slang: language mechanisms for
  extensible real-time shading systems," *ACM Transactions on Graphics* 37(4),
  SIGGRAPH 2018, [doi:10.1145/3197517.3201380](https://doi.org/10.1145/3197517.3201380).
  Interfaces with explicit conformance and generics specialized at compile time.
  LUSL lowers interfaces to them unchanged.
- ISO/IEC 14882:2020 (C++20), [temp.constr] "Template constraints".
  Concepts, `requires` and ordering overloads by constraints. LUSL keeps that
  ordering, but member requirements stay in interfaces.
- The Go Programming Language Specification,
  ["Constants"](https://go.dev/ref/spec#Constants). Untyped constants, which
  replace implicit conversions for literals.
- D Language Specification, ["Void Initializations"](https://dlang.org/spec/declaration.html#void_init).
  The `= void` opt-out from initialization.
- The Python Language Reference, ["Calls"](https://docs.python.org/3/reference/expressions.html#calls).
  Positional-then-keyword argument binding.
- ECMA-334 C# Language Specification, "Definite assignment" and output
  parameters. The definite-assignment analysis and the `out` marker at call
  sites.
