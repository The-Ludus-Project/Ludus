# L0: localization schemas, message profile and ICU backend probe

**Status:** L0 of the [localization architecture](localization.md), implemented on
Ludus `5117c8c`. It specifies the `ludus-icu-mf1-v1` authoring documents, release
manifest and message profile; checks them against a representative corpus; and
proves the pinned ICU4C backend in a private, exception-free probe on native and
wasm32. No public localization API changes.

Outcome: ICU4C 78.3 is **accepted** as the L2 formatting backend for semantics,
exception-free builds and size, and **rejected** as a recoverable-out-of-memory
backend. Every corpus output matched on both targets, but single injected
allocation failures crash ICU, select the wrong plural branch with a success
status, leave sticky errors and leak past `u_cleanup`. L2 must treat an ICU
allocation failure as fatal; see [allocation-failure findings](#allocation-failure-findings).
The CAT/XLIFF exit criterion is **not met**: no CAT/TMS product has been chosen,
so the interchange profile below is a candidate. See [open gates](#open-gates).

The shipped static slice ([Localization](../modules/localization.md),
`ludus-static-utf8-v1`) is unchanged. Its JSON shape, receipts and limits are the
starting point for the documents below; differences are called out.

## Reproduce

`LUDUS_BUILD_ICU_PROBE` defaults to OFF. Ordinary configure, build, install and
SDK consumers neither download nor require ICU. Bootstrap is the only network
action.

```sh
./init.sh --cli macos-clang-development --preset-only --locked --no-system-install
./scripts/icu-probe bootstrap
./scripts/icu-probe cook
./scripts/icu-probe run --preset macos-clang-development
./scripts/icu-probe run --preset macos-clang-asan-ubsan
./scripts/icu-probe check --preset macos-clang-development
./scripts/localization-corpus validate --manifest modules/localization/corpus/release.json
python3 -m unittest discover -s scripts/python -p test_localization_corpus.py -v

# Browser toolchain from config/web_toolchain.json, then:
./scripts/icu-probe run --preset web-emscripten-release
```

Use the `linux-clang-*` presets on Ubuntu. The same bootstrapped tree now backs
the opt-in [`Ludus::Unicode`](../modules/foundation/unicode.md) owner and L2
formatting: [`cmake/IcuProfile.cmake`](../../cmake/IcuProfile.cmake) compiles it
once for the probe, the L1 checker and `Ludus::Unicode`, and `run` also builds and
tests those. `bootstrap` verifies the pinned source
and data archives in [`config/icu_toolchain.json`](../../config/icu_toolchain.json),
builds upstream's data tools in a separate host process, and slices the data with
[`tools/icu-probe/data-filter.json`](../../tools/icu-probe/data-filter.json).
Upstream tools never link into Ludus. `cook` embeds that data and the corpus as
private generated sources under `out/icu-probe/generated`. `verify` (run by CMake
at configure and before every build) compares each compiled upstream file with
the verified archive and rejects stale data, corpus or generator inputs. Each run
writes `evidence.json` beside the probe executable.

## Representative corpus

[`modules/localization/corpus`](../../modules/localization/corpus/release.json)
holds one `game/ui` domain: an `en-US` source, `fr-FR`, `ru-RU`, `ar` and `ja-JP`
translations, a release manifest, 125 expectations (109 formatted outputs and 16
argument failures) and 23 profile rejections. It covers literal text, ICU
apostrophe escaping, an intentionally empty approved translation, an optional
message, exact selectors, plural offsets, cardinal and ordinal rules (including
French `many`, Russian `few`/`many` and Arabic `two`/`few`/`many`/`other`
boundaries with offsets), declared enums with both unknown
policies, nested select/plural, number skeletons, grouping separators, user text
in another script and a translation that legitimately omits an argument.

The translations are engineering fixtures written to exercise grammar paths.
They have **not** been reviewed by qualified translators and are not product
copy; expected outputs record what the pinned CLDR data must produce for them.

## Source document, version 1

UTF-8 JSON, one domain per file. Duplicate or unknown members, invalid UTF-8,
embedded NUL, more than 32 nested containers or more than 8 MiB fail.

| Field | Rule |
| --- | --- |
| `version` | Integer `1` |
| `domain` | Content ID grammar, at most 128 bytes |
| `source_locale` | Project locale label (static-profile shape), at most 128 bytes |
| `profile` | `ludus-icu-mf1-v1` |
| `messages` | At most 65,536 records with unique keys |
| `messages[].key` | Content ID grammar; the stable identity |
| `messages[].source_revision` | Positive uint32; increments with wording, argument meaning or meaning-bearing context |
| `messages[].pattern` | Profile pattern, at most 16 KiB UTF-8 |
| `messages[].arguments` | At most 32 declarations with unique names, possibly empty |
| `messages[].context` | Nonempty translator context, at most 16 KiB |
| `messages[].required` | Optional boolean, default `true`; release policy, excluded from receipts |
| `messages[].symbol` | Optional PascalCase binding suffix, as in the static profile |

Argument names match `[a-z][a-z0-9_]*` within 64 bytes. Each declaration has one
of these shapes:

| `type` | Required fields | Meaning |
| --- | --- | --- |
| `text` | `origin`: `game` or `user`; optional `max_bytes` 1–4096 (default 4096) | Counted literal UTF-8, never interpreted as a message |
| `count` | `min`, `max`: integers within ±1,000,000,000 | `int64`; plural/ordinal selector, `#`, or integer number display |
| `number` | `min`, `max`: integers within ±1,000,000,000 | Finite `float64` measurement; display only, never a selector |
| `enum` | `values`: 1–64 unique lowercase identifiers except `other`; `unknown`: `other` or `reject` | Finite authored tokens for `select` |

Version 1 spells numeric bounds as JSON integers so review receipts have one
canonical serialization. Fractional bounds need a later schema version.

## Translation document, version 1

One locale and domain per file. The header repeats `version`, `domain`,
`source_locale` and `profile` exactly, and adds `locale` (different from the
source). Each record has exactly these fields:

| Field | Rule |
| --- | --- |
| `key` | An existing source key; at most once per file |
| `source_revision` | The source revision that was reviewed |
| `reviewed_source_digest` | Lowercase SHA-256 receipt, produced by tooling |
| `translation_revision` | Positive uint32 owned by the translation workflow |
| `status` | `draft`, `translated`, `reviewed` or `approved` |
| `pattern` | Profile pattern in the translation's locale, at most 16 KiB |

The receipt is SHA-256 of compact, key-sorted, non-ASCII-preserving JSON of the
source `version`, `domain`, `source_locale`, `profile` and the message's `key`,
`source_revision`, `pattern`, `arguments` and `context`. It extends the static
profile receipt with the argument schema and replaces `text` with `pattern`.
`required` and `symbol` are excluded: release policy and code bindings are not
reviewed wording.

`stale` is derived, never authored. A record whose revision or receipt differs
from the current source is stale whatever its `status`; this also detects an
edit that reused a revision. Only an `approved`, current record can provide text.
An approved empty pattern is present text, not a fallback request.

## Release manifest, version 1

| Field | Rule |
| --- | --- |
| `version` | Integer `1` |
| `source_locale` | Must be declared in `locales` with `parent: null` |
| `profiles` | Exactly `["ludus-icu-mf1-v1"]` |
| `linguistic_data` | `backend: "icu4c"` and the pinned `icu`, `cldr`, `unicode` versions the runtime must report |
| `locales` | 1–128 unique `{locale, parent}` entries |
| `domains[].domain` | Unique domain ID |
| `domains[].source` | Relative path (no `..`) to the source document |
| `domains[].translations` | One relative document path for every non-source locale |
| `domains[].allow_required_fallback` | Translated locales whose required messages may resolve through fallback |

`parent` links form the message-fallback graph. Every path must be acyclic, reach
the source locale and contain at most 8 catalogs. Each `(locale, key)` resolves,
offline, to the first approved provider on its path. An optional message may
fall back; a required one may fall back only for a listed locale. Otherwise
validation fails. Locale negotiation, which chooses one of these locales from
player preferences, is a separate L3 policy.

`scripts/localization-corpus validate` checks all of the above. It does not
parse patterns: the probe validates them with ICU's own `MessagePattern`, so
Ludus maintains no second MessageFormat grammar. A shipping cooker (L1) must run
the same native profile check before it writes a catalog; the L1 release cooker
does so through `ludus_localization_profile`, which shares the probe's walk in
[`tools/icu-probe/profile.hpp`](../../tools/icu-probe/profile.hpp) (see
[release cooking](../modules/localization.md#release-cooking-ludus-icu-mf1-v1)).

## Message profile `ludus-icu-mf1-v1`

Patterns use ICU MessageFormat 1 with named arguments and apostrophe mode
`DOUBLE_OPTIONAL` (ICU's default, asserted by the probe): `''` is an apostrophe,
and `'` quotes only before syntax characters, so `don't` stays literal.

| Construct | Accepted only when |
| --- | --- |
| `{name}` | `name` is a declared `text` argument |
| `{name, number}` | `count` or `number`; style absent, `integer`, `::.0` or `::.00` |
| `{name, plural, …}`, `{name, selectordinal, …}` | `count`; optional integer `offset:` ≥ 0 |
| `=N` selector | Integer within the argument's declared bounds |
| Keyword selector | A category of the provider locale's cardinal or ordinal rules |
| `{name, select, …}` | `enum`; keywords are declared tokens or `other` |
| `#` | Inside a plural/ordinal branch |

ICU requires `other` in every selector. Rejected: numbered arguments, undeclared
arguments, `choice`, `date`, `time`, `spellout`, `ordinal`, `duration`, other
number styles (percent, currency, pattern strings and other skeletons), type
mismatches, dead plural keywords, non-integer exact values or offsets, nesting
deeper than 8 messages, more than 256 selector branches, and patterns over
16 KiB. Each rejection reason has a fixture in
[`profile-rejections.json`](../../modules/localization/corpus/profile-rejections.json)
or a generated probe case.

Translations may reorder, repeat or omit declared arguments and add their own
plural categories. Callers always supply the complete source schema: omission
in one translation never relaxes argument validation.

At the formatting boundary Ludus, not ICU, enforces the schema:

- Every declared argument exactly once; unknown, duplicate or missing names,
  wrong value kinds and out-of-range values return `InvalidArguments`.
- `number` must be finite. Display rounds half-even (`0.25` → `0.2` with `::.0`)
  and never changes gameplay state.
- `text` must be valid UTF-8 without NUL and within `max_bytes`. `user` text
  also rejects C0/C1 controls, bidi embeddings/overrides (U+202A–U+202E) and
  isolates (U+2066–U+2069); combining marks, ZWJ/ZWNJ and implicit marks stay.
  Every inserted text value is wrapped in FSI (U+2068) … PDI (U+2069).
- An `enum` token outside the declaration is rejected, or mapped to `other` when
  the declaration says `unknown: "other"`.
- Output is copied only when complete; a short buffer reports the required size
  and leaves caller bytes unchanged.

The isolates mean formatted text contains invisible controls. Text must keep
them for bidi (L4); tests and inspectors should display them explicitly.

## ICU backend configuration

ICU4C 78.3 (CLDR 48, Unicode 17.0; sources include tzdata 2026a) is pinned by
archive SHA-256. ICU 79.1 was a release candidate when L0 was implemented. The
probe compiles every upstream `common/` and `i18n/` translation unit with the
Ludus project options: C++23, `-fno-exceptions`, sanitizers and flavor, with
upstream warnings silenced. The pinned configuration statically links ICU,
assumes UTF-8 and excludes file I/O, services, MF2, codepage conversion, break
iteration, collation, transliteration, regular expressions and IDNA. Ludus code
additionally compiles with `U_HIDE_DEPRECATED_API`; upstream needs its own
deprecated declarations. Break iteration returns for L4 paragraph work through
the planned `Ludus::Unicode` owner, not through this profile.

The data filter keeps the five corpus locales with their parents, plus plural,
plural-range, numbering-system, locale-alias, key-type and version data, and the
currency tree. Two dependencies are not obvious: `DecimalFormatSymbols` fails
with `U_MISSING_RESOURCE_ERROR` without the `curr` tree (it loads currency
spacing even for plain numbers), and `PluralRules` fails without `pluralRanges`.
The slice removes zones, units, collation and conversion data and is 601,168
bytes. Whole locale bundles remain; resource-level filtering is a later size
optimization that needs its own tests.

The host owns ICU's process state:

1. Install `u_setMemoryFunctions` before any other ICU call.
2. Register the immutable embedded package with `udata_setCommonData`; ICU never
   opens files.
3. Warm ICU's lazily created process singletons: the static locale cache
   (`Locale::getRoot()`), `UnifiedCache` (create one `PluralRules`) and the
   number-skeleton tables (`NumberFormatter::forSkeleton`). A failure while
   creating any of them is undefined behavior (below), so this step runs at
   startup and its failure is fatal.
4. Create every `icu::Locale` from an explicit tag. The default constructor
   reads the host environment and, like the static locale getters, is not safe
   under allocation failure. Never set ICU's default locale.
5. Treat any ICU allocation failure after startup as fatal. The production hook
   reports through FoundationBase's emergency path instead of returning null.
6. Call `u_cleanup` only at host teardown, after every ICU object is destroyed.
   It also clears the memory hooks and data registration.

Other ICU error-channel gaps the adapter handles: `UnicodeString::fromUTF8`
silently replaces invalid bytes with U+FFFD, `Formattable::setString` cannot
report allocation failure, a missing named argument is formatted as its literal
`{name}` with success, and NaN formats as `NaN` with only a warning. The adapter
validates UTF-8 and the full schema first and adopts its own null-checked copies.

## Evidence

Recorded on macOS 26.6 arm64 with the pinned Clang 18 (`macos-clang-development`)
and Emscripten 4.0.23 in its bundled Node 22.16 (`web-emscripten-development`,
`web-emscripten-release`). The pinned Clang 18 ASan runtime deadlocks during its
own initialization on that macOS version, even for an empty program, so sanitized
evidence comes from batched CI. Run
[39026440464](https://github.com/The-Ludus-Project/Ludus/actions/runs/39026440464)
(develop `ea1c7d6`) repeated the probe on Ubuntu 24.04 x86_64 with `linux-clang-asan-ubsan` and
uploaded its `evidence.json` as `localization-l0-linux-sanitized-evidence`. Its
`ASan and UBSan` job passed; other jobs failed and the candidate was not promoted,
so the run is sanitizer evidence for this probe, not platform qualification. The
corpus run, cost measurements and host cleanup completed there without an ASan,
UBSan or LeakSanitizer report; UBSan halts on its first finding
(`-fno-sanitize-recover=undefined`), so any report would have failed the run.

**Build.** All 457 upstream `common/`, `i18n/` and `stubdata/` translation units compile with
`-fno-exceptions` under C++23 on both targets; `verify.py` rejects any ICU or
probe compile command without it. No ICU source is modified.

**Semantics.** Identical on macOS arm64, wasm32 and sanitized Ubuntu x86_64:

- The pinned service reports ICU 78.3, CLDR 48 and Unicode 17.0 and round-trips
  every corpus tag; each locale's number symbols come from its own data, not root.
- All 55 corpus patterns pass the profile check; all 23 fixture rejections and the
  generated oversized, over-branched and invalid-UTF-8 patterns fail with their
  expected reason.
- All 109 expected outputs match byte for byte, including Arabic dual and
  `few`/`many` boundaries with offsets, French `many` at 1,000,000 with U+202F
  grouping, Russian U+00A0 grouping, English ordinals, half-even rounding and
  isolate-wrapped text. CLDR 48 formats `ar` with Latin digits. All 16 invalid
  argument sets, duplicate names, NaN, infinity and invalid UTF-8 are rejected
  before ICU; a short buffer reports its required size and leaves caller bytes
  unchanged.

**Size.** The probe is compared with a baseline linking the same Ludus logging,
UTF-8 and time code without ICU. Totals include the 601,168-byte data slice.

| Target | Probe | Baseline | ICU, data and probe |
| --- | --- | --- | --- |
| wasm32 Release `.wasm` | 2,234,739 B (833,380 gzip) | 142,638 B (67,269 gzip) | 2.09 MB (766 KB gzip) |
| macOS arm64 Development, stripped | 2,869,136 B (1,149,725 gzip) | 113,976 B (34,884 gzip) | 2.76 MB (1.11 MB gzip) |

**Cost.** For the nested Arabic select/plural message: a cold first format after
host start makes 161 ICU allocations, constructing a `MessageFormat` with warm
caches makes 3, and reformatting a prepared formatter with a changed count makes
2, on both targets and under the Ubuntu sanitizers. Native Development times
were stable at about 61 µs, 2.7 µs and 1.1 µs. wasm32 Release times in Node
varied widely across repeated runs (cold 0.17–0.30 ms, construction 6–23 µs,
reformat 3–87 µs), so they are not usable as estimates. Peak ICU heap was about 100 KB. These are single-machine
diagnostics, not budgets.

### Allocation-failure findings

`verify.py` counts the allocations of host initialization and of three
representative formats, then denies each allocation once, in a fresh process per
point (a crash is data, not a verifier failure). After each attempt the same
process formats again without injection, then calls `u_cleanup` and counts
blocks still live. The final point of each sweep, with no denial, must be
correct and leak-free.

| Sweep (macOS arm64 Development) | Points | Crash | Reported OOM | Other error | Silent wrong output | Correct | Sticky | Leaks after cleanup |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Arabic nested select/plural | 161 | 2 | 83 | 47 | 7 | 22 | 80 | 16 |
| French `::.0` with grouping | 61 | 1 | 51 | 0 | 1 | 8 | 12 | 25 |
| Russian offset plural with text | 201 | 3 | 90 | 57 | 21 | 30 | 109 | 25 |
| Host initialization and warm-up | 41 | 9 | 27 (init failed) | 0 | 0 | 5 | 0 | 2 |

wasm32 Release shows the same classes with fewer crashes (1, 0, 1 and 1): a null
read in linear memory does not trap, so the same defects continue silently.

Ubuntu x86_64 under ASan/UBSan (`linux-clang-asan-ubsan`, the CI run above).
`verify.py` keeps a signal, including one that ASan reports (`SEGV`, `BUS`,
`FPE`, `ILL`, `ABRT`, stack overflow), as `crash` with its exit code; any other
ASan or UBSan finding is `sanitizer-report` with its kind; a LeakSanitizer report
at exit keeps the point's result and counts as a leak after cleanup. Each sweep in
`evidence.json` maps sanitizer-report points to their kind in
`sanitizer_report_points` (the ASan kind, `undefined-behavior` or `leak`) and to
the check that fired in `sanitizer_report_checks`: the ASan kind, `leak`, the
UBSan check name when the report prints one (`report_error_type=1`), or else the
first UBSan runtime error without its types, values and addresses, such as
`reference binding to null pointer` or `member access within null pointer`. A
UBSan report whose message cannot be parsed keeps `undefined-behavior`. UBSan halts
on its first finding, so a sanitizer-report point never reformats or reaches
`u_cleanup`, and Sticky and Leaks count only the points that ran to the end.

| Sweep (Ubuntu ASan/UBSan) | Points | Crash | Sanitizer report | Reported OOM | Other error | Silent wrong output | Correct | Sticky | Leaks after cleanup |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Arabic nested select/plural | 161 | 0 | 2 UBSan | 83 | 47 | 7 | 22 | 80 | 16 |
| French `::.0` with grouping | 61 | 0 | 21 UBSan | 31 | 0 | 1 | 8 | 6 | 15 |
| Russian offset plural with text | 201 | 0 | 3 UBSan | 90 | 57 | 21 | 30 | 109 | 25 |
| Host initialization and warm-up | 41 | 0 | 9 UBSan | 27 (init failed) | 0 | 0 | 5 | 0 | 2 |

- No point crashes with a signal and ASan reports nothing. Every sanitizer report
  is a UBSan runtime error, and every leak after cleanup is also a LeakSanitizer
  report. This run predates `sanitizer_report_checks`, so its evidence records
  only the kind `undefined-behavior`, not the UBSan check that fired.
- Arabic, Russian and host initialization match macOS in every other column; each
  macOS crash count reappears as the same number of UBSan reports (2, 3 and 9).
- French reports UBSan at points 8–27 and 30: by count, the macOS crash and 20
  points that report OOM there. Those points never reach the clean reformat or
  `u_cleanup`, so the French sticky and leak counts (6 and 15) exclude them;
  macOS counts 12 and 25.
- The 10 French exits that the earlier run
  [38973276690](https://github.com/The-Ludus-Project/Ludus/actions/runs/38973276690)
  could not attribute are UBSan findings. Its classifier recorded every nonzero
  exit as `crash`; its 36 French exits contain these 21 UBSan points, and the
  other 15 match the 15 LeakSanitizer reports, not 1 crash and 25 leaks.
- The silent wrong outputs are the same fail points (7, 1 and 21) on macOS,
  wasm32 and sanitized Ubuntu. Those processes exit cleanly under ASan, UBSan and
  LeakSanitizer: ICU selects the wrong branch with a success status, which
  sanitized testing does not detect.

Causes confirmed in the pinned sources:

- `Locale::getLocale` returns a reference through a null cache when the static
  locale cache cannot be allocated (`common/locid.cpp`, "return a nullptr
  reference"); `Format::Format()` copies `Locale::getRoot()`, so every later
  `MessageFormat` construction dereferences it. The failed init-once persists
  until `u_cleanup`.
- `Locale::Locale()` canonicalizes `uprv_getDefaultLocaleID()`, which is null
  after an allocation failure (`common/locid.cpp`, `locale_set_default_internal`).
- A failed `UnifiedCache` constructor is deleted by `cacheInit`; its destructor
  flushes a null hashtable (`common/unifiedcache.cpp`).
- `DecimalFormat::touch` calls `setupFastFormat()` after `NumberPropertyMapper`
  stored an allocation failure inside the formatter rather than in `status`,
  dereferencing null symbols (`i18n/decimfmt.cpp`).
- `PluralSelectorProvider::select` turns a failed `Formattable::getDouble` into
  `U_INTERNAL_PROGRAM_ERROR` (`i18n/msgfmt.cpp`).

Observed but not root-caused: a crash in `ures_getByKeyWithFallback` during
`DecimalFormatSymbols::initialize`, the success-status wrong plural branches
(for example the Arabic dual message replaced by its `few`, `other` or `one`
branch, and once an empty string), and the sticky and leaking states.

Consequences for L2: a recoverable-OOM contract for dynamic formatting cannot be
built on ICU4C 78.3. Ludus warms the singletons at startup, bounds inputs with
the profile limits, measures and budgets ICU memory, and makes an allocation
failure inside ICU fatal. Sanitized testing cannot stand in for that policy:
the wrong-branch results pass ASan, UBSan and LeakSanitizer cleanly. Static
catalogs never call ICU and keep their transactional OOM contract. These defects
should be reported upstream with the fail-point reproductions; a later ICU version must pass this sweep before Ludus
offers recoverable formatting.

## CAT interchange profile (candidate)

Exit requires a confirmed profile with a chosen CAT/TMS product. That product has
not been chosen, so nothing here is confirmed. The candidate for L5:

- XLIFF 2.1 core, one `<file>` per domain and target locale, one `<unit>` per
  message with `id` equal to the key. Never split selector branches into units.
- The complete ICU pattern is the `<source>`/`<target>` text. Arguments and
  selector syntax are protected either through the product's ICU MessageFormat
  filter or as `<ph>`/`<pc>` inline codes, whichever the product preserves
  losslessly. XLIFF 2.1 has no plural/select module.
- Context, argument schema, source revision and receipt travel as `<notes>` and
  `mda` metadata; import requires them unchanged, re-runs the profile check and
  produces a draft.

Confirmation needs a lossless export → product → import round trip of this whole
corpus, including the nested Arabic and Russian patterns, apostrophe escapes and
the empty Japanese translation, plus a stale-import conflict.

## Open gates

- **CAT/TMS profile:** unconfirmed (above).
- **Platforms:** local native evidence is macOS arm64 Development; wasm32
  evidence runs in the SDK's Node. Ubuntu x86_64 ASan/UBSan evidence from batched
  CI shows the same semantics and allocation-failure classes, with UBSan reports
  where macOS crashes and no ASan finding (above). The pinned macOS ASan
  runtime does not start on macOS 26.6. Windows, Android, iOS and real browsers
  have no L0 evidence.
- **Recoverable allocation failure:** ICU4C 78.3 fails this gate. The defects
  above are not yet reported upstream; a newer ICU must pass the sweep before L2
  can offer anything but fail-stop. ICU memory is measured, not capped.
- **Linguistic review:** fixture translations need qualified review before any
  product use.
- **Size:** whole-locale data slicing only; no resource-level filter or
  compression evaluation.
- **Unicode services:** bidi, line breaking and canonicalization beyond the
  corpus tags remain L4 work.
