# ShaderLanguage: the LUSL front end

`Ludus::ShaderLanguage` is the compiler front end of LUSL, the Ludus Shading
Language. The [LUSL design](../../architecture/shading-language.md) owns the
language and the delivery plan. This page covers what is implemented:

- the preprocessor (L1a);
- the parser and the source formatter (L1b; see [Parsing](#parsing) and
  [Formatting](#formatting));
- semantic checking: types, names and calls (L1c-1), then flow rules, style
  rules and fixes (L1c-2); see [Checking](#checking);
- lowering to one Slang module (L1d); see [Lowering to Slang](#lowering-to-slang);
- the `luslc` command-line compiler (L1d); see [luslc](#luslc);
- `.lusl` sources in `ShaderCompiler` requests and the native worker, and SDK
  declarations of FoundationMath (L1e-1); see
  [LUSL in compiler requests](#lusl-in-compiler-requests) and
  [SDK declarations](#sdk-declarations);
- render-graph programs written in LUSL, include directories and the ray
  callback declarations (L1e-2); see [Render-graph programs](#render-graph-programs);
- interfaces, extensions, struct constants and interface-constrained templates
  (L2a); see [Interfaces and templates](#interfaces-and-templates).

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
| `#include "path"` | Searches beside the including file, from the snapshot root, then below each `PreprocessRequest::IncludeDirectories` entry. Accepts only `.luslh` headers; Slang code is declared with `extern slang` instead. Paths cannot be absolute, use `\`, or leave the root. No `<path>` form |
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
| `MAX_INCLUDE_DIRECTORIES` | 8 | `PreprocessRequest::IncludeDirectories` |
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
| `MAX_TEMPLATE_PARAMETERS` | 8 | Type parameters of one template |
| `MAX_ARRAY_LENGTH` | 1,048,576 | Elements of one array dimension |
| `MAX_DIAGNOSTICS` | 256 | Retained diagnostics; later ones are counted |
| `MAX_DIAGNOSTIC_MESSAGE_BYTES` | 256 | One message; longer ones are clipped |
| `MAX_IR_NODES` | 4,194,304 | Lowered IR nodes; fallthrough copies make it exceed `MAX_SYNTAX_NODES` |
| `MAX_GENERATED_HELPERS` | 65,536 | Generated Slang helpers per translation unit |

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
| LUSL0304 | `later-phase-construct` | `concept`, `requires`, `dynamic` and generic structs, which later phases add |
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
| LUSL0541 | `use-before-assignment` | A variable read, or written in part, before it is assigned on every path |
| LUSL0542 | `out-parameter-not-assigned` | An `out` parameter may be unwritten at a return |
| LUSL0543 | `missing-return` | A function with a return type can reach its end |
| LUSL0544 | `recursion` | A function calls itself, directly or indirectly |
| LUSL0545 | `groupshared-initializer` | A `groupshared` variable not declared `= 0` or `= void` |
| LUSL0546 | `reserved-name` | A name the generated Slang cannot declare: a Slang keyword such as `let` or `this`, or a name starting with `_lusl` or `__` |
| LUSL0547 | `missing-requirement` | A struct or extension does not provide a method or constant an interface it conforms to requires |
| LUSL0548 | `template-deduction` | A template parameter that no parameter's type names, or call arguments that do not satisfy the template's constraints |
| LUSL0601 | `naming-style` | A name that breaks the naming rules (style) |
| LUSL0602 | `bare-bool-operand` | A `bool` used directly as a condition or `&&`/`||` operand (style) |
| LUSL0701 | `extern-slang-not-found` | The file an `extern slang` block names is not in the snapshot |
| LUSL0702 | `lowering-limit` | The generated Slang would exceed `shader_compiler::MAX_SOURCE_BYTES`, or lowering exceeds `MAX_IR_NODES` or `MAX_GENERATED_HELPERS` |

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
| Declarations | Variables with `static`, `const` or `groupshared`, arrays and `= void`; functions with `in`, `out` and `inout` parameters, semantics and defaults; `struct` with fields, `static const` members, methods and a base list (`struct Paint : Material`); `interface` with method and constant requirements; `extension Type : Interface { ... }`; `template <Interface T>` and `template <typename T>` functions; `enum class` with an underlying type; `extern slang "path" { ... }`; `static_assert`; attribute lists such as `[stage(compute), threads(8, 8, 1)]` |
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

Concepts, `requires` clauses and generic structs are reported as later-phase
constructs.
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
and optional `CheckOptions`, and produces a `SemanticModel`:

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
- **`extern slang { ... }`**, without a path, declares what the including Slang
  code provides, such as the types a render-graph wrapper defines before it
  includes a callback. Lowering writes no `#include` for it.
- **Extern structs and the zero rule:** `= 0` and designated initializers of an
  extern struct set only the fields LUSL declares. Declare every field when
  either is used.
- **Reserved names:** declarations keep their names in the generated Slang, so
  a LUSL name cannot be a word Slang reserves (`let`, `var`, `this`, `import`,
  `typealias`, ...) or start with `_lusl`, which generated helpers use, or
  `__`. Names inside `extern slang` blocks are exempt.

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
  Overloads must differ in parameter types: two that differ only in
  directions are a redeclaration, because call-site marks do not take part in
  the choice.
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

### Flow rules

When types, names and calls check without errors, `Check` analyzes each function's
paths. Flow rules are skipped after a type error, so they never cascade from one.

- **Definite assignment.** A local declared without an initializer must be
  written on every path before it is read. Writing part of it (`v.x = 1`) or
  passing it `inout` counts as a read. An `out` argument writes it. A loop may
  run zero times, unless its condition is constant `true`. `T value = void;`
  opts out: such a variable is not tracked.
- **`out` parameters** must be written on every path before each `return` and
  before the end of the function. `discard` ends a path without writing them.
- **Returns.** A function with a return type must not reach its end. An
  infinite `while (true)` loop without `break` does not reach it.
- **Case endings.** Every `switch` case except the last ends in `break`,
  `return` or `[[fallthrough]]`.
- **Recursion** is rejected, direct or through other functions and methods;
  GPU code has no call stack.
- **`groupshared`** variables are declared `= 0` or `= void`.

Bounds checks on runtime indices and the `groupshared` clear itself are
emitted by the [lowering](#lowering-to-slang).

### Style rules

Style rules report as `CheckOptions::Style` says:
- `StyleSetting::Strict` (the default, used by the engine repository): errors.
- `Warn`: warnings, and the result stays Ready.
- `Off`: not reported.

Language rules do not depend on the setting. Declarations in `extern slang`
blocks are exempt, because they mirror Slang names.

| Name | Rule |
| --- | --- |
| Structs, enums, enumerators, functions, methods, fields | PascalCase |
| Parameters, locals, `groupshared` variables | lowerCamelCase |
| `out` and `inout` parameters | `out` or `inout` then PascalCase: `outRadiance`, `inoutSeed` |
| `static const` constants | UPPER_CASE |
| `bool` fields, parameters and locals | A question: `IsVisible`, `isVisible`, `outIsVisible`, also with Has, Should or Can |

Naming diagnostics suggest a conforming name; renaming is left to the author
because it changes every use. A `bool` value used directly as a condition or as
an `&&`/`||` operand is reported too: write `isVisible == true`.

### Fixes

A `Diagnostic` may carry a `DiagnosticFix`:
- **What it is:** up to `MAX_FIX_EDITS` text edits to the request's files. Apply
  them from the highest byte offset down.
- **When it is offered:** only when the affected tokens are spelled in a file,
  not produced by a macro or a configuration value, and when the repair keeps
  the expression's grouping.

| Diagnostic | Fix |
| --- | --- |
| `logical-not` | `!x` becomes `x == false`, parenthesized where the context binds tighter than `==` |
| `type-mismatch` with an explicit conversion available | Wrap the value: `float32(i)`, `float32x3(1.0)` |
| `non-bool-condition` on a numeric scalar | Append ` != 0` when the condition binds tighter than `!=` |
| `direction-mismatch` | Insert, change or remove the `out`/`inout` mark |
| `use-before-assignment` | Initialize the declaration with ` = 0` |
| `groupshared-initializer` without an initializer | Append ` = 0` |
| `bare-bool-operand` | Append ` == true` |

Capability-gated types (`float16`, `int16`, 64-bit types) are accepted here;
target profiles gate them when LUSL lowers to Slang.

## Lowering to Slang

`EmitSlang` (in `emitter.hpp`) lowers a checked translation unit to one Slang
module. `LowerToSlang` (in `pipeline.hpp`) runs every stage from a snapshot:

```cpp
#include <ludus/tooling/shader_language/pipeline.hpp>

bool LowerShade(const language::PreprocessRequest& request, language::LoweringResult& outResult) noexcept
{
    language::LoweringOptions options;
    options.Emit.ShouldCheckBounds = true; // development build
    return language::LowerToSlang(request, options, outResult) == language::Status::Ready;
}
```

`LowerToSlang` runs Preprocess, Parse, Check and EmitSlang, and stops after the
first stage that fails; warnings do not stop it. The result owns its text, the
source map, the bounds-check sites, the read masks and every stage's
diagnostics.

The generated module belongs beside its source: `main_shaders/shade.lusl`
lowers to `main_shaders/shade.lusl.slang` (`GENERATED_SLANG_SUFFIX`). Add it
to the snapshot under that path and compile it with `ShaderCompiler`.

Lowering works in two steps:
- **Lowering** resolves every rule into a typed, target-independent IR.
- **The emitter** spells that IR as Slang.

The IR is internal. It is the seam for direct backends later.

### What lowering does

| LUSL | Generated Slang |
| --- | --- |
| Types | Fixed-width scalars become Slang scalars (`uint32` is `uint`, `int64` is `int64_t`, `float16` is `half`); vectors and matrices use the short names (`float3`, `uint4x4`) where Slang has them, otherwise `vector<T, N>` and `matrix<T, R, C>` |
| Untyped constants | Spelled in the type the checker gave them: `4u`, `0.5f`, `9000000000ll`, `2.0h`, `0.1l`; integer and bool constant expressions are emitted as their exact folded value |
| The literal `0` | An explicit zero: `float3(0.0f)`, or a generated `_luslZeroN()` that zeroes every field and element |
| Designated initializers | A generated `_luslInitN(...)` that takes the written fields in written order; omitted fields take their default or zero |
| Named arguments and defaults | Positional arguments with defaults filled in. When the written order differs from parameter order and an argument calls a function, a generated adapter `_luslCallN` takes them in written order, so they are evaluated as written |
| `M * v`, `A * B` on matrices | `mul(M, v)`, `mul(A, B)` |
| Signed `+`, `-`, `*`, negation, compound assignment, `++`, `--` | A generated `_luslWrapN` helper whose `__target_switch` writes the operation directly, and through unsigned arithmetic on Metal ([signed overflow](../../architecture/shading-language.md#signed-overflow)) |
| Shifts by a runtime count | The count masked to the operand's width: `value << (count & 31u)` |
| `[[fallthrough]]` | The next case's statements are copied into the case, each case's statements in their own block, so no target needs switch fallthrough |
| `groupshared T name = 0;` | Every compute entry point that reaches the variable, through any calls, clears it first: each thread zeroes a strided slice of its elements, then `GroupMemoryBarrierWithGroupSync()`. The entry point reuses its `GroupIndex` parameter, or gains a hidden `SV_GroupIndex` one. `= void` variables are not cleared |
| Fields inside methods | `this.Field`, `this.Method(...)` |
| `[stage(compute), threads(x, y, z)]` | `[shader("compute")]`, `[numthreads(x, y, z)]`; semantics become `SV_DispatchThreadID`, `SV_GroupThreadID`, `SV_GroupID` and `SV_GroupIndex` |
| `extern slang "path"` | `#include` of the resolved snapshot file, relative to the generated module. The file is searched as `#include` searches: beside the declaring file, from the snapshot root, then below each include directory. Each file is included once; a missing file is `extern-slang-not-found`. `extern slang { ... }` writes nothing |
| `static_assert` | Nothing; the checker evaluated it |

Declarations keep their LUSL names, so Slang code can include the generated
module and call them. Generated helpers start with `_lusl`, which the checker
reserves, and are numbered in first-use order. Equal inputs give
byte-identical text on every host.

### Development bounds checks

With `EmitOptions::ShouldCheckBounds`, every runtime index into a fixed-size
array, vector or matrix goes through a generated `_luslCheckIndexN`:
- an index inside the object is used as is;
- an index outside it is clamped to the last element, and the access's
  failure code is latched in `FAILURE_CODE_VARIABLE` (`_luslFailureCode`), a
  private per-invocation `uint`.

The first failure is kept; zero means no check failed. `EmitResult::BoundsChecks`
maps each failure code to its index expression. Constant indices are checked by
the checker in every build, and release builds emit no checks. Reporting the
latched code through the render graph's invocation failure arrives with the
render-graph integration in L1e.

### Line mapping

The generated text carries `#line` directives, so Slang's diagnostics, and
debuggers that read them, name LUSL lines. A directive is written only where the
generated line would not already count to the right LUSL line. Code that a
macro produced maps to the macro's outermost use. `EmitResult::SourceMap` also
lists the generated line and the LUSL line and column of every declaration and
statement.

### Targets

`ShaderCompiler` checks one module against every portable target at once:
SPIR-V, WGSL and Metal. A type that one of them lacks therefore fails them all.
For example, WGSL has no 16-bit or 64-bit integers, so `int16` and `int64`
values fail the portable set, and the Slang diagnostic names the type.

### Current limits

- **Floating constant expressions** such as `0.1 * 3.0` are evaluated in the
  target type when the shader runs. Only integer and bool constants are folded
  exactly.
- **GPU readback** covers wrapping, the zero rule and shift masks through a
  render graph (see [Render-graph programs](#render-graph-programs)). Not yet
  covered: bounds-check failures, which need the render graph to report
  `FAILURE_CODE_VARIABLE`, and the Metal loop code-generation measurement.

## luslc

`luslc` (`tools/luslc`) is the command-line front of the same library:

```bash
luslc check  --root shaders main/shade.lusl
luslc lower  --root shaders --define SHADOW_MODE=2 --bounds-checks --output out/shade.lusl.slang main/shade.lusl
luslc format --root shaders --check main/shade.lusl main/common.luslh
```

- **Snapshot.** Paths are `/`-separated and relative to `--root`, which
  defaults to the current directory. `luslc` reads the root file, then every
  file that `#include` and `extern slang` name, transitively. Each name is
  resolved beside the naming file, then from the root, as the preprocessor
  resolves it. The scan is lexical, so it may read files that a conditional
  group never uses. The snapshot keeps `ShaderCompiler`'s limits: 64 files and
  4 MiB.
- **Options:**
  - `--include-directory DIR` adds an include directory, relative to `--root`;
    repeat it for more.
  - `--define NAME=VALUE` passes a configuration value; repeat it for more.
  - `--style strict|warn|off` selects the [style setting](#style-rules).
  - `--bounds-checks` emits [development bounds checks](#development-bounds-checks).
- **`check`** reports diagnostics only.
- **`lower`** writes the Slang module to `--output`, or to standard output.
  The output file is replaced atomically.
- **`format`** formats each file:
  - by default it prints the result;
  - `--check` fails for files that would change;
  - `--write` rewrites them in place.
- **Diagnostics** go to standard error, one per line:
  `main/shade.lusl:4:16: error LUSL0504 type-mismatch: ...`. Diagnostics from
  a macro also name the macro's use.
- **Exit codes:** 0 on success, 1 when a stage fails or a file cannot be read or
  written, 2 for a usage error.

## LUSL in compiler requests

`ShaderCompiler` accepts a `.lusl` root. When `Request::Source` ends in
`.lusl`, `Compiler::Compile` lowers it with `LowerToSlang` before Slang sees
anything. The native worker does the same for requests it receives.

- **Snapshot.** Slang compiles the generated module at
  `Request::Source + ".slang"`, beside the source, so its `#include`
  directives resolve. A file already at that path in the snapshot is replaced.
- **LUSL fields of `Request`** apply only to a `.lusl` Source:
  - `Configuration`: the selected configuration values, visible as macros.
    `shader_language::ConfigurationValue` is the same record.
  - `Style`: the [style setting](#style-rules), `LanguageStyle::Strict` by default.
  - `ShouldCheckBounds`: [development bounds checks](#development-bounds-checks).
- **Results:**
  - `SourceReadMask` has one byte per request file. It counts the files that
    either the LUSL preprocessor or Slang read, so build identity tracks LUSL
    headers and the Slang files their `extern slang` blocks name.
  - `ConfigurationReadMask` records which configuration values the source read.
  - `GeneratedSource` holds the generated module, for read-only display and
    build identity.
- **Diagnostics** take Slang's form: `shaders/shade.lusl(5,16): error LUSL0504:
  expected uint32, found float32`. Existing consumers of compiler diagnostics,
  such as the editor's shader overlay, read them unchanged. LUSL warnings come
  before Slang's diagnostics, and Slang's diagnostics name LUSL lines through
  `#line`.
- **Failures.** If lowering fails, Slang does not run, and the result carries the
  LUSL errors with `CompileFailed`, or `InvalidRequest` for a malformed LUSL
  request such as an invalid configuration name.
- **Wire format.** The worker protocol carries the same fields as optional JSON
  members: `configuration` as `[name, value]` pairs, `style` and
  `bounds_checks` in requests; `configuration_read_mask` and
  `generated_source` in replies. Slang requests omit them.
- **Worker identity.** The worker's implementation identity hashes the LUSL
  sources too, so build keys that record the worker change with the LUSL
  compiler.

## SDK declarations

`math.luslh` declares FoundationMath's `math.slang` for LUSL:
- its constants, such as `PI` and `RANDOM_MAX_DIMENSION`;
- `RandomAddress`;
- its functions, from `ComputeErrorFunction` to `ApplyNormalWorld`.

It is installed beside `math.slang` in `lib/cmake/Ludus/shaders`. Declarations
keep their Slang names and parameter names, so named arguments use the Slang
parameter names: `MakeRandomAddress(scope = id.xy, event = 0, dimension = 1)`.
A target test calls every declaration and compiles the module for SPIR-V, WGSL
and Metal.

`ray_shader.luslh` declares the ray callback types `Ray`, `RayLaunch` and
`RayHit` for [render-graph programs](#render-graph-programs). The ray wrapper
defines them before it includes a callback, so the header uses a path-less
`extern slang` block. `RayLaunch` declares the members every wrapper provides;
scene-interface wrappers add more, such as `SampleIndex`, which LUSL cannot read
yet. The scene queries (`TraceClosest`, `TestVisibility`, ...) get declarations
generated from Slang reflection in L3.

## Interfaces and templates

L2a adds the nominal half of the [hybrid design](../../architecture/shading-language.md#interfaces-concepts-and-templates):
interfaces, conformance, extensions and templates constrained by interfaces.
Concepts, `requires`, `if constexpr` and constraint ordering arrive in L2b.

```c
interface Material
{
    static const uint32 LOBE_COUNT;
    float32x3 Evaluate(in float32x3 normalWorld, in float32x3 lightWorld);
};

struct CarPaint : Material
{
    static const uint32 LOBE_COUNT = 3;
    float32x3 Albedo;
    float32x3 Evaluate(in float32x3 normalWorld, in float32x3 lightWorld)
    {
        return Albedo * dot(normalWorld, lightWorld);
    }
};

extension FoundationGgx : Material // an extern slang struct
{
    static const uint32 LOBE_COUNT = 1;
    float32x3 Evaluate(in float32x3 normalWorld, in float32x3 lightWorld)
    {
        return float32x3(Roughness);
    }
};

template <Material M>
float32x3 Shade(in M material, in float32x3 normalWorld)
{
    return material.Evaluate(normalWorld, lightWorld = normalWorld) * float32(M::LOBE_COUNT);
}
```

- **Interfaces** declare method requirements without bodies and `static const`
  requirements without values. They inherit other interfaces with a base list.
  An interface is a constraint, not a value type: a parameter or variable of
  interface type is an error that suggests a template parameter.
- **Conformance** is explicit. A struct names its interfaces in its base list,
  and an `extension` adds conformances and members to an existing struct,
  including an `extern slang` one. The checker verifies that each requirement,
  including those of base interfaces, is provided:
  - a method with the same name, parameter types, directions and return type;
  - a constant with the same name and type.

  Requirements of `extern slang` structs are left to Slang.
- **Struct constants:** `static const` members with constant values, read as
  `CarPaint::LOBE_COUNT`, or by name inside the struct's methods.
- **Templates** take interface-constrained parameters (`template <Material M>`) or
  unconstrained ones (`template <typename T>`) and declare one function.
  - Calls deduce every template argument from the arguments.
  - Each template parameter must be the type of some parameter.
  - A deduced type must conform to its constraints, through base interfaces too.
  - Inside the template, a value of type `M` offers the methods its constraints
    require, and `M::NAME` reads a constant requirement. Fields, `= 0` and
    designated initializers are not available for `M`, since its type is not
    known there.
  - At equal cost a function is preferred to a template.
  - Templates cannot be entry points.
- **Lowering.** Interfaces, base lists and extensions lower 1:1 to Slang
  `interface`, `struct Name : I` and `extension`. Templates lower to Slang
  generics (`float3 Shade<M : Material>(M material, ...)`), and calls pass
  their deduced arguments explicitly (`Shade<CarPaint>(paint, normalWorld)`).
  `M::NAME` becomes `M.NAME`. Call adapters for reordered named arguments
  become generic too.

## Render-graph programs

A render-graph program's `source` may name a `.lusl` file:

```json
{
  "id": "Miss",
  "stage": "miss",
  "source": "main_shaders/miss.lusl",
  "function": "ShadeMiss",
  "input": "Ray",
  "output": "Color",
  "bindings": []
}
```

```c
#include "ray_shader.luslh"

float32x3 ShadeMiss(in Ray ray)
{
    return float32x3(0, 0, 1);
}
```

- **Lowering.** The build lowers each `.lusl` source once with `luslc`, from
  the same captured snapshot as the rest of the build
  (`scripts/python/ludus_tools/render_graph_language.py`). SDK headers resolve
  from `_ludus_ray`, where ray builds stage engine helpers. The generated module
  joins the snapshot at `<source>.slang`, and the program names it from then on,
  so wrappers, closures, caches and diagnostics treat it as authored Slang.
  Slang diagnostics name LUSL lines through `#line`.
- **Finding `luslc`.**
  - Build-tree graphs use the `luslc` target.
  - Installed SDKs use their `bin/luslc`.
  - `LUDUS_LUSLC` overrides both, for example for the CLI or editor live builds.
  - A graph without LUSL programs needs no `luslc`.
- **Build identity.** The generated module's bytes are part of each program's
  source closure. Packages with LUSL programs also record the `luslc` executable
  and the lowering module, so a new compiler rebuilds them.
- **Style setting.** The project-wide key is the CMake cache variable
  `LUDUS_LUSL_STYLE`, with the values `strict` (the default), `warn` and `off`.
  A game project sets it once, in its `CMakeLists.txt` before compiling graphs or
  in its presets. `ludus_tools` builds outside CMake read the `LUDUS_LUSL_STYLE`
  environment variable. The engine repository always builds `strict`.
- **Current limits:**
  - LUSL programs that need wrapper types use the
    [ray callback declarations](#sdk-declarations), or declare them in their
    own path-less `extern slang` block. Raster schema structs have no SDK
    declarations yet; the schema generator gains a LUSL output in L3.
  - Configuration values do not reach LUSL programs from the render graph yet,
    and render-graph builds emit no development bounds checks.
- **Evidence:**
  - `rg2_raytracing_lusl` is `rg2_raytracing` with its generation, miss and
    closest-hit callbacks ported to LUSL. A GPU test renders both packages and
    compares every pixel of three frames.
  - `rg2_lusl_semantics` colors its miss pixels with checks computed on the
    GPU: wrapping of signed `+`, `-`, `*`, negation and `++`, the zero rule on a
    struct with an array and a defaulted field, and masked shift counts.
    On Metal this exercises the unsigned-arithmetic rewrite.

## Tests and fuzzing

`ludus_shader_language_tests` (CTest labels `unit;shader;tooling`) covers:

- the lexer;
- every directive, the C rescanning rules, and `#` and `##` with placemarkers;
- conditions, configuration values, includes and request validation;
- every grammar construct as a golden tree, the parser's syntax rules, and
  error recovery;
- each checker rule with a failing case, and the model's types, symbols,
  bindings and constants;
- each flow and style rule, the style setting, and every fix applied to the
  source and checked again;
- the formatter's golden layout, token preservation and idempotence;
- every lowering rule as generated Slang text, the source map and bounds-check
  sites, `extern slang` includes, and lowering failures;
- limits and determinism.

Seeded random tests run the preprocessor, the parser, the formatter and the
checker over token mixtures.

`ludus_shader_language_target_tests` compiles lowered modules with the pinned
Slang through `ShaderCompiler`, for SPIR-V, ray-query SPIR-V, WGSL and Metal,
with and without bounds checks. It also checks:
- that Slang diagnostics name LUSL lines;
- how the portable set treats 16-bit integers;
- `.lusl` requests: read masks, configuration values, diagnostics and the style
  setting;
- the [SDK declarations](#sdk-declarations).

It is built when the native compiler library is (`LUDUS_BUILD_SHADER_WORKER=ON`).
`luslc_tests` runs `luslc` on temporary snapshots and checks that every `.lusl`
and `.luslh` file in the repository is formatted. The worker protocol test
sends a `.lusl` request through a running worker. `test_render_graph_language.py`
covers render-graph lowering with a stand-in `luslc`. The GPU tests of
`ludus_render_graph_ray_tests` cover ported pixels and defined behavior.

For coverage-guided fuzzing with native Clang, configure with
`-DLUDUS_BUILD_SHADER_LANGUAGE_FUZZERS=ON` and run `ludus_shader_language_fuzzer`.
- Its input is split at the first zero byte into a root module and one header.
- It preprocesses, parses and checks the snapshot with style rules off, lowers
  it with and without bounds checks, and formats the root.
