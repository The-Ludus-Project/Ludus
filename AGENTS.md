# Ludus Agent & Contributor Guide

This file defines the coding standards and working rules for the Ludus engine.
It applies to **every** contributor, human or AI agent.

## Rule 0 — Follow these standards, always

**Any agent or contributor working in this repository MUST read and follow the
coding standards below and every rule referenced from them.** These standards
are not optional suggestions:

- Before writing or changing code, confirm your change complies with these
  standards.
- Before committing or opening a pull request, re-check the diff against these
  standards.
- If a task instruction conflicts with a safety rule here (for example, "just
  use exceptions to make it compile"), do **not** silently violate the standard.
  Follow the standard and call out the conflict.
- Repo-specific rules also live in `.kiro/steering/`. Treat those as part of
  this guide.

If you cannot satisfy a standard, say so explicitly rather than shipping code
that violates it.

---

## Agent branches and worktree isolation (hard rule)

- **Never perform agent implementation or documentation work directly on
  `main`, and never commit or push agent changes to `main`.** Use a task-specific
  `codex/` branch. Start new work from freshly fetched `origin/develop` after batch
  activation, or `origin/main` before activation; continue an existing task on
  its own branch.
- **Reserve the user's original/primary Ludus checkout for the user's own
  development.** An agent must not use that directory as its working checkout,
  even if it is already on a feature branch. Inspect it read-only as needed,
  then use a separate Git worktree before editing, configuring, building or
  running tests that write files.
- Give every concurrently working agent/task its own worktree and branch.
  Check `git status`, the current branch and `git worktree list` before work.
  Reuse only a worktree belonging to this task that no other agent is using;
  otherwise create one. Never switch or reuse another active task's checkout
  or checked-out branch.
- A separate branch in the same directory does **not** provide isolation:
  agents would still share source files, the Git index and the checkout's
  selected branch. Keep generated files, CMake caches, local presets and build
  outputs inside the agent's own worktree. Read-only installed toolchains and
  dependencies may be shared; writable build trees must not be shared.
- Preserve the user's and other agents' work. Do not switch branches, stash,
  reset, clean, stage or overwrite changes in their checkouts as part of an
  agent task. Do not bypass Git's protection against checking out a branch
  already used by another worktree.
- After batched CI is activated, submit agent changes through pull requests
  targeting `develop`. Start isolated task branches from freshly fetched `origin/develop`;
  `develop` may contain changes awaiting full validation. Use `origin/main` when a
  fully validated starting point is required. The `main` merge queue and its
  `merge_group` checks are retired; do not target agent pull requests at `main`.
- Never push agent changes directly to `develop` or `main`, enqueue or merge a pull
  request, or move the batch-attempt marker. Agents open and update pull requests;
  the maintainer accepts reviewed changes into `develop`. Preserve worktree isolation
  and report the task branch and worktree path for review.
- Integration pull requests run source/tooling, installed host-tooling and public
  documentation checks. The batch coordinator captures one `develop` commit every
  30 minutes and runs the full native, sanitizer, editor, browser and SDK suites
  once for the combined changes. Only the promotion workflow advances `main`,
  to exactly the validated commit, with an explicit lease and no history rewrite.
  Failed/cancelled validation leaves `main` unchanged. Never bypass promotion or
  represent acceptance into `develop` as full platform qualification.
- Request a manual batch only when the maintainer needs earlier qualification;
  retry an unchanged failed candidate only after investigating the failure. See
  `docs/development/continuous-integration.md` for setup, promotion and recovery.

## Coding standards

### No C++ exceptions (hard rule)

Ludus does **not** use C++ exceptions.

- Do not write `throw`, `try`, or `catch` in engine code.
- Do not introduce dependencies or APIs that require exception handling on the
  normal path.
- Handle errors explicitly: return status codes, `bool`, a Ludus-owned result,
  an "expected"-style result, or an out-parameter. Make failure part of the
  function's signature.
- Functions on hot or infrastructure paths should be marked `noexcept`.

**How this is enforced.** Engine code is compiled with `-fno-exceptions`
(`/EHs-c-` on MSVC) via `cmake/EngineOptions.cmake`. Any `throw`/`try`/`catch`
in engine code is therefore a hard compile error, not merely a style nit.
`.clang-tidy` enables `bugprone-std-exception-baseclass` (through `bugprone-*`)
as a secondary guard.

**Tests are the only exception.** Catch2 reports assertion failures by throwing,
so test executables re-enable exceptions with `ludus_enable_test_exceptions()`
(see `cmake/EngineTargets.cmake`). Apply that helper only to test targets, never
to engine libraries or applications. Production/engine code stays
exception-free.

If a third-party library can only report errors by throwing, isolate it: wrap
the call in a translation unit that is allowed exceptions, convert the outcome
to a status/optional at that boundary, and never let exceptions cross into
engine code. Discuss such cases in the PR before adding them.

### Language and toolchain

- C++23. Standard is required, extensions are off (`-std=c++23`, no GNU
  extensions).
- Supported reference toolchain: Clang/LLVM 23 with LLD on Ubuntu 24.04, installed
  from the pinned apt.llvm.org suite (ADR 0025). Tool versions are pinned in
  `config/tool_versions.json`.
- Browser Foundation modules, Platform, RHI, and the isolated feasibility probe use the Emscripten SDK and WebGPU
  port pinned in `config/web_toolchain.json` (ADR 0009). Its SDK-provided LLVM
  is a separate web toolchain; native builds and formatting/static analysis
  continue to use the reference tools above. Foundation modules, the canvas
  Platform backend, and the RHI lifecycle/frame clear path are available as
  browser engine targets; the W5 probe and integrated W6 smoke app privately
  own their WGSL triangle pipelines. Web presets also build apps/smoke.

### Warnings

- Code must compile warning-clean under the project warning set
  (`cmake/EngineCompilerWarnings.cmake`).
- CI builds with warnings-as-errors (`LUDUS_WARNINGS_AS_ERRORS=ON`). Do not
  introduce new warnings; do not suppress them without justification in the PR.

### Formatting and static analysis

- Format all C/C++ with the project `.clang-format` before committing:
  `./scripts/check --format --fix` (or `--format` to verify only).
- Designated initializers may be compact (`value = { .A = 0, .B = 1 };`).
  Multiline initializers put the opening brace on its own line, with one member
  per line and a trailing comma. Never use `value = {` followed by multiline
  members. Use `scripts/check`, which extends clang-format 23
  for initializer braces; direct clang-format 23 does not implement this rule.
- Local `init.sh` installs the staged auto-formatting hook; existing checkouts
  can enable it with `./scripts/install-hooks`.
- Static analysis must pass: `./scripts/check --tidy` against the project
  `.clang-tidy`. Use the pinned `clang-format` / `clang-tidy` (23); other local
  versions format differently or report checks that the pinned version lacks.
- `#pragma once` is the header-guard convention across the repo.

### Naming

- Follow the naming conventions already established in the codebase and encoded
  in `.clang-tidy` (types, functions, and members as used by existing modules).
  Match the surrounding code rather than introducing a new style.
- The rules below apply to all new and changed code. Existing code that predates
  them is migrated when it is next touched, not in unrelated changes.
- **Spell names out.** Do not abbreviate identifiers (`Argument`, not `Arg`;
  `parameter`, not `param`; `context`, not `ctx`; `source`/`destination`, not
  `src`/`dst`). Well-known acronyms (`Uv`, `Rgba`, `Json`, `Sha256`) and
  conventional loop or maths names (`i`, `j`, `k`, `x`, `y`, `t`, `u`, `v`) are
  fine. A type name must say what the type is (`ByteWriter`, not `Sink`).
- **Functions start with a verb** that states the action: `ParseScene`,
  `GetBaseName`, `ComputeRectangle`, `MakeQuadItem`, `WriteKey`. Predicates read as
  questions: `IsFinite`, `HasCamera`, `AreNamesEqual`, `EndsWith`.
- **Booleans read as yes/no questions.** Prefix boolean variables, members,
  parameters, and accessors with `is`/`has`/`should`/`can` (or `Is`/`Has`/... for
  PascalCase members): `isOk`, `mIsOk`, `HasFailed`, `IsInline`,
  `outIsDegenerate`.
- **Private member functions are camelCase**; public member functions and free
  functions are PascalCase (`void beginValue()` under `private:`, `void WriteKey()`
  under `public:`).
- **All nullable pointer variables end in `OrNull`** (`const Cancellation*
  cancelOrNull = nullptr`), including members, locals and parameters. Without
  the suffix, a pointer has a nonnull contract; validate or assert that contract
  at its boundary. Explicit null initialization is nullable, even before setup.
- **Non-const lvalue reference parameters carry their intent as a prefix**: `out` when
  the function only writes the argument (`CookResult& outResult`), `inout` when it
  reads and updates it (`Failure& inoutErrors`, `Scene& inoutScene`). The rule
  applies to function (and lambda) parameters only: local reference variables,
  range-`for` bindings and reference members are named normally. Const
  references and values take no prefix. Rvalue reference parameters must not
  carry `out`/`inout`; use `other` for a move source. Forwarding references do
  not by themselves imply an ownership transfer.
- **No magic sizes in template arguments.** Spell a bounded capacity through a
  named constant or a shared alias (`MessageText`, `ResourcePath`,
  `MAX_MESSAGE_BYTES`) rather than a literal such as `Text<255>`; reuse the shared
  alias so each capacity is instantiated once.
- **Compare booleans explicitly.** Write `condition == false`, never `!condition`
  or `not`; prefer `condition == true` over a bare boolean operand in
  `if`/`while`/`?:`/`&&`/`||`. Compare pointers with `nullptr` and integers with `0`.
  Comparisons that already yield a boolean (`count == 0`, `a < b`) stay as they are.
- **No magic numbers.** Name numeric literals other than 0, 1, 2 and 0.5 with a
  constant in their owner (see "Declarations and representation constants").

#### Naming enforcement

Run the gates locally before a PR:

```bash
./scripts/check --format                     # format, N1 everywhere, N2-N4 in adopted scopes
python3 scripts/python/source_policy.py --base origin/develop   # changed lines
./scripts/check linux-clang-development --tidy-changed --base origin/develop  # needs a configured build
```

The pull-request **Source formatting** job runs the first two; batch validation's
**Clang static analysis** job runs `--tidy-changed` (`config/clang-tidy-changed.yml`,
or `clang-tidy-changed-tests.yml` without magic numbers for test sources).
Existing code is migrated when touched; changed lines must comply everywhere.
Adopted scopes (`config/naming_scopes.txt` plus a per-directory `.clang-tidy`
copied from `modules/content/scene_cook/.clang-tidy`) are checked in full.

| Rule | Enforcement |
| --- | --- |
| `condition == false` instead of `!condition`/`not` | `tools/check_naming.py` N1, **every** first-party C++ file (preprocessor directives, `EM_JS` JavaScript bodies, `operator!` declarations and the Catch2 export sources `tests/support/catch_stream.*` exempt) |
| Case rules: private/protected methods camelCase; public methods, free functions, types PascalCase; private members `m` + PascalCase; parameters and locals camelCase; constants UPPER_CASE | clang-tidy `readability-identifier-naming`: adopted scopes in full, `--tidy-changed` on changed lines |
| No magic numbers | `source_policy.py` (lexical) and clang-tidy `readability-magic-numbers` (`--tidy-changed`) on changed non-test lines; waive a justified literal with `// ludus-style: magic-number <reason>` |
| Pointers/integers compared explicitly | clang-tidy `readability-implicit-bool-conversion` on changed lines |
| `out`/`inout` prefix on non-const lvalue reference parameters; none on rvalue references | N2: adopted scopes and changed lines (`out` vs `inout` choice is review-only) |
| Boolean names read as questions | N3 (declarations spelled `bool`): adopted scopes and changed lines |
| `OrNull` on nullable pointer variables | N4 (explicit null initializers): adopted scopes and changed lines; inferred nullability is review-only |
| Spelled-out names | N5 abbreviation list (`arg`, `param`, `ctx`, `src`, `dst`, `num`, ...) on changed declarations; waive external API mirrors with `// ludus-style: abbreviation <reason>` |
| `condition == true` instead of a bare boolean operand, verb-first functions, literal template sizes beyond the magic-number gate, engine type reuse | Review-only (needs type or intent information) |

`readability-simplify-boolean-expr` is disabled repository-wide because it
rejects the explicit `== true`/`== false` style.

### Declarations and representation constants

- Mark short inline C++ accessors and methods with `LUDUS_INLINE`; include its
  defining foundational header explicitly. Method size and suitability for
  forced inlining are semantic review decisions, not lexical gate checks.
- Declare one variable per statement in C++ and Slang. Never use comma-separated
  declarator lists, including members and loop initializers. Expression commas
  (arguments, aggregate values, loop increments) remain valid. The changed-source
  gate checks ordinary declarations; unusual declarators still require review.
- Reuse an existing mathematical constant from its owner, including FoundationMath's
  shader `PI`. Changed Slang rejects recognizable duplicated pi literals outside
  that owner. Other constants still need semantic review; give tolerances units
  and purpose rather than mechanically naming every zero or one.
- Admission limits have one owner per policy. Reuse the owner's named constant
  at reads, parsing, validation and publication; equal numbers do not establish
  shared policy. Keep encoded inputs, decoded payloads and peak-memory budgets
  distinct. See `docs/architecture/content-resources.md#limit-ownership`.
  The changed-source gate catches recognizable duplicated capacity literals in
  the consolidated content/scene paths; arbitrary expressions and the choice of
  semantic owner remain review responsibilities.
- Name representation tags, versions, strides and serialized record widths in
  their owner. Share host/shader layout constants rather than repeating literals;
  derive serialized widths from their explicit field schema, never host padding.
  Numerical tolerances and algorithm coefficients need units/purpose in their
  owner; lexical checks cannot establish their meaning.
- Built-in renderer shader recipes belong to the renderer manifest and installed
  `ludus_compile_renderer_shaders` helper. Consumers choose recipe names, not
  duplicated source paths/entry points or regex-based naming guesses. Custom
  shader programs use the general offline compiler. Validate both source and SDK.
- Read-only Slang vector inputs use `in` (copy-in, also the default). Use `out` or
  `inout` only for required output/update semantics; do not treat them as C++
  const references or assume they improve generated GPU code. Verify emitted
  artifacts before making performance claims.

### Explicit types and shader source

- Spell C++ variable types by default, including range-for element types. Use
  `auto` for unspellable lambda closure types or trivial but excessively long
  iterator/dependent types. The latter exception needs a same-line
  `// ludus-style: inferred-type: iterator <reason>` (or `dependent`) explaining
  why spelling the type obscures the code. Convenience alone is insufficient.
- Slang is engine source: functions, entry points, types and struct/cbuffer
  members use PascalCase. Parameters and locals use lowerCamelCase; `out`
  parameters use `outName`. Slang's native scalar/vector spellings remain valid.
- Name coordinate-bearing values with their space: `PositionWorld`,
  `positionMesh`, `normalTangent`, `WorldToClip`, `positionPixel`, etc. Matrices
  name source and destination. For shader interfaces and local arithmetic use
  space suffixes; introduce distinct host types when crossing a public math
  boundary can otherwise mix spaces. A suffix checker cannot prove correctness;
  review the actual transformations and units.
- Share GPU math/random primitives in FoundationMath's shader sources and GPU
  interface annotations in the RHI shader boundary. Slang's `vk` slot/location
  annotation is translated for all targets; it is not a Vulkan-only algorithm.
  Do not scatter that target vocabulary or runtime backend branches through
  common material/transport shaders. Preserve reflection and installation tests.
  C++ math headers are not automatically Slang-compatible: keep language-specific
  declarations in their owner's shader include, with matched numerical tests.
- Run `python3 scripts/python/source_policy.py --base <base-commit-or-ref>`.
  CI checks added/modified lines against the PR base, including standalone
  samples and tests, without a debt baseline. Existing violations do not permit
  new ones. This lexical gate checks explicit types, single-variable declarations,
  built-in CMake recipe ownership, C++ boolean negation, shader case/out names,
  coordinate member labels, interface annotation placement and the naming and
  magic-number rules in "Naming enforcement". Compiler, reflection tests,
  formatting, tidy and semantic review remain required.

### CPU and GPU reference path tracing (hard rule)

- Always implement and maintain both a CPU oracle and a GPU reference path
  tracer when building or extending reference path tracing. Deliver each new
  reference transport, material, sampling or accumulation feature in both paths
  in the same change, with parity evidence before claiming completion.
- The GPU implementation must execute transport, shading and accumulation on the
  GPU. CPU transport with GPU intersection queries does not satisfy this rule.
  Retain the CPU implementation for independent correctness checks and debugging.
- Both paths must estimate the same declared integral. Compare raw linear outputs
  with matched inputs and documented numerical/statistical tolerances; do not use
  denoising or temporal reuse to establish reference parity. Unsupported GPU
  capabilities remain explicit and do not silently select the CPU oracle.
- Follow the canonical [implementation and acceptance contract](docs/architecture/renderer-systems.md#cpu-and-gpu-reference-implementation-requirement).
  Record existing coverage gaps explicitly rather than treating a preview or a
  CPU-only implementation as a completed pair.

### Review feedback and viewport acceptance

- Render-graph authoring must be available directly from the graph canvas.
  Every Properties operation needs an equivalent canvas gesture or context-menu
  action, including node fields and connection removal/replacement. Share the
  document operations and field controls so both paths preserve the same
  validation, undo and save behavior; test the canvas path with Properties hidden.

- Report rejected edits and failed navigation in the surface where the user acted,
  including modal node Details. Keep the cause and explicit repair visible without
  requiring access to the main editor's status or Error List; preserve the accepted
  draft. Test the modal path with Properties hidden and clear the failure after a
  successful repair.
- Show inspectable, copyable full shader program IDs and explain their owning
  manifest scope. Graph-local node IDs do not scope manifest program IDs. Generated
  names must avoid existing package-wide program IDs and source files on creation
  and row rename; explicit compatible program assignments remain shareable.

- Read current human PR reviews and inline threads before claiming readiness,
  and again after the final implementation. Passing CI does not address a review.
  Reconcile each comment with code/evidence; preserve unresolved scope questions.
  Generalizable feedback belongs in these rules and the owning checks, with
  regressions for what is enforced and an explicit account of semantic limits.
- An interactive editor viewport renders the selected project's current World
  extraction and camera, responds to edits/navigation/resize, and presents GPU
  output with tracked producer/consumer ownership. A fixed scene, offline export
  or progressive reference capture does not establish that acceptance. Optional
  path tracing must not become the prerequisite for ordinary raster navigation.
  Do not invent scene lights to hide missing lighting; state the selected
  renderer profile's limits and validate the authored inputs.
- Pending preparation must retain the last drawable viewport view until the
  replacement reaches Ready. A failed replacement must not erase that view.
  Distinguish an accepted candidate's diagnostic report from the last presented
  image; verify pending, failure and successful publication with regression tests.

### Public SDK documentation

- Treat documentation as part of every new feature. Include appropriate Doxygen
  comments for its new or changed public API in the same change, following the
  requirements below.
- When a new feature needs setup instructions, usage examples, or an explanation
  of its workflow or limitations, update its canonical Markdown page under
  `docs/` in the same change. The wiki renders that file; do not write a second
  version for publication.
- Document new or changed public classes, structs, functions, methods, aliases,
  enum values, constants and macros beside their declarations using Doxygen
  `///` comments (or `/** ... */`). Plain `//` comments are not API descriptions.
- State purpose and the meaningful contract: parameters and units, return/status
  meanings, failure/output preservation, ownership/lifetime, thread affinity,
  and platform/build limitations where applicable. Use `@param`, `@return`,
  `@pre`, `@note`, and `@warning` when useful. Do not invent guarantees or repeat
  the signature as a substitute for behavior. No `@throws`: engine errors are
  explicit and exception-free.
- Preserve reference attribution. Detailed architecture prose belongs in its
  existing owner; link to it rather than duplicating it in every declaration.
- Public header inventory is checked against literal CMake public file sets.
  Private code, third-party headers and `detail`/`internal` helpers must not enter
  the public reference. Generated SDK policy headers are documented from templates.
- Build MkDocs, then run `python scripts/build-api --bootstrap` (or provide the
  pinned executable with `--doxygen`) and `python scripts/check-wiki` when changing
  public API documentation or its tooling. See `docs/development/api-reference.md`.
- CI rejects undocumented symbols outside `docs/api-undocumented.json`, including
  removal of existing descriptions. This baseline records legacy debt; do not add
  new gaps or regenerate it to bypass checks. Remove resolved entries in the change that documents them.
  Generated HTML/XML/report files stay in ignored `out/`, never source control.
- The legacy API documentation baseline may only shrink. CI compares it with
  the PR's base commit and rejects added entries, including renamed signatures.
  Resolved/deleted entries must be removed; do not regenerate the baseline to
  excuse missing comments. Public API documentation runs as an independent CI
  job, even when the browser editor build fails.
- Review API comments for meaningful caller contracts as well as extraction.
  CI detects missing descriptions; it cannot prove that ownership, lifetime,
  thread-affinity or failure claims agree with the implementation. Clear names
  and readable implementation code do not replace public API contracts.

### One documentation source, online and offline

- This rule is mandatory for every agent and contributor creating or changing
  **public** documentation. `docs/` is the canonical source of authored public
  Markdown and local documentation assets. Every public document must appear in
  wiki publication, navigation through task menus or the **All documents** index,
  and search. Do not put reader pages in an unregistered location, exclude them
  from publication, or treat a GitHub source link as wiki inclusion. Tooling
  inputs and generated API pages follow their existing publication paths.
- Before creating a page, search `docs/` for its existing owner and update that
  source first. Add a page only for a distinct reader task, contract, decision or
  evidence record with a concrete purpose. Do not create miscellaneous task
  reports, summaries or handoffs just to record work.
- `docs/wiki/` contains distinct task guides and reading paths, not separately
  maintained copies of engineering documents. Never create a wiki mirror,
  shortened copy or paraphrased version of an existing design/development page.
  A guide and a review may serve different reader needs, but reading paths link
  to the owner of detailed contracts instead of restating them. Contributors must
  never have to synchronize prose between a local document and a wiki version.
- Publish the same Markdown content and local assets online and offline.
  Presentation and navigation may differ; contracts, examples, status and
  attribution must not. When consolidating a published page, preserve useful
  unique material and attribution in its owner, update links and task navigation,
  and keep old URLs usable through a redirect or short compatibility link.
  Compatibility entry points must not carry a second content body.
- Keep module, sample and tool READMEs as short links to their guide under `docs/`,
  so those instructions enter the wiki and offline bundle. Label historical
  milestone scope and link current usage/evidence before retaining older plans.
- Use relative Markdown links for destinations inside `docs/` and local files
  for essential images. Keep examples and essential instructions readable in a
  plain text/Markdown reader without a website, plugin, network request or
  include directive. External reference citations may remain online.
- API comments beside public declarations are the canonical API contract;
  Doxygen/MkDocs output is generated from them, never edited independently.
- Keep private notes, credentials, machine-specific output and separately
  supplied reference books outside `docs/`. The publication requirement applies
  to public docs; it does not authorize publishing private material.
- Keep the local index current with `python scripts/check-docs --fix-index`.
  Before a documentation commit or PR, run `python scripts/check-docs`, a strict
  MkDocs build and `python scripts/check-wiki --documentation-only`. Run the
  documentation regressions with `python -m unittest discover -s scripts/python
  -p test_docs.py -v` and the corresponding `test_wiki.py` suite. Confirm new pages
  appear in the index and wiki search, with edit links to their canonical sources;
  report unavailable checks explicitly. The checks require every authored page
  to appear in the site and reject stale source/output manifests. Publication
  additionally checks the combined browser editor payload without that flag.
  See `docs/development/wiki.md` for API generation and offline packaging.
- Ship Markdown sources and the checked HTML site together for offline use.
  Package an existing build with `python scripts/package-docs`; reading the
  bundle must not download dependencies or regenerate documentation.

### Reference comments and attribution

- Whenever a paper, article, book chapter, or technical document materially
  informs a design, algorithm, or implementation, acknowledge and thank its
  authors with a concise reference comment near the affected code. For a design
  spanning a file or subsystem, place the comment at the relevant file or section
  boundary and link to the detailed design review when available.
- Identify the author or organization, exact title, and publication/book with
  chapter, section, or page numbers as applicable. Include a stable URL or DOI
  when available. Explain which idea was adopted or adapted and any significant
  departure; a bibliography in a design document supplements these code comments.
- Credit sources actually consulted and relevant to the implementation. Verify
  citation details and distinguish design inspiration from copied/adapted code.
  Preserve existing attribution and license notices when editing or refactoring.

### Primitive types

- Use the Ludus fixed-width aliases from `ludus/foundation/base/types.h` (also
  surfaced, with the rest of the foundational vocabulary, by
  `ludus/foundation/base/core.h`):
  `uint8` / `uint16` / `uint32` / `uint64`, `int8` / `int16` / `int32` /
  `int64`, `usize` (sizes/indices), `isize`, and `float32` / `float64`.
- Do not write `std::uint32_t`, `std::size_t`, raw `float`/`double`, etc. in new
  code. The aliases are exact aliases of the `<cstdint>` / `<cstddef>` types, so
  they interoperate with the standard library while keeping widths explicit.
- Byte widths and IEEE binary32/binary64 representation are checked by
  FoundationBase (ADR 0015). `usize`/`isize` remain native sizes/differences;
  serialized fields must specify a fixed width and byte order.
- Opt in to `checked_integer.hpp` for `TryIntegerCast` and checked
  add/subtract/multiply at numeric boundaries; `byte_order.hpp` provides bounded
  unsigned little/big endian codecs. These preserve outputs on failure and are
  never included by `types.h`/`core.h`. See `docs/architecture/primitive-types.md`.
- `usize` is the size/index type (aliases `std::size_t`) — use it for `sizeof`
  results, container sizes, and array indices; do not substitute `uint64`.

### Standard library and C runtime usage

The canonical policy is
[`docs/decisions/0003-standard-library-usage-policy.md`](docs/decisions/0003-standard-library-usage-policy.md).
It applies to production code in `modules/`, `apps/`, and `examples/`.

- **Default deny:** do not introduce a standard-library runtime facility merely
  because it is available locally or convenient. Check the canonical allowlist
  and its implementation-boundary restrictions first. C++23 language support
  does not establish library availability on every supported toolchain.
- **Use Ludus-owned containers and algorithms.** Do not add STL containers,
  owning strings, smart pointers, `<algorithm>`, `<ranges>`, `<numeric>`, or
  standard sorting/searching/collection algorithms. Inspect the existing Ludus
  modules first; implement a missing operation in its owning module, with an
  explicit allocation/error contract and appropriate tests. A namespace alias or
  wrapper around an STL container/algorithm is not a Ludus implementation.
- Prefer `StaticArray<T, N>` for fixed-size owned sequences, with a named count.
  Pass `GetData()` to native pointer/count APIs. Keep raw arrays only when a
  required ABI or array-reference signature cannot use the engine wrapper.
- **No standard formatting machinery.** Do not include `<format>` or call
  `std::format`, `std::vformat`, `std::format_to`, `std::vformat_to`, or
  `std::make_format_args` in repository-owned code, including tests. Use the
  bounded Ludus formatter with explicit error/truncation status. Moving an
  expensive STL facility into a `.cpp` does not by itself authorize it.
- **Keep the explicit exceptions narrow.** Concepts, type traits and approved
  compile-time utilities are permitted. Approved low-level atomics, construction
  primitives and C runtime operations have the restrictions recorded in ADR 0003;
  approval of one function does not approve its entire header.
- **Own text-to-number conversion.** New parsing uses Ludus-owned conversion
  code with bounded input, explicit errors and documented rounding/range behavior;
  do not introduce `std::from_chars`, `std::sto*`, or a third-party conversion
  library as a replacement. Existing conversions are migration debt.
- **Existing usage is not precedent.** Preserve necessary calls inside an
  existing implementation during maintenance, but do not spread them to new
  APIs, subsystems or dependencies. Migrate existing facilities in focused
  changes; do not rewrite unrelated code to satisfy this policy.
- If a missing facility needs a new standard-library exception, document the
  exact operation, boundary and native/browser evidence in ADR 0003 as an
  explicit policy change for review. Do not silently add it or disable a
  supported platform to accommodate it.
- C++ exceptions, iostream/sstream, standard primitive spellings and printf-family
  diagnostics remain banned. Use `LUDUS_LOG_*` for ordinary diagnostics and
  FoundationBase's independent emergency path for assertions.

### Build-time hygiene (heavy headers)

- **Do not instantiate heavy standard-library templates in public headers, and
  do not `#include` heavy headers (`<format>`, `<chrono>`, `<filesystem>`,
  `<regex>`, `<iostream>`) from a public header.** Type-erase or PIMPL the
  expensive facility behind a `.cpp` boundary; templated public APIs should erase
  to a non-template implementation as early as possible (see ADR 0004: the
  logging call sites pack `FormatArg` values for the compiled `FormatInto` parser).
- Trace transitive dependencies, including test framework headers. Do not
  substitute another heavy formatting/streaming package, use PCH to hide a
  regression from the budget, exclude compilations, or increase a limit to
  avoid implementing a cheaper Ludus operation. Compare uncached profiles with
  the same compiler, test graph and concurrency; preserve assertion diagnostics.
- Build time is budgeted, not vibes. `config/build_budget.json` sets a per-header
  average parse-time budget; the `Build-time budget` CI job (and
  `./scripts/check-build-budget`) fails if a project header exceeds it. If a build
  gets slow, run `./scripts/profile-build` and fix the offending include; raise a
  budget only with a measurement and a note in the PR. See
  `docs/development/build-profiling.md` and ADR 0005.
- `./scripts/check --include-cleaner` gives an advisory include-hygiene report;
  it is not a gate.
- **Editor precompiled headers are the one default-on exception.** The optional
  Qt editor targets in `apps/editor` precompile a fixed list of stable Qt Core
  and standard library headers (`LUDUS_EDITOR_PRECOMPILED_HEADERS`, default ON
  for native builds; `apps/editor/cmake/EditorPrecompiledHeaders.cmake`). The
  list never contains a Ludus header, so engine headers, the build-time budget
  and header self-sufficiency are unaffected; `./scripts/profile-build` turns it
  off. Editor sources must still include every Qt and standard header they use:
  the batch **Editor, shader worker and unit tests** job compiles the editor
  with the option OFF to catch a missing include. Do not add Ludus headers or
  extend the exception to engine modules.

### Foundational headers and the include boundary

Ludus has a two-tier include model (ADR 0007;
`docs/architecture/foundational-headers.md`). Learn these three lists — they are
enforced by the `ludus_header_self_sufficiency` / `ludus_foundational_includes`
gates and by `./scripts/check`, not just by convention.

**Allowed everywhere (may be assumed available via `core.h`).**
`#include <ludus/foundation/base/core.h>` is the single foundational header. It
guarantees a translation unit has: the fixed-width types (`uint32`, `usize`,
`float32`, …); the build/OS-family/CPU-arch/compiler configuration macros
(`LUDUS_BUILD_*`, `LUDUS_PLATFORM_*`, `LUDUS_ARCH_*`, `LUDUS_COMPILER_*`); the
codegen macros (`LUDUS_INLINE`/`NOINLINE`/`COLD`/`LIKELY`/`UNLIKELY`/
`DEBUG_BREAK`/`UNREACHABLE`); `Move`/`Forward`/`DerivedFrom`/`nullptr_t`; and the
assertion entry points (`LUDUS_ASSERT`/`REQUIRE`/`CHECK`/`FATAL`). A file that
needs only part of this may include the specific Band 0 header instead
(`types.h`, `config.h`, or `compiler.h`).

**Required explicitly (include the header that declares what you name).**
Everything above the foundation is opt-in: containers (`vector.hpp`,
`array.hpp`), `pointer.hpp` (`UniquePtr`), strings/`<string_view>`, logging,
profiling, platform, graphics, math, threading. Do **not** rely on these
arriving transitively. `core.h` is what you *may* rely on; it is not mandatory
boilerplate — if a file already includes exactly what it uses, do not add
`core.h` just for cohesion.

**Forbidden in the foundational headers.** `core.h`, `config.h`, `compiler.h`,
and `types.h` must never include a string/container header (Ludus or std),
`<string_view>`, `<memory>`, `<span>`, a heavy STL header (`<format>`,
`<chrono>`, `<filesystem>`, `<regex>`, `<iostream>`, …), or any `ludus/` header
outside `foundation/base`. More generally, no public header may include a heavy
STL header (type-erase behind a `.cpp`; ADR 0004) or a private `internal/`
header.

**Include ordering.** In an engine header, include `core.h` (or the specific
Band 0 header you need) first; then other Ludus headers grouped by layer,
most-foundational first; then standard-library headers; then third-party
headers. Separate the groups with a blank line.

### Module and dependency layout

- **Reuse engine types before writing your own.** Use FoundationMath
  (`Vector3d`, `Matrix4d`, `Cross`, `CompensatedSum`, `IsFinite`, `Abs`),
  FoundationStrings (`StaticString`, `String`) and the shared Content aliases
  instead of local vector, matrix, identity, or fixed `char[]` buffer types. If
  the engine lacks a general-purpose facility, add it to the owning Foundation
  module (with tests) and use it from there.
- **Data formats shared by tools and runtime are owned by neither.** A cooked
  or interchange format (for example LUSCN in `Ludus::SceneFormat`,
  `modules/content/scene_format`) lives in its own header-only INTERFACE target
  that depends only on FoundationBase and holds the versioned POD record
  layouts, limits and constants with `static_assert`ed sizes. Producers (the
  cooker, Editor composers) and consumers (Resources decoders, the renderer)
  include it; a cooker never links the renderer and the runtime never links a
  cooker. Serialise records field by field in little-endian order; the structs
  document the layout and are not `memcpy`ed to or from files.
- Engine modules live under `modules/<layer>/<module>/` with public headers in
  `include/` and private implementation headers in `src/internal/` (and
  `src/sinks/`, etc.). Only public headers are installed/exported.
- Respect the dependency direction. In particular, `FoundationBase` must not
  depend on any higher-level module.
- Internal implementation details (private headers, sink interfaces, etc.) must
  not leak into the installed SDK.

### Responsibility boundaries and incremental refactoring

- Before adding behavior to a large source file, identify the owning operation,
  its inputs, mutable state, errors and resource lifetime. Extract an independent
  responsibility when the addition would combine unrelated workflows. File and
  function size are review signals, not arbitrary line-count gates.
- Extract private components with narrow contracts and their own state. Moving
  member definitions into several files while retaining an all-purpose shared
  state object, or including implementation bodies from a header, does not
  establish an ownership or compilation boundary. Preserve module dependency
  direction and installed SDK boundaries; do not add a framework to split files.
- Refactor one boundary per reviewable change. Keep structural moves separate
  from intentional behavior fixes, identify any necessary differences explicitly,
  and preserve source attribution. Establish relevant regression coverage before
  changing transactional, asynchronous or GPU lifetime code.
- Retain one authoritative document history, publication transaction and native
  resource lifetime ledger in their existing owners. Model asynchronous stages
  with named states and explicit Pending/Ready/failure outcomes. Candidates must
  not mutate the live resource generation or replace drawable output before
  readiness; stale callbacks must carry and validate the relevant identity.
- Preserve failure/output behavior, cancellation, rollback, bounded capacities,
  deterministic cooked bytes and native/browser support during extraction.
  Exercise the production paths with focused tests, then run the applicable
  build, sanitizer, formatting, tidy and changed-source checks. A shorter file or
  passing lexical check alone is not evidence of reduced coupling.
- Prioritize the preview resource/publication boundary, World inspector/import
  workflows, asset preparation/publication, then renderer and reference cooker
  decomposition. See the existing
  [editor GUI systems context](docs/architecture/editor-gui-systems.md#implementation-boundaries-and-refactoring)
  for the staged scope and remaining work. These are incremental extractions,
  not permission to rewrite all five subsystems together.

### Memory and error handling

- Express new borrowed contiguous inputs with Ludus-owned bounded views; validate
  extents before reading, and document who keeps storage alive. Use Ludus-owned
  smart owners for ownership transfer, never a borrowed raw pointer as an owner.
  A view does not establish storage lifetime. Shared/weak ownership needs a
  separately implemented Foundation facility; do not substitute STL smart pointers.
- Prefer zero-allocation on hot paths; reserve/reuse buffers where practical.
- Never let a failure in infrastructure (e.g. logging) propagate into or crash
  engine code. Degrade gracefully.

---

## Build, test, and validate

### Game project setup checks

When creating, loading, updating, or repairing a game project, check its CMake
setup as part of the work. Hidden base presets alone are not a usable setup.
Verify selectable configure/build/test presets for the supported profiles,
their inheritance and conditions, SDK/dependency prefixes, compiler, Ninja,
shader tools when required, and the IDE's CMake executable/preset mode. Use the
selected CMake's `--list-presets=configure`, `--list-presets=build`, and
`--list-presets=test`; parsing JSON alone does not prove preset availability.

Loading must report missing or stale setup without downloading, configuring,
building, or rewriting files. Explicit setup/update/repair should regenerate
owned local settings, preserve custom presets and unrelated editor settings,
refresh stale CMake caches when SDK/toolchain inputs change, and verify a real
configure/build/test before reporting success. Keep machine paths in ignored
local files such as `CMakeUserPresets.json`. Add regressions for a fresh clone,
missing local presets, moved tools/SDKs, and repeated repair. Future CLI/Editor
project operations must share these checks (see the project-sdk-workflow spec).

```bash
./init.sh                                   # prepare pinned tools + deps
./scripts/build   linux-clang-debug         # build
./scripts/test    linux-clang-debug         # unit tests
./scripts/check   linux-clang-development --all   # format + tidy
./scripts/build   linux-clang-asan-ubsan    # sanitizer build
./scripts/test    linux-clang-asan-ubsan    # sanitizer tests
./scripts/install-sdk linux-clang-development     # SDK install + consumer
```

A change is ready for review only when: it builds warning-clean, unit tests
pass, ASan/UBSan is clean, and format/tidy pass — all with the pinned toolchain.
