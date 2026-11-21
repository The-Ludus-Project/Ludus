# Engine configuration

Status: first implementation. `FoundationConfig`, `FoundationConfigJson`,
`RuntimeConfiguration`, `RuntimeConfigurationSchemas` and `AudioConfiguration`
are portable SDK modules. GameHost consumes its startup schema. Audio owns the
first Live apply group: user mix volumes reach the renderer at an owner safe
point with renderer acknowledgement. The optional Qt Editor and native
`ludus-config` tool select either schema explicitly. Other subsystems migrate
separately, retaining their ordinary options APIs.

The design keeps configuration work at control boundaries. Subsystems use plain,
module-owned options in their hot loops. Human-readable names, layered overrides,
parsing, validation, provenance, and persistence belong to a small control core.
One immutable descriptor per setting drives runtime validation and tool metadata.
No global registry, static constructor registration, writable member pointers,
scripting interpreter, general reflection framework, or hot-loop string lookup.

## Quick workflow

Project build settings answer **how this game builds and launches**. Engine
configuration answers **which typed runtime settings a supported host uses**.
The Configuration tab is an offline preview with explicit file loading.

### Use the Configuration workspace

1. Open **Configuration** in the editor.
2. Inspect shared defaults, types, help, effective values, source and apply policy.
3. Load a cooked project bundle or preference file explicitly.
4. Edit a preference or reset it to its inherited value.
5. Save a sparse preferences file. Only explicit preference overrides are saved.

Saving detects changes to a previously loaded preference file. On conflict,
preserve the draft and reconcile the external edit before retrying. This check is
best-effort revision detection, not a general interprocess file lock.

These edits affect a future launch. They do not mutate a running GameHost. Closing
a project preserves this independent preview; quitting prompts for its unsaved
preferences.

### Cook a small host layer

The initial host schema includes `host.headless` and `host.max_frames`. Create:

```toml
# project.toml
[host]
headless = true
max_frames = 120
```

With `ludus_config` built in the engine checkout:

```bash
python3 tools/configuration/cook.py project.toml project.config.json \
    --validator out/build/linux-clang-development/tools/configuration/ludus-config
```

TOML cooking requires Python 3.11+; the ordinary host tools retain Python 3.10+
support. The cooker validates against the actual runtime schema before publishing.

GameHost accepts a cooked startup manifest or an explicit `--config` file,
plus `--preferences` and supported command-line overrides. An explicit CLI override wins over the corresponding preference and
project value. Use the schema/validator rather than guessing a JSON payload.

The sections below define layers, transactions, source explanations, diagnostics
and host launch examples.

## Architecture and ownership

```mermaid
flowchart LR
    TOML[Authoring TOML] --> Cook[Offline cooker]
    Schema[Module-owned descriptors] --> Tool[Native schema / validator tool]
    Cook --> Tool
    Tool --> Bundle[Versioned JSON layer]
    Bundle --> Adapter[Bounded JSON adapter]
    CLI[Explicit launch arguments] --> Core[Configuration context]
    Adapter --> Core
    Schema --> Core
    Editor[Editor inspector] --> Core
    Core --> Candidate[Prepared candidate]
    Candidate --> Owner[Owner safe point]
    Owner --> Options[Plain typed options]
    Options --> Loop[Subsystem hot loops]
```

`FoundationConfig` depends on Base, Memory, Strings, and private Math. It knows
neither files nor JSON. `FoundationConfigJson` adds the existing bounded shared
JSON parser/writer. `RuntimeConfiguration` owns the first GameHost schema and its
plain `HostOptions` adapter. Qt, TOML, Python, and filesystem discovery stay out
of these modules. Native and browser builds compile the same core.

A context has one control owner. Its methods are deliberately not thread safe.
The supplied immutable schema, its names/choices/help, validation callback state,
and allocation domain must outlive the context. Module unload requires destroying
its contexts before code or descriptor memory is released. Destroyed contexts'
bindings cannot validate in another context: each context gets a nonwrapping
process-local identity. Bindings are local capabilities, never serialized IDs.

Consumers materialize ordinary typed options at a frame/job/session boundary.
They do not retain references into context storage. For workers, distribute an
owner-created immutable options snapshot or copy into job inputs; publication,
retirement, and cross-thread synchronization remain with the subsystem. The core
has no background callbacks or automatic device mutation.

