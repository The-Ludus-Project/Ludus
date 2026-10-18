# Ludus Parsing Architecture

**Status:** P1 implementation added with the bounded parsing foundation and
JSON extraction. Later milestones remain proposed.

**Implementation baseline:** `55501b0` (2026-10-04). The original architecture
and Gems review were drafted against `52e29ce`; the implementation uses the
AllocationDomain and checked-integer APIs now present on main.

Ludus should share source ranges, bounded workspaces, explicit errors, and
checked reading primitives. Each format should retain its own grammar and each
subsystem its own typed validation. Start with the existing pinned JSON codec;
use an ordinary handwritten lexer and recursive descent for a small custom
language only when a real consumer needs one. Keep parsing off gameplay and
audio hot paths. Compile frequently loaded authored data into validated,
versioned runtime records when measurements justify that pipeline.

The design follows [AGENTS.md](../../AGENTS.md), the
[steering rules](../../.kiro/steering/coding-standards.md),
[ADR 0003](../decisions/0003-standard-library-usage-policy.md), the
[include boundary](foundational-headers.md), and the
[memory](memory-management.md), [containers](containers.md),
[strings](strings.md), and [content](content-resources.md) contracts.
FoundationMemory and the core FoundationStrings APIs now exist. P1 uses the
existing AllocationDomain for its fallible JSON workspace; later dictionary
and authoring proposals are not prerequisites.

“Best” here means correct, predictable, understandable, and fast on Ludus's
actual workloads. Algorithm selection below is engineering judgment informed
by current primary sources. No benchmark or universal speed advantage is
claimed. The companion review records the baseline, chapter evidence, adopted
changes, and rejected alternatives from the requested Gems reading.

## Decisions

1. Share a small parsing foundation, not a universal language framework.
2. Keep the bundled yyjson 0.10.0 backend for current JSON content. Preserve
   strict schemas and public Content APIs during incremental migration.
3. Separate syntax recognition, typed decoding, reference resolution, and
   resource preparation. Only a fully validated candidate can be published.
4. Use immutable input, counted byte ranges, explicit owners, and bounded
   workspaces. Allocation and cancellation failures are ordinary results.
5. Build no mandatory token array, DOM, AST, or concrete syntax tree across
   all formats. Choose the smallest representation each consumer requires.
6. Keep format grammar, schema version, and binary format version independent.
7. Reserve lossless and incremental parsing for source editing that needs it;
   reserve streaming and SIMD for workloads that demonstrate their benefit.
8. Keep dependency selection, numeric conversion, generated code, and module
   exports visible and reviewable. No hidden exception or allocator behavior.
9. Keep finite field/keyword vocabularies in one domain-owned definition; derive
   runtime dispatch and authoring metadata from it when duplication appears.
10. Preserve source provenance across decoding and cooking, with exact spans
    only where the backend or a validated source-map adapter can supply them.
11. Cook typed domain records and deterministic dictionaries, then test the
    same representation that will ship. Do not ship a generic token replay VM.
12. Give common resource formats explicit import profiles and domain owners.
    Reuse audited image, audio, font, and scene codecs behind private adapters;
    share checked reading and error contracts without adding those dependencies
    to the parsing core.

## Current implementation and migration boundary

| Existing code | Observed behavior | Design implication |
| --- | --- | --- |
| [Content JSON](../../modules/content/src/json.cpp) and [public facade](../../modules/content/include/ludus/content/json.h) | yyjson DOM with a fallibly allocated pool, 1 MiB input cap, postparse depth and duplicate-key checks, canonical bounded writer | Reuse the dependency and behavior; improve diagnostics and limits before optimizing |
| [yyjson build](../../third_party/yyjson/CMakeLists.txt) | Bundled 0.10.0 C11 source verified by SHA-256; static SDK dependency | A backend extraction must preserve the pin, hashes, license, and installed static link closure |
| [Catalog](../../modules/content/src/catalog.cpp) and [audio definitions](../../modules/audio/content/src/definitions.cpp) | Domain readers build typed values above JSON | Keep validation in those domains; Parsing must not understand audio or resource identity |
| [Game-host protocol](../../modules/runtime/game_host/src/protocol_codec.cpp) | Private closed JSON subset, bounded framing, scalar fields and flat records | Preserve wire grammar and error semantics; extraction is a separate compatibility change |
| [Diagnostic control codec](../../modules/foundation/base/src/diagnostic_control_codec.cpp) | Independent bounded infrastructure codec in Base | Keep it independent: Base cannot acquire a dependency on higher parsing modules |
| Editor/tool configuration | Existing Qt and Python readers | Do not force all tools through a new C++ abstraction; align document contracts where needed |

The Content facade returns a syntax error offset, or a field label through
domain validation. yyjson values do not supply a complete original-source span
map. Some protocol storage still uses standard allocating containers; a boolean
result alone does not prove recoverable OOM. Neither parser consolidation nor
adding `noexcept` fixes those existing paths automatically.

The first migration preserves today's JSON acceptance set. Changes such as
different number overflow behavior or configurable field limits need explicit
compatibility tests and release notes. Do not silently change an audio or
project schema while moving its parser implementation.

## Pipeline and responsibilities

```mermaid
flowchart LR
    IO[Content or tool acquires bytes] --> SRC[Immutable source + limits]
    SRC --> FORMAT[Format-specific syntax reader]
    FORMAT --> DECODE[Domain typed decoder]
    DECODE --> VALIDATE[Validate values and references]
    VALIDATE --> CANDIDATE[Owned candidate]
    CANDIDATE --> ACTIVATE[Owner publishes at safe boundary]
    CANDIDATE --> COOK[Optional deterministic cooker]
    COOK --> BINARY[Checked binary reader]
    BINARY --> ACTIVATE
    SRC --> EDIT[Optional editor syntax snapshot]
    EDIT --> DRAFT[Draft diagnostics and source edits]
    DRAFT --> RECHECK[Strict revalidation before Apply or Cook]
    RECHECK --> FORMAT
```

| Stage | Owns | Must not do |
| --- | --- | --- |
| Acquisition | File/fetch limits, trusted root policy, immutable byte owner | Hide I/O inside token access |
| Syntax | Grammar, encoding, delimiters, raw number spelling, structural limits | Interpret resource IDs, load assets, mutate the world |
| Typed decoding | Required/unknown fields, exact scalar types, owned values, schema version | Publish partially decoded objects |
| Semantic validation | Ranges, uniqueness, capabilities, cross references | Depend on parser storage after returning |
| Preparation/publication | Candidate budgets, asynchronous resources, safe activation/retirement | Treat syntax recovery as valid content |
| Editor | Dirty revisions, selection, source text, optional lossless tree | Activate an erroneous draft |
| Cooker | Validated dependency closure, explicit versions, deterministic output | Serialize native C++ layouts or process-local IDs |

