# Game release setup, packaging and publishing

For native payload rules and detailed package verification, read
[native release packages](game-packaging-publishing.md). This guide owns shared
Editor/CLI setup, browser releases and explicit uploads.

Open an existing `ludus.project.json` in the Ludus Editor. Release actions require
an idle workspace and a clean, saved version-2 CMake project.

Use **Release > Set Up Releases** to select Linux native, macOS app or Emscripten browser
packaging and an optional itch.io `username/game`. Setup adds game-owned
`ludus.release.json`, explicit `cmake/GameRelease.cmake` install rules, notices,
release instructions and a GitHub workflow. It appends one CMake include and
preserves application source, descriptor and engine lock. It refuses existing
release files or notices instead of replacing authored content. Review the files
and complete asset/dependency licenses before committing.

An empty itch.io destination leaves it configurable through the GitHub repository
variable `ITCH_IO_TARGET`. A project may provide `scripts/release-prepare` to
prepare CI build inputs; setup records that script as an argv array, without a
shell. Browser projects must provide a `web-emscripten-release` preset, a matching
Release browser SDK and an executable with `OUTPUT_NAME index` producing
`index.js` and `index.wasm`. Generated install rules also require `index.html`.
Other filenames/extra workers need explicitly authored install rules.

Use **Release > Package Release** with `linux-release`, `macos-release` or `web-release`, a version
and an optional Release SDK prefix. The Editor uses the shared installed backend
through its asynchronous tooling adapter. Configure, build, install and macOS signing retain
owned process groups, bounded output, cancellation and parent-loss cleanup.
The verified package directory and SHA256 appear in Output. Packaging never uploads.

The equivalent CLI commands are:

```bash
ludus project release init MyGame --platform web --tools-ref <immutable-Ludus-commit> --itch-target username/game
ludus project package MyGame --profile web-release --version 0.1.0
ludus project package verify <package-directory>
ludus project publish plan MyGame --package <package-directory> --destination web --allow-local-inputs
```

Native policies are documented in the native release guide. Browser verification checks
archive paths, hashes/modes, root HTML and local HTML asset references, required
JavaScript/WebAssembly, Wasm header/section bounds and absence of debug custom
sections/native binaries, plus notices/licenses. Installation must match the
selected CMake executable's current JavaScript and WebAssembly. Verification does
not execute game code or prove all dynamically constructed JavaScript URLs resolve;
playtesting the extracted and hosted game remains a separate acceptance step.

Browser SDKs currently use the project's explicit CMake preset, rather than the
native SDK catalog resolver. Their actual SDK manifest identity is recorded, and
these packages are classified as local inputs. CI upload opts into those inputs
explicitly after a separate build job and exact artifact digest check.

macOS creates a local ad-hoc signed app and a package-only GitHub workflow.
Set `LUDUS_RELEASE_SDK_URL` and `LUDUS_RELEASE_SDK_SHA256` for a matching thin
Release SDK. It has no upload step or account secret. Live upload transport,
Developer ID signing and notarization remain separate; see the
[macOS package contract](game-packaging-publishing.md#macos-app-packages).

## Automatic GitHub uploads (Linux and browser)

The generated workflow runs for `v*` tags and manual dispatch. Actions and the
Ludus tooling source are pinned to immutable commits. It builds and verifies the
package in one job, transfers it using GitHub artifacts, verifies again in another
job, and uploads through the explicit `ludus project publish upload` command.
Repository releases are serialized without cancelling an active upload. PR events
never trigger this workflow; fork code receives no upload credential.

Configure these settings in the game repository:

- An `itch-release` GitHub environment with secret `BUTLER_API_KEY`.
- Repository variable `ITCH_IO_TARGET` if no destination was saved into the config.
- For native CI: `LUDUS_RELEASE_SDK_URL` and `LUDUS_RELEASE_SDK_SHA256`, identifying
  a matching Release SDK archive: a public HTTPS URL, or a release asset of the
  game repository itself, which the job fetches with its read-only token (see
  [project CI](project-sdk-workflow.md#continuous-integration-github-actions)).
  Project-specific shader tools and assets should be prepared by
  `scripts/release-prepare`.

The secret is scoped to the upload step only. Tool installation, project bootstrap,
CMake, package verification and compiler subprocesses receive no upload secret.
The uploader copies the archive and sidecars into a private directory, verifies
the snapshot and approved digest, downloads official butler 15.31.0 using pinned
archive/executable SHA256s, and calls `butler push` with an argv array. It uses an
isolated credential home, streams redacted bounded diagnostics and makes one
attempt. Success produces an `upload-submitted` receipt under `out/publish-receipts`;
it does not claim processing or hosted gameplay was verified. Failures do not
retry automatically. Receipt files contain no credentials and leave packages unchanged.

For a browser game, create its itch.io page and select the HTML game type. After
the first push, mark the uploaded channel playable in browser in Edit game. Butler
cannot configure that page setting. See the official
[push documentation](https://itch.io/docs/butler/pushing.html),
[CI authentication documentation](https://itch.io/docs/butler/login.html), and
[installation documentation](https://itch.io/docs/butler/installing.html).
