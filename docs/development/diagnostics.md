# Native diagnostics and assertion policies

Use this guide for diagnostic-helper launches and build-policy validation.
The [assertion architecture](../architecture/assertions.md) owns the detailed
error and lifetime contracts; [debugging](debugging.md) covers RAD sessions.

## Build flavor and assertion policy

Every preset now supplies an explicit `LUDUS_BUILD_FLAVOR`. Debug uses CMake
Debug; Development and Profile both use RelWithDebInfo; Release uses Release
(or MinSizeRel for a manual build). Direct CMake invocations must supply the
matching flavor. Profile is its own engine flavor even though its optimization
configuration matches Development. Assertion policy never follows `NDEBUG`.

Debug/Development enable `LUDUS_ASSERT` and Check inspection breaks. Profile and
Release compile Assert away and disable Check breaks. Require/Check/Fatal stay
active everywhere. `REQUIRE`/`FATAL` always terminate, including after a debugger
continuation; `CHECK` returns its boolean. Enabled `ASSERT`/`ASSERT_F` is now
**resumable through explicit developer action** in non-CI runs — a debugger
Continue (Debug or Development) or, with no debugger in a non-CI Debug build, the
external helper's Continue-once dialog — otherwise it terminates. This is gated
by the generated `LUDUS_ASSERT_DIALOGS_AVAILABLE` (1 only for non-CI Debug) plus
a runtime CI veto; the assertion-policy version is 2. See
[interactive assertion contract](../architecture/assertions.md#51-interactive-development-resumable-assert) and
[ADR 0006](../decisions/0006-resumable-development-assertions.md).

The installed `assert_config.hpp` and SDK manifest carry the built variant's
policy. A Release consumer of a Development SDK uses Development's assertion
policy. Do not override the generated macros or mix headers/libraries from
different variants. Use separate prefixes. Installation refuses an incompatible
or legacy unversioned Ludus prefix before overwriting files; move an old generated
`out/install/<preset>` aside, then rerun `scripts/install-sdk`. Multi-config SDK
generation is explicitly unsupported until per-configuration packages exist.

### Diagnostic helper and report delivery

The external diagnostic helper is a Python development tool,
`tools/diagnostics/ludus_diagnostic_helper.py` (installed to the SDK `bin`
directory as `ludus_diagnostic_helper`). It launches an engine binary with the
diagnostic channels wired up so assertion/`FATAL`/`CHECK` reports are captured
independently of normal Logging. Run an engine binary under it with:

```bash
# Convenience launcher (resolves the installed or source-tree helper):
scripts/run out/build/linux-clang-debug/apps/smoke/ludus_smoke
# Or invoke the helper directly:
python3 tools/diagnostics/ludus_diagnostic_helper.py -- <engine-binary> [args...]
# or, from an installed SDK:
ludus_diagnostic_helper -- <engine-binary> [args...]
```

A binary launched **directly** (without the helper) also works: it configures
diagnostics at startup and runs report-only when no helper/channel is present.
When launched under the helper in a local, non-CI **Debug** build with a usable
terminal or live display, a
failed `ASSERT` presents a **Continue once / Terminate** prompt; *Continue once*
resumes past that assertion (the invariant is now known-broken), a second failure
is reported again, and *Terminate* exits. CI and headless runs stay report-only
and never wait for input.

The engine calls `ludus::diagnostics::InitializeDiagnosticSession()` (from
`ludus/diagnostics/session.hpp`, in the `Ludus::DiagnosticsIntegration` target,
which depends on FoundationBase but is not part of it) once at the top of `main`,
before workers or the logger. It reads these descriptors/policy from the
environment:

- `LUDUS_DIAGNOSTIC_REPORT_FD` — connected `AF_UNIX`/`SOCK_DGRAM` report socket.
- `LUDUS_DIAGNOSTIC_CONTROL_FD` — connected `AF_UNIX` control socket:
  `SOCK_SEQPACKET` on Linux, framed `SOCK_STREAM` on macOS (the same versioned
  Hello/HelloAck handshake and little-endian wire encoding).
- `LUDUS_DIAGNOSTIC_INTERACTIVE=0` — force report-only (local headless Debug).

The helper sets the first two for its child; a directly launched binary with no
helper simply runs report-only with no transport. Interactive presentation is
eligible only in a local, non-CI Debug build with a live terminal/display; CI is
auto-detected and forced report-only, and CI workflows additionally set
`LUDUS_DIAGNOSTIC_INTERACTIVE=0` explicitly. This layer changes no assertion's
fatal action.

### macOS native tooling

Use the same launcher with `out/build/macos-clang-debug/apps/smoke/ludus_smoke`.
The external Python helper captures report datagrams before logger initialization
and after shutdown. A local Debug launch in a usable terminal can ask for an
explicit Continue once or Terminate answer through `/dev/tty`; press `c` or type
`continue` to resume that one assertion. Blank input, unknown input, EOF and
helper errors terminate. Cocoa assertion dialogs are deferred, so a graphical
session alone does not make a macOS launch interactive. Development, CI,
headless launches and `scripts/run --report-only <binary>` capture reports without
prompting. Debugger continuation still follows the assertion policy above.

Darwin does not provide Unix sequenced-packet sockets. The private transport
reads a complete bounded header and payload across stream fragmentation; one
monotonic two-second deadline covers the startup acknowledgement. A decision
wait has no human-response timeout and never auto-continues. Closed/truncated
channels, malformed frames, oversized payloads and stale incident IDs resolve
to Terminate. Native sockets suppress SIGPIPE without replacing the process's
signal handler. The [assertion architecture](../architecture/assertions.md#52-startup-and-report-delivery-external-diagnostic-helper)
owns the wire and descriptor lifetime contracts.

Run local native helper regressions with:

```bash
./scripts/build macos-clang-debug
out/host-tools/venv/bin/ctest --preset macos-clang-debug --label-regex diagnostic-startup --output-on-failure
```

Startup tests run in every test-enabled native flavor. Explicit decision and
real-helper terminal tests require a Debug build with dialogs available; their
controlled child environments clear CI markers and use a PTY instead of user
input. macOS CI separately builds those test children with `LUDUS_CI_BUILD=OFF`
to exercise the capability and verifies the runtime CI veto as a regression.
Production CI remains noninteractive. Linux-only allocator interposition and
fake debugger/death harnesses remain guarded. Native assertion dialogs remain
future work; the macOS Editor has the separate crash-capture path below.

References: Apple's [socket(2), DESCRIPTION](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/socket.2.html)
explains stream/packet semantics; [XNU `bsd/sys/socket.h`, SO_NOSIGPIPE](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/socket.h)
describes per-socket broken-peer signal suppression. Thanks to Apple for these
OS contracts; the bounded framing adapts the existing Ludus codec.

To run Release policy tests without changing the production preset:

```bash
out/host-tools/venv/bin/cmake --preset linux-clang-release -B out/build/linux-clang-release-assert-tests -DLUDUS_BUILD_TESTS=ON -DLUDUS_WARNINGS_AS_ERRORS=ON
out/host-tools/venv/bin/cmake --build out/build/linux-clang-release-assert-tests
out/host-tools/venv/bin/ctest --test-dir out/build/linux-clang-release-assert-tests --output-on-failure
```

### macOS Editor crash reporting

Native macOS editor builds enable `LUDUS_EDITOR_CRASH_CAPTURE` by default. The
editor registers with an external Crashpad handler **before QApplication starts**,
so direct executable, Finder, IDE and `scripts/editor` launches share capture.
Labmates do not need a special launcher, environment variables or Sentry accounts.
The handler is built and copied to
`ludus_editor.app/Contents/MacOS/ludus_crash_handler`; nothing downloads at launch.
Missing-handler or database failures produce a visible startup warning without
preventing editor use.

Only Crashpad is compiled from the checksummed source distribution pinned in
`config/editor_crash_reporting.json`. No Sentry SDK enters the editor, engine or
installed SDK. Healthy startup registers a Mach exception port; the external
handler saves the minidump and handles optional HTTPS submission. Ludus never
calls normal logging, a symbolizer, UI or network code on the failing thread.
Linux and browser editors do not build this adapter.

#### Configure the team's Sentry destination

The maintainer creates a Sentry organization and **Native/C++** project, then
copies the project DSN from its settings. This is a public ingestion credential:
**never substitute an auth/API token**. Sentry's free Developer plan can be used
within its current quotas; paid features are not required for ingestion.
Submitting labmates do not need dashboard accounts.

After normal editor setup, configure the destination for the distributed app:

```sh
out/host-tools/venv/bin/cmake --preset macos-clang-development \
  -DLUDUS_BUILD_EDITOR=ON \
  -DLUDUS_EDITOR_SENTRY_DSN=https://publickey@ingest.example.com/123
out/host-tools/venv/bin/cmake --build --preset macos-clang-development --target ludus_editor
```

Replace the example DSN. Without one, capture still works locally.
**Help > Crash Reporting** can also configure a local installation without
rebuilding, show pending/uploaded counts and open local reports/logs. Production
accepts HTTPS DSNs with no password, query or fragment. A destination change
pauses this database immediately and takes effect after restart. Restart all
open editor instances after changing destinations or defaults. Destinations use
separate databases: old reports are never silently sent to a new project.
Previously unconfigured reports stay in the `unconfigured` folder and are not
automatically migrated.

On first launch with a configured destination, a dialog offers **Automatically
send crash reports**. Saving with that box selected consents to pending and future
submission; Cancel leaves submission off. Preferences persist across ordinary
launches. Turning submission off pauses uploads without deleting pending reports;
an already in-flight request may finish.

#### Evidence, privacy and storage

Reports live under
`~/Library/Application Support/Ludus/Editor/CrashReports/<destination-hash>/`;
unconfigured builds use `unconfigured` instead. Crashpad's default pruning keeps
approximately 128 MiB of report payloads or one year of reports, deleting older
reports periodically. A new capture may temporarily exceed the limit until
pruning. Reports contain thread stacks/registers, loaded modules, selected process
memory and OS information. Sentry metadata includes revision, configure-time
modified-source status, build flavor and CPU architecture. Mach-O UUIDs identify
the actual binaries; matching commits alone do not establish matching builds.

Process memory can expose paths or research data. Project documents, screenshots
and session logs are **not** attached. Keep the Sentry project private, select
appropriate service retention/access controls, and keep dumps out of public
GitHub issues. Local logs are retained separately in `logs/`: 4 MiB segments,
32 MiB inactive-session budget and 14-day age cap. Active sessions can exceed
inactive retention budgets.

Paused/offline reports persist. The handler retries transient failures with
bounded backoff and scans pending reports on subsequent launches. Permanent
rejections or exhausted retries can leave retained, unsubmitted reports: capture
does not establish server ingestion. Uploaded counts reflect acknowledgements.
The upstream hourly limiter is disabled because it permanently skips additional
reports; service quotas govern admission. No paid overage or account upgrade is
configured by the editor.

#### Retain and upload matching symbols

Every successful editor link generates `ludus_editor.app.dSYM` beside the app.
Archive the **exact app and dSYM** for each distribution until its reports expire.
Do not regenerate symbols by rebuilding the commit. Compare UUIDs using
`llvm-dwarfdump --uuid` or Xcode's `dwarfdump --uuid` on both files.

An authorized maintainer uploads the dSYM/executable with Sentry's CLI or debug
file upload interface. After authenticating the CLI locally, for example:

```sh
sentry-cli debug-files upload --org YOUR_ORGANIZATION --project YOUR_PROJECT \
  out/build/macos-clang-development/apps/editor/ludus_editor.app.dSYM \
  out/build/macos-clang-development/apps/editor/ludus_editor.app/Contents/MacOS/ludus_editor
```

CLI authorization is separate from the public app DSN. Keep upload tokens in
local credentials or CI secrets, never in preferences/source control. The example
does not upload source files. Service processing/symbol quotas still apply.
Verify that a real Sentry event shows a caller stack with source lines before
declaring a distributed build fully qualified.

#### Distribute and validate normal launches

Preserve the bundled handler and `Contents/Resources/crash-reporting-licenses/`.
When deploying to another Mac, use the matching Qt `macdeployqt` for normal
runtime dependencies and also pass
`-executable=/absolute/path/ludus_editor.app/Contents/MacOS/ludus_crash_handler`.
Sign the embedded handler as well as the app when signing/notarizing a release.
Crash reporting does not replace [editor tooling/SDK setup](editor-workspace.md)
or supply a signed installer.

Enable `LUDUS_EDITOR_CRASH_CAPTURE_TESTS=ON`, build
`ludus_editor_crash_probe`, then run CTest's `ludus_editor_crash_capture` test.
It crashes only subprocess fixtures against the real handler and a local HTTP
receiver, checking pre-Qt startup, consent-off persistence, later submission,
acknowledgements, missing-handler failure and DSN validation. Preferences and
reports stay in private temporary directories. Real-account ingestion and
symbolization additionally require the maintainer's Sentry project and symbol
upload authorization.

This adapter covers native macOS editor crashes. It does not qualify standalone
games, browser failures, hangs, GPU device loss, faults before registration,
forced kills or whole-machine failures. Debuggers can intercept native faults;
macOS can also inherit the handler into launched child processes. Such reports
may be captured under this editor session, but child-process coverage is not
qualified by the editor tests. The consent dialog discloses this inheritance.
immediate-exit assertion recursion can bypass capture. Assertion packets and the
logging breadcrumb ring are not yet dedicated Sentry attachments.

Thanks to the Chromium authors, **Crashpad Overview Design**, Overview,
Registration/macOS and Privacy Considerations
([design](https://github.com/chromium/crashpad/blob/main/doc/overview_design.md)),
for external Mach capture and consent/storage boundaries; and Sentry,
**example-minidump-upload**, `upload.py`
([source](https://github.com/getsentry/example-minidump-upload)), for DSN routing
and the JSON event form protocol. This adapter uses Crashpad directly.
