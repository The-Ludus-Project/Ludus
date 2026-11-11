# Native game release packages

The installed `ludus` host tools can build and validate a native Linux x64 or macOS arm64/x64 Release
package for a version-2 CMake game project. Packaging is independent of an
itch.io account. This implements the first part of the
[packaging and publishing architecture](../architecture/game-packaging-publishing.md).
Editor setup, browser packaging and automated uploads are described in
[Editor-managed game releases](editor-game-releases.md).

Windows SDK, game and staged Editor distributions use the explicit
[Windows portable Release workflow](#windows-portable-release-distributions-d8)
below. The installed `ludus project package` and Editor release actions retain
their existing supported platforms.

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
action; see the [setup guide](editor-game-releases.md). The release workflow shares
its toolchain steps, tools pin and SDK variables with the
[build and test workflow](project-sdk-workflow.md#continuous-integration-github-actions).

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
Games linking `Ludus::Platform` and `Ludus::GraphicsRhi` (including through
`Ludus::GraphicsRenderer`) load `libbsm`, `IOSurface` and `MetalFX`. MetalFX
first shipped in macOS 13.0, so an image with an older baseline must weak-link it.
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
workflow (`macos-15`, arm64 only) for tags and manual dispatch. An x64 workflow is refused:
hosted Intel runners have no prebuilt LLVM 23. Set `LUDUS_RELEASE_SDK_URL` and
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

## Windows portable Release distributions (D8)

`scripts/package-windows` produces a versioned ZIP from an explicit installed
Windows x64 payload. It includes a SHA-256 inventory, SDK identity, candidate
revision, dependency/license records, matching PDBs, and the existing offline
Markdown/HTML documentation archive. Python 3.10+ is sufficient for internal
bundle creation and verification; public signature verification requires Windows
and the Windows SDK's `signtool.exe`. It never builds, downloads, signs, launches,
installs prerequisites, uploads, or modifies an existing version directory.

The channel is **portable ZIP**, with manual side-by-side upgrades and removal.
There is no MSI/MSIX registration, automatic updater or store submission.
`internal` bundles permit unsigned images; `public` bundles require successful
`signtool verify /pa /all` on every executable image, including third-party DLLs
and a bundled VC Redistributable. Sign the final staged bytes before packaging.
Signature verification establishes Windows trust, not the release publisher's
identity policy, timestamp longevity, SmartScreen reputation or store acceptance.
Acquire/approve certificates and perform signing in the release channel's own
workflow; this command never accesses private keys.

### Install a Release payload and its symbols

Use the MSVC 14.44 / Windows SDK 10.0.26100.0 developer environment. The admitted
Foundation/Platform/RHI SDK closure can be installed from a Visual Studio build:

```powershell
cmake --preset windows-msvc-visual-studio -B out/build/windows-release-sdk -DLUDUS_BUILD_TESTS=OFF -DLUDUS_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-release-sdk --config Release --target ludus_windows_sdk
cmake --install out/build/windows-release-sdk --config Release --prefix out/install/windows-release-sdk
```

This scope is the SDK's declared component set; it does not admit other modules
or certify the D7 physical-adapter matrix. Native MSVC Release engine objects
retain CodeView information with `/Z7`, so SDK `.lib` files contain their debug
information. Retain the exact libraries with their final executable/PDB pair.
PDB identity checks cover final images, not each static library's debug coverage.

Game/tool CMake install rules can use the SDK's
`ludus_install_windows_application` helper after creating an executable:

```cmake
find_package(Ludus CONFIG REQUIRED)
add_executable(MyGame src/main.cpp)
target_link_libraries(MyGame PRIVATE Ludus::FoundationBase)
ludus_install_windows_application(MyGame
    DESTINATION bin COMPONENT GameRelease SYMBOL_COMPONENT GameSymbols)
# When this executable enables Agility, also install its runtime:
# ludus_install_d3d12_agility_runtime(MyGame bin COMPONENT GameRelease)
install(FILES NOTICE.txt LICENSE.txt DESTINATION . COMPONENT GameRelease)
# Install authored assets, cooked DXIL/reflection, and dependency licenses here.
```

The helper applies `/Z7`, `/DEBUG:FULL`, `/INCREMENTAL:NO`, `/OPT:REF`, `/OPT:ICF`
and a basename-only `/PDBALTPATH` for Release. It preserves optimization and
installs the runtime and PDB into separate components. Build successfully first,
then install `GameRelease` and `GameSymbols` to different empty prefixes with
`cmake --install <build> --config Release --component <component> --prefix <prefix>`.
Use the exact Release SDK identity used to link the game. The packager checks
that identity's platform/flavor/compiler and the final PDB GUID/age; it cannot
infer compilation flags or prove the declared project's provenance from PE bytes.

For an Editor distribution, first supply a complete installed D6d Release
payload: Editor, shader worker, runtime helpers, authored templates/assets,
manifest-owned shaders and their notices. Deploy the matching Qt runtime into
the Editor executable's directory with `windeployqt --release --no-compiler-runtime
<editor.exe>` from that Qt installation. Include `platforms/qwindows.dll` and a
relative `qt.conf` (`[Paths]`, `Prefix=.` and `Plugins=.` on separate lines). Record all Qt
DLLs/plugins and their license texts in `dependencies`; dynamically loaded
helpers/assets must also be listed in `requiredFiles`. The packager requires the
platform plugin and configuration, but does not build or qualify the D6d Editor
or infer every dynamically loaded asset. The existing Editor remains a developer
tool outside the default SDK install; this workflow accepts a separately staged
Editor payload without changing that boundary.

### Declare the payload

Keep a release spec outside the installed payload. Paths inside the payload use
forward slashes; PDB/installer input paths are local release-machine inputs and
are omitted from the published manifest. Replace the example revision with the
actual full game candidate commit and include every owned EXE/DLL in `symbols`:

```json
{
  "schemaVersion": 1,
  "kind": "game",
  "version": "0.1.0",
  "revision": "0123456789abcdef0123456789abcdef01234567",
  "channel": "internal",
  "entryPoint": "bin/MyGame.exe",
  "license": "LICENSE.txt",
  "requiredFiles": ["assets/catalog.json"],
  "symbols": {"bin/MyGame.exe": "out/install/game-symbols/bin/MyGame.pdb"},
  "dependencies": []
}
```

`kind` is `sdk`, `game`, or `editor`. SDK bundles omit `entryPoint` and may use
an empty symbol map when they contain only static Ludus libraries and licensed
third-party runtime images. Their installed `share/Ludus/LudusSdkManifest.json`
must equal the selected manifest, including the candidate source revision.
Every bundled PE image has exactly one owner: an owned symbol mapping, or a
`dependencies` record with `name`, exact `version`, payload-relative `license`,
and a nonempty `files` list. The installed SDK identity additionally inventories
its static dependencies. Include `NOTICE.txt`, full license texts and any asset
licenses. Listing a license does not establish redistribution permission.

Non-system normal and delay imports must resolve inside the package beside their
image/hosts; developer PATH and installed Qt/SDK locations are never searched.
Windows OS DLLs and API sets use an explicit allowlist. Debug CRTs, non-x64 PEs,
missing assets, missing/mismatched PDBs, SDK/debug files in player payloads,
links/reparse points, Windows reserved names, case collisions and overly long
relative paths fail with a repair. Limits are 10,000 entries, 30 GB expanded,
200 characters per bundle-relative path, 8 MiB ZIP central metadata, 4 MiB manifest,
and 16 MiB per binary metadata read. Extract under a short root; this does not
claim support for every consumer's MAX_PATH behavior.

When images import the dynamic MSVC CRT, add `redistributable` with `path`,
reviewed `sha256`, exact `version`, and a payload-relative `license` notice.
Supply the official `VC_redist.x64.exe` matching or newer than the build tools,
with its redistribution terms; do not sweep DLLs from the developer machine.
The installer is included under `prerequisites/`; users run it interactively,
with its own elevation/reboot behavior. The packager never installs it. For
example, the license path can name an installed notice that preserves Microsoft's
terms and redistribution attribution. Review permission for the exact installer.

### Create, verify, install and update

Build/check docs using the [wiki workflow](wiki.md), then run
`python scripts/package-docs` to obtain `out/ludus-docs-offline.zip`. Packaging
requires the canonical README, site index and source ledger in this archive;
use the producer's wiki checks to establish freshness before passing it in.

```powershell
python scripts/package-windows create --payload out/install/game-release --output out/packages/windows-0.1.0 --spec release-windows.json --sdk-manifest out/install/windows-release-sdk/share/Ludus/LudusSdkManifest.json --docs out/ludus-docs-offline.zip
python scripts/package-windows verify out/packages/windows-0.1.0/windows.zip
```

For `public`, pass `--signtool <absolute-path-to-signtool.exe>` to both commands.
Output is `windows.zip` and `SHA256SUMS`. Preserve both through the release's
support lifetime. Verification privately extracts and checks the inventory,
loader closure and PDBs again before the destination directory becomes visible.
Existing destinations are never replaced. ZIP hashes/inventories detect changes;
a separately trusted channel must authenticate the expected archive digest.
Symbols live under `symbols/`, separately from `payload/`; treat them as potentially
sensitive release engineering artifacts when deciding who can access the bundle.

Extract into a new version directory, install the bundled prerequisite if
present, then launch the manifest's entry point under `payload/` or explicitly
select that SDK prefix. Keep projects, saves and settings outside the version
directory. To upgrade, close the old process, extract/verify the new version
beside it, test it, then explicitly select it. To roll back, select the old
version. To uninstall, close processes and delete only that version directory;
retain authored data and the shared Microsoft runtime. No migration or registry
cleanup is implied. Applications must themselves honor their declared external
save/settings paths; portable packaging cannot enforce that runtime behavior.

The **Windows Release distribution** workflow builds checked offline docs and a
Release MSBuild SDK, runs the archive guards with Python and `python -O`, then
packages/verifies the installed SDK. It also compiles a real MSVC application,
installs its runtime/PDB components and launches a relocated copy with developer
PATH entries removed. The SDK and logs are candidate artifacts, never published
releases. Its developer runner already has Windows/CRT/tooling: this is not
pristine-machine, GPU or interactive Editor acceptance.

Before distribution acceptance, record the exact archive SHA, D7 candidate,
Windows/driver/tool versions and physical adapter; on a clean supported Windows
machine without Visual Studio/Qt/Ludus, install the prerequisite, verify/launch,
exercise an authored project and shader-worker path, trigger useful missing-file
failure diagnostics, upgrade with nonempty saves/settings/projects, roll back,
and remove the version while checking those data remain. For public delivery,
record signing trust and timestamp/channel policy separately. These observations
remain pending until recorded; the archive manifest explicitly says
`pending-clean-machine-and-D7-acceptance`.

Thanks to Microsoft, [PE Format](https://learn.microsoft.com/windows/win32/debug/pe-format),
[SignTool](https://learn.microsoft.com/windows/win32/seccrypto/signtool) and
[Redistribute Visual C++ files](https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files);
LLVM contributors, [MSF](https://llvm.org/docs/PDB/MsfFile.html) and
[PDB Info Stream](https://llvm.org/docs/PDB/PdbStream.html); and The Qt Company,
[Qt for Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html).
Their loader, identity and deployment contracts inform these checks; no
third-party parser or deployment implementation is copied into the packager.