A simple config uses bytes -> bounded JSON DOM -> typed candidate. A custom
expression uses bytes -> on-demand tokens -> Pratt parser -> compact AST.
An editor may retain a lossless tree; a cooked runtime reader usually needs
neither tokens nor an AST. Sharing the error and ownership contracts is enough;
there is no mandatory virtual `IParser` hierarchy or universal value variant.

### Choosing a reader

| Consumer | Default | Reconsider when |
| --- | --- | --- |
| Current bounded content/config JSON | Pinned yyjson DOM and typed decoder | DOM storage or syntax time materially dominates measured load budgets |
| Tiny closed wire protocol | Existing bounded private codec | Compatibility-preserving extraction demonstrably removes duplication or defects |
| Small custom statements/expressions | Handwritten lexer, recursive descent, optional Pratt expressions | Grammar ambiguity, growth, or recovery maintenance becomes costly |
| Large sequential import | Format library or bounded event reader building a private candidate | Full DOM peak memory fails the import budget |
| Edited source | Debounced immutable full parse | Measured latency or source-preserving operations justify a lossless/incremental tree |
| Repeated release loads | Validated domain binary reader after a measured cooker investment | Text parsing is already negligible, making another format unjustified |

For a streaming importer, `NeedMoreInput` preserves scanner state across chunk
boundaries, including split UTF-8, escapes, numeric exponents, and newlines.
An explicit end-of-input distinguishes incomplete input from a completed
document; require full consumption and valid terminal state. Limits are
cumulative across chunks. Borrowed event text expires before buffer refill;
retained values must be copied or refer to owned immutable chunks. Bound
candidate memory and queued events with backpressure. A low-memory syntax
reader cannot by itself make an unbounded output candidate low-memory.
Incremental input parsing is distinct from reparsing after editor edits.

## Game resource formats

The resource-format roadmap below was added on 2026-10-06. It proposes new
support; the rows marked existing identify current consumers, not a promise
that every variant of a file extension is accepted. The Gems review informed
the pipeline and ownership rules. The format specifications cited below supply
the separate evidence for these profiles.