## Schema contract

A `Descriptor` contains an exact dotted lowercase name, typed default, help,
units, numeric bounds, enum choices or bounded text, allowed source mask,
application group, apply policy, persistence policy, and simulation flag.
Examples: `host.headless` and `host.max_frames`. Names are at most 128 bytes;
segments start with a lowercase letter and continue with letters/digits/underscore.
Text is valid UTF-8, contains no NUL, and has an explicit byte bound of at most 256.
Booleans, signed/unsigned 32/64-bit integers, finite float32/float64, enums, and
strings are supported. Containers and asset/entity properties belong to their
own domain schemas, rather than enlarging this configuration vocabulary.

Schema validation rejects duplicate names, invalid defaults, inconsistent bounds,
invalid metadata, duplicate enum choices, unsupported sources/groups, and
persistence without permission to use the Preference layer. Values have exact
kinds; booleans and enum/string values never coerce from numbers or each other.
Float32 narrows once, verifies the rounded value against bounds, and canonicalizes
zero. Float64 and Float32 reject nonfinite values. Cross-field constraints use an
optional pure, `noexcept` callback over the entire candidate in descriptor order.
Callbacks must not mutate engine state or reenter the context. Live changes also
validate the projected active options against restart values still in use; the
pure callback may run more than once per prepare.

Schema identity is an explicit owner-managed string (`ludus.host.v1`). Bump it
for incompatible names, types, allowed values, semantics, or ownership contracts.
Compatible default/help updates retain identity so inherited values can evolve.
V1 rejects stale identities and unknown keys/fields. Migration is an offline,
explicit transformation followed by native validation; V1 has no implicit aliases
or coercion. A generated descriptor fingerprint may be added when larger schema
composition warrants it; an arbitrary hash would not replace semantic versioning.

## Deterministic layers and persistence

Lowest to highest precedence:

| Rank | Purpose |
| --- | --- |
| Implicit default | Module-owned typed default |
| Engine | Distributed engine baseline |
| Project | Authored project configuration |
| Profile | Explicit platform/build profile selection |
| Device | Explicit device capability selection |
| Quality | Explicit quality preset |
| Preference | Deliberate persistent user overrides |
| Developer | Local developer overrides |
| Launch | Explicit invocation arguments |
| Session | Temporary tools/console edits |

Ranks are fixed and independent of load order. V1 stores at most one assignment
per key per rank. Named profile/device selection and same-rank file composition
must happen outside the context with an explicit manifest; there is no include,
glob, environment substitution, guessed platform path, or hidden current-directory
search. GameHost loads the cooked startup selection in rank order. A legacy
standalone Project file and a cooked startup selection are alternative inputs.
An explicit Preference file and Launch options can override either input.

Every supplied value is validated, including values masked by higher ranks.
`Prepare(..., replace=true)` replaces an entire layer; a patch changes only named
keys. Duplicate edits fail. Reset removes an assignment and exposes inheritance;
it never writes the current inherited value into preferences. `ReadLayer` can
inspect every overridden assignment. `Explain` reports requested/active values,
winner/source/line, override count, pending status, and group generation.

Save only explicit Preference assignments, including deliberate assignments that
happen to equal today's default. Never save the merged effective configuration as
preferences: that would pin defaults and stop future engine/project improvements
from reaching users. Diagnostic effective dumps must use a separate export path.
Persistent flags are permissions, not an instruction to save every setting.

## Transaction and apply lifecycle

1. Capture context revision. Prepare a bounded patch or layer replacement.
2. Validate exact kinds, ranges, sources, text, and full candidate constraints.
3. Inspect `ReadPrepared`; the subsystem may construct prospective options or
   resources. Any fallible resource preparation happens before publication.
4. At the owner-selected boundary, verify the revision and commit. Copy prepared
   options into their owner as part of that boundary; no other readers observe a
   halfway-applied group. On preparation failure, discard candidate/resources.
5. Report pending restart or success. No callback runs in `Commit`.

One transaction may be prepared at a time. Conflicting revisions never publish.
After startup is sealed, an effective value change may involve only one apply
group and policy. Group transactions can include paired constraints such as width
and height. Independent groups should be submitted separately; V1 does not offer
a distributed rollback coordinator. Startup accepts multiple groups because no
consumer has begun using options yet.

