"""Build and test the D7 Windows source runtime on WARP or a named physical adapter."""

import argparse
import json
import math
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
    "ludus_world_demo", "ludus_world_frame_renderer_tests", "ludus_rhi_render_tests", "ludus-shader-worker",
    "ludus_game_host_app", "ludus_cook",
)
OTHER_TESTS = (
    "ludus_world_packed_room_d3d12", "ludus_world_frame_renderer_pixels", "ludus_shader_worker_protocol",
    "ludus_game_host_windows_symbols", "ludus_game_host_windows_supervisor", "ludus_behavior_cook",
    "ludus_behavior_profiles", "ludus_behavior_evidence", "ludus_localization_cooker",
    "ludus_localization_release", "ludus_rhi_windows_profile_smoke",
)
SANITIZER_TARGET_TESTS = (
    "ludus_foundation_base_tests", "ludus_foundation_memory_tests",
    "ludus_foundation_containers_tests", "ludus_foundation_time_tests",
    "ludus_foundation_threading_tests", "ludus_threading_startup_tests",
    "ludus_threading_profiling_tests", "ludus_foundation_logging_tests",
    "ludus_foundation_profiling_tests", "ludus_foundation_identity_tests",
    "ludus_filesystem_tests", "ludus_platform_entropy_tests", "ludus_input_tests",
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


def check_performance_limits(report, path):
    limits = json.loads(path.read_text(encoding="utf-8"))
    if (not isinstance(limits, dict) or set(limits) != {"schema", "scenarios"}
            or isinstance(limits["schema"], bool) or limits["schema"] != 1):
        raise RuntimeError("Performance limits require schema 1 and a scenarios object.")
    scenarios = {entry["id"]: entry for entry in report["scenarios"]}
    if not isinstance(limits["scenarios"], dict) or set(limits["scenarios"]) != set(scenarios):
        raise RuntimeError("Declare performance limits for every measured scenario.")
    for name, thresholds in limits["scenarios"].items():
        metrics = scenarios[name]["metrics"]
        if not isinstance(thresholds, dict) or not thresholds:
            raise RuntimeError(f"Declare at least one measured limit for {name}.")
        for metric, upper_bound in thresholds.items():
            if (isinstance(upper_bound, bool) or not isinstance(upper_bound, (float, int))
                    or not math.isfinite(upper_bound) or upper_bound <= 0 or metric not in metrics):
                raise RuntimeError(f"{name}: invalid or unmeasured limit {metric!r}.")
            measured = metrics[metric]["max"]
            if not math.isfinite(measured) or measured < 0 or measured > upper_bound:
                raise RuntimeError(f"{name}: {metric} measured {measured}, exceeding limit {upper_bound}.")
    gpu = scenarios.get("rhi-ordered-graph-gpu-timing")
    if gpu is not None:
        required = {name for name in gpu["metrics"]
                    if name.endswith(("/peak_api_bytes_maximum", "/gpu_duration_p95_ns"))}
        availability = [value for name, value in gpu["metrics"].items()
                        if name.endswith("/api_bytes_available_minimum")]
        if (not required or not required.issubset(limits["scenarios"][gpu["id"]]) or not availability
                or any(value["min"] != 1 for value in availability)):
            raise RuntimeError("GPU qualification requires available API allocation bytes, peak memory limits "
                               "and p95 GPU limits for every workload.")



def profile_runtime(args, identity, environment):
    sys.path.insert(0, str(ROOT / "scripts/python"))
    import runtime_profile
    scenarios = runtime_profile.load_manifest(ROOT / runtime_profile.MANIFEST_PATH)
    requested = ("world-headless", "world-presented", "rhi-ordered-graph", "rhi-ordered-graph-gpu-timing")
    capabilities = {"shader-tools", "presentation", "gpu", "gpu-windows-qualification"}
    selection = runtime_profile.select_scenarios(scenarios, requested, capabilities)
    report_directory = args.output / "performance"
    # The scenario runner already owns logs, traces, warmups and repetitions.
    # Supply the validated adapter requirement to every spawned workload.
    previous = os.environ.copy()
    try:
        os.environ.update(environment)
        report = runtime_profile.run_scenarios(
            ROOT, args.preset, ROOT / "out/build" / args.preset, report_directory, selection, capabilities,
            compiler=identity["host"]["compiler_version"],
            device=f'{args.adapter_name}, driver {args.driver}, Windows {identity["host"]["version"]}',
            controlled_environment=runtime_profile.environment_names(scenarios),
            system_gpus=lambda: identity["host"]["adapters"])
    finally:
        os.environ.clear()
        os.environ.update(previous)
    if any(entry["status"] != "passed" for entry in report["scenarios"]):
        raise RuntimeError(f"Windows performance workload failed. Inspect {report_directory / 'report.json'}")
    identity["performance_report"] = "performance/report.json"
    identity["has_passed_performance_limits"] = False
    if args.performance_limits is not None:
        check_performance_limits(report, args.performance_limits)
        identity["performance_limits"] = json.loads(args.performance_limits.read_text(encoding="utf-8"))
        identity["has_passed_performance_limits"] = True



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
        "candidate": revision, "preset": args.preset, "adapter_mode": args.adapter, "host": host,
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
    run("cmake", "--fresh", "--preset", args.preset, "-DLUDUS_BUILD_SHADER_WORKER=ON", "-DLUDUS_BUILD_BEHAVIOR=ON",
        "-DLUDUS_BUILD_BEHAVIOR_ACCEPTANCE=ON", environment=environment)
    sanitizer_tests = SANITIZER_TARGET_TESTS if args.preset == "windows-msvc-asan" else ()
    targets = tuple(dict.fromkeys(TARGET_TESTS + OTHER_TARGETS + sanitizer_tests))
    if sanitizer_tests:
        targets += ("ludus_asan_control_child",)
    run("cmake", "--build", "--preset", args.preset, "--target", *targets,
        "--", "-k", "0", environment=environment)
    expected = tuple(dict.fromkeys(TARGET_TESTS + OTHER_TESTS + sanitizer_tests))
    if sanitizer_tests:
        expected += ("ludus_asan_control",)
    ctest = ("ctest", "--preset", args.preset, "--tests-regex", test_inventory(expected))
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
    if args.performance:
        profile_runtime(args, identity, environment)
    identity["passed_tests"] = sorted(results)
    evidence.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--preset", choices=(PRESET, "windows-msvc-asan"), default=PRESET)
    parser.add_argument("--adapter", choices=("warp", "hardware"), required=True)
    parser.add_argument("--adapter-name")
    parser.add_argument("--driver")
    parser.add_argument("--performance-limits", type=Path,
                        help="Schema 1 JSON limits for every measured workload, including peak GPU API bytes")
    parser.add_argument("--performance", action="store_true",
                        help="Record native World and ordered-graph CPU/GPU and allocation workloads")
    parser.add_argument("--output", type=Path, required=True, help="New directory for evidence and test results")
    args = parser.parse_args()
    if args.adapter == "hardware" and not (args.adapter_name and args.driver):
        parser.error("hardware evidence requires --adapter-name and --driver")
    if args.performance_limits is not None and not args.performance:
        parser.error("--performance-limits requires --performance")
    if args.performance and args.adapter != "hardware":
        parser.error("--performance requires a named physical adapter")
    if args.adapter == "warp" and (args.adapter_name or args.driver):
        parser.error("--adapter-name and --driver apply only to physical hardware")
    args.output = args.output.resolve()
    try:
        qualify(args)
    except (RuntimeError, OSError, ValueError, KeyError, TypeError, ET.ParseError, subprocess.SubprocessError) as error:
        print(f"Windows runtime qualification: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
