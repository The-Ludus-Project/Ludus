# ShaderLanguage: the LUSL front end

`Ludus::ShaderLanguage` is the compiler front end of LUSL, the Ludus Shading
Language. The [LUSL design](../../architecture/shading-language.md) owns the
language and the delivery plan. This page covers what is implemented:

- the preprocessor (L1a);
- the parser and the source formatter (L1b; see [Parsing](#parsing) and
  [Formatting](#formatting)).

Semantic checking and lowering to Slang arrive in later L1 changes, and
`ShaderCompiler` does not accept `.lusl` files yet.

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

## Tests and fuzzing

`ludus_shader_language_tests` (CTest labels `unit;shader;tooling`) covers:

- the lexer;
- every directive, the C rescanning rules, and `#` and `##` with placemarkers;
- conditions, configuration values, includes and request validation;
- every grammar construct as a golden tree, the parser's syntax rules, and
  error recovery;
- the formatter's golden layout, token preservation and idempotence;
- limits and determinism.

Seeded random tests run the preprocessor, the parser and the formatter over
token mixtures.

For coverage-guided fuzzing with native Clang, configure with
`-DLUDUS_BUILD_SHADER_LANGUAGE_FUZZERS=ON` and run `ludus_shader_language_fuzzer`.
- Its input is split at the first zero byte into a root module and one header.
- It preprocesses and parses the snapshot, and formats the root.
