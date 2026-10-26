"""Explicit, no-overwrite release setup for existing Editor/CLI projects."""
from __future__ import annotations

import os
import re
import tempfile
from pathlib import Path

from . import descriptor, operations, workflow_steps
from .buildlock import BuildTreeLock
from .package_verify import file_digest
from .release_model import canonical, fail, json_bytes, load_release
from .release_template import release_files
from .templates import TemplateFile

WORKFLOW = r'''name: itch.io release
on:
  workflow_dispatch:
    inputs:
      version:
        description: Release version
        required: true
        type: string
  push:
    tags: ['v*']
permissions:
  contents: read
concurrency:
  group: itch-release
  cancel-in-progress: false
jobs:
  package:
    runs-on: ubuntu-24.04
    timeout-minutes: 60
    outputs:
      digest: ${{ steps.package.outputs.digest }}
    steps:
CHECKOUT_TOKEN      - name: Install build prerequisites
        run: |
BUILD_PREREQUISITES_TOKEN      - name: Prepare project build inputs (no upload credentials)
        run: |
          python3 - <<'PYTHON'
          import json, subprocess
          config = json.load(open('ludus.release.json'))
          argv = config.get('automation', {}).get('buildCommand', [])
          if argv:
              subprocess.run(argv, check=True)
          PYTHON
NATIVE_SDK_STEP
      - name: Build and verify release package
        id: package
        env:
          REQUESTED_VERSION: ${{ inputs.version }}
          REF_NAME: ${{ github.ref_name }}
        run: |
          python3 - <<'PYTHON'
          import json, os, subprocess
          version = os.environ['REQUESTED_VERSION'] or os.environ['REF_NAME'].removeprefix('v')
          argv = ['ludus', '--json', 'project', 'package', '.', '--profile', 'PROFILE_TOKEN', '--version', version]
          if os.environ.get('LUDUS_RELEASE_SDK_PREFIX'):
              argv += ['--sdk', os.environ['LUDUS_RELEASE_SDK_PREFIX']]
          result = subprocess.run(argv, text=True, capture_output=True)
          print(result.stderr)
          if result.returncode:
              print(result.stdout)
              raise SystemExit(result.returncode)
          # Build output is streamed before the final JSON CLI result.
          package = json.loads(result.stdout.splitlines()[-1])['package']
          metadata = json.load(open(package + '/package.json'))
          with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
              output.write('digest=' + metadata['archiveSha256'] + '\n')
          with open(os.environ['GITHUB_ENV'], 'a') as output:
              output.write('RELEASE_PACKAGE=' + package + '\n')
          PYTHON
      - uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02 # v4.6.2
        with:
          name: game-release
          path: ${{ env.RELEASE_PACKAGE }}/
          if-no-files-found: error
  upload:
    needs: package
    # Fork PRs never trigger this workflow. The repository owner configures this
    # environment and its BUTLER_API_KEY; approvals are an optional repo policy.
    environment: itch-release
    runs-on: ubuntu-24.04
    timeout-minutes: 20
    steps:
CHECKOUT_TOKEN      - uses: actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093 # v4.3.0
        with:
          name: game-release
          path: out/verified-release
      - name: Install pinned publishing tools (no upload credentials)
        run: |
PUBLISHING_TOOLS_TOKEN      - name: Verify downloaded package
        run: ludus project package verify out/verified-release
      - name: Upload verified snapshot to itch.io
        env:
          BUTLER_API_KEY: ${{ secrets.BUTLER_API_KEY }}
          ITCH_IO_TARGET: ${{ vars.ITCH_IO_TARGET }}
          APPROVED_DIGEST: ${{ needs.package.outputs.digest }}
        run: ludus project publish upload . --package out/verified-release --destination DESTINATION_TOKEN --itch-target "$ITCH_IO_TARGET" --allow-local-inputs --expected-digest "$APPROVED_DIGEST"
      - name: Save upload receipt
        if: always()
        uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02 # v4.6.2
        with:
          name: itch-upload-receipt
          path: out/publish-receipts/
          if-no-files-found: ignore
'''

# The release job's pinned host tools live in this runner-local virtual environment.
RELEASE_TOOLS_VENV = "/tmp/ludus-release-tools"


