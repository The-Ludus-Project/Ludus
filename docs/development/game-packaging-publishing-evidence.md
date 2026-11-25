# Game packaging and publishing implementation evidence

This ledger records the first implementation of
[the architecture](../architecture/game-packaging-publishing.md). Starting
revision: `f8b00760c18b803cc904f8134462f060941ea60e`. Branch:
`codex/game-packaging-publishing`. Results below were collected on 2026-10-02.
This is a first implementation PR, not completion of R0 through R5.

## Implemented scope

Release schema version 1, optional release files inside atomic project creation,
native Release build/install staging, CMake File API artifact validation, ELF
runtime checks, canonical archives, bounded clean extraction, payload/metadata
verification, atomic no-replace package publication and offline itch.io planning
are implemented in the installed Python tooling. Usage is documented in
[the release guide](game-packaging-publishing.md).

Existing native project creation stored SDK overrides under the SDK's
`x86_64-linux-gnu` identity, while unresolved-lock resolution defaulted to
`linux-x64`. The default now matches the SDK identity. The installed wheel also
ships the existing standalone `cmake_targets` resolver, and artifact resolution
uses a lightweight error shim instead of importing checkout-only `engine`.
The generated minimal template is version 2 and queries the public engine version
instead of using printf diagnostics. Its CMake project identifier uses the
validated target rather than the human name.

`BUTLER_API_KEY` is excluded from project build environments, compiler probes,
source-state probes and ELF inspection. No live upload, account login, network
request or butler installation occurs in these operations. There is no generated
itch.io destination unless the project creator supplies one explicitly.

## Local validation

Baseline before edits: the four existing host-tooling suites ran 96 tests and
passed. After implementation, this command ran 117 tests and passed, including
21 release tests and their parameterized rejection cases:

```bash
PATH=/home/alegruz/workspace/Ludus/out/host-tools/venv/bin:$PATH \
  python3 -m unittest discover -s scripts/python -p 'test_ludus*.py' -q
```

The host Python was 3.12.3; managed CMake was 3.29.6 and Ninja 1.11.1. The release
roundtrip compiler was Ubuntu Clang 18.1.3. Tests compile real ELF binaries with
C++23, exceptions disabled and warning flags, build a generated CMake project,
install its release component, validate/extract/run the executable, and inspect
an offline plan after removing the fixture SDK. Separate tests verify a failed
rebuild cannot package an older binary, cancellation/lock contention expose no
partial package, SDK mutation fails, executable mode survives ZIP extraction,
corrupt payloads/metadata fail and explicitly bundled relative dependencies work. A CMake install-time loader
path rewrite changes the executable bytes but preserves its program-section
identity; the package validator accepts this while rejecting a different program.
No test writes to itch.io.

Archive/configuration cases include traversal, absolute and malformed paths,
symlinks, unknown schema fields, duplicate JSON fields, non-finite JSON numbers,
JSON bounds, case and parent-directory collisions, size/entry bounds, missing
notices/licenses, debug sidecars, unknown runtime dependencies and absolute loader
paths. The native dependency checker uses readelf, never ldd or game execution.
Only the explicit clean-extraction acceptance test runs the known fixture binary.

A real host-tool wheel was built and installed under a new temporary directory.
From `/tmp`, the installed package and its File API resolver imported without
`engine`, Qt or a producer checkout on their module paths. The release suite also
ran all 21 release tests against that installed backend. The existing Editor
adapter suite also passed all 35 tests. CI repeats this with a fresh venv and the
pinned CMake/Ninja versions, Clang 18 and binutils.

`./scripts/check --format` and the foundational include-boundary gate passed.
Python compileall and `git diff --check` passed. New production logic is Python;
no runtime engine C++ implementation or public header changed. The generated
C++ application is compiled by the release tests.

## Pending acceptance and limitations

R0 and R1 remain partially validated. Tests use a deliberately small fixture SDK,
not an accepted production SDK archive. They do not prove full real SDK dependency
relocation, complete third-party license closure, windowed/GPU runtime acceptance,
itch-app installation, or every supported OS ABI. The external ELF policy starts
with explicitly listed Linux C/C++ ABI libraries; other runtime libraries need
explicit installation and notices or a reviewed policy extension. Presence of
notice files is not proof that every license obligation has been satisfied.

The existing synchronous CLI process runner is reused. The cancellation test
proves staging cleanup when the operation is interrupted; it does not certify
all descendant-process cleanup semantics. Full process-supervision acceptance
remains part of the project tooling gates before shipping unattended deployment.
Cooperative locks do not protect against raw CMake invocations or malicious
same-user filesystem mutations.

