# Localization: catalogs, release cooking and formatting

`Ludus::Localization` builds on native and Emscripten. It delivers exact authored
identities, offline translation validation, immutable catalog leases and a
portable cooked format for two profiles:

- `ludus-static-utf8-v1` (package version 1): literal text cooked from one source
  and one translation document. No formatting grammar is interpreted: `{player}`,
  apostrophes, combining marks and whitespace remain literal bytes. Passing an
  ICU-profile source to this cooker fails explicitly.
- `ludus-icu-mf1-v1` (package version 2), phase L1 of the
  [architecture](../architecture/localization.md): a whole release manifest is
  validated with pinned ICU4C, argument-free messages are cooked as the static
  text ICU decodes (`'{'name'}'` becomes `{name}`, `''` becomes `'`), messages with
  arguments keep their pattern and argument schema, fallback is baked per record,
  and per-domain descriptors bind keys and schemas exactly. See
  [release cooking](#release-cooking-ludus-icu-mf1-v1).

Phase L2 formats messages with arguments through typed, schema-validated calls
and the pinned ICU4C backend in the opt-in [`Ludus::Unicode`](foundation/unicode.md)
owner; see [formatting](#formatting-messages-with-arguments). Builds without
`LUDUS_BUILD_UNICODE` keep the same API, never link ICU and report
`BackendUnavailable`. Locale negotiation, schema-preserving language switches,
consumer presentation caches (`LocalizedText`) and Text paragraph integration
remain later phases. UTF-8 validity does not imply font coverage, bidi or
rendering.

## Author and cook

Source JSON uses profile `ludus-static-utf8-v1`. Each record contains a stable
Content-style key, positive `source_revision`, literal `text`, and nonempty
translator `context`. See [the example](../../modules/localization/tests/source.json). Keep whole labels or
sentences in one record; localize complete messages rather than joining words.

```sh
./scripts/localization export-review --source modules/localization/tests/source.json \
    --locale fr-FR --output out/localization/fr-FR.json
```

The export carries source wording, context and a SHA-256 review receipt. A
translator edits `text` and deliberately changes `status` from `needs-review` to
`approved`. Approval certifies this exact source text, context and revision.
Regenerating an export resets approval. The cooker never automatically approves
or copies source wording into translated text.

```sh
./scripts/localization cook --source modules/localization/tests/source.json \
    --translation out/localization/fr-FR.json --output out/localization/fr-FR.loc \
    --header out/localization/ui_keys.hpp --namespace game::l10n::ui
```

Without `--translation`, cook the source locale. Every translated key must have
current source provenance and approved text by default. `--allow-source-fallback`
explicitly allows missing/unapproved entries to use source text. It never accepts
stale receipts or malformed translation records. Each resolved entry reports
`Source`, `Translation` or `SourceFallback`; the catalog retains requested and
source locale labels. Fallback is baked offline, with no runtime fallback search.

The generated header contains only exact `MessageKey` constants. Translation,
context and wording edits leave it unchanged; schema/key edits rebuild consumers.
Default names derive from keys, e.g. `menu/play` becomes `kMenuPlay`. Ambiguous
names fail generation; provide a distinct PascalCase `symbol` suffix on the source
record. Use a distinct explicit C++ namespace for each domain's header.

Unknown fields, duplicate JSON members/keys, unpaired surrogates, embedded NUL,
excess nesting and limits fail with a diagnostic. Input files are never overwritten.
Outputs are deterministic, individually replaced atomically, and left untouched
when bytes are unchanged. All content/header validation completes before writes;
an I/O failure between two output replacements may leave different generations.
Treat the command's successful exit as the publication boundary.

Locale labels are project-configured ASCII labels with the shape
`[A-Za-z]{2,8}(-[A-Za-z0-9]{1,8})*`, bounded to 128 bytes. They are case-sensitive;
the codec does not canonicalize, validate the IANA registry, accept every BCP47
grandfathered/private tag, negotiate a language or consult OS preferences.

## Runtime use

Link `Ludus::Localization` and load bytes through the application's existing
Content/file owner. The runtime does no I/O, parsing of authoring JSON or logging.
Failures are explicit statuses plus cooked-byte offsets; presentation policy
chooses a visible missing-text marker and rate-limited diagnostics.

```cpp
#include <ludus/localization/catalog.hpp>

using namespace ludus::localization;
Catalog active;
Diagnostic diagnostic;
// `bytes` is a borrowed std::span<const ludus::foundation::uint8>.
const auto status = PrepareCatalog(bytes, ludus::foundation::GetSystemAllocationDomain(),
                                   active, diagnostic);
if (status == Status::Ok)
{
    Catalog lease = active;
    MessageBinding play;
    ResolvedText text;
    if (lease.BindMessage({"game/ui", "menu/play"}, play) == Status::Ok &&
        lease.ResolveStatic(play, text) == Status::Ok)
    {
        // Consume text.Text while `lease` remains alive.
    }
}
```

`PrepareCatalog` validates bounds, exact ordering, UTF-8 and provenance before
making one fallible shared allocation through `AllocationDomain`. It owns a copy
of the bytes and preserves the previous output on any failure. The domain/context
must outlive all leases. Copying/acquiring a lease allocates nothing; separate
copies may be read concurrently after synchronized publication. Access to the
same mutable handle requires caller synchronization.

Binding performs an exact domain comparison and binary search over readable keys.
For a version 2 package it also compares the expected argument schema (see
[descriptors](#generated-descriptors)); a key-only `MessageKey` bind is the
empty-schema case, so a message with arguments returns `SchemaMismatch`.
`ResolveStatic` checks a non-reused process token and accesses one record in O(1),
with no allocation, hash table, formatting or locks. It returns
`RequiresArguments` for a message that declares arguments, even when one
translation happens to omit them all; [`Format`](#formatting-messages-with-arguments)
serves those records.
`ResolvedText` reports `Origin` (`Source`, `Translation`, `SourceFallback` or, for
an intermediate catalog on the manifest's fallback path, `Fallback`) and the
provider's locale label. Keep bindings and leases in
the consumer and resolve when presentation changes. This is a complexity and
allocator-tested contract, not a throughput benchmark.

Each successful load has a fresh token, including identical files. Rebind after
replacement; cross-catalog handles fail with `WrongCatalog` and preserve the
result. Old leases retain their token and text. This deliberately conservative
first phase does **not** implement the full design's schema-compatible binding
reuse, context manager or visible-view language-switch transaction. An application
can prepare a candidate, bind all its visible labels, and swap its own state at a
safe UI boundary. Catalog copies are the lifetime mechanism, not a global service.

Borrowed strings are counted views and are not NUL-terminated. Keep at least one
lease alive through consumption. Public view-producing operations reject temporary
catalogs, but C++ cannot detect a view retained after destruction of the last lease.
Moved-from catalogs are invalid. Every failing binder/resolver preserves its output.

## Release cooking (`ludus-icu-mf1-v1`)

Documents, receipts and the release manifest follow the
[L0 specification](../architecture/localization-l0.md#source-document-version-1);
[`modules/localization/corpus`](../../modules/localization/corpus/release.json) and
the smaller [fallback fixture](../../modules/localization/tests/release/release.json)
are complete examples. A release is cooked in one command:

```sh
./scripts/icu-probe bootstrap      # once; pinned, checksum-verified ICU4C
./scripts/icu-probe cook
./scripts/icu-probe run --preset macos-clang-development   # builds ludus_localization_profile
./scripts/localization cook-release --manifest modules/localization/corpus/release.json \
    --output out/localization/release \
    --profile-checker out/build/macos-clang-development/tools/icu-probe/ludus_localization_profile \
    --bindings game/ui=game::l10n::ui
./scripts/localization inspect out/localization/release/release.json
./scripts/localization inspect out/localization/release/packages/game/ui/fr-FR.loc
```

Use the `linux-clang-*` presets on Ubuntu. The cooker first validates the
manifest, every document, every review receipt and the fallback graph, then
resolves each `(locale, key)` to its first approved provider. It never parses
MessageFormat itself: the `ludus_localization_profile` host tool validates every
resolved pattern with pinned ICU4C, in the locale that provides it, through the
same `MessagePattern` profile walk as the [L0 probe](../architecture/localization-l0.md#message-profile-ludus-icu-mf1-v1).
The tool also rejects a locale whose language has no data in the pinned slice and
reports its ICU, CLDR and Unicode versions, which must equal the manifest's
`linguistic_data`. Any rejection lists every failing `(domain, locale, key)` and
writes nothing.

For an argument-free message, the checker decodes ICU's apostrophe syntax by
walking ICU's parsed parts and cross-checks the result against ICU
`MessageFormat` with zero arguments; the package stores that text. A message whose
source schema declares arguments is stored as its pattern plus the full schema,
in every locale. The checker is a cooking-host tool: it is never installed or
built for the browser. Runtime formatting uses the same pinned build through the
opt-in `Ludus::Unicode` owner. Following the L0 allocation-failure findings, an
ICU allocation failure in the checker is fatal rather than reported as a verdict.

### Recorded verdicts

`--record-profile-results FILE` (with `--profile-checker`) writes the checker's
identity, per-locale data verdicts and each item's verdict and decoded text, keyed
by the SHA-256 of its canonical `(locale, argument schema, pattern)`.
`--profile-results FILE` replays such a recording instead of running ICU: an item
absent from it fails the cook, so a recording can only reproduce verdicts the
pinned checker gave for identical inputs. Ordinary builds and the wasm tests use
this to cook their fixtures without ICU; the ICU-enabled
`ludus_localization_profile` test regenerates the committed recordings
([corpus](../../modules/localization/tests/corpus-profile-results.json),
[fixture](../../modules/localization/tests/release/profile-results.json)) and
requires byte equality. Shipping releases should run the native checker;
`release.json` records which kind produced the verdicts.

### Outputs

| Path under `--output` | Content |
| --- | --- |
| `packages/<domain>/<locale>.loc` | One package version 2 per domain and manifest locale |
| `bindings/<domain>.hpp` | Descriptors for each `--bindings DOMAIN=NAMESPACE` |
| `source-map.json` | Per record: provider, document, source and translation revisions, kind |
| `release.json` | Completion manifest: profile, linguistic data, checker identity and kind, input and cooker SHA-256, every output's size and SHA-256 |

All validation completes in memory before any write. Outputs are deterministic,
contain no absolute paths or timestamps, are replaced atomically one by one,
are left untouched when unchanged, and `release.json` is written last. Consumers
treat `release.json` as the publication boundary and verify package digests
against it; `inspect release.json` does that and summarizes each package. An I/O
failure between replacements can leave files that disagree with the previous
`release.json`, which that verification reports. Stale files from earlier
releases are not deleted.

### Generated descriptors

Each header contains, per source message in key order, a `MessageDescriptor` with
its exact key and complete argument schema:

```cpp
#include "bindings/game/ui.hpp" // generated

ludus::localization::MessageBinding count;
if (lease.BindMessage(game::l10n::ui::kInventoryItemCount, count) == Status::Ok)
{
    // ResolveStatic returns RequiresArguments; format through a prepared context.
}
```

Each message with arguments also gets a typed helper that erases its parameters
to the non-template `Format` (no pattern parsing or templates in the header):

```cpp
ludus::localization::FormatResult result;
char text[256];
const Status status = game::l10n::ui::FormatInventoryItemCount(context, count, 3, text, result);
```

Helper parameters follow the schema: `text` and `enum` become `std::string_view`,
`count` becomes `int64` and `number` becomes `float64`. Snake-case names become
camelCase (`picker_gender` is `pickerGender`); a name that would be a C++ keyword
or a helper parameter (`context`, `binding`, `output`, `outResult`, `arguments`)
gains an `Argument` suffix.

Symbols derive from keys as in the static profile (`inventory/item-count` becomes
`kInventoryItemCount`, its arguments `kInventoryItemCountArguments`, an enum's
tokens `kInventoryItemCount<Argument>Values`, its helper `FormatInventoryItemCount`),
or from a source `symbol`. Any
symbol collision fails generation. The header depends only on the source keys and
argument schemas: wording, context, revision and translation edits leave it
byte-identical. Binding compares the name and type of every argument, plus
`origin`/`max_bytes` for text, `min`/`max` for count and number, and `unknown` and
the ordered `values` for enums; any difference returns `SchemaMismatch`.

## Formatting messages with arguments

Phase L2 of the [architecture](../architecture/localization.md#runtime-api-and-lifetime).
Configure with `-DLUDUS_BUILD_UNICODE=ON` after `./scripts/icu-probe bootstrap`
(see [Unicode](foundation/unicode.md#acquiring-icu)), start the host once, then
prepare a formatter context for the dynamic messages a view will show before it
becomes visible:

```cpp
#include <ludus/foundation/unicode/unicode.hpp>
#include <ludus/localization/formatter.hpp>
#include "bindings/game/ui.hpp" // generated

using namespace ludus::localization;
// Host startup: memory hooks, embedded data, warm-up (see the Unicode page).
static_cast<void>(ludus::foundation::unicode::InitializeUnicodeHost(ludus::foundation::GetSystemAllocationDomain()));

MessageBinding count;
MessageBinding loot;
// lease.BindMessage(game::l10n::ui::kInventoryItemCount, count), ... as above.
const MessageBinding visible[] = {count, loot};
FormatterContext context;
if (PrepareFormatterContext(lease, visible, ludus::foundation::GetSystemAllocationDomain(), context) == Status::Ok)
{
    char text[256];
    FormatResult result;
    if (game::l10n::ui::FormatLootPickedUp(context, loot, "feminine", 3, text, result) == Status::Ok)
    {
        // std::string_view(text, result.Written); result.ProviderLocale names the grammar used.
    }
}
```

`PrepareFormatterContext` is the cold step: it checks the lease (a version 2
package), every binding, the running host and the package's linguistic-data
label against the runtime's (`LinguisticDataMismatch` otherwise), then parses
each dynamic pattern once with ICU `MessageFormat` in its **provider** locale,
so a fallback message keeps the grammar of the catalog that provided it (an
`fr-CA` package formats an `fr-FR` fallback with French plural rules and reports
`Origin::Fallback`). Static bindings and duplicates need no preparation. At most
`MAX_PREPARED_MESSAGES` (4096) bindings fit one context. The context copies the
lease, so its `FormatResult::ProviderLocale` views stay valid while it lives.

`Format` validates the complete source schema before ICU sees a value, following
the [L0 boundary rules](../architecture/localization-l0.md#message-profile-ludus-icu-mf1-v1):

| Schema type | Accepted value | Rejected with `InvalidArguments` |
| --- | --- | --- |
| `text` | `MakeTextArgument`: UTF-8 without NUL within `max_bytes` | Invalid UTF-8, NUL, too long; for `user` origin also C0/C1 controls, U+202A–U+202E and U+2066–U+2069 |
| `count` | `MakeCountArgument` within `min`/`max` | Another kind (including a `float64`), out of range |
| `number` | `MakeNumberArgument` (finite) or an integral `MakeCountArgument`, within `min`/`max` | NaN, infinity, out of range, text |
| `enum` | `MakeEnumArgument` with a declared token; any valid token selects `other` for `unknown: other` | Undeclared token for `unknown: reject` |

Every declared argument must appear exactly once, in any order; missing,
duplicate and undeclared names fail, even when this locale's translation omits
an argument. Inserted text is wrapped in FSI (U+2068) … PDI (U+2069); keep those
invisible controls for bidi (L4) and show them explicitly in tools. Number
display rounds half-even (`0.25` with `::.0` is `0.2`). A static record copies its
cooked text without ICU and still rejects arguments.

Output is copied only when complete: `BufferTooSmall` reports
`FormatResult::RequiredBytes` and leaves the caller's bytes unchanged, every
failure leaves them unchanged and resets the result, results above
`MAX_FORMATTED_BYTES` (64 KiB) return `Limit`, and output is counted, never
NUL-terminated or truncated. Other statuses: `WrongCatalog` for another lease's
binding, `NotPrepared` for an unprepared dynamic message, `BackendFailure` if ICU
rejects what cooking accepted, and `OutOfMemory` when the context's own
`AllocationDomain` storage fails (the output context is then unchanged). An ICU
allocation failure is never reported: ICU4C 78.3 cannot recover, so it terminates
through FoundationBase's emergency path.

A context owns mutable ICU formatters and UTF-16 scratch. It is neither reentrant
nor shareable: one thread uses it at a time, and a worker prepares its own. There
is no implicit global or thread-local cache. Destroy every context before
`ShutdownUnicodeHost`. Retained presentation (`LocalizedText`, argument revisions
and cached results) is consumer-owned and remains later work, as do date, money
and other typed formatters.

**Measured cost (diagnostics, not budgets).** `ludus_localization_format_probe`
measures the nested Arabic select/plural message on macOS arm64 Development
under a loaded machine. Allocation counts were stable across runs and match the
L0 shape: host initialization 45 ICU allocations; the cold first preparation
and format 162; preparing one message with warm caches 7; reformatting a changed
count 3; a static record 0. Peak ICU heap was about 48 KB. Wall times varied by
an order of magnitude between runs (for example reformat 3–5 µs, preparation
8–31 µs), so they are not estimates. Each run also prints a digest of every
corpus status and output byte; native and wasm32 print the same value.

## Cooked wire format, v1

All integer fields are unsigned little-endian uint32, decoded with FoundationBase
bounded byte codecs. There are no native structs, pointers, alignment assumptions,
hash identities, compressed blocks or private SDK headers.

| Header byte offset | Field |
| --- | --- |
| 0 | Magic `LLOC` (`0x434f4c4c`) |
| 4 | File version, 1 |
| 8 | Exact total byte count |
| 12 | Message count, at most 65,536 |
| 16, 20, 24 | Domain, selected-locale, source-locale byte lengths, each at most 128 |
| 28 | Record width, 20 |
| 32 | Literal UTF-8 profile ID, 1 |
| 36, 40, 44 | Reserved, zero |

Immediately after the 48-byte header are the three counted ASCII labels, then
`count` records. Each record contains absolute key offset, key length, absolute
text offset, text length, and origin (`0` source, `1` translation, `2` source
fallback). Records are sorted strictly by exact ASCII key bytes. A source-locale
catalog accepts origin 0 only; other catalogs accept 1 or 2 only.

Payload follows the record directory. Each key and text is packed consecutively
in record order. The loader requires exact offsets, no overlapping slices/gaps,
and no trailing bytes; empty text is valid. Keys are at most 128 bytes, text at
most 64 KiB, and a package at most 32 MiB. Authoring JSON is capped at 8 MiB and
32 nested containers. Cooked text preserves scalar values without normalization.

The file is structurally validated, not authenticated. Approved-source SHA-256
receipts belong to the offline workflow and are not a runtime signature. Content
transport/package integrity remains the acquiring owner's responsibility. Future
pattern codecs must use a new profile/version and validate their own schema.

## Cooked wire format, v2

Package version 2 carries profile ID 2, `ludus-icu-mf1-v1`. The same reader entry
point accepts both versions. All fields are unsigned little-endian uint32 unless
noted; bounds are signed int32 (two's complement).

| Header byte offset | Field |
| --- | --- |
| 0, 4, 8, 12 | Magic `LLOC`, version 2, exact total byte count, message count (at most 65,536) |
| 16, 20, 24 | Domain, locale, source-locale byte lengths, each at most 128 |
| 28, 32 | Record width 40, profile ID 2 |
| 36 | Provider count P, 1–8 |
| 40, 44 | Argument count A (at most 32 per message), argument record width 36 |
| 48 | Enum value count E (at most 64 per argument) |
| 52 | Linguistic data label length, 1–128 printable ASCII, for example `icu4c-78.3/cldr-48/unicode-17.0` |
| 56, 60 | Reserved, zero |

After the 64-byte header come the domain, locale, source-locale and linguistic
labels, then four directories: P provider slices, the message records, A argument
records and E enum-value slices. A slice is an absolute offset and a length.

| Record field (40 bytes) | Meaning |
| --- | --- |
| 0, 4 | Key slice |
| 8, 12 | Value slice: static UTF-8 text (at most 64 KiB) or dynamic pattern (at most 16 KiB) |
| 16 | Kind: 0 static, 1 dynamic |
| 20 | Provider index into the provider path |
| 24, 28 | First argument index, argument count; static records have none, dynamic records at least one |
| 32 | Source revision, nonzero |
| 36 | Flags: bit 0 required; other bits zero |

| Argument field (36 bytes) | Meaning |
| --- | --- |
| 0, 4 | Name slice, `[a-z][a-z0-9_]*` within 64 bytes, unique in its message |
| 8 | Type: 0 text, 1 count, 2 number, 3 enum |
| 12 | Policy: text origin (0 game, 1 user) or enum unknown policy (0 other, 1 reject); zero otherwise |
| 16 | Text `max_bytes` 1–4096; zero otherwise |
| 20, 24 | Count/number `min` ≤ `max` within ±1,000,000,000; zero otherwise |
| 28, 32 | First enum value index and count (1–64 unique tokens, never `other`); zero otherwise |

The provider path is the locale's manifest fallback path: it starts at the
package locale, ends at the source locale, has unique labels and has one entry
exactly for a source-locale package. Payload strings are packed with no gaps,
overlap or trailing bytes in this order: provider labels; then for each record,
its key, its value, and for each argument its name followed by its enum tokens.
Records are sorted strictly by key bytes; argument and enum indices are
consecutive. The reader validates all of this, UTF-8 without NUL and every limit
before its one allocation. Like version 1, a package is structurally validated,
not authenticated: transport integrity belongs to the acquiring owner (the
filesystem pack's CRC32, for example), and review receipts stay offline.

The profile checker protocol between the cooker and `ludus_localization_profile`
is private and versioned (`LPRQ`/`LPRS`, version 1); see
[`profile_checker.cpp`](../../tools/icu-probe/profile_checker.cpp) and
[`localization_release.py`](../../scripts/python/localization_release.py).

## Validation

Native Catch2 tests use production-cooker fixtures: source/translation/fallback,
exact binding, default/moved owners, transactional OOM, one-allocation preparation,
zero-allocation leases/resolution, concurrent retained readers, unaligned bytes,
every truncation, corrupt offsets/metadata/UTF-8 and a byte-mutation corpus.
Python tests validate source receipts, strict authoring, deterministic outputs,
limits, generated bindings and CLI failures. The same cooker fixtures and runtime
execute under wasm32 through the pinned SDK Node emulator. The installed SDK
consumer checks the public API and static link closure.

Version 2 adds, natively and under wasm32: ICU-decoded static escaping in every
corpus locale; per-record fallback providers and origins; dynamic records that
refuse static resolution; exact descriptor binding under every schema-field
change, prefix and near-collision keys; lease lifetime across replacement,
transactional failure and OOM; every truncation, targeted field corruption and a
single-byte mutation sweep. The release-cooker tests cover stale and reused
receipts, unrecorded inputs, profile rejections, linguistic-data and locale-data
mismatches, invalid UTF-8 and limits, deterministic machine-independent outputs,
preserved outputs on failure, descriptor collisions and header stability, the
package decoder, the checker protocol and the `cook-release`/`inspect` CLI. With
ICU built, `ludus_localization_profile` additionally regenerates both recordings,
checks every [profile rejection fixture](../../modules/localization/corpus/profile-rejections.json),
static decoding, missing locale data and a malformed request.

L2 adds, with `LUDUS_BUILD_UNICODE`: all 125 [corpus expectations](../../modules/localization/corpus/expectations.json)
(109 outputs and 16 argument failures) formatted from the cooked version 2
packages, natively in Catch2 and through `ludus_localization_format_probe` on
native and wasm32 with byte equality against the same expected bytes; typed
helpers; provider-locale fallback grammar; nonfinite, invalid-UTF-8, NUL, control,
override, isolate and duplicate arguments; enum unknown policies; the output and
result-reset contract; unprepared, foreign and version 1 bindings; preparation
limits and transactional `OutOfMemory`; linguistic-data mismatch on a relabelled
package; lease retention and moves; host shutdown after contexts. The probe's
driver ([`test_format_probe.py`](../../modules/localization/tests/test_format_probe.py))
denies each ICU allocation of host initialization, preparation and the first
format in a fresh process and requires every point to terminate through the
emergency path (207 of 207 natively). Without the option, the tests check the
`BackendUnavailable` path instead.

```sh
python3 -m unittest discover -s scripts/python -p test_localization.py -v
python3 -m unittest discover -s scripts/python -p test_localization_release.py -v
./scripts/icu-probe run --preset linux-clang-development   # profile checker, Unicode and L2 format tests
./scripts/icu-probe run --preset web-emscripten-release    # L2 format probe under wasm32
./scripts/test linux-clang-development
./scripts/test linux-clang-asan-ubsan
./scripts/check linux-clang-development --all
./scripts/test web-emscripten-development
./scripts/install-sdk linux-clang-development
```