Before `SealStartup`, commits set requested and active values. Afterwards:

| Policy | Core behavior | Owner responsibility |
| --- | --- | --- |
| Live | Update requested and active at commit | Publish plain options at safe point |
| RestartSession | Update requested; preserve active | Recreate a session/context explicitly |
| RestartEngine | Update requested; preserve active | Restart engine explicitly |
| NextLaunch | Update requested; preserve active | Persist selected layer and launch again |

Simulation settings reject effective changes after sealing. Changes hidden under
higher ranks can change provenance, but cannot later become effective while frozen.
Groups increment a nonwrapping generation only for changed **active semantic
values**. Provenance-only changes increment context revision, preserving conflict
checks without forcing subsystem work. Generations and identities are not wrap-safe
counters: exhaustion is an explicit error.

`Active` means the caller fulfilled this ownership contract; it is not asynchronous
GPU/OS acknowledgement. The GameHost adapter publishes only startup settings and
the Editor describes its context as an offline preview. A live owner reports
device application separately, after its device acknowledges the handoff; the
[audio mix owner](#audio-configuration) is the first. Future live renderer
adapters must supply the same safe resource handoff and acknowledgement before
claiming device application. Restart pending values activate through a fresh
context on restart; V1 deliberately has no misleading unconditional “acknowledge”.

## Audio configuration

`Ludus::AudioConfiguration` owns schema `ludus.audio.v1`:

| Setting | Type and range | Apply | Group |
| --- | --- | --- | --- |
| `audio.volume.master` | float32 amplitude, 0 to 1 (default 1) | Live | 1 (mix) |
| `audio.volume.music` | float32 amplitude, 0 to 1 (default 1) | Live | 1 (mix) |
| `audio.volume.effects` | float32 amplitude, 0 to 1 (default 1) | Live | 1 (mix) |
| `audio.session.sample_rate` | uint32 Hz, 8000 to 192000 (default 48000) | RestartSession | 2 (session) |

All four may be saved as preferences. `ReadAudioSessionOptions` copies the active
sample rate for `SystemConfig`; `ReadAudioMixOptions` copies the active volumes.
The application maps each mix role to an index in its own bus tree
(`AudioMixBuses`). A role mapped to `NO_BUS` is stored but never applied.

`AudioMixOwner` is the Live owner. It shares the audio control-owner thread and
borrows the context and `AudioSystem`:

1. `Initialize` binds the mix settings, checks the mapped buses and publishes the
   startup mix before `SealStartup`.
2. The caller prepares an edit with `Context::Prepare`.
3. `Apply` reads the prepared mix. If it changed, `Apply` requires a Ready
   session, renderer command room for one `SetBusUserGain` per mapped role and no
   publication awaiting acknowledgement. Only then does it commit the context and
   submit the batch, which the renderer applies at its next 128-frame control
   boundary.
4. `Service` compares `AudioSystem::GetAppliedSequence` with the batch's
   `BatchResult::Sequence`. The renderer advances that counter after the boundary
   recomputes bus gains. On acknowledgement the published snapshot becomes the
   applied snapshot and the superseded one retires.

`GetReport` returns the published and applied volumes with their mix-group
generations. Applied changes only after acknowledgement, so inspection never
claims audio the renderer has not used. At most two snapshots are retained:
applied and in flight.

Each failure before publication discards the context candidate. The context, the
published and applied snapshots and the renderer's bus gains stay unchanged:

| Status | Cause | Repair |
| --- | --- | --- |
| `AudioNotReady` | Session not Ready (starting, suspended, shut down) | Retry after the session is Ready |
| `QueueFull` | Not enough renderer command room | Call `AudioSystem::Service`/render, then prepare again |
| `AwaitingAcknowledgement` | Previous mix not yet acknowledged | Call `Service` after the next boundary, then prepare again |
| `ConfigurationFailed` | Context rejected the commit, such as a stale revision | Read the diagnostic, prepare against the current revision |

Callers keep their own draft, so a retry re-prepares the same edits.
Restart-only edits commit with `PendingRestart` and publish nothing: `Explain`
shows the requested sample rate as pending, while the session keeps its active
rate. To restart, shut down the `AudioSystem`, discard the owner, build a fresh
context from the persisted layers, read the session options, create the new
session and initialize a new owner. A sealed transaction cannot change mix and
session settings together (`MixedApplyGroups`).

Owner work is cold and bounded: it allocates nothing, never blocks on the
renderer, and runs no callback in `Commit`. Device mode reads an acknowledgement
the render thread publishes with release ordering. Offline mode advances it
during `RenderOffline`. Commands still queued at shutdown are never
acknowledged. Remote transport of live edits into a running game remains
separate work.

## Representation, quotas, and errors

V1 cooked layers use bounded canonical JSON through `FoundationParsingJson`.
Using the existing tested parser is a maintainability choice, not a claim that
JSON outperforms a packed binary format. Input parses once at a cold boundary;
no authored TOML syntax remains in shipping code. Add a packed format only when
startup/size measurements justify another codec and its inspector/migration cost.

The exact root fields are `version`, `schema`, `layer`, `values`; each record has
`key`, `type`, `value`, `source`, `line`. Version is 1. 64-bit integers are canonical
decimal JSON strings so intermediate tools cannot round them through binary64.
32-bit integers are exact JSON integers; floating settings accept JSON numbers.
Source is at most 128 UTF-8 bytes. Line zero means unavailable; TOML's stdlib
parser supplies no locations, so the cooker does not fabricate line numbers.
The in-memory and JSON adapters preserve supplied nonzero lines.

Defaults bound contexts to 256 settings, 64 edits per transaction, and 64 MiB of
control storage. Hard ceilings are 4096 settings/edits and 32 groups. JSON input
is at most 256 KiB, depth 4, 8 object fields, 4096 records, 50,000 values, and 16 MiB
parser workspace. Limits can be lowered. Startup owners with more than 64 records
must explicitly raise `MaxEdits` within the hard ceiling. The ordinary host has two.

Initialization preallocates current/candidate layer cells and requested/candidate/
active values through a fallible allocation domain. Core prepare, lookup, explain,
read, commit, and discard allocate nothing. Storage grows with settings × ranks;
values favor explicit fields and bounded owned text over tagged allocator-heavy
objects. Names bind with a sorted index and binary search. Validation and edits are
cold bounded work; duplicate checks are quadratic under explicit ceilings.

Errors are status codes plus key/record diagnostics and a separate optional
parser byte offset, never exceptions. Writers enforce the same 256 KiB layer
ceiling as readers, even when their caller supplies a larger buffer. Failed
initialization releases all allocations; failed prepare exposes no partial change;
failed writes preserve the caller's output view. Writers can touch a partial prefix
of their caller buffer on failure; only successful views may be published. File
adapters reject malformed or oversized files before starting the host. Mandatory
configuration failures abort startup explicitly rather than silently falling back
to a surprising configuration.

## Tools and Editor workflow

Build `ludus_config`. It is also installed as `bin/ludus-config` in native SDKs.
`--schema` emits the selected schema's descriptors. `--validate` reads a bundle
from stdin and emits its canonical layer on stdout. Choose `engine`, `project`
(the default), `profile`, `device`, `quality`, or `preference`.
A leading `--schema-id IDENTITY` selects an engine schema from the fixed
`RuntimeConfigurationSchemas` catalog (`ludus.host.v1`, the default, or
`ludus.audio.v1`). An unknown identity exits with status 2 and lists the
available schemas. A bundle never validates against a schema it does not name.
Errors use engine logging on stderr. It validates through the same core as runtime.

The schema export lists settings in descriptor order. Each record exposes
`allowed_sources` (the bitmask built by `SourceMask`, indexed by `Layer`),
`group` (the schema-local application group ID), and the existing apply and
persistence metadata. Numeric settings include inclusive descriptor `min` and
`max`. Integer bounds use canonical decimal strings, including for 32-bit
settings whose descriptor bounds can be wider than their value type. Real
bounds are JSON numbers. Enum and string settings include `max_text_bytes`,
which counts UTF-8 bytes without a terminator. Boolean settings have no bounds.
Use native validation for the type range, Float32 rounding, preference
eligibility, and cross-field constraints.

```toml
# project.toml
[host]
headless = true
max_frames = 120
```

```sh
python3 tools/configuration/cook.py project.toml project.config.json \
  --validator out/build/linux-clang-development/tools/configuration/ludus-config
out/build/linux-clang-development/apps/game_host/ludus_game_host \
  --module /absolute/path/game.so --config project.config.json \
  --preferences player.config.json --max-frames 20
```

Cook audio preferences by naming the schema:

```sh
python3 tools/configuration/cook.py player-audio.toml player-audio.config.json \
  --layer preference --schema-id ludus.audio.v1 \
  --validator out/build/linux-clang-development/tools/configuration/ludus-config
```

The explicit CLI limit wins over preferences and project values. TOML cooking
requires Python 3.11+ (`tomllib`) only for this optional authoring command; native
validation and engine builds keep the repository's existing Python minimum. The
cooker accepts strict scalar types, rejects unknown keys/arrays/dates/nonfinite
numbers, invokes the native validator, and atomically replaces the output only
after validation. It never changes CMake/toolchain/project descriptors.

### Cook a named startup selection

Keep the startup manifest in the project owner's source tree alongside the
explicit TOML files it names. This `startup.toml` selects a preview profile.

```toml
version = 1
engine = ["config/base.toml"]
project = ["config/game.toml", "config/game-window.toml"]

[profiles.preview]
files = ["config/preview.toml"]
devices = ["portable"]
qualities = ["low"]

[devices]
portable = ["config/portable.toml"]

[qualities]
low = ["config/low.toml"]
```

```sh
python3 tools/configuration/cook.py startup.toml startup.config.json \
  --validator out/build/linux-clang-development/tools/configuration/ludus-config \
  --profile preview --device portable --quality low
out/build/linux-clang-development/apps/game_host/ludus_game_host \
  --module /absolute/path/game.so --startup-config startup.config.json \
  --preferences player.config.json --max-frames 20
```

Select one profile. A device and quality preset are optional. If specified, each
must be declared and listed by that profile. Omitted selections contribute no
layer. `engine` and `project` are optional file lists. Each profile requires a
`files` list and may omit either compatibility list when it permits no choices
of that kind. Selection names are case sensitive and use 1–64 ASCII letters,
digits, hyphens or underscores, starting with a letter or digit.

Paths are explicit, relative to the manifest directory, and use forward slashes
without dot or parent components. Each list accepts up to 32 distinct `.toml`
paths. The manifest, each source and the cooked startup file each have a 256 KiB
limit. Only selected source contents are read. To compose a rank, assign each
setting in at most one of its files. The cooker reports both sources for a duplicate.
The same setting can appear in different ranks. The cooker validates every
selected rank through the native validator, including assignments that higher
ranks override, and replaces the destination only on success.

The cooked version-1 file contains a `layers` object keyed by rank. Each entry
is a canonical version-1 layer bundle with its original schema identity and
source paths. GameHost applies Engine, Project, Profile, Device and Quality in
that order regardless of the JSON member order. Preference and Launch retain
their higher ranks. `--schema-id` applies to every rank of a manifest, so each
cooked layer names the selected schema. Pass one of `--startup-config` and `--config`. Keep player
preferences in the separate `--preferences` file.

The Editor's Configuration tab is a standalone offline workspace. Its schema
selector chooses the engine schema to preview, GameHost by default. Switching
asks before discarding unsaved preferences. Loading a file that names another
catalog schema switches to that schema. A clean preview switches
automatically. A project bundle asks before discarding unsaved preferences, and
cancelling keeps the draft and names the file's schema. The workspace loads
explicit cooked Project/Preference files, displays shared defaults/types/help/
requested values/source/apply policy, edits preferences, resets to inheritance,
and saves a sparse layer with `QSaveFile` and no direct-write fallback. Edits do
not mutate a running host. Save detects external changes to the previously loaded
preference file; this is best-effort revision detection, not an interprocess lock.
Closing prompts for unsaved preferences. Opening a game project performs no
configuration reads, writes, downloads, configure, or build.

The Project settings Arguments field can supply the explicit GameHost flags for
Play/Run. A future per-project configuration manifest/launcher UI should integrate
with the project store and share read-only opening checks. A live remote console,
undo history, watchers, device probing, locks/signatures, profile selection UI,
replay/network identity export, secret management, and broad subsystem migration
are separate additions with their own ownership/transport requirements.

## Initial architecture completion and improvement sequence

The first implementation already supplies the V1 control core, bounded JSON
adapter, cooker, offline Editor preview and two GameHost startup settings. Finish
the remaining integration of this architecture before starting the broader
[conference and journal research queue](engine-configuration-gems-review.md#post-implementation-research-queue).
The queue does not change the contracts above or require a research-led redesign
before the initial implementation has a validated comparison baseline.

The following gates track the remaining initial integration. They are planned
work, not claims that these features are implemented or that new checks pass.

| Gate | Remaining implementation | Completion evidence |
| --- | --- | --- |
| C1: shared metadata and SDK contracts | Export descriptor numeric bounds, allowed-source masks, groups and text limits alongside the metadata already emitted by `WriteSchema`. Document the configuration public API with Doxygen contracts and remove resolved undocumented-baseline entries. | Schema export agrees with native descriptors and validation, including exact integer limits. SDK API coverage and the documentation checks pass. |
| C2: explicit layer selection | Define an owner-managed manifest for baseline files, named profile/device/quality selection and deterministic composition within a rank. Connect supported startup layers to the host/cooker workflow using the existing fixed precedence. | Selection and composition are independent of file load order; malformed, unknown, incompatible and oversized inputs fail explicitly. Existing Project/Preference/Launch precedence remains covered. |
| C3: project and launcher integration | Associate explicit configuration inputs with the project store and expose the author, cook, preview, sparse-save and launch workflow in the Editor. Share validation with the native tool and preserve unrelated project/editor settings. | A project can launch with its selected cooked inputs without manually assembling configuration flags. Opening remains read-only and reports missing/stale inputs; explicit cooking or saving publishes atomically and preserves drafts on conflict/failure. |
| C4: module owners and application | Select the first subsystem settings to migrate, define their module-owned descriptors and ordinary options adapters, and make the tool/Editor select the appropriate schema explicitly. Integrate at least one production Live apply group with fallible preparation, publication at an owner safe point, snapshot lifetime and any required device acknowledgement. Implemented by the [audio mix owner](#audio-configuration) and the explicit schema catalog. | `ludus_audio_configuration_tests` cover these through the production offline renderer; `ludus_configuration_cooker_tests` and the Editor workspace tests cover schema selection. Integration tests demonstrate successful publication, preparation failure preserving existing options/resources, restart values staying pending, and safe resource retirement. Inspection only reports device application after the owner fulfills its contract. |
| C5: validated initial baseline | Validate C1-C4 with the pinned native/web toolchains and installed SDK consumer. Record the initial implementation revision, supported schemas/targets, representative workloads and reproduction commands before research trials begin. | Relevant unit and integration tests, warning-clean builds, ASan/UBSan, format/tidy, browser smoke, SDK consumer and documentation checks pass. Record cold initialization/prepare/commit costs, allocation/storage bounds, apply latency and author-to-launch workflow evidence without inventing performance budgets. |

C1-C3 can proceed while module owners define C4. C5 completes after all preceding
gates have evidence. C2 selects explicitly named device configurations; automatic
device probing and a richer profile selection UI retain their separate scope.
C4 establishes the first integrated owner (audio mix volumes); renderer adapters,
broad subsystem migration, remote console transport and the other additions
listed above remain separate work.

After C5, read the queued resources in priority order, propose bounded experiments,
and compare them with the recorded baseline. Adopt, defer or reject each idea
from measured results and correctness evidence. Update this architecture only
when a reviewed experiment supports a contract change, and retain attribution
near any affected implementation.

## Verification and literature

Tests cover precedence/reset, source explanations, semantic generations, stale
revisions/bindings, atomic cross-field constraints, restart pending state, source
permissions, simulation freezing, mixed apply groups, numeric/text validation,
all initialization allocation failures, no allocations on prepare/commit, strict
bundle decoding, exact 64-bit round trips, sparse preferences, cooker failures,
Editor validation and external-change detection, explicit schema selection, and
the audio mix owner's publication, acknowledgement, preserved failures, pending
restart and snapshot retirement. A browser smoke and installed
SDK consumer exercise exported APIs. Native host acceptance verifies the actual
configuration startup path, CLI precedence, and malformed input rejection.

The [literature review](engine-configuration-gems-review.md) records consulted
chapters and specific adopted/adapted ideas. Source attribution also sits near
the implementation. This design establishes contracts and test evidence; it
makes no unmeasured performance or universal “best engine” claim.