An attempt to run `./scripts/check linux-clang-development --all` stopped because
the isolated checkout lacked its bootstrap CMake/Ninja/Conan artifacts and Conan
Development toolchain. Native engine unit tests, tidy, ASan/UBSan and full SDK
consumer gates were not run locally for this PR. Existing repository CI must
validate the affected baseline; keep the PR draft until the required gates and
production SDK release acceptance are recorded. Do not mark these checks passed
from the fixture's native compilation.

R2 live uploading, pinned butler setup, credential/login UI, per-destination
upload locks and remote outcome receipts are not implemented. R3 generated CI,
R4 Editor controls and R5 browser project packaging are also pending. No changes
were made to an external game repository. This PR provides the package and
inspection boundary those phases consume.

## Initial CI result

The host-tooling job in [project-sdk run 37091095268](https://github.com/The-Ludus-Project/Ludus/actions/runs/37091095268)
passed for the first PR commit, including the installed release backend. The SDK
candidate relocation job was skipped by the pre-existing feature-branch policy.
This result predates the additional loader-rewrite regression; CI must validate
the updated PR head independently. The initial [ci run 37091095312](https://github.com/The-Ludus-Project/Ludus/actions/runs/37091095312)
also passed source formatting, Development/SDK validation, Clang static analysis,
ASan/UBSan, Optional Editor, build budget, PCH and assertion-policy variants.
These are results for the initial commit, not the later loader-rewrite fix.

## Editor/browser/upload follow-up

The follow-up adds no-overwrite existing-project setup, Editor controls, browser
Release packaging and a generated two-job GitHub upload workflow. Local acceptance
built a full Release browser SDK, compiled Sandbox with pinned Slang/SPIRV-Tools/
SPIRV-Cross, and packaged/verified its current JS/Wasm plus licenses. Tests cover
setup conflicts/cancellation, archive/policy checks, failed builds, SDK mismatch,
Editor request validation and an isolated upload stub verifying exact private
snapshot bytes, credential scope, failed/interrupted receipts and no automatic retries.

Validation: 132 host-tool tests, 36 release tests against an installed wheel outside
the checkout, 35 adapter regression tests and 37 C++ Editor cases (185 assertions).
The Editor built warning-clean, passed ASan/UBSan and leak checks outside the
sandbox, and all four changed Editor translation units passed pinned clang-tidy 18.
Formatting and the foundational include gate passed. A real Qt controller loaded
Sandbox's descriptor and packaged through its real adapter with cleanup confirmed
and zero dropped output. Extracted JavaScript passed Node syntax checking and the
WebAssembly compiled in Node without instantiating or running game code.

Butler 15.31.0 was acquired from the official versioned broth endpoint and its
archive/executable SHA256s are recorded in `ludus_tools.itch`. No itch.io account
was authenticated and no live upload/page change was performed. GitHub environment
secrets/destination and itch.io HTML page configuration remain user-owned setup.
Browser verification is static and does not establish dynamic JavaScript URL
closure or hosted runtime behavior. A submitted-upload receipt deliberately does
not assert processing completion. Native runtime dependency closure is not
asserted for Sandbox.

## macOS release packaging

Branch `codex/macos-release-packaging` extends the installed CLI and native
Editor's shared backend with `macos-release` thin arm64/x64 app payloads.
Local Apple silicon acceptance uses macOS 26.6, upstream Clang 18.1.8,
CMake 3.29.6, Ninja 1.11.1.3, Qt 6.11.2 Cocoa and a macOS 14.0 deployment target.
The isolated checkout used the available macOS 26.5 SDK: rebuilding Catch2
against the newly installed SDK 27.0 fails in Clang 18 libc++'s `INFINITY` use.
No shared tools or user checkout were changed to address that local setup issue.

A complete production Release SDK built warning-clean and was installed with
its bundled dependencies. Its 68 CTest cases passed, with the native Cocoa test
explicitly opted in through `LUDUS_TEST_COCOA=1` after the initial headless
invocation skipped every case in that executable. The full offscreen Editor
suite passed; `[.macos-release-journey]` passed 23 assertions for a real native
Cocoa window, Development project creation, no-overwrite release setup,
verified Release-SDK packaging and Stop cleanup without another published package.
Modified Editor sources passed pinned formatting and clang-tidy 18.

```bash
PATH="$PWD/out/host-tools/venv/bin:$PWD/out/host-tools/bin:$PATH" \
    PYTHONPATH=scripts/python out/host-tools/venv/bin/python -m unittest \
    test_macos_release test_ludus_release test_ludus_itch_release \
    test_macos_project_tooling test_editor_tool test_process_ownership -v
out/host-tools/venv/bin/python tests/sdk_consumer/verify_macos_release.py \
    --sdk "$PWD/out/install/macos-clang-release" --tools "$PWD" \
    --cli "$PWD/out/project-tooling-venv/bin/ludus"
QT_QPA_PLATFORM=cocoa \
    LUDUS_SETUP_TEST_SDK="$PWD/out/install/macos-clang-development" \
    LUDUS_RELEASE_TEST_SDK="$PWD/out/install/macos-clang-release" \
    out/build/macos-clang-development/apps/editor/ludus_editor_tests '[.macos-release-journey]'
```

Portable tests reject malformed/foreign/debug Mach-O images, bad loader paths
and mismatched platform/preset metadata; native cases check deterministic
signed archives, sealed-resource tampering, architecture mismatch, a relocated
relative dylib closure, real CMake install-name/RPATH rewriting with preserved
program identity, failed/cancelled signing and atomic no-replace publication.
The installed-wheel acceptance runs outside the checkout with a relocated full
Release SDK, links the native component closure, signs and verifies an app,
reuses identical packages, executes clean extracted code without an SDK/tooling
runtime environment, builds an offline plan, rejects a stale executable after
a failed current build and preserves existing release setup. A conflicting SDK
preset cannot override the resolved Release SDK used for packaging.

macOS CI runs those native and Cocoa journeys; the existing macOS 14 sanitizer
Editor gate covers the changed controller/UI code. Linux host-tooling and
installed-wheel jobs run the portable new contracts alongside existing ELF/web
release and upload regressions. Local Clang 18 ASan startup on macOS 26.6 remains
unverified as recorded in the build guide; CI provides native sanitizer evidence.
Documentation link/index, strict MkDocs, API coverage, wiki parity and offline
source/HTML packaging checks passed without new public documentation gaps.

The generated game workflow chooses an explicit CPU runner, pins tools/source
and the Release SDK digest, and produces a verified GitHub artifact. It has no
live account/upload step. Generating its YAML is covered; a newly generated game
repository's hosted release needs its own configured SDK archive and execution.
No Developer ID signing, notarization, Gatekeeper approval, universal binary,
native Intel execution, distributable Qt Editor or live itch.io upload is claimed.
See the [current package contract](game-packaging-publishing.md#macos-app-packages).

## Native macOS butler acquisition

The shared installer selects the pinned 15.31.0 `darwin-arm64` or
`darwin-amd64` archive, retaining the existing `linux-amd64` pin. The explicit
`tools install-butler` command works in the standalone wheel. Hashes were
recorded from the versioned upstream HTTPS archives; the Apple silicon executable
reports `v15.31.0` with revision `54a2dd86ed68ab72b25a8d753fa7f88dc552ef7f`.
See [the installation contract](game-packaging-publishing.md#install-the-pinned-butler-tool).

Portable tests cover host admission/aliases, archive and binary corruption,
malformed/duplicate/linked ZIP entries, bounded copying, insecure redirects,
interruption cleanup, existing output preservation, publication races and
structured CLI results. CI additionally installs the wheel and downloads the
real pin on Linux x64, macOS arm64 and macOS Intel, runs only `butler version`
with a clean environment, and verifies repeated installation preserves the
existing executable. No login, live push or upload-transport qualification is
part of these checks.

## R5 independent browser release

[Issue #1276](https://github.com/The-Ludus-Project/Ludus/issues/1276) uses
Color Rally, a small generated CMake game linked to the installed browser
SDK's public Platform and GraphicsRhi targets. The game explicitly requests
WebGPU, renders the canvas through RHI, and increments the score and changes
the presented color when the player presses Space. Its HTML exposes the
controls and platform requirements before loading the game.

### Generate and package Color Rally

From the Ludus checkout, use a prepared `web-emscripten-release` build and
install its SDK. Use a new project destination and keep the game worktree
clean so the package records its source revision.

```bash
source out/host-tools/emsdk/emsdk_env.sh
export PATH="$PWD/out/host-tools/venv/bin:$PATH"
cmake --install out/build/web-emscripten-release
export LUDUS_SDK_PREFIX="$PWD/out/install/web-emscripten-release"
fixture=tests/sdk_consumer/hosted_release
project=out/r5-game
./scripts/ludus project create "$project" --name R5Game --engine 0.1.0 \
    --components FoundationBase,GraphicsRhi,Platform
cp "$fixture/main.cpp" "$project/src/main.cpp"
cp "$fixture/index.html" "$fixture/browser.cmake" "$fixture/CMakeUserPresets.json" "$project/"
printf '\ninclude(browser.cmake)\n' >> "$project/CMakeLists.txt"
./scripts/ludus project release init "$project" --platform web
git -C "$project" init
git -C "$project" add -f CMakeUserPresets.json
git -C "$project" add .
git -C "$project" commit -m "Create independent Color Rally browser release"
./scripts/ludus project package "$project" --profile web-release --version 0.1.0 \
    --sdk "$LUDUS_SDK_PREFIX"
```

Run the [browser Release verifier](editor-game-releases.md)
against the printed package directory, then extract its `game.zip` into an empty
directory and serve only that directory over HTTP on localhost. From a fresh
visit, use the keyboard to open **Controls and access**, activate **Start**,
wait for the score, and press Space. Observe a score of one and a blue canvas.
In the browser Network panel, check the runtime requests for `index.js` and
`index.wasm`, including their paths, status and content types.

Keep the SHA-256 from the Release verifier and upload those ZIP bytes directly
to an authorized itch.io draft as the browser-playable file. In the actual
draft embed, repeat the fresh-visit controls and gameplay checks, including
user-triggered fullscreen and return to the page. Record parent and game origin
and whether they match, the iframe's `sandbox`, `allow` and `allowfullscreen`,
and relevant response restrictions. In both DevTools frame contexts record
`isSecureContext` and `crossOriginIsolated`. Retain the browser, hardware,
WebGPU adapter, flag state, network and gameplay results with the package
digest. Omit private draft tokens, cookies and credentials from the evidence.
See the [HTML iframe sandbox specification](https://html.spec.whatwg.org/multipage/iframe-embed-object.html#attr-iframe-sandbox)
and the [itch.io HTML5 hosting guide](https://itch.io/docs/creators/html5).

### Color Rally 0.1.0 evidence, 2026-11-19

- Producer SDK: Ludus 0.1.0, `wasm32-unknown-emscripten` Release, engine
  revision `b6c7a6d8ce14e7dcaf7df1da6bf5e42c3b27301f`, Emscripten 4.0.23.
  The generated game worktree recorded clean revision
  `9a1889dd11d379b08f6edf4aa5915f664280ecd4`.
- `game.zip` (148,699 bytes) SHA-256
  `63d3f723f1defd8fac4018b2b87931a915203e47cb28c34033b1d39c0b32e4ad`.
  The Release verifier and a separate extraction into an empty directory passed.
  The extracted JavaScript and Wasm SHA-256s matched the CMake build outputs.
- Clean extraction, Edge 157.0.4341.60, macOS 26.6 arm64: Tab and Enter opened
  **Controls and access** before startup. Enter on **Start** focused the canvas,
  the game displayed score zero, and Space displayed score one with a blue
  canvas. Both `/color-rally/index.js` and `/color-rally/index.wasm` returned
  HTTP 200 from the extraction directory. Edge reported no console warnings or
  errors during startup. The browser also requested an unshipped site favicon
  and received HTTP 404. Fullscreen attempts in the automated local tab were
  denied and showed the in-page fallback message while preserving the score.
#### Hosted itch.io draft

- Uploaded the original 148,699-byte `game.zip` to the unpublished
  [Color Rally draft](https://alegruz.itch.io/color-rally) as its browser-playable
  file. The original ZIP downloaded from itch.io matched the local file byte
  for byte and had the same SHA-256 above.
- On Edge 157.0.4341.60 arm64 (macOS 26.6, build 25G72, Apple M5), the
  `edge://version` command line showed an empty flag-switch section and no GPU
  override. DevTools in the game frame reported Apple, `metal-3`, and
  `isFallbackAdapter: false` for the WebGPU adapter.
- The runtime requests for `index.js` and `index.wasm` both returned HTTP 200.
  The response types were `text/javascript; charset=utf-8` and
  `application/wasm`, and both response hashes matched the archive. The
  served `index.html` returned 200 and began with all 2,539 original HTML
  bytes, verified by SHA-256. itch.io appended 87 bytes referencing its
  `htmlgame.js` script.
- The parent was `https://alegruz.itch.io` and the game frame was
  `https://html-classic.itch.zone`, so the origins differ. The iframe had no
  `sandbox` attribute and the game response had no Content-Security-Policy,
  Permissions-Policy, COEP or COOP header. Both contexts reported
  `isSecureContext: true` and `crossOriginIsolated: false`.
  The iframe had `allowfullscreen` and its `allow` value was
  `autoplay; fullscreen *; geolocation; microphone; camera; midi; monetization; xr-spatial-tracking; gamepad; gyroscope; accelerometer; xr; cross-origin-isolated; web-share`.
  The game frame reported `document.fullscreenEnabled: true`. These are
  observed host settings. The game did not exercise the other delegated
  features.
- From a fresh visit, the host page showed the controls and build requirements
  before play. After **Run game**, keyboard navigation reached **Controls
  and access**. Enter opened it, then Enter on **Start** focused the canvas.
  Space changed the score from zero to one and showed a blue frame. The
  640 × 540 embed had scrolling and itch.io's **Enter fullscreen** enabled.
  That button entered fullscreen and Escape returned to the page with
  score one. The host logged an unsupported screen-orientation lock during
  fullscreen, but entry and exit worked. The game canvas's own **Fullscreen**
  request was denied and showed its in-page fallback. The draft's access note
  directs players to the verified itch.io control. The ZIP remained the same.