def workflow_text(profile: str, destination: str, platform: str) -> str:
    text = WORKFLOW
    native = "linux-x64" if platform == "web" else platform
    if platform.startswith("macos-"):
        text = text.split("  upload:\n", 1)[0].replace("name: itch.io release", "name: macOS release package").replace("runs-on: ubuntu-24.04", "runs-on: " + workflow_steps.runner_for(platform))
    text = (text.replace("CHECKOUT_TOKEN", workflow_steps.CHECKOUT_STEP)
            .replace("BUILD_PREREQUISITES_TOKEN", workflow_steps.build_prerequisites(native, RELEASE_TOOLS_VENV))
            .replace("PUBLISHING_TOOLS_TOKEN", workflow_steps.host_tools(RELEASE_TOOLS_VENV)))
    return text.replace('PROFILE_TOKEN', profile).replace('DESTINATION_TOKEN', destination).replace('NATIVE_SDK_STEP', workflow_steps.release_sdk_step(
        "Acquire digest-pinned Release SDK (no upload credentials)",
        "Set LUDUS_RELEASE_SDK_URL and LUDUS_RELEASE_SDK_SHA256 for native Release packaging").rstrip() if platform != 'web' else '')


def setup_release(project: Path, *, platform: str = "linux-x64", itch_target: str | None = None,
                  tools_ref: str | None = None, build_command: list[str] | None = None,
                  expected_descriptor_digest: str | None = None, cancel_check=None) -> list[str]:
    paths = operations.locate_project(project)
    root = paths.project_dir.resolve()
    model = descriptor.parse_descriptor_file(paths.descriptor_path)
    if model.version != 2 or model.provider != "cmake" or model.engine is None:
        fail("release setup requires a saved version-2 CMake project")
    if model.source_dir != ".":
        fail("release setup currently requires source_dir '.'; add install rules manually for nested sources")
    if platform not in ("web", "linux-x64", "macos", "macos-arm64", "macos-x64"):
        fail("unsupported release platform")
    if tools_ref is not None and not re.fullmatch(r"[a-f0-9]{40}", tools_ref):
        fail("tools ref must be an immutable 40-character Ludus commit")
    if itch_target and not tools_ref and not platform.startswith("macos"):
        fail("CI upload setup requires --tools-ref <Ludus commit>")
    if platform == "macos":
        from .native import target_for_profile
        platform = "macos-arm64" if target_for_profile("macos-clang-release") == "arm64-apple-darwin" else "macos-x64"
    rendered = release_files(model.target, itch_target, platform=platform if platform != "web" else "linux-x64")
    config = json_bytes(rendered[0].content.encode(), "release template")
    profile, destination = ("macos-release", "macos") if platform.startswith("macos-") else ("linux-release", "linux")
    if platform == "web":
        profile, destination = "web-release", "web"
        config['profiles'] = {profile: {'targetPlatform': 'web', 'buildProfile': 'release', 'target': model.target,
            'installComponent': 'GameRelease', 'entryPoint': 'index.html', 'configurePreset': 'web-emscripten-release'}}
        if itch_target:
            config['itch']['channels'] = {destination: {'packageProfile': profile, 'channel': 'html5'}}
        rendered[1].content = '''# Explicit Emscripten release payload. Add assets and corresponding notices here.
if(EMSCRIPTEN)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/index.html"
                  "$<TARGET_FILE:TARGET_TOKEN>"
                  "$<TARGET_FILE_DIR:TARGET_TOKEN>/index.wasm"
            DESTINATION . COMPONENT GameRelease)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.txt" DESTINATION . COMPONENT GameRelease)
    if(NOT DEFINED Ludus_SDK_MANIFEST)
        message(FATAL_ERROR "Release packaging requires an SDK identity manifest")
    endif()
    get_filename_component(_game_ludus_share "${Ludus_SDK_MANIFEST}" DIRECTORY)
    install(DIRECTORY "${_game_ludus_share}/licenses/" DESTINATION licenses/Ludus COMPONENT GameRelease)
endif()
'''.replace('TARGET_TOKEN', model.target)
    if tools_ref:
        if not itch_target:
            config["itch"] = {"channels": {destination: {"packageProfile": profile, "channel": "html5" if platform == "web" else destination + "-stable"}}}
        config['automation'] = {'provider': 'github-actions', 'releaseTags': True, 'buildCommand': build_command if build_command is not None else (['./scripts/release-prepare'] if (root / 'scripts/release-prepare').is_file() else [])}
        if platform != 'web':
            workflow_steps.require_hosted_toolchain(platform)
        rendered.append(TemplateFile('.github/workflows/itch-release.yml', workflow_text(profile, destination, platform)))
        if workflow_steps.has_tools_revision(root, tools_ref) is False:
            rendered.append(TemplateFile(workflow_steps.TOOLS_REVISION_RELPATH, tools_ref + '\n'))
    rendered[0].content = canonical(config).decode()
    if platform == 'web':
        next(file for file in rendered if file.relpath == 'RELEASING.md').content = next(file for file in rendered if file.relpath == 'RELEASING.md').content.replace('linux-release', 'web-release').replace('--destination linux', '--destination web')
    release_guide = next(file for file in rendered if file.relpath == "RELEASING.md")
    if platform.startswith("macos-") and tools_ref:
        release_guide.content += "\nCI: set LUDUS_RELEASE_SDK_URL and LUDUS_RELEASE_SDK_SHA256 for a matching Release SDK. Push a v* tag or run the macOS release package workflow. It has no upload job or account secret.\n"
    elif not platform.startswith("macos-"):
        release_guide.content += '''
Editor: use Release > Package Release after setup. Commit the generated files.
For CI, set the itch-release environment secret BUTLER_API_KEY. If no target
was saved, set the repository variable ITCH_IO_TARGET to username/game. Push a v* tag or
run the itch.io release workflow manually. Build jobs receive no upload secret.
A missing itch destination requires ITCH_IO_TARGET before CI can upload; rerunning setup
never overwrites existing files. Edit configuration and add the workflow explicitly.
Native CI requires repository variables LUDUS_RELEASE_SDK_URL and
LUDUS_RELEASE_SDK_SHA256 for a matching Release SDK archive. An optional
scripts/release-prepare script can prepare project-specific inputs.
Browser uploads need an itch.io HTML game page and the uploaded channel marked
playable in browser once, in the itch.io Edit game page. A successful butler push
means upload submitted; verify hosted gameplay separately.
'''
    cmake_path = root / 'CMakeLists.txt'
    original = cmake_path.read_bytes()
    if len(original) > 2 * 1024 * 1024:
        fail("CMakeLists.txt exceeds setup limit")
    include = b'\ninclude(cmake/GameRelease.cmake)\n'
    if b'cmake/GameRelease.cmake' in original:
        fail("CMake release include already exists; setup will not overwrite authored release rules", "Conflict")
    descriptor_digest = file_digest(paths.descriptor_path)
    if expected_descriptor_digest and descriptor_digest != expected_descriptor_digest:
        fail("descriptor changed before release setup", "Conflict")
    # Cooperates with Editor/CLI jobs for the saved profile. Every output is new;
    # the release config is the final commit point. An interrupted transaction
    # can leave reviewable generated files, but cannot claim setup complete.
    generated: list[Path] = []
    with BuildTreeLock(root / 'out/build' / model.preset), tempfile.TemporaryDirectory(prefix='.release-setup-', dir=root) as temp:
        stage = Path(temp)
        for file in rendered:
            path = root / file.relpath
            if path.exists() or path.is_symlink():
                fail(f"release setup refuses to overwrite {file.relpath}", "Conflict")
            staged = stage / file.relpath
            staged.parent.mkdir(parents=True, exist_ok=True)
            staged.write_text(file.content, encoding='utf-8')
        load_release(stage)  # strict validation before the first mutation
        if cmake_path.read_bytes() != original or file_digest(paths.descriptor_path) != descriptor_digest:
            fail("project changed while preparing release setup", "Conflict")
        if cancel_check:
            cancel_check()
        cmake_changed = False
        try:
            for file in rendered[1:]:
                path = root / file.relpath
                path.parent.mkdir(parents=True, exist_ok=True)
                os.link(stage / file.relpath, path)  # no-replace, including races
                generated.append(path)
            updated = stage / 'CMakeLists.txt'
            updated.write_bytes(original + include)
            os.replace(updated, cmake_path)
            cmake_changed = True
            os.link(stage / rendered[0].relpath, root / rendered[0].relpath)
            generated.append(root / rendered[0].relpath)
        except OSError:
            if cmake_changed:
                rollback = stage / 'CMakeLists.rollback'
                rollback.write_bytes(original)
                os.replace(rollback, cmake_path)
            for path in generated:
                path.unlink(missing_ok=True)
            raise
    return [p.relative_to(root).as_posix() for p in generated] + ['CMakeLists.txt']
