"""Build and test the D7 Windows source runtime on WARP or a named physical adapter."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
PRESET = "windows-msvc-runtime"
TARGET_TESTS = (
    "ludus_platform_win32_tests", "ludus_rhi_d3d12_tests", "ludus_rhi_lifecycle_tests",
    "ludus_world_demo_tests", "ludus_content_tests", "ludus_resources_tests",
    "ludus_gameplay_world_tests", "ludus_gameplay_camera_tests", "ludus_gameplay_sequencer_tests",
    "ludus_text_tests", "ludus_graphics_text_tests", "ludus_localization_tests",
    "ludus_ui_tests", "ludus_audio_tests",
    "ludus_network_core_tests", "ludus_physics_fluid_tests", "ludus_platform_pacing_tests",
    "ludus_shader_compiler_tests", "ludus_render_graph_dxil_tests",
    "ludus_behavior_tests", "ludus_behavior_acceptance", "ludus_behavior_safety",
    "ludus_behavior_allocator_contract", "ludus_runtime_game_api_tests",
    "ludus_static_game_host_tests", "ludus_game_host_loader_tests", "ludus_game_host_reload_tests",
    "ludus_game_host_protocol_tests", "ludus_game_host_timing_tests", "ludus_game_host_session_tests",
    "ludus_scene_cook_tests", "ludus_scene_assets_tests", "ludus_best_effort_import_tests",
    "ludus_scene_import_tests",
)
OTHER_TARGETS = (
    "ludus_world_demo", "ludus_world_frame_renderer_tests", "ludus-shader-worker",
    "ludus_game_host_app", "ludus_cook",
)
OTHER_TESTS = (
    "ludus_world_packed_room_d3d12", "ludus_world_frame_renderer_pixels", "ludus_shader_worker_protocol",
    "ludus_game_host_windows_symbols", "ludus_game_host_windows_supervisor", "ludus_behavior_cook",
    "ludus_behavior_profiles", "ludus_behavior_evidence", "ludus_localization_cooker",
    "ludus_localization_release",
)
HOST_IDENTITY = r"""
$ErrorActionPreference = 'Stop'
$os = Get-CimInstance Win32_OperatingSystem
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
$update = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -Name UBR
@{
    os = $os.Caption
    version = "$($os.Version).$($update.UBR)"
    architecture = $os.OSArchitecture
    compiler_version = (Get-Item -LiteralPath $compiler).VersionInfo.FileVersion
    adapters = @(Get-CimInstance Win32_VideoController |
        Select-Object Name, DriverVersion, PNPDeviceID)
} | ConvertTo-Json -Depth 4 -Compress
"""


def run(*command, environment=None, capture=False):
    return subprocess.run(command, cwd=ROOT, env=environment, check=True, timeout=2700,
                          stdout=subprocess.PIPE if capture else None, text=True,
                          encoding="utf-8", errors="replace").stdout


def test_inventory(tests):
    return "^(" + "|".join(re.escape(name) for name in tests) + ")$"


def require_inventory(expected, actual):
    if sorted(expected) != sorted(actual):
        missing = sorted(set(expected) - set(actual))
        raise RuntimeError(f"Required CTest inventory mismatch (missing {missing}): {actual}")


def check_results(path, expected):
    cases = ET.parse(path).findall(".//testcase")
    require_inventory(expected, [case.attrib["name"] for case in cases])
    for case in cases:
        if case.attrib.get("status") != "run" or any(
            case.find(element) is not None for element in ("skipped", "failure", "error")
        ):
            raise RuntimeError(f"Required test did not pass: {case.attrib['name']}")
    return {case.attrib["name"]: case.findtext("system-out", "") for case in cases}


def check_adapter(host, test_output, adapter, driver):
    adapters = host["adapters"]
    matches = [entry for entry in adapters
               if (entry["Name"] or "").casefold() == adapter.casefold() and entry["DriverVersion"] == driver]
    if len(matches) != 1 or not (matches[0]["PNPDeviceID"] or "").upper().startswith("PCI\\"):
        raise RuntimeError(f"Expected one PCI display adapter named {adapter!r} with driver {driver!r}")
    selected = re.findall(r"Direct3D 12 hardware adapter '([^\r\n]+)'", test_output)
    if [name.casefold() for name in selected] != [adapter.casefold()]:
        raise RuntimeError(f"Packed World selected {selected!r}. Select {adapter!r} on this host and retry.")
    return matches[0]


def qualify(args):
    if os.name != "nt":
        raise RuntimeError("Use an x64 Windows developer shell for the pinned MSVC and Windows SDK.")
    if not re.fullmatch(r"[0-9a-f]{40}", args.candidate):
        raise RuntimeError("Pass the full 40-character candidate SHA.")
    revision = run("git", "rev-parse", "HEAD", capture=True).strip()
    if revision != args.candidate or run("git", "status", "--porcelain", "--untracked-files=no", capture=True).strip():
        raise RuntimeError("Check out the exact candidate in a clean worktree before collecting evidence.")

    args.output.mkdir(parents=True, exist_ok=False)
    host = json.loads(run("powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                          "[Console]::OutputEncoding = [Text.Encoding]::UTF8\n" + HOST_IDENTITY, capture=True))
    identity = {
        "candidate": revision, "preset": PRESET, "adapter_mode": args.adapter, "host": host,
        "msvc_toolset": os.environ["VCToolsVersion"], "windows_sdk": os.environ["WindowsSDKVersion"],
        "cmake": run("cmake", "--version", capture=True).splitlines()[0],
        "ninja": run("ninja", "--version", capture=True).strip(),
        "shader_toolchain": json.loads((ROOT / "config/shader_toolchain.json").read_text(encoding="utf-8")),
    }
    evidence = args.output / "runtime.json"
    evidence.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
    environment = os.environ.copy()
    for variable in ("LUDUS_TEST_REQUIRE_WARP", "LUDUS_TEST_REQUIRE_HARDWARE"):
        environment.pop(variable, None)
    environment[f"LUDUS_TEST_REQUIRE_{args.adapter.upper()}"] = "1"
    environment["LUDUS_DIAGNOSTIC_INTERACTIVE"] = "0"
    run("cmake", "--fresh", "--preset", PRESET, "-DLUDUS_BUILD_SHADER_WORKER=ON", "-DLUDUS_BUILD_BEHAVIOR=ON",
        "-DLUDUS_BUILD_BEHAVIOR_ACCEPTANCE=ON", environment=environment)
    run("cmake", "--build", "--preset", PRESET, "--target", *TARGET_TESTS, *OTHER_TARGETS,
        "--", "-k", "0", environment=environment)
    expected = TARGET_TESTS + OTHER_TESTS
    ctest = ("ctest", "--preset", PRESET, "--tests-regex", test_inventory(expected))
    inventory = json.loads(run(*ctest, "--show-only=json-v1", capture=True, environment=environment))
    (args.output / "inventory.json").write_text(json.dumps(inventory, indent=2) + "\n", encoding="utf-8")
    require_inventory(expected, [test["name"] for test in inventory["tests"]])
    report = args.output / "ctest.xml"
    run(*ctest, "--output-on-failure", "--no-tests=error", "--output-junit", str(report), environment=environment)
    results = check_results(report, expected)
    packed_world = results["ludus_world_packed_room_d3d12"]
    if args.adapter == "warp" and "Direct3D 12 software adapter" not in packed_world:
        raise RuntimeError("The packed World test did not record its software adapter.")
    if args.adapter == "hardware":
        identity["physical_adapter"] = check_adapter(host, packed_world,
                                                      args.adapter_name, args.driver)
    identity["passed_tests"] = sorted(results)
    evidence.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--adapter", choices=("warp", "hardware"), required=True)
    parser.add_argument("--adapter-name")
    parser.add_argument("--driver")
    parser.add_argument("--output", type=Path, required=True, help="New directory for evidence and test results")
    args = parser.parse_args()
    if args.adapter == "hardware" and not (args.adapter_name and args.driver):
        parser.error("hardware evidence requires --adapter-name and --driver")
    if args.adapter == "warp" and (args.adapter_name or args.driver):
        parser.error("--adapter-name and --driver apply only to physical hardware")
    args.output = args.output.resolve()
    try:
        qualify(args)
    except (RuntimeError, OSError, ValueError, KeyError, ET.ParseError, subprocess.SubprocessError) as error:
        print(f"Windows runtime qualification: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
