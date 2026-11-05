# ADR 0027: Ludus Shading Language as a front end that emits Slang

## Status

Proposed. It takes effect when the maintainer accepts this ADR into `develop`.
No compiler code lands before acceptance. The
[LUSL design](../architecture/shading-language.md) owns the language rules,
compiler architecture and delivery plan; this ADR records the decision and the
alternatives.

## Context

Ludus shaders are written in Slang and compiled by the pinned Slang 2026.1.2
chain ([ADR 0010](0010-shader-toolchain-feasibility.md),
[ADR 0013](0013-browser-glsl-es-shader-toolchain.md)). `Ludus::ShaderCompiler`
owns the Slang session that produces SPIR-V, ray-query SPIR-V, WGSL and Metal.
GLSL ES and DXIL come from their existing paths. The render graph build owns
configurations, budgets, build identity and packages. About 450 `.slang` files
depend on this chain, as do the render-graph wrappers and the editor's Slang
language service.

The engine's shader style rules are enforced only partly. `source_policy.py` is
lexical, and several rules need type information it does not have: explicit
boolean comparisons, no implicit conversions, `out` intent and aliasing, and
reads before writes. The author also wants features Slang lacks or spells
differently:

- named arguments at call sites;
- `= 0` zero-filling any type;
- a preprocessor restricted to declared configurations;
- C++-style concepts to choose material and permutation variants.

## Decision

- **Create LUSL, the Ludus Shading Language.** It is C-based, with `.lusl`
  modules and `.luslh` include-only declarations.
- **Its compiler is a front end that emits Slang source.**
  - The pipeline is: preprocessor, parser, semantic checker, a typed LUSL IR,
    then a deterministic Slang emitter with a source map.
  - The existing `ShaderCompiler` and render-graph build consume the emitted
    Slang unchanged, so LUSL gets every current target, the reflection and
    layout checks, and the package format.
- **The LUSL IR is the seam for later direct backends** (SPIR-V or DXIL). They
  are not part of this decision.
- **Two new owners:**
  - `Ludus::ShaderLanguage` (`modules/tooling/shader_language`): the C++23
    library, following the engine rules.
  - `luslc` (`tools/luslc`): a command-line driver over the same library.
  - `ShaderCompiler` accepts `.lusl` files in its request snapshot and lowers
    them before the Slang session. Offline and live builds share one compiler
    core, and the editor gets no second compiler.
- **Language decisions:**
  - **Kept:** C-like declarations, one declarator per declaration, and a C
    preprocessor whose external macros come only from selected configuration
    values.
  - **Removed:** unary `!`, implicit conversions, references and pointers.
  - **Untyped literal constants** take the type of their target.
  - **Literal `0`** zero-fills any value.
  - **Definite-assignment analysis**, with an explicit `= void` opt-out.
  - **`in`/`out`/`inout` copy parameters**, with writable arguments marked at
    the call site and rejected when they alias.
  - **Named arguments** and designated initializers.
  - **Nominal interfaces** that lower 1:1 to Slang interfaces, with
    **structural concepts** on top for selecting variants.
- **Configuration ownership is unchanged.** Template specializations and
  preprocessor options are render-graph configurations: they count toward the
  authored budget, and LUSL never creates one the author did not list.
- **Slang interoperability:** LUSL reads Slang declarations through
  `extern slang` blocks. Slang files can include the generated Slang of a LUSL
  module. Existing Slang code is not rewritten.

## Alternatives considered

| Alternative | Why it was not chosen |
| --- | --- |
| Emit HLSL or GLSL text | DXC or glslang could produce DXIL and SPIR-V. Metal, WGSL and GLSL ES would need newly pinned translators. Slang features the render graphs already use (interfaces, generics, the shared scene API) would be lost, and the Slang interop would break. |
| Emit SPIR-V directly from the LUSL IR | Requires SSA construction, control-flow structurization, a SPIR-V writer and its validation. Other targets would need SPIRV-Cross, Tint or naga, and DXIL only exists through Mesa's spirv-to-dxil. Months of work before the first shader, with a second toolchain to pin. The LUSL IR keeps this open for later. |
| Emit DXIL or LLVM IR | DXIL is LLVM-bitcode based and the upstream LLVM DirectX backend is still experimental. It covers one target. |
| Emit Slang IR | Slang has no public API for constructing its IR, and its serialized modules are internal formats tied to a compiler version. |
| Extend or fork Slang | Every Slang update would need the fork rebased and requalified, and the pinned compiler would no longer be upstream's. |
| Keep Slang and add lint rules | Lexical checks cannot see types, so conversions, `out` aliasing and definite assignment stay review-only. Named arguments and the zero rule cannot be added by linting. |
| An embedded C++ shader DSL | Ties shader authoring to the host compiler and build. It would not run in the browser editor or `luslc` without a C++ toolchain, and the diagnostics would be C++ template errors. |

## Consequences

- **What authors get:** a shader language whose rules are compiler errors with
  structured fixes, and every existing target from day one.
- **New maintenance:** a front-end library, a formatter, a grammar and,
  later, a language server.
- **Coupling to Slang:** LUSL semantics must stay expressible in the pinned
  Slang. A Slang update re-runs the LUSL golden and target tests alongside the
  existing shader qualification.
- **Shipping is unaffected.** Runtimes still consume cooked packages and need
  neither compiler.
- **Build identity grows.** Render-graph build keys gain the LUSL compiler
  version, the emitted-Slang hash and the configuration macros read.
- **Hand-maintained declarations until L3.** The shared scene API and
  FoundationMath need `.luslh` declaration files, kept in sync by tests that
  call every declaration on every target. L3 generates them from Slang
  reflection.
- **Not fixed by this ADR:**
  - the exact grammar, limits and diagnostic codes, set in L1;
  - the standalone binding model, set in L3;
  - the open questions listed in the
    [design](../architecture/shading-language.md#open-questions).
