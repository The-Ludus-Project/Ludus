# ShaderLanguage: the LUSL front end

`Ludus::ShaderLanguage` is the compiler front end of LUSL, the Ludus Shading
Language. The [LUSL design](../../architecture/shading-language.md) owns the
language and the delivery plan. This page covers what is implemented:

- the preprocessor (L1a);
- the parser and the source formatter (L1b; see [Parsing](#parsing) and
  [Formatting](#formatting));
- the first half of semantic checking (L1c-1; see [Checking](#checking)).

Flow analysis, the style rules, lowering to Slang and `luslc` arrive in later
L1 changes, and `ShaderCompiler` does not accept `.lusl` files yet.

Link `Ludus::ShaderLanguage` and include
`ludus/tooling/shader_language/preprocessor.hpp`. The library builds for native
and browser targets, uses no exceptions and performs no file access.

```cpp
#include <ludus/tooling/shader_language/preprocessor.hpp>

namespace language = ludus::tooling::shader_language;
using ludus::foundation::BorrowedView;

bool PreprocessShade(std::string_view path, std::string_view text, language::PreprocessResult& outResult) noexcept
{
    ludus::tooling::shader_compiler::SourceFile file;
    file.Path = BorrowedView<const char>(path.data(), path.size());
    file.Text = BorrowedView<const char>(text.data(), text.size());
    constexpr std::string_view MODE = "SHADOW_MODE";
    constexpr std::string_view MODE_VALUE = "2";
    language::ConfigurationValue mode;
    mode.Name = BorrowedView<const char>(MODE.data(), MODE.size());
    mode.Value = BorrowedView<const char>(MODE_VALUE.data(), MODE_VALUE.size());

    language::PreprocessRequest request;
    request.Files = BorrowedView<const ludus::tooling::shader_compiler::SourceFile>(&file, 1);
    request.Source = file.Path;
    request.Configuration = BorrowedView<const language::ConfigurationValue>(&mode, 1);
    return language::Preprocess(request, outResult) == language::Status::Ready;
}
```

## Requests and results

A request is the same immutable snapshot `ShaderCompiler` receives: relative
`/`-separated paths and UTF-8 texts, with at most
`shader_compiler::MAX_SOURCE_FILES` files and `shader_compiler::MAX_SOURCE_BYTES`
bytes. The root must be a `.lusl` module.

- **Configuration values** are the selected render-graph configuration values.
  Each becomes an object-like macro that the source can read but not `#define`
  or `#undef`. They are the only macros from outside the source; there is no
  free-form `-D`.
- **Ready** publishes the whole translation unit as tokens, with comments
  removed. Token spellings borrow the request's file texts, so keep them alive
  and use `GetTokenSpelling`.
- **Failed** and **InvalidRequest** publish no tokens. The diagnostics and the
  read masks still describe what was found.
- **Read masks**: `SourceReadMask` and `ConfigurationReadMask` record which
  files and configuration values the source used. Build identity uses them as
  dependencies.
- **Diagnostics** carry a stable code, a location and a message. For a token
  produced by macro expansion, `Location` is where it is spelled (inside the
  definition) and `ExpansionLocation` is the outermost macro use.

Equal requests produce equal results on every host. Calls are reentrant and
keep no state.

## Directives and departures from C

| Directive | Behavior |
| --- | --- |
| `#include "path"` | Searches beside the including file, then from the snapshot root. Accepts only `.luslh` headers; Slang code is declared with `extern slang` instead. Paths cannot be absolute, use `\`, or leave the root. No `<path>` form |
| `#pragma once` | The only pragma; others are errors |
| `#define`, `#undef` | Object-like and function-like macros with C rescanning, `#` and `##`. Identical redefinitions are allowed. Not variadic |
| `#if`, `#elif` | int64 integer constant expressions with decimal and `0x` literals, `true`, `false` and `defined` |
| `#ifdef`, `#ifndef`, `#else`, `#endif` | As in C; no trailing tokens |
| `#error` | Fails with the directive's text |
| `#line` | Reserved for generated code |

Where LUSL differs from C:

- **An identifier left in a condition is an error.** C treats it as `0`, which
  hides misspelled option names.
- **`defined`, `#ifdef` and `#ifndef` must name a known macro**: a
  configuration value, or a macro defined or undefined earlier in the
  translation unit.
- **There is no unary `!`.** Write `defined(X) == 0` or use `#ifndef`.
- **Overflow and division by zero are errors** when the operand is evaluated.
  Operands that `&&`, `||` and `?:` skip are not evaluated.
- **No octal literals and no integer suffixes** in conditions.
- **No character literals.** Non-ASCII text is allowed only in comments and
  strings.

Macro expansion follows Dave Prosser's
["Complete macro expansion algorithm"](https://www.spinellis.gr/blog/20060626/x3J11-86-196.pdf)
(X3J11/86-196), with C99 placemarkers for empty `##` operands.

## Limits

`ludus/tooling/shader_language/limits.hpp` owns these bounds:

| Constant | Value | Bounds |
| --- | --- | --- |
| `MAX_FILE_TOKENS` | 1,048,576 | Tokens lexed from one file or configuration value |
| `MAX_OUTPUT_TOKENS` | 1,048,576 | Tokens in one translation unit |
| `MAX_INCLUDE_DEPTH` | 32 | Include nesting, counting the root |
| `MAX_CONDITIONAL_DEPTH` | 64 | `#if` nesting |
| `MAX_CONDITION_NESTING` | 64 | Nesting inside one condition |
| `MAX_MACROS` | 4,096 | Distinct macro names |
| `MAX_MACRO_PARAMETERS` | 32 | Parameters of one macro |
| `MAX_MACRO_EXPANSION_DEPTH` | 32 | Nested argument expansion |
| `MAX_MACRO_EXPANSIONS` | 262,144 | Expansions per translation unit |
| `MAX_SYNTHESIZED_BYTES` | 1 MiB | Spellings made by `#`, `##` and configuration values |
| `MAX_CONFIGURATION_VALUES` | 256 | Values per request |
| `MAX_CONFIGURATION_TEXT_BYTES` | 1,024 | One configuration name or value |
| `MAX_SYNTAX_NODES` | 2,097,152 | Syntax tree nodes per translation unit |
| `MAX_SYNTAX_DEPTH` | 64 | Statement and expression nesting in the parser; keeps the browser build within its stack |
| `MAX_SEMANTIC_TYPES` | 65,536 | Interned types, built-ins included |
| `MAX_SYMBOLS` | 1,048,576 | Symbols, built-in functions included |
| `MAX_FUNCTION_PARAMETERS` | 64 | Parameters of a function and arguments of a call |
| `MAX_ARRAY_LENGTH` | 1,048,576 | Elements of one array dimension |
| `MAX_DIAGNOSTICS` | 256 | Retained diagnostics; later ones are counted |
| `MAX_DIAGNOSTIC_MESSAGE_BYTES` | 256 | One message; longer ones are clipped |

Exceeding a limit is a diagnostic and a failed result, never truncated output.
The values will be measured against built-in content once shaders are ported in
L1e.

## Diagnostic codes

Codes are displayed as `LUSL` and four digits. `GetDiagnosticCodeName` returns
the name.

| Code | Name | Meaning |
| --- | --- | --- |
| LUSL0001 | `logical-not` | Unary `!` does not exist |
| LUSL0002 | `invalid-request` | Malformed request: path, root, configuration value or snapshot limit |
| LUSL0101 | `invalid-utf8` | A file is not UTF-8 |
| LUSL0102 | `invalid-character` | A byte that cannot start a token |
| LUSL0103 | `unterminated-comment` | `/*` without `*/` |
| LUSL0104 | `unterminated-string` | String without a closing quote on its line |
| LUSL0105 | `invalid-escape` | Escape other than `\\ \" \n \r \t` |
| LUSL0106 | `character-literal` | Character literals do not exist |
| LUSL0107 | `file-token-limit` | `MAX_FILE_TOKENS` exceeded |
| LUSL0201 | `unknown-directive` | Not a LUSL directive |
| LUSL0202 | `malformed-directive` | Missing or extra directive operands |
| LUSL0203 | `include-not-found` | No snapshot file matches |
| LUSL0204 | `include-not-header` | Included file is not `.luslh` |
| LUSL0205 | `invalid-include-path` | Absolute, `\`, empty component or outside the root |
| LUSL0206 | `recursive-include` | A file includes itself without `#pragma once` |
| LUSL0207 | `include-depth-limit` | `MAX_INCLUDE_DEPTH` exceeded |
| LUSL0208 | `macro-redefined` | Different redefinition without `#undef` |
| LUSL0209 | `configuration-modified` | `#define` or `#undef` of a configuration value |
| LUSL0210 | `reserved-macro-name` | `defined` used as a macro name |
| LUSL0211 | `variadic-macro` | `...` in a parameter list |
| LUSL0212 | `duplicate-macro-parameter` | Repeated parameter name |
| LUSL0213 | `stringify-not-parameter` | `#` not followed by a parameter |
| LUSL0214 | `paste-at-edge` | `##` at the start or end of a body |
| LUSL0215 | `invalid-paste` | `##` result is not one token |
| LUSL0216 | `macro-argument-count` | Wrong number of arguments |
| LUSL0217 | `unterminated-macro-invocation` | No closing `)` before the end of the file |
| LUSL0218 | `directive-in-macro-arguments` | A directive inside macro arguments |
| LUSL0219 | `undefined-identifier-in-condition` | Identifier left in a condition |
| LUSL0220 | `unknown-macro-name` | `defined` of an unknown name |
| LUSL0221 | `invalid-condition` | Not a valid condition |
| LUSL0222 | `condition-division-by-zero` | Evaluated division or modulo by zero |
| LUSL0223 | `condition-overflow` | Evaluated int64 overflow or shift out of range |
| LUSL0224 | `unterminated-conditional` | `#if` without `#endif` in its file |
| LUSL0225 | `unmatched-conditional` | `#elif`, `#else` or `#endif` without `#if`, or after `#else` |
| LUSL0226 | `error-directive` | `#error` reached |
| LUSL0227 | `unknown-pragma` | Pragma other than `once` |
| LUSL0228 | `line-directive` | `#line` is reserved |
| LUSL0229 | `conditional-depth-limit` | `#if` or condition nesting limit exceeded |
| LUSL0230 | `macro-expansion-limit` | Expansion count, depth or synthesized bytes exceeded |
| LUSL0231 | `output-token-limit` | `MAX_OUTPUT_TOKENS` exceeded |
| LUSL0232 | `macro-limit` | Too many macro names or parameters |
| LUSL0233 | `defined-from-expansion` | A macro produced `defined` in a condition |
| LUSL0301 | `unexpected-token` | A token the grammar does not allow; the message says what was expected |
| LUSL0302 | `assignment-in-expression` | Assignment, compound assignment, `++` or `--` inside an expression |
| LUSL0303 | `multiple-declarators` | More than one variable in one declaration |
| LUSL0304 | `later-phase-construct` | `interface`, `concept`, `template` and other L2 constructs |
| LUSL0305 | `keyword-as-name` | A keyword where a name is required |
| LUSL0306 | `c-style-cast` | `(Type)value`; write `Type(value)` |
| LUSL0307 | `expression-without-effect` | A statement that is not a call, assignment or `++`/`--`, or an empty `;` |
| LUSL0308 | `positional-after-named` | A positional argument after a named one |
| LUSL0309 | `unscoped-enum` | `enum` without `class` |
| LUSL0310 | `syntax-node-limit` | `MAX_SYNTAX_NODES` exceeded |
| LUSL0311 | `syntax-depth-limit` | `MAX_SYNTAX_DEPTH` exceeded |
| LUSL0401 | `formatted-source-limit` | The formatted text would exceed `shader_compiler::MAX_SOURCE_BYTES` |
| LUSL0501 | `unknown-name` | No visible declaration of the name |
| LUSL0502 | `unknown-type` | Not a built-in or declared type |
| LUSL0503 | `redeclaration` | A name declared twice in one scope, or a function signature defined twice |
| LUSL0504 | `type-mismatch` | A value of the wrong type; LUSL has no implicit conversions |
| LUSL0505 | `constant-out-of-range` | An untyped constant its target cannot represent exactly, or a constant index out of range |
| LUSL0506 | `invalid-literal` | Octal, suffixed or malformed numbers |
| LUSL0507 | `invalid-operands` | An operator its operand types do not define |
| LUSL0508 | `non-bool-condition` | A condition that is not a `bool` scalar |
| LUSL0509 | `not-assignable` | Assignment, `++`/`--` or a writable argument whose target is not assignable |
| LUSL0510 | `no-matching-overload` | No overload or built-in function accepts the arguments |
| LUSL0511 | `ambiguous-call` | Several overloads accept the arguments equally well |
| LUSL0512 | `unknown-named-argument` | A named argument that matches no parameter |
| LUSL0513 | `duplicate-argument` | A parameter bound twice |
| LUSL0514 | `missing-argument` | A parameter without default and without argument |
| LUSL0515 | `direction-mismatch` | An `out`/`inout` mark that does not match its parameter |
| LUSL0516 | `aliased-arguments` | Writable arguments that may overlap |
| LUSL0517 | `array-size-literal` | An array size written as a literal |
| LUSL0518 | `not-constant` | A constant expression is required |
| LUSL0519 | `invalid-array-size` | An array size that is not 1 to `MAX_ARRAY_LENGTH` |
| LUSL0520 | `unknown-member` | No such field, method or swizzle |
| LUSL0521 | `invalid-initializer` | A designated initializer without a struct target, or with an unknown or repeated field; misplaced bodies and initializers in `extern slang` |
| LUSL0522 | `invalid-zero-initializer` | `= 0` on an opaque type |
| LUSL0523 | `invalid-return` | A return that does not match the function; `break`/`continue` outside a loop |
| LUSL0524 | `invalid-top-level-variable` | A top-level variable that is neither `static const` nor `groupshared` |
| LUSL0525 | `static-assert-failed` | A false `static_assert` |
| LUSL0526 | `invalid-attribute` | An unknown attribute, or wrong arguments or target |
| LUSL0527 | `invalid-semantic` | An unknown or misplaced system-value semantic |
| LUSL0528 | `invalid-switch` | A non-integer selector, or a repeated or non-constant label |
| LUSL0529 | `constant-overflow` | Constant arithmetic that overflows or divides by zero |
| LUSL0530 | `not-callable` | A call of something that is not a function or type |
| LUSL0531 | `unsupported-construct` | A construct of a later phase, such as type arguments |
| LUSL0532 | `void-value` | The result of a `void` call used as a value |
| LUSL0533 | `invalid-construction` | A construction or conversion with the wrong arguments |
| LUSL0534 | `undefined-function` | A function declared but never defined |
| LUSL0535 | `checker-limit` | Too many types, symbols or arguments |

## Parsing

`Parse` (in `syntax.hpp`) turns a Ready preprocessing result into a syntax tree.
Each node has a `SyntaxKind`, its principal token, the range of tokens it
covers, and its children. The header documents every kind's children and their
order; later stages rely on that order.

```cpp
#include <ludus/tooling/shader_language/syntax.hpp>

bool ParseShade(const language::PreprocessRequest& request, const language::PreprocessResult& preprocessed,
                language::SyntaxTree& outTree) noexcept
{
    return language::Parse(request, preprocessed, outTree) == language::Status::Ready;
}
```

The grammar covers L1 LUSL:

| Area | What parses |
| --- | --- |
| Declarations | Variables with `static`, `const` or `groupshared`, arrays and `= void`; functions with `in`, `out` and `inout` parameters, semantics and defaults; `struct` with fields and methods; `enum class` with an underlying type; `extern slang "path" { ... }`; `static_assert`; attribute lists such as `[stage(compute), threads(8, 8, 1)]` |
| Types | Names, scoped names (`A::B`) and type arguments (`Texture2D<float32x4>`, `Outer<Inner<T>>`) |
| Statements | Blocks, assignments and compound assignments, `++` and `--`, call statements, `if`/`else`, `for`, `while`, `do`/`while`, `switch` with `case`, `default` and `[[fallthrough]]`, `break`, `continue`, `return`, `discard` |
| Expressions | C precedence, `?:`, unary `+ - ~`, calls, member access, indexing, scoped names, named arguments (`ray = r`), call-site marks (`out radiance`), designated initializers (`{ .A = a, .B = b }`) |

Parsing enforces LUSL's syntactic rules:

- one declarator per declaration;
- assignment, `++` and `--` only as statements, so `if (a = b)` cannot compile;
- no unary `!`;
- no C casts;
- positional arguments before named ones;
- only calls, assignments and `++`/`--` as statements.

Interfaces, concepts and templates (L2) are reported as later-phase constructs.
After an error the parser skips to the next statement or declaration and
continues, so one request reports several independent errors. A Failed tree is
still published, with Error nodes where tokens were skipped, for editors; later
stages consume only a Ready tree.

Types, names, literal values and the remaining rules (conversions, the zero
rule, definite assignment, named-argument binding, style rules) belong to the
checker (L1c).

## Formatting

`FormatSource` (in `formatter.hpp`) formats one source file in the repository's
style: four-space indentation, braces on their own lines, indented `case`
labels, single spaces around binary operators, `{ .A = a }` initializers, and
directives indented after `#` by their conditional depth.

**Only whitespace changes.**
- The formatter works on the unpreprocessed tokens and comments, not the syntax
  tree. Macros, conditional groups and comments therefore survive.
- Lexing the output gives the same tokens and comments, in order, as lexing the
  input.
- Formatting the output again changes nothing.
- Where removing a space would fuse two tokens (`. 1`, `- -`, `/ /`), the space
  stays.
- Tests check these properties on fixed and random sources.

**Current limits.**
- Line breaks inside a statement are kept as written, with continuation lines
  aligned after their open bracket. Long lines are not wrapped.
- Directive lines are kept as written after the `#`.
- A source that does not lex, such as one with an unterminated comment, is not
  formatted.

## Checking

`Check` (in `semantic.hpp`) takes the Ready preprocessing and parsing results
and produces a `SemanticModel`:

- **interned types**;
- **symbols**: variables, parameters, functions, structs, fields, enums,
  enumerators and built-in functions;
- **per-node facts**:
  - each expression's type;
  - the symbol each name, member, call and declaration refers to;
  - the parameter each argument binds to;
  - how each call is lowered (function, method, built-in, construction or
    conversion);
  - the value of each integer or boolean constant.

```cpp
#include <ludus/tooling/shader_language/semantic.hpp>

bool CheckShade(const language::PreprocessRequest& request, const language::PreprocessResult& preprocessed,
                const language::SyntaxTree& tree, language::SemanticModel& outModel) noexcept
{
    return language::Check(request, preprocessed, tree, outModel) == language::Status::Ready;
}
```

### Names and declarations

- **C order:** a name is visible after its declaration. A function may be
  declared with `;` and defined later. A LUSL function that is never defined is
  an error; functions in `extern slang` blocks have no body.
- **Overloads:** functions with the same name and different parameter types are
  overloads. Inner scopes may shadow outer names; one scope cannot declare a name
  twice.
- **Structs:** fields may have constant defaults. Methods read fields by name
  and may call any method of their struct. Assigning a field, or calling a
  mutating method, requires the method to be `[mutating]`. A struct cannot
  contain itself.
- **`enum class`:** the underlying type is an integer scalar. Enumerators number
  from zero or take constant values, and are named as `Mode::On`.
- **Top-level variables** are `static const` (with a constant initializer) or
  `groupshared`. Other globals come from render-graph bindings or `extern slang`
  blocks.
- **`extern slang "path" { ... }`** declares Slang functions, constants and
  structs. `struct Name;` declares an opaque type that LUSL can pass around but
  not look inside or zero.

### Types, constants and conversions

| Rule | Behavior |
| --- | --- |
| Built-in types | `bool`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float16`, `float32`, `float64`, vectors `float32x3` and matrices `float32x4x4`, plus the exact aliases `float`, `int`, `uint`, `half`, `double`, `float3`, `bool2`, `float4x4` and so on |
| No implicit conversions | Values convert only through a call such as `float32(count)`, `uint32x3(v)`, `uint32(mode)` or `Mode(word)`; `bool` and numbers never convert |
| Untyped constants | Literals and constant expressions have no type until they meet a target. An integer constant converts to an integer type that holds it, or to a float type that represents it exactly; a floating constant converts to float types only. A constant never spreads into a vector implicitly |
| Literals | Decimal and `0x` hexadecimal integers, floating literals such as `1.5`, `.5`, `1.` and `2e-3`; no octal and no suffixes |
| Integer constant folding | Exact int64 arithmetic, so `1 / 2` is `0`; overflow and division by zero are errors, and a typed result must fit its type |
| Zero rule | The literal `0` initializes or assigns a value of any type except an opaque one, zero-filling it: `Hit hit = 0;`, `v = 0;`, field defaults |
| Array sizes | A positive integer constant written as a name or expression, never a bare literal: `float32 w[COUNT];` |
| Conditions | `if`, `while`, `for`, `do` and `?:` take a `bool` scalar; `if (count)` is an error, `if (count != 0)` is not |

**Operators:**
- Arithmetic takes matching numeric operands, or a vector or matrix with a scalar
  of its element type.
- `*` between a matrix and a vector or matrix is the linear-algebra product with
  column vectors. `vector * matrix` is an error.
- `&&` and `||` take `bool`; bitwise operators take integers.
- Comparisons yield `bool`, or `boolxN` for vectors.

**Constructions:**
- Vectors build from components of their element type, or splat one scalar:
  `float32x4(v, 1)`, `float32x3(1.0)`.
- Matrices build from rows or from every component.
- Structs use designated initializers, such as `{ .PositionWorld = p }`, whose
  omitted fields take their default or zero.

### Calls

- **Binding:** positional arguments bind first, then named ones. Each parameter
  binds once, and unbound parameters need defaults.
- **Overload choice:** among the overloads that accept the arguments, the one
  needing the fewest constant adaptations wins. A tie is an ambiguity error.
- **Writable arguments:** an `out` or `inout` parameter needs the matching mark
  on its argument (`out radiance`), and an input parameter takes no mark. A
  marked argument must be assignable.
- **Aliasing:** two writable arguments of one call may not overlap. Distinct
  fields, distinct constant indices and disjoint swizzles are separate;
  run-time indices are assumed to overlap.

Built-in functions take positional arguments and lower to the Slang intrinsic of
the same name:

| Signature | Functions |
| --- | --- |
| float T → T | `sqrt`, `rsqrt`, `exp`, `exp2`, `log`, `log2`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `floor`, `ceil`, `round`, `trunc`, `frac`, `saturate`, `ddx`, `ddy` |
| float vector V → V | `normalize` |
| numeric T → T | `abs` |
| numeric (T, T) → T | `min`, `max` |
| float (T, T) → T | `pow`, `atan2`, `step`, `fmod` |
| numeric (T, T, T) → T | `clamp` |
| float (T, T, T) → T | `lerp`, `smoothstep` |
| (V, V) → scalar | `dot` (numeric), `distance` (float) |
| float V → scalar | `length` |
| float (V, V) → V | `reflect`; `cross` for 3-vectors |
| bool T → bool | `any`, `all` |
| 32-bit T → same shape | `asuint`, `asint`, `asfloat` |
| 32-bit integer T → uint32 T | `countbits`, `firstbithigh`, `firstbitlow`, `reversebits` |
| matrix R x C → C x R | `transpose` |

In the two-, three-operand element-wise rows, a scalar may stand beside vectors:
`lerp(a, b, 0.5)`, `max(v, 0)`.

### Entry points and attributes

**Compute entry points:**
- They are marked `[stage(compute), threads(x, y, z)]` with positive integer
  constants, and return `void`.
- Every parameter is an input with a system-value semantic:
  - `DispatchThreadId`, `GroupThreadId` and `GroupId` (`uint32x3`);
  - `GroupIndex` (`uint32`).
- Semantics appear on entry point parameters only.

**Other attributes:**
- Functions also take `[noinline]` and `[forceinline]`; methods take
  `[mutating]`.
- Loops take `[unroll]`, `[unroll(count)]` and `[loop]`.
- `if` and `switch` take `[branch]` and `[flatten]`.

**Switch statements:**
- The selector is an integer or an enum.
- Case labels are distinct constants.
- `[[fallthrough]]` ends a case.

### Not checked yet

These rules arrive with the second half of the checker (L1c-2):
- definite assignment and `= void`;
- `out` parameters written before every return;
- missing returns;
- recursion;
- the `groupshared` `= 0` or `= void` requirement;
- the style rules and their `strict`/`warn`/`off` setting;
- automatic fixes attached to diagnostics.

Capability-gated types (`float16`, `int16`, 64-bit types) are accepted here;
target profiles gate them when LUSL lowers to Slang.

## Tests and fuzzing

`ludus_shader_language_tests` (CTest labels `unit;shader;tooling`) covers:

- the lexer;
- every directive, the C rescanning rules, and `#` and `##` with placemarkers;
- conditions, configuration values, includes and request validation;
- every grammar construct as a golden tree, the parser's syntax rules, and
  error recovery;
- each checker rule with a failing case, and the model's types, symbols,
  bindings and constants;
- the formatter's golden layout, token preservation and idempotence;
- limits and determinism.

Seeded random tests run the preprocessor, the parser, the formatter and the
checker over token mixtures.

For coverage-guided fuzzing with native Clang, configure with
`-DLUDUS_BUILD_SHADER_LANGUAGE_FUZZERS=ON` and run `ludus_shader_language_fuzzer`.
- Its input is split at the first zero byte into a root module and one header.
- It preprocesses, parses and checks the snapshot, and formats the root.
