"""Optional release files rendered inside atomic project creation."""
from __future__ import annotations

from .release_model import canonical
from .templates import TemplateFile
from .toolchain import PINNED_LLVM_MAJOR


def release_files(target: str, itch_target: str | None = None, *, platform: str = "linux-x64") -> list[TemplateFile]:
    from .release_model import fail
    if platform not in ("linux-x64", "macos-arm64", "macos-x64"):
        fail("unsupported native release platform")
    macos = platform.startswith("macos-")
    profile, destination = ("macos-release", "macos") if macos else ("linux-release", "linux")
    config = {"schemaVersion": 1, "profiles": {profile: {
        "targetPlatform": platform, "buildProfile": "release", "target": target,
        "installComponent": "GameRelease", "entryPoint": f"{target}.app/Contents/MacOS/{target}" if macos else f"bin/{target}",
    }}}
    if itch_target is not None:
        from .release_model import ITCH_TARGET, string, fail
        if not ITCH_TARGET.fullmatch(string(itch_target, "itch target", 128)):
            fail("itch target must be lower-case username/game")
        config["itch"] = {"target": itch_target, "channels": {
            destination: {"packageProfile": profile, "channel": destination + "-stable"},
        }}
    helper = '''# Generated native release component. These files belong to the game.
# Declare all runtime assets/libraries explicitly; never install the SDK itself.
install(TARGETS TARGET_PLACEHOLDER RUNTIME DESTINATION bin COMPONENT GameRelease)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.txt" DESTINATION . COMPONENT GameRelease)
if(NOT DEFINED Ludus_SDK_MANIFEST)
    message(FATAL_ERROR "Release packaging requires an SDK with Ludus_SDK_MANIFEST")
endif()
get_filename_component(_game_ludus_share "${Ludus_SDK_MANIFEST}" DIRECTORY)
install(DIRECTORY "${_game_ludus_share}/licenses/" DESTINATION licenses/Ludus COMPONENT GameRelease)
# Add your own assets and their license notices here using COMPONENT GameRelease.
# Runtime dependency policy: only the documented Linux ABI baseline is external.
'''.replace("TARGET_PLACEHOLDER", target)
    if macos:
        helper = helper.replace("install(TARGETS " + target + " RUNTIME DESTINATION bin COMPONENT GameRelease)", """if(NOT APPLE)
    message(FATAL_ERROR "This release component requires macOS")
endif()
set_target_properties(TARGET_TOKEN PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.ludus.game.TARGET_TOKEN"
    MACOSX_BUNDLE_BUNDLE_NAME "TARGET_TOKEN"
    INSTALL_RPATH "@executable_path/../Frameworks"
    INSTALL_RPATH_USE_LINK_PATH FALSE)
install(TARGETS TARGET_TOKEN BUNDLE DESTINATION . COMPONENT GameRelease)""".replace("TARGET_TOKEN", target)).replace("Linux ABI baseline", "macOS 14.0 system-library baseline")
    result = [TemplateFile("ludus.release.json", canonical(config).decode("utf-8")),
            TemplateFile("cmake/GameRelease.cmake", helper),
            TemplateFile("NOTICE.txt", "This game uses Ludus. See licenses/Ludus/ for SDK license notices.\n"
                         "Add notices for your game assets and every bundled dependency before distribution.\n"),
            TemplateFile("RELEASING.md", '''# Releasing this game

Build a native package with `ludus project package . --profile linux-release --version 0.1.0`.
Select a Release SDK explicitly if your local SDK is another flavor. Packaging
builds before installing and validates a clean extraction; it never uploads.

Verify the printed directory with `ludus project package verify <directory>`.
Run release tests against extracted game.zip before distributing it. Read the
manifest for external system libraries and SDK/platform requirements. NOTICE.txt
and SDK notices are a starting point; add licenses for authored assets/dependencies.

If itch.io is configured, use `ludus project publish plan . --package <directory>
--destination linux` to inspect the offline plan. Packages from local overrides
or dirty/unknown sources require `--allow-local-inputs`. This command does not
upload, authenticate, install butler or generate CI. See the installed tooling
guides for explicit setup, Editor controls and supported upload hosts. A channel name does
not change itch.io project visibility. Account credentials never belong in Git.
''')]

    if macos:
        result[-1].content = result[-1].content.replace("linux-release", profile).replace("--destination linux", "--destination macos")
        result[-1].content += f"""
macOS: use a matching thin arm64 or x86_64 Release SDK and Clang {PINNED_LLVM_MAJOR}/libc++.
The generated install component creates an app bundle. Declare assets and
regular runtime dylibs explicitly; use package-relative @rpath/@loader_path
install names and search paths. Symlinked framework deployments and universal
binaries are outside this initial policy. Libraries require their own relative
search paths; inherited dyld run-path stacks are not emulated.
Packaging signs nested images, then the app, locally with an ad-hoc identity.
It verifies signatures again after clean extraction. This is not Developer ID
signing, notarization or Gatekeeper qualification; those require a separate
explicit distribution workflow. No keychain identity or account is used.
macOS CI packages only. Live itch.io upload transport remains Linux-only.
"""
    return result