Resource import follows bytes -> format reader/decoder -> owned domain
candidate -> semantic validation -> optional cooked artifact -> subsystem
preparation. Content owns acquisition, identity, dependency resolution, and
publication. Mesh/image import tools own source interpretation; Audio and Text
retain their codec owners. RHI owns device upload and retirement. Parsing owns
only the reusable reading primitives, limits, and diagnostic contracts.
See [resource preparation](resource-management.md#architecture-and-ownership)
and [cooking](resource-management.md#cooking-and-storage).

### Initial format inventory

| Format | Intended use and owner | Status and initial scope |
| --- | --- | --- |
| Strict JSON | Catalogs, game definitions, and domain configuration readers | Existing parsing backend; each domain supplies its own versioned schema |
| WAV / FLAC | Sound and music sources; Audio/AudioContent | Existing private pinned miniaudio decoder; preserve accepted sample/channel profiles and preparation limits |
| TrueType / OpenType fonts | Font faces, shaping, and rasterization; Text | Existing private FreeType/HarfBuzz integration; use the tested font profile, with collections and advanced font features separately qualified |
| PNG | UI images, sprites, and texture source pixels; image import tools | First proposed image slice: static images decoded into owned RGBA8 pixels under an explicit color policy |
| Wavefront OBJ | Simple static mesh interchange; mesh import tools | Implemented for scene import as `obj-best-effort-v1` with MTL (October 2026); a strict mesh-only profile remains proposed |
| glTF 2.0 / GLB | Scene, mesh, and material interchange; scene import tools | Implemented for scene import as the static triangle-mesh `gltf-best-effort-v1` profile (October 2026); skins, animation, morph targets, sparse accessors and compression remain separate gates |
| JPEG | Photographic texture sources; image import tools | Baseline and progressive decoding implemented for scene import textures through the private stb configuration; orientation metadata is ignored |
| Radiance RGBE / OpenEXR | HDR environment maps and textures; scene import tools | Implemented for scene import: Radiance through stb, an original OpenEXR subset reader (scanline/tiled, NONE/RLE/ZIPS/ZIP) |
| KTX2 | Mipmapped texture artifacts; texture cooker and graphics adapter | Later runtime texture slice when RHI supports the required formats; transcoders and supercompression are opt-in audited backends |
| Ludus domain binary | Cooked meshes, textures, levels, or other typed definitions | Per-domain versioned layouts using the checked binary contract below; each requires a real consuming subsystem |

Existing implementation owners are [Audio decode](../../modules/audio/src/internal/decode.cpp),
[Text font loading](../../modules/text/src/font_system.cpp), and the
[JSON facade](../../modules/foundation/parsing/include/ludus/foundation/parsing/json.hpp).
Adding this inventory does not extend their APIs or the version-1 Content
catalog's audio-only kinds. New resource kinds need their own compatibility
decision. FBX, USD, SVG, video, and additional audio codecs remain separate
consumer requests rather than dependencies of the first image/mesh slices.

### First image and geometry profiles

**PNG:** Use a proven private decoder, with allocator/error behavior audited
before selection. The initial profile accepts static grayscale, indexed, RGB,
and alpha images at legal sample depths up to 8 bits, including transparency
and both PNG interlace methods. It returns counted, owned RGBA8 rows with an
explicit row pitch. Reject 16-bit samples and APNG animation in this first
profile; do not silently discard precision or animation. Validate signature,
chunk order/lengths/CRCs, required chunks, and the end of the datastream.
Unknown critical chunks fail; permitted ancillary chunks can be skipped under
the metadata budget. Bound decompression and metadata as well as pixel output.
Texture import declares color versus data intent, color conversion, alpha
representation, and row orientation. Unsupported color profiles fail unless
the caller explicitly selects a documented fallback.

**OBJ:** Begin with a bounded line/token reader using a reviewed decimal
conversion backend. Accept `v`, `vt`, `vn`, and triangle `f` records, comments,
and declared object/group/smoothing metadata. Resolve positive one-based and
negative relative indices against the correct position/UV/normal arrays;
zero and out-of-range indices fail. Preserve face-corner tuples before
deterministically producing the domain's vertex/index representation. Check
finite values and numeric narrowing; document missing attribute and degenerate
triangle policy. The first mesh-only profile rejects material references,
nontriangle faces, lines, points, and free-form geometry explicitly. MTL
dependency resolution, polygon triangulation, normal/tangent generation, and
coordinate/unit conversion need declared later profiles or import settings.
Do not silently ignore a record that changes the geometry's meaning.

**glTF/GLB:** Reuse strict JSON recognition with limits chosen for scene import,
then validate container version/lengths, buffer views, accessors, indices, node
relationships, and finite transforms before exposing a scene candidate. The
first profile covers static triangle meshes; its accepted attributes and
material features must match a real consumer. Reject unsupported required
extensions and unsupported core features used by the asset. Optional extension
fallback follows the specification and the declared profile. Sparse accessors,
compressed geometry, embedded image codecs, skins, and animation are explicit
profile decisions. External files and decoded data URIs pass through a bounded
Content/tool resolver; a parser does not perform network or filesystem I/O.
Count embedded and external dependencies together and record their digests.

### Implemented scene-import profiles

The best-effort scene importers implement the OBJ and glTF/GLB proposals above
for *scene import*, where the goal is to fit authored content into the cooked
scene format with explicit diagnostics rather than to reject it. They follow
the structural rules of this section — bounded line and JSON readers,
content-based detection, a resolver for every dependency, digests for every
input, malformed/unsupported/limit/out-of-memory distinctions, and failures
that leave outputs untouched — but deliberately accept more than the strict
first profiles: OBJ polygons are fan-triangulated, MTL libraries are resolved,
glTF optional extensions are ignored per the specification and missing normals
or tangents are generated. Each such decision is reported in the import
report. Their supported-subset matrices, budgets and material conversions live
in [best-effort scene import](../development/best-effort-scene-import.md).
A strict mesh-only OBJ profile and the reusable PNG image profile for other
consumers remain proposals.

### Admission and compatibility gates

Each format profile specifies concrete default budgets and hard maxima before
implementation. The current JSON document cap is not an image, scene, audio,
or decoded-resource budget. Check arithmetic and admit growth before allocating.

| Resource family | Additional budgets and validation |
| --- | --- |
| Images | Width/height, pixel count, row/output products, compressed and expanded bytes, decoder scratch, chunks and metadata; retain color/alpha policy |
| Meshes/scenes | Line/token/numeric lengths, vertices, face corners, indices, nodes, materials, dependency count/bytes, tuple conversion and semantic work |
| Audio | Encoded bytes, channel/sample profile, frame count, decoded PCM, resampling workspace, and per-playback stream storage; preserve current behavior |
| Fonts | Font bytes, face/table counts, glyph and raster dimensions, shaping/output storage; qualify the existing backend's supported profile |
| Texture artifacts | Dimensions, layers/faces/mips, section ranges, compressed/expanded sizes, formats, decoder/transcoder workspace, and upload staging |

Readers distinguish malformed input, an unsupported valid feature, a configured
limit, and allocation failure. A failure leaves the prior resource and output
unchanged. A successful candidate owns surviving pixels, vertices, strings,
and dependency records, or documents an immutable blob lease. Format detection
checks content and the declared profile; an extension alone does not validate
bytes. Codec types and headers remain private, with explicit errors across the
adapter boundary. Decode/import stays off gameplay, GUI, and audio callback
paths; browser work also needs a measured scheduling and latency contract.

The proposed order is PNG and OBJ as independent first slices, followed by a
static glTF/GLB consumer, then JPEG and KTX2 when their consumers need them.
Each slice includes a written supported-subset matrix, representative exported
fixtures, a consuming tool or app, and usage/API documentation. Test truncation,
corrupt lengths/indices, expansion limits, allocation failure, unsupported
features, ownership, and old-output preservation; fuzz the format and typed
validation boundary under sanitizers. Compare image pixels or mesh semantics
with independently verified fixtures, not just a codec's own round trip.
Prove native/web and installed-SDK behavior for the targets that expose support.
Runtime GPU upload, cooked acceptance, and source-import acceptance are separate
claims. Selecting an established codec still requires a dependency pin, license,
exception/allocation audit, and source attribution near its eventual adapter.

### Consulted format specifications

Thanks to the following authors and organizations for the format contracts.
The supported subsets, proposed order, budgets, and Ludus module placement above
are engineering choices, not requirements of these specifications:

- W3C PNG Working Group, [*Portable Network Graphics (PNG) Specification (Third Edition)*](https://www.w3.org/TR/2025/REC-png-3-20250624/),
  sections 5, 8, 11, and 13: chunk validation, image representations, and decoder
  behavior; Ludus's first profile deliberately limits precision and animation.
- Wavefront Technologies, *The Advanced Visualizer*, version 3.0, Appendix B1,
  [*Object Files (.obj)*](https://www.martinreddy.net/gfx/3d/OBJ.spec)
  (archived original specification): face tuples and index semantics; the
  proposed triangle-only subset excludes the manual's wider geometry grammar.
- Khronos Group, [*glTF 2.0 Specification*](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html),
  sections on buffers/accessors, meshes/nodes, extensions, and GLB: distinguish
  container/schema validation from the supported scene profile.
- ITU-T / ISO/IEC, [*T.81: Digital compression and coding of continuous-tone still images — Requirements and guidelines*](https://www.itu.int/rec/T-REC-T.81/en):
  [1992 specification, annexes F and G](https://www.w3.org/Graphics/JPEG/itu-t81.pdf)
  (W3C-hosted copy): sequential and progressive coding processes; the eventual
  image adapter must declare which it accepts.
- Khronos Group, [*KTX File Format Specification, version 2*](https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html),
  sections 3 and 4: indexed mip data, representation metadata, and independently
  declared supercompression; a container reader does not establish GPU support.
- Industrial Light & Magic and the OpenEXR project, [*OpenEXR File Layout*](https://openexr.com/en/latest/OpenEXRFileLayout.html):
  header attributes, channel lists, offset tables, scanline/tile chunks and the
  RLE/ZIP predictors read by the scene importer's subset reader.
- Greg Ward, "Real Pixels", *Graphics Gems II* (1991), pages 80-83: the shared
  exponent RGBE encoding of Radiance `.hdr` files.
- Microsoft, [*Resource Interchange File Format (RIFF)*](https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-),
  and IETF, [*RFC 9639: Free Lossless Audio Codec (FLAC)*](https://www.rfc-editor.org/rfc/rfc9639):
  format context for the existing Audio codec; the repository's pinned backend
  and compatibility fixtures remain authoritative for current acceptance.

## Module boundary

Proposed placement is `modules/foundation/parsing/`, exported as
`Ludus::FoundationParsing`. Its initial surface is checked byte/text cursors,
source ranges, parse statuses, limits, and bounded diagnostic storage. It
depends on Base and explicitly used Containers, never Content, Platform,
Logging, Profiler, Qt, Audio, World, or RHI. No changes to `core.h` are needed.

An optional sibling target `Ludus::FoundationParsingJson` in the same module
owns the JSON facade and private yyjson integration. Consumers opt into it;
including the common parsing header does not include a JSON library. Content
eventually uses that facade through a compatibility adapter. Do not create
these targets until the initial extraction has a consumer and a tested SDK
export graph.

Public headers live under `include/ludus/foundation/parsing/`; implementation
and third-party interfaces stay under `src/` and `src/internal/`. Headers
include the required Base vocabulary first, then explicitly named Ludus
facilities, then lightweight standard headers. No public heavy STL includes,
regex engine, parser combinator templates, generated backend headers, or
third-party node types. Keep most behavior in `.cpp` files.

Domain schemas stay under Content, AudioContent, game content, or tools.
Language-specific lexers belong with their consumers until two concrete users
justify extraction. Shader languages keep their established compiler
toolchains; future scripting uses the selected language runtime/compiler.
Neither becomes a reason to build a new general compiler now.

## Representation and ownership

| Concept | Contract |
| --- | --- |
| `SourceView` | Borrowed immutable counted bytes plus a caller-provided source label; caller retains bytes through parsing, decoding, and any operation reading source ranges |
| `SourceRange` | Source index and half-open byte interval `[Begin, End)`; interpreted only through the owning source set |
| `ParseWorkspace` | Move-only scratch owner or caller-supplied storage; one active document at a time; explicit reset after all views die |
| JSON/node/token view | Borrowed, valid only while its document/workspace owner and required source bytes remain alive |
| Typed candidate | Owns all surviving strings, arrays, and values; independent of parse scratch |
| Editor snapshot | Immutable owned source, syntax storage, diagnostics, and optional span index for one document revision |

Synchronous loaders already own the bytes: borrow them rather than copy them
again. yyjson's existing non-in-situ mode owns decoded strings in its document
pool. A decoded escaped string is not necessarily a slice of original input.
Custom lexers return original spelling as a range and decode string escapes
only when the consumer requests the semantic value. Unescaped text can remain
a view until typed ownership is established.

Background editor work retains an immutable source snapshot, or receives an
explicit fallible byte copy. Never borrow from a mutable UI buffer. Every
result carries the caller's document/revision/request identity; the owner
discards results for superseded requests. Local source/node indices are not
global IDs and are not persisted. A byte offset without its owner and revision
is not a usable cross-job reference.

Use `usize` for arithmetic, container sizes, and indexing. A compact token
may encode offsets and node indices as `uint32` only after admission proves
the entire document fits and reserved sentinels cannot occur. Widen before
arithmetic; check conversion on every input boundary. The initial 1 MiB
documents easily fit; huge importers should use explicit chunks/streaming
instead of inflating every engine token. Exact token size is measured, not
fixed here as an ABI promise.

Allocate pools through the existing AllocationDomain, with explicit fallible
admission and release. Reusable caller storage can be a later entry point when
a consumer requires externally supplied scratch. Check allocation-size arithmetic;
preserve alignment; free with the matching owner. Do not allocate one heap
object per token, use global intern tables, or reset scratch on a frame marker.

Editor snapshots outlive worker scratch and own their persistent storage.
Parsed documents should be move-only with opaque internal state. Avoid the
legacy public opaque node field in future incompatible APIs; use a private
lightweight handle and checked accessors. P1 retains the field through the
Content compatibility facade; backend types remain private. This discourages misuse but does not make C++
borrowed views lifetime-safe: callers must still retain their owner.

## JSON policy and typed decoding

Retain strict UTF-8 JSON for existing content. JSON has no comments, macros,
includes, implicit expressions, or trailing commas. Do not add JSON5 features
under an unversioned flag. Reject malformed UTF-8, invalid surrogate escape
pairs, and trailing data. JSON strings can contain an escaped NUL; domain
types such as resource IDs and paths may reject it explicitly.

[RFC 8259](https://www.rfc-editor.org/rfc/rfc8259) supplies the syntax and
interoperability context. Ludus strengthens it with duplicate-key rejection,
explicit numeric domains, strict schema versions, and bounded documents.
Compare duplicate keys after escape decoding, so `"x"` and `"\u0078"` collide.
Default equality is exact decoded UTF-8 bytes, with no case folding or Unicode
normalization. Resource identity rules remain owned by Content.

Decode each small schema object in one pass where practical: classify each
field, reject unknown/duplicate fields, record a required-field bitset, then
validate completeness. For a fixed vocabulary, a short exact string dispatch
is clearer than a runtime hash map. Keep the existing facade where migration
cost outweighs saved scans. Large extensible schemas can use a bounded sorted
table or measured flat map with exact equality and collision-safe lookup.

Types stay exact: do not coerce a string to a number, a missing field to null,
or a fractional real to an integer. Required, optional, defaulted, and explicit
null are distinct schema decisions. Version dispatch happens before domain
interpretation; migrations are explicit transformations of owned definitions.
Unknown future versions fail rather than being rewritten destructively.

### Shared schema vocabulary

Boer's shared constants and Price's generated schema workflow expose a real
maintenance problem: runtime field names, authoring completion, and cooker
tags can drift. Keep each small schema in a readable domain-owned definition
of spelling, field ID, type, required/default policy, and version. Initially a
private C++ descriptor table plus explicit decoder is sufficient; cross-field
and reference validation remain ordinary named functions.

If an editor and cooker need the same vocabulary, generate their metadata
from that one definition with a small explicit generator. Record the input
digest and generator version, use deterministic field IDs, and check generated
outputs in CI. IDs must be explicitly stable for persisted/wire schemas;
renaming or reordering a descriptor must not accidentally change compatibility.
Do not infer disk layout from arbitrary C++ source or build a reflection system.

Reinalter's literal hashing is useful only if a measured vocabulary warrants
it: precompute a discriminator with the proposed Strings literal descriptors,
then compare length and exact bytes before choosing a field. Collision tests
are required in Release too, including adversarial unknown names. For today's
small objects, straightforward exact dispatch remains the default.

Code/data sharing does not require preprocessing authoring files with C++.
Keep JSON's existing grammar. A future DSL import or constant facility needs
a separate language decision: explicit resolver-owned I/O, dependency records,
cycle/depth/file/expanded-byte limits, deterministic resolution, and use-site
plus definition-site provenance. No ambient filesystem search, arbitrary code
execution, or unbounded C-like macro substitution in the parsing foundation.

### Numbers

Recognize the complete format-specific numeric grammar and consume its whole
token. Integer decoding uses checked sign/magnitude accumulation and range
checks; handle `int64` minimum without negating an unrepresentable value.
JSON forbids a leading plus, leading-zero integers, hex notation, NaN, and
Infinity. Schema decoders then enforce their signedness and target range.

Keep integer identity separate from `float64`. yyjson's pinned default
converts an integer outside its signed/unsigned 64-bit domains to a real.
Never then round that real back into an ID. A proposed exact-number mode can
use `YYJSON_READ_NUMBER_AS_RAW` privately and convert according to the schema;
it must be tested separately from preserving the current Content behavior.

Reuse the pinned backend's numerical conversion for JSON. Require finite
results, explicit overflow behavior, round-to-nearest conversion, and a
documented underflow/signed-zero policy. For a future custom language,
first select a proven private numerical implementation and its policy.
[fast_float](https://github.com/fastfloat/fast_float) is a candidate with
explicit conversion errors, not a dependency introduced by this proposal.
Do not write a naive decimal-to-binary conversion algorithm.

ADR 0003 currently grants `<charconv>`/`std::to_chars` only to the assertion
formatter. General `std::from_chars` use requires a focused policy amendment
and toolchain evidence before implementation; `noexcept` alone is not an
exemption. Similarly, any new third-party conversion dependency needs a pin,
license, allocation audit, and native/browser validation. Keep conversion
headers and implementation private and compile the checked path without
fast-math assumptions.

For new schemas, the default policy accepts finite subnormals, rejects a nonzero
value that rounds to zero in its target type, and preserves signed zero unless
the domain explicitly canonicalizes it. Check narrowing to `float32` as well as
initial `float64` conversion. Existing schemas retain their observed behavior
until a versioned compatibility decision changes it. Numeric policy belongs
to the typed decoder, not a process-wide locale or compiler optimization flag.
Enforcing this policy needs original numeric spelling, including `-0` and
nonzero underflow cases. Use the exact-number mode or a verified lexeme map;
the current converted DOM alone cannot recover information already rounded away.

## Small custom languages

Do not invent a language for current JSON content. When expressions, commands,
or a material DSL have a concrete need, write its grammar and examples first.
Use a small deterministic byte lexer and named recursive-descent functions.
Add Pratt precedence parsing only for expressions; its operator table defines
precedence and associativity explicitly. Unary minus is normally a grammar
operator, rather than being fused with every numeric token.

The lexer reads tokens on demand with bounded lookahead. Each token carries
kind, raw source range, and flags needed for decoding; retain an entire
contiguous token array only for a real revisiting/tooling consumer. Define
maximal munch, keyword boundaries, comment nesting, escapes, newline rules,
and identifier grammar explicitly. Start with ASCII identifiers unless a
language requirement justifies Unicode identifier handling. Unicode string
values remain independent of identifier grammar.

Use a bounded recursion depth for a small grammar or an explicit stack where
the allowed nesting makes recursion unsafe. Parsing must advance, succeed,
or return a diagnostic at every loop; recovery must consume input or exit.
Typed AST nodes use contiguous storage and local indices, not virtual nodes
and per-node allocation. Pure recognition builds a candidate; semantic
callbacks cannot mutate engine state halfway through parsing.

Avoid speculative backtracking by designing predictable productions. An
ambiguous or large language is a trigger to reconsider a parser generator,
not to add unbounded memoization to a tiny config parser.
[re2c](https://re2c.org/manual/manual_c.html) is a future lexer-generator
candidate with explicit bounds-checking strategies; adoption needs a pinned
generator, reproducible output, and a measured consumer. Do not assume its
fastest sentinel configuration permits reading beyond an arbitrary view.

Kelly's chapter makes generator adoption a maintenance option rather than a
blanket speed optimization. Reconsider a generated parser when the grammar's
conflicts/recovery or parallel handwritten variants become harder to audit
than a grammar specification. Require reentrant per-job state, pinned and
reproducible generation, reviewable conflict reports, bounded stack growth,
fallible allocation, and cleanup of discarded semantic values. Generated
actions build a private candidate rather than mutating a global weapon table.

For example, [Bison's manual](https://www.gnu.org/software/bison/manual/bison.html)
documents pure/reentrant C parsers and bounded parser stacks. This establishes
available mechanisms, not compatibility of any untested skeleton. Audit the
actual generated code under `-fno-exceptions`; reject skeletons using exceptions
or unsupported stack allocation. Generation happens on the build host, while
generated parsing code must compile for native and web targets. SDK consumers
must not require a generator just to link the engine library.

## Errors, limits, and progress

All production APIs that can fail return a small `ParseStatus`, with optional
bounded diagnostics. Proposed statuses are `Ok`, `InvalidSyntax`,
`InvalidEncoding`, `InvalidValue`, `UnsupportedVersion`, `LimitExceeded`,
`OutOfMemory`, `Cancelled`, and `InternalError`. Streaming adapters additionally
use `NeedMoreInput`; that status does not mean a complete document succeeded.
I/O and publication failures belong to their owning subsystem.

Diagnostics are data: stable error code, severity, source/revision identity,
known range or explicit unknown-range marker, bounded property path, and
bounded expected/actual detail. Retain the first terminal failure even when the
diagnostic array is full. Report suppressed counts and label truncated text.
An allocation failure must still fit in an inline status/diagnostic record.
Rendering into log text or UI happens above Parsing. Malformed input never
uses assertions, and diagnostic failure never changes failed parsing to success.

Byte offsets are authoritative. CLI rendering adds one-based line and display
column; editor protocols may need UTF-16 columns. Compute that conversion
against the exact immutable snapshot, with an explicit tab/newline convention.
Do not confuse UTF-8 bytes, Unicode scalars, UTF-16 units, and grapheme display
width. Build line starts only for consumers needing repeated location lookup;
a one-off error can scan its source prefix under the byte/work budget.

The yyjson adapter supplies real syntax offsets and domain property paths.
It must not invent precise semantic spans by searching for the first matching
text. Initially report a semantic path and unknown range when no reliable span
exists. Exact node spans need a tested optional source-map implementation or
a suitable backend capability; the editor/cooker milestone must account for
that extra storage and scanning work.

### Provenance and useful diagnostics

Boer's parser keeps file/line identity; carry that principle through typed
decoding and cooking. Domain validation errors retain source label, document
revision/digest, property path, and a range when known. A duplicate definition
can carry both the new definition's origin and the previous origin. Reference
errors identify the authored reference site rather than only the resolved
target. Do not retain an entire parser DOM just to retain these records.

The initial yyjson limitation remains explicit. For exact tooling spans, an
optional structural locator may walk already validated immutable JSON in
source order and match it to DOM traversal, recording key/value/container
ranges. This is a bounded location pass, not a competing syntax validator.
Verify decoded key identity, array order, and node counts; a mismatch reports
an internal mapping failure and never publishes inaccurate spans. Run it only
for consumers requesting exact locations, account for the extra pass/storage,
and test escapes, repeated spellings, nested arrays, and empty containers.
Custom language tokens already have exact ranges from their lexer.

Cooked debug metadata maps a domain record/field ID to source label, source
digest, property path, and optional range. It is attached to the cooked blob's
digest/version, and may be shipped separately from runtime payload. Retain
only project-relative labels in portable artifacts. If source bytes have
changed, the inspector reports stale provenance instead of highlighting a
different revision. Excluding source text from shipping data does not exclude
human-readable artifact inspection in tools.

The closest-string Gem adds an optional error-path improvement: suggest a few
known fields or symbols after exact lookup fails. Limit candidate count,
identifier length, and total comparison work; use caller scratch and a frozen
domain vocabulary. Break ranking ties deterministically. Suggestions are
advisory and never autocorrect, establish identity, or run on successful
lookups. OOM or exhausted suggestion work preserves the original error.

For example, an illustrative diagnostic can say `impact.sound.json
variatons: unknown field; did you mean 'variations'?`. The property path stays
valid even when exact location data is unavailable. CLI, editor, cooker, and
CI consume the same structured diagnostic; their renderers choose presentation.

Limits are explicit caller input with validated maxima, not ambient globals.
Current Content caps are compatibility defaults, not universal limits:

| Budget | Enforce |
| --- | --- |
| Input and retained source bytes | At acquisition/admission, before large allocation |
| Workspace and candidate bytes | Before every allocation/growth; include source, DOM, diagnostics, maps, and retained old revision |
| Nesting, members, elements, tokens/nodes | At structural admission and during parsing/traversal |
| Encoded/decoded string bytes and numeric token length | Before decode/append/conversion work |
| Diagnostics and recovery work | At emission and recovery loops |
| Reference count and total semantic work | In domain validation |

The current Content depth check runs after yyjson construction. The proposed
adapter should run a small string-aware structural admission scan before pool
allocation when the backend cannot enforce caller depth/count limits itself.
That scan tracks delimiter depth and scalar/member counts conservatively;
it recognizes string boundaries and escapes, but yyjson remains the syntax
authority. It never treats braces inside strings as nesting, never accepts a
document by itself, and reports which configured budget was exceeded. Tests
must prevent false rejection of valid inputs within limits. Duplicate checking
and field checks then use iterative/bounded traversal of the validated DOM.

Reject size multiplication/addition overflow before allocation, including a
zero failure from `yyjson_read_max_memory_usage`. A pool below the configured
cap failing to allocate is OOM; a requested pool above the cap is a limit.
Budget exhaustion reports the budget, limit, and observed/requested value.
For bounded small objects, quadratic duplicate comparison is acceptable with
a hard small member cap. Larger objects require a bounded nonquadratic method;
hostile input must not obtain unbounded hash-probe or comparison work.

Independent document jobs can run in parallel with independent workspaces.
Reuse each worker's bounded scratch only after its borrowed results die.
No shared mutable parser, hidden TLS allocator, or global token registry.
Cooperative cancellation occurs at bounded checkpoints in owned loops. A
third-party monolithic parse cannot promise immediate cancellation: bound
its input and discard obsolete results afterward. Browser parsing must fit
a measured event-loop budget; larger work moves to a supported worker or
incremental task adapter rather than blocking the UI indefinitely.

## Editor parsing and writing

Start with debounced full parses of bounded immutable revisions. Keep the
last valid typed candidate alongside the current draft diagnostics. Strict
runtime validation always decides whether Save/Cook/Apply can publish; a
recovering tree alone cannot establish success.

Add a lossless concrete syntax tree only when source-preserving edits,
formatting, or language services need punctuation/trivia and incomplete input.
Keep original bytes and explicit error/missing nodes. Do not route every
runtime load through an editor tree.
[Tree-sitter](https://tree-sitter.github.io/tree-sitter/) is a candidate for
a substantial edited language: its incremental concrete-tree and error
recovery model is relevant. Its own trees still need domain validation,
source ownership, budget and OOM audits, and acceptance tests against the
strict reader. For a small DSL, sharing the lexer/grammar with a bounded
recovery mode can be simpler.

An editor tree representing broken text carries errors as values; inserted
tokens are marked synthetic. Recovery is bounded at grammar synchronization
points, with forward progress and a capped number of follow-on diagnostics.
If full-parse latency is already acceptable, do not implement subtree
invalidation, persistent green/red trees, ropes, or packrat caches.

Two writer contracts are explicit. A canonical writer serializes an owned
typed value with versioned field order, stable IDs, finite numeric spelling,
indentation, and a final newline. A source-preserving editor applies checked
nonoverlapping byte-range edits to the exact draft revision, retaining untouched
bytes. It verifies the expected revision/digest before replacement and reparses
the result. JSON preserves whitespace through source edits, not comments.
Neither writer silently rewrites unsupported future schemas.

Writers return explicit capacity/conversion errors and never publish truncated
output. File conflict detection and atomic replacement reuse Content's save
contract. Parsing itself does not perform filesystem replacement.

## Cooked data and checked binary reading

Current shipping JSON remains supported. Add a binary format per domain only
when startup, memory, bandwidth, or repeated loading warrants it. Cook from
the same validated typed definition consumed by development loads, not from
a second loose interpretation of source text.

The TokenFile, XDS, fast-load, and packed-asset chapters strengthen this into
an explicit cooking contract. Resolve names/references offline into typed
domain records; eliminate runtime preprocessing and repeated string-to-number
conversion. A generic token tape still requires runtime interpretation, so it
is an optional tool cache rather than the shipping architecture.

| Cooked section | Contract |
| --- | --- |
| Header/directory | Magic, schema/format versions, complete length, bounded section counts, offsets, and representation features |
| Typed records | Stable domain tags and fixed-width fields; explicit optional/default representation and reference indices |
| String dictionary | Deduplicated exact UTF-8 bytes and checked offset/length directory; deterministic ordering within an artifact |
| Dependency metadata | Explicit external resource IDs/revisions and resolved internal references; no hidden constructor-driven file loads |
| Optional provenance | Domain record/field origins, bound to this artifact and source digests; independently inspectable |

Use the proposed [cooked Strings dictionary](strings.md#cooked-dictionaries-and-persistence)
contract rather than creating a competing global symbol service. Sort by
defined exact byte ordering, deduplicate exact spelling, and remap records
after assigning artifact-local indices. Case-insensitive domains need their
own explicit schema policy; cooking never merges `Foo` and `foo` by default.
Keep blob-owned indices local: regeneration can change them, and live reload
must resolve stable resource identities again rather than reuse old ordinals.

Cache keys include source/dependency digests, schema and format versions,
cooker/generator/conversion-policy versions, and relevant target/settings
features. Host absolute paths and timestamps alone do not establish identity.
Publish complete output and its manifest only after validation; failed cooking
leaves the prior artifact usable. Corrupt or mismatched cached output is not
valid content just because its filename/key is familiar.

Once a domain opts into cooked release content, integration and acceptance
tests load its exact shipping representation, including a runtime target that
can omit the authoring parser/writer if its dependency graph allows it. Do not
validate only text in development and discover binary-reader defects at release.
Text and cooked paths produce semantically equivalent typed definitions under
the same schema/numeric policy. Release selection is explicit; a bad cooked
file does not silently fall back to potentially stale source text. A developer
tool may explicitly recook and then validate the replacement.

The binary reader uses a checked byte cursor and explicit fixed-width
little-endian fields. Validate magic, independent format/schema versions,
section sizes/counts, allowed tags, string encoding, ranges, and references
before exposing typed records. For an offset/length check use `Offset <= Size`
then `Length <= Size - Offset`; do not overflow `Offset + Length`. Check
count/stride products similarly. Decode fields without unaligned native loads
or assuming the C++ struct layout is the file layout.

Own the blob through Content. Runtime readers use validated offsets/indices,
not pointer patching or serializing vtables, native pointers, `NameId`, or
workspace node indices. File-local dictionary indices are valid only under
their blob owner. A mapped view is optional; web fetched bytes and native
byte owners must share the same representation. External or modified cooked
files still require structural validation; a content digest is not a substitute
for bounds checks or authenticated provenance.

Packing reduces ownership and allocation count; it does not prove zero-copy
or zero-validation loading. Compression, byte swapping, target alignment,
reference resolution, and output ownership can still require work. Keep
offsets encoded and resolve through checked views; measure before exposing
native aligned mapped records as an additional platform-specific format.
An offline inspector prints versions, sections, dictionary bytes, decoded
records, dependencies, and origins using the same checked reader. This keeps
binary data reviewable without reenacting a generic pointer-fixup engine.

## API contracts and reviewability

Use a few concrete non-template operations rather than a callback framework:

| Operation shape | Outcome and failure contract |
| --- | --- |
| `ReadJson(source, limits, workspace, diagnostics)` | Status plus document-owned borrowed root on success; no usable partial root on failure |
| `DecodeSound(root, limits, diagnostics, output)` | Existing domain operation builds a separate owned candidate; failure leaves `output` unchanged |
| `ParseExpression(source, options, workspace, diagnostics)` | Optional consumer-specific AST; no allocation hidden in token access |
| `ReadCooked(bytes, limits, diagnostics, output)` | Validated owned/view candidate with explicit blob lifetime; failure leaves prior output unchanged |
| `WriteCanonical(definition, outputBuffer, diagnostics)` | Complete byte count on success; partial scratch bytes are never published |

These are proposed shapes, not declarations of APIs that already exist.
All failing operations are `[[nodiscard]]` and infrastructure methods are
`noexcept`. An out-parameter does not grant permission to partially publish:
build locally, validate, then transfer ownership at the commit point.
Document workspace invalidation explicitly. An internal impossible state is
an engine invariant; malformed external bytes remain a recoverable error.

Make debugging straightforward: named grammar functions, explicit cursor
positions, small status enums, private backend calls, and dumpable ranges.
Add deterministic text/JSON diagnostic rendering in tools, a token/tree dump
for languages that have those representations, and schema-path inspection.
Counters expose bytes, workspace high-water, values/tokens, deepest nesting,
phase durations, and first failure code. Instrumentation is opt-in at the
caller; the core does not depend on Logging/Profiling or allocate per event.

## Validation and performance gates

The implementation must prove contracts before speed claims:

- Valid/invalid corpora cover complete input consumption, UTF-8, decoded
  duplicate keys, escaped NUL, numeric boundaries, trailing garbage, and
  exact schema behavior. Include pinned-backend differential cases, using
  Ludus's stricter policy as the oracle where library policies differ.
- Vocabulary generation is deterministic; explicit persisted IDs survive
  descriptor reorderings. Inject hash collisions and verify exact dispatch.
  If streaming is added, split valid and invalid input at every byte boundary
  for small corpus cases and compare results with complete-buffer reading.
- Exercise every configured cap at and just beyond its boundary. Include
  deep delimiters in strings, long numeric tokens, many duplicate names,
  empty/truncated input, and arithmetic-overflow cases.
- Inject allocation failure at each parser and candidate allocation boundary;
  verify cleanup, inline failure diagnostics, workspace reuse, and unchanged
  active output. No success path may rely on exception-enabled engine code.
- Fuzz syntax readers, string decoding, typed schemas, and binary cursors
  independently and together under ASan/UBSan. Add exact round-trip and
  mutation properties: typed write/read equality, bounded progress, and no
  partial activation. Recovery and strict modes must agree on complete valid
  inputs; editor error trees must never activate invalid definitions.
- Text and cooked paths produce equivalent typed values and deterministic
  output. Corrupt sections/offsets/indices, truncate each small fixture at every
  boundary, and verify bounded rejection. Artifact/source digest mismatches
  must disable stale provenance rather than point to unrelated source.
- Test source lifetime, stale async revisions, snapshot cancellation, source
  edits, old/new candidate peak budgets, and parser-view invalidation.
- Build with pinned native and web toolchains, warnings as errors, format/tidy,
  header self-sufficiency/build budgets, and installed SDK consumer coverage.

Benchmark acquisition, syntax, decode, validation, preparation, and activation
separately, then report their total. Use actual catalog/audio/project files,
small repeated loads, large representative content, string-heavy escaped data,
deep but valid data, malformed near-limit inputs, and edited revisions.
Record p50/p95/p99 latency, cold/warm runs, bytes/second where meaningful,
peak retained/workspace/candidate bytes, allocation count, and executable size.
Control corpus, hardware, compiler flags, backend pin, pool reuse, and
concurrency. Compare equivalent full validation and output ownership.

Keep yyjson as the baseline. SIMD JSON, streaming SAX, and compiled records
are competing strategies for particular workloads, not stacked requirements.
[simdjson's On-Demand API](https://simdjson.org/api/4.6.4/md_doc_2basics.html)
adds traversal/lifetime rules, input padding, and lazy interpretation. An
experiment must demonstrate that all required data and malformed skipped
values are validated, not merely that a requested field was extracted quickly.
Use its explicit error API; audit allocation failure, architecture dispatch,
Wasm behavior, and pool ownership before adopting it.

As provisional complexity gates, consider an alternative only if parsing is
a material share of an observed end-to-end budget and the alternative produces
a repeatable benefit, for example at least 15% lower p95 total load time or
25% lower peak parse storage on representative workloads without correctness,
small-file, binary-size, or web regressions. These are proposed evaluation
thresholds, not observed results or universal SLA values. Set absolute editor
latency and runtime loading budgets with the actual consumer.

## Delivery sequence

| Phase | Deliverable | Exit evidence |
| --- | --- | --- |
| P0: Contract and baseline | Inventory current readers, accepted grammar/numbers, allocation coverage, corpus, timing and memory | Documented compatibility matrix and measured baseline; no parser rewrite |
| P1: Bounded JSON foundation (implemented) | Common errors/limits/cursors, current backend facade, compatibility adapter, better admission/error handling | Existing Content tests plus budget/OOM/encoding tests, clean native/web build and SDK consumer; unchanged schema behavior |
| P2: Domain integration | Transactional owned decoding and richer paths in selected readers | One real catalog/audio consumer, old-output preservation, source lifetime and candidate budget evidence |
| P3: Authoring diagnostics | Revision snapshots, exact provenance where supported, source edits; recovery/CST only if needed | Stale results discarded, accurate locations, lossless edits, strict Apply/Cook gate, measured latency |
| P4: Domain resource reader, measured language or cooker | One consumer's resource-format profile, required DSL, or binary output/reader | Format/grammar fixtures or text/cooked typed equivalence, fuzzing, corruption and decoded-size limits; measured whole-pipeline benefit for cooking |
| P5: Optional optimization | SIMD, generated lexer/parser, streaming, incremental parsing, or schema codegen | Equivalent validation and ownership, repeatable material gain, maintained native/web and tooling support |

Each phase is independently useful. Do not wait for a universal parser,
reflection registry, language server, job system, new string library, or
binary asset format to improve today's bounded JSON diagnostics.

### Continuing improvement after P0-P5

Complete and validate the initial P0-P5 delivery plan before beginning the
broader research and improvement pass. Optional mechanisms retain their existing
consumer and measurement gates: record delivered evidence or an explicit
defer/not-applicable decision instead of implementing every P5 alternative.
Required format specifications and backend checks remain part of initial delivery.

Preserve the resulting revision, compatibility policy, corpora, workloads and
measurements as the baseline for later experiments. The
[continuing research backlog](parsing-gems-review.md#continuing-research-after-p0-p5)
owns the recommended venues, attributed reading queue, review status and
experiment records. Read relevant sources, trial concrete improvements against
that baseline, and record adopt/defer/reject decisions with regression evidence.
Research completion does not substitute for the delivery gates above.

## Changes after the Gems review

The baseline was drafted before reading the selected chapters. The
[companion review](parsing-gems-review.md) records seven relevant readings
and their concrete effects. The revised design adds shared schema vocabulary,
generator adoption criteria, provenance through cooking, bounded spelling
suggestions, deterministic compiled dictionaries, offline inspection, and
acceptance testing of the shipping representation. It retains the initial
small core, pinned JSON backend, typed ownership, and measured optimization
strategy. Older unchecked numbers, global parser state, native object dumps,
hash-only identity, and unrestricted preprocessing are not adopted.


## P1 implementation and compatibility notes

The exported `Ludus::FoundationParsing` target provides transactional byte
cursors, UTF-8 validation, byte-based source locations, and allocation-free
error records. `Ludus::FoundationParsingJson` adds `JsonDocument`, `JsonValue`,
and `JsonWriter` over the unchanged pinned yyjson backend. Linking the core
alone does not require yyjson. Public headers expose no yyjson types.

`JsonDocument::Read(input, error, limits)` invalidates old views on every
attempt and exposes a root only after syntax and structural validation succeed.
Input must outlive Read and must not refer to this document's previous borrowed
strings. Read copies into one pool owned by an AllocationDomain. Reset retains
capacity; Release returns it. Growth frees the old pool before allocation so
peak owned workspace stays within the configured budget. A lower subsequent
budget also releases an oversized retained pool. WorkspaceBytes reports
retained capacity. Distinct documents can run independently; the same document
requires external synchronization.

Input bytes, workspace bytes, and excessive container nesting are admitted
before allocation. Full grammar and UTF-8 escape decoding remain yyjson's
responsibility. Scalar depth, per-object members, per-array elements, total
values, decoded string length, and exact duplicate keys are checked after the
bounded DOM is constructed. These latter limits bound accepted data and
validation work; they do not promise early DOM rejection. Root depth is zero;
keys do not count as values. Hard ceilings of depth 64 and 256 members per
object bound recursion and quadratic duplicate comparison. The default Content
profile retains its depth 32 and 32-member acceptance limits.

Syntax/encoding errors have byte offsets. DOM validation errors use
UNKNOWN_BYTE_OFFSET because yyjson supplies no source span map. No approximate
field offset is reported as exact. Domain field diagnostics remain in Content
and Audio. No source snapshot, lexer, AST, editor recovery, cancellation,
streaming reader, exact-number mode, or cooker has been introduced by P1.

The Content facade keeps its public method signatures and opaque JsonValue
member, strict document grammar, field limits, schema readers, unsigned-integer
checks, float64 fallback for oversized JSON integers, and canonical spelling
for valid output. It still rejects a second successful read with Status::Limit.
Failed reads now leave no root and may be retried without leaking scratch.
The bounded writer now rejects invalid UTF-8 strings, and Finish is idempotent;
writing after Finish fails. Content maps writer failures to its existing Limit
or OutOfMemory statuses and never overwrites output on a failed write.

Contract tests cover malformed and truncated input, decoded duplicate keys,
integer boundaries, exact budget boundaries, deep nesting before allocation,
OOM, pool reuse/growth, move ownership, invalid-root isolation, Unicode scalar
boundaries, canonical output, and deterministic byte mutations. The installed
SDK consumer and browser smoke target exercise the new exports and codec link
closure. Performance claims still require measured workload baselines.
