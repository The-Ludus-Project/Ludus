# Native game release packages

The installed `ludus` host tools can build and validate a native Linux x64 or macOS arm64/x64 Release
package for a version-2 CMake game project. Packaging is independent of an
itch.io account. This implements the first part of the
[packaging and publishing architecture](../architecture/game-packaging-publishing.md).
Editor setup, browser packaging and automated uploads are described in
[Editor-managed game releases](editor-game-releases.md).

Native ZIP manifests (4 MiB), central directories (8 MiB), Mach-O load-command
tables (8 MiB) and native inspection output (8 MiB) use named policies from
`scripts/python/ludus_tools/package_limits.py`. These encoded metadata and
diagnostic ceilings are independent of the engine's
[scene and Content limits](../architecture/content-resources.md#limit-ownership).

## Create a project with release files

Use an installed Release SDK that matches the project's engine requirement and
the consuming toolchain. Other SDK flavors remain valid for development but
cannot produce a Release package. The example below selects macOS explicitly;
use `linux-clang-release` for Linux.

```bash
ludus project create MyGame --name MyGame --sdk /path/to/release-sdk --profile macos-clang-release --tools /path/to/Ludus --release
```

Add `--itch-target username/game` to generate an optional destination mapping
for offline upload planning. Nothing authenticates or uploads during creation.
Without `--release`, creation retains its ordinary project layout. The minimal
native template is version 4; existing projects are never overwritten.

Generated `ludus.release.json` selects a release profile, executable target,
install component and entry point. `cmake/GameRelease.cmake` installs the
executable, `NOTICE.txt` and SDK license files. Edit its explicit install rules
to add runtime assets, dependencies and the corresponding notices. The generated
notice is a starting point: the game author must complete the license inventory.
CMake remains authoritative for the payload; no build-tree directory sweep occurs.

Existing projects can use `ludus project release init` or the Editor release setup
action; see the [setup guide](editor-game-releases.md).

## Build and inspect the package

Required host tools are Python 3.10+, the project's pinned CMake/Ninja/compiler
and GNU `readelf` on Linux, or Apple's `/usr/bin/codesign` on macOS.
Atomic publication uses Linux `renameat2` or Darwin `renamex_np(RENAME_EXCL)`.
SDK identity and Release flavor must match the selected platform and compiler.
Linux uses `linux-release` / `linux-x64`; macOS uses `macos-release` and
`macos-arm64` or `macos-x64`. macOS package verification requires a macOS host.

```bash
ludus project package MyGame --profile linux-release --version 0.1.0
ludus project package MyGame --profile linux-release --version 0.1.0 --sdk /path/to/release-sdk
ludus project package verify /path/to/package-directory
```

The command holds the shared Editor/CLI build-tree lock throughout configure,
build and installation. It requires a successful current build, checks the CMake
File API artifact's program sections against the installed entry point while
allowing CMake's loader-path rewrite, and validates SDK stamps
and project descriptor/lock/release configuration. Temporary staging is private
and cleaned on failure. Packages publish as complete directories through an
atomic no-replace rename. Identical verified packages are reused.

Output is `MyGame/out/packages/<profile>/<archive-sha256>/`, containing
`game.zip`, `package.json` and `validation.json`. The SHA256 identifies the exact
archive. ZIP entries are sorted with fixed timestamps and normalized permissions.
This is deterministic archiving of identical payloads, not a claim of reproducible
compilation. Keep `out/` ignored. Do not mutate a completed package: verify and
plan recheck its content rather than treating its pathname as proof of validity.

Extraction validation checks entry count, path lengths, expanded sizes, types,
case collisions, permissions, payload hashes, entry point, notices and native
runtime dependencies. It never executes the binary or invokes `ldd`. Regular
ELF libraries must be explicitly installed under loader search paths using
`$ORIGIN`; symlinks are rejected. Install libraries under the names actually
required by `DT_NEEDED`, including a regular copy for a versioned SONAME.

The initial external runtime policy allows only the Linux C/C++ ABI libraries:
libc, libm, libdl, libpthread, librt, libstdc++, libgcc_s, libc++, libc++abi and
libunwind, with specific SONAMEs recorded in `package_native.py`. Unknown native
dependencies fail until bundled or supported by a reviewed policy revision.
The manifest records the libraries actually required and the SDK's distro and
system prerequisites. SDK headers/static archives, debug sidecars, debug sections
and observed producer paths are rejected. License presence checks do not certify
that the author supplied every legally required notice.

## macOS app packages

Create against a matching Release SDK with `--release`, or explicitly set up an
existing project. Setup defaults to macOS on Darwin; `--platform macos` selects
the current host architecture. The existing setup is never overwritten.

```bash
ludus project release init MyGame --platform macos
ludus project repair MyGame --tools /path/to/Ludus --profile macos-clang-release --sdk /path/to/release-sdk
ludus project package MyGame --profile macos-release --version 0.1.0 --sdk /path/to/release-sdk
ludus project package verify /path/to/package-directory
```

The generated `GameRelease` component installs `<target>.app`, `NOTICE.txt` and
SDK licenses. The entry point is `<target>.app/Contents/MacOS/<target>` and must
agree with `Info.plist`. Install authored assets under `Contents/Resources` and
regular runtime dylibs under `Contents/Frameworks`, using the same component.
CMake sets `INSTALL_RPATH` to `@executable_path/../Frameworks`. Runtime dylibs need
relative install names and their own relative search paths. The validator accepts
`@loader_path` / `@executable_path` paths within the payload and resolves `@rpath`
against each image's declared paths; inherited dyld run-path stacks are outside
this initial policy. Absolute producer paths and unresolved dependencies fail.

Policy `macos-release-adhoc-1` requires thin little-endian 64-bit Mach-O images
with matching CPU architecture and a deployment baseline no newer than macOS
14.0. Bounded command, section, symbol and entry tables are inspected without
running the game. Debug sections/symbols, foreign/fat binaries, symlinks, escaped
loader paths and loader environment commands are rejected. The closed external
baseline consists of Apple's system C/C++/Objective-C dylibs and the named system
frameworks in `package_macos.py`; every actual dependency appears in the manifest.
Unknown dependencies must be explicitly bundled or gain a reviewed policy revision.

Packaging signs nested Mach-O images first, then the app, using a local ad-hoc
identity and no timestamp. It uses no keychain identity or account credential.
The installed program's code/allocated-section identity must match the current
CMake artifact despite loader/signature metadata changes. Code and app resource
signatures are verified with `codesign --verify --strict`, again after checked
archive extraction. Signature failure, a failed current build, changed inputs or
Stop/cancellation prevents package publication.

These signatures enable local validation and execution. Developer ID signing,
notarization, Gatekeeper qualification, universal binaries, symlinked framework
deployment, DMG installers and a distributable Qt Editor are separate work.
Native Apple silicon relocation/execution is covered by acceptance; x64 identity
and malformed-image tests do not establish native Intel runtime acceptance.

Supplying `--tools-ref <immutable-Ludus-commit>` also generates a native GitHub
workflow (`macos-14` for arm64, `macos-15-intel` for x64) for tags and manual dispatch. Set `LUDUS_RELEASE_SDK_URL` and
`LUDUS_RELEASE_SDK_SHA256` for a matching Release SDK archive. Its package job
installs Clang 23 and managed CMake/Ninja, verifies the SDK digest and publishes
the checked package as a GitHub artifact. It has no upload job or account secret.
The native Editor shares this backend through **Release > Set Up Releases** and
**Package Release**; signing commands retain owned-process cancellation.

Optional itch.io destination `macos` / channel `macos-stable` supports offline
planning on macOS. The current live upload transport remains Linux-only and
fails explicitly on macOS before installing butler or acquiring credentials.

Thanks to Apple for the Mach-O layouts in
[XNU loader.h](https://github.com/apple-oss-distributions/xnu/blob/main/EXTERNAL_HEADERS/mach-o/loader.h),
[Run-Path Dependent Libraries](https://developer.apple.com/library/archive/documentation/DeveloperTools/Conceptual/DynamicLibraries/100-Articles/RunpathDependentLibraries.html),
and [Creating distribution-signed code for macOS](https://developer.apple.com/documentation/xcode/creating-distribution-signed-code-for-the-mac/).
The implementation adopts bounded static inspection and nested-code-first signing;
it deliberately uses an ad-hoc identity rather than distribution signing.

## Inspect an itch.io plan

```bash
ludus project publish plan MyGame --package /path/to/package-directory --destination linux
ludus project publish plan MyGame --package /path/to/package-directory --destination linux --allow-local-inputs
```

Planning validates the archive, manifest, profile, current release configuration
and engine lock. It needs no installed SDK, butler, account login or network.
It reports destination/channel, version, exact digest, payload bytes and a future
butler argument plan with a placeholder for a verified private snapshot.
The reported plan is not an executable upload command. Explicit uploading is
available through `project publish upload`, described in the setup guide.

Packages from dirty/unknown sources or local SDK overrides require the explicit
local-input option. This is a planning choice; no upload occurs. A channel named
`beta` or `staging` does not change the itch.io project's visibility.
`BUTLER_API_KEY` is stripped from project commands and compiler/native inspection
subprocesses. Do not store credentials in the project or its manifests.

Global `--json` emits schema-1 CLI results. Configuration and package failures
use stable `InvalidRelease`, `UnsupportedReleaseTarget`, `InvalidPackage`,
`Conflict`, `BuildFailed`, `SdkIncompatible` and `MissingTools` categories through
the existing host-tooling error boundary. A failed operation never uploads.

## Install the pinned butler tool

The installed CLI can explicitly acquire butler 15.31.0 for Linux x64,
macOS Apple silicon, or macOS Intel:

```bash
ludus tools install-butler /path/to/private-tools
/path/to/private-tools/butler version
# Machine-readable installation result:
ludus --json tools install-butler /path/to/another-tools-directory
```

The running host's OS and CPU select the versioned upstream archive. A Python
process running under Rosetta reports x64 and selects the Intel executable;
native arm64 Python selects the Apple silicon executable. Other hosts fail
before downloading or creating directories.

Both archive and executable SHA-256 values are pinned in the shared installer.
Downloads and extracted executable bytes are bounded to 128 MiB; redirects must
remain HTTPS. Only `butler` is installed, with owner-only executable permissions.
Optional 7-zip libraries are excluded, following the
[upstream installation manual](https://itch.io/docs/butler/installing.html).

The destination may be a new or existing ordinary directory. Existing files are
preserved; an existing `butler`, including a symlink, causes `Conflict` before
any download. Use another destination to reinstall. Verified bytes are published
atomically without replacement. Failure or interruption removes private staging
and a newly created empty destination; parent directories may remain.

Installation does not execute butler, modify PATH/shell settings, use
`BUTLER_API_KEY`, log in, or upload. No project check/open/build implicitly
installs it. macOS live upload supervision remains guarded; this command supplies
an independently usable pinned tool without enabling `project publish upload`.

## Validation scope

Run `python3 -m unittest discover -s scripts/python -p 'test_ludus*.py' -v` with
the pinned tools on PATH. Release tests compile real native executables, exercise
CMake installation, corruption/failure cases and clean-extraction execution.
Linux fixture tests use a small SDK. macOS additionally runs
`test_macos_release` and `tests/sdk_consumer/verify_macos_release.py` with the
installed wheel and a relocated full production Release SDK. The Cocoa Editor's
`[.macos-release-journey]` checks real setup, verified packaging and Stop cleanup.
See CI for macOS 14 native and sanitizer gates. These checks establish the recorded
package/relocation scope; real hosted gameplay needs its own acceptance. The transport is tested with
isolated stubs; no live-account upload is part of local validation. See the
[evidence ledger](game-packaging-publishing-evidence.md).
