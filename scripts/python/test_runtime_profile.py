"""Runtime profiling runner: manifest contract, parsers, device identity, failure semantics and comparison."""
import copy
import json
import os
import runpy
from pathlib import Path
import subprocess
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import runtime_profile

ROOT = Path(__file__).resolve().parents[2]

# Captured from the Profile-flavor harnesses; trimmed but unedited lines.
THREADING_OUTPUT = """jobs=256 iterations=0 workers=0 median_ns=1833 checksum=256
jobs=256 iterations=0 workers=4 median_ns=27167 checksum=256
jobs=256 iterations=10000 workers=4 median_ns=803750 checksum=18367026793921049022
"""
STRINGS_OUTPUT = """String_bytes,48
SharedString_bytes,8
case,ns_per_operation
unique_construct_assign,26
xxh3_64,3
checksum,26371200
"""
INPUT_OUTPUT = """# Ludus keyboard reducer — performance evidence
allocations: construction=1, hot-loop(5000 iters)=0
idle step:              median=42.0 ns  p95=42.0 ns
256 transitions/256 bindings (map=Ok): median=20292.0 ns  p95=20791.0 ns
trace off: median=42.0 ns   trace on: median=42.0 ns
"""
CONTAINER_OUTPUT = """count,sorted_lookup_ns,hash_lookup_ns,tree_lookup_ns,sorted_insert_ns,hash_insert_ns,sorted_storage_bytes
1,4.86,7.35,5.82,3.25,37.83,16
16,9.97,6.03,15.08,11.81,20.18,256
"""
# The line shapes ludus_rhi_render_tests prints with LUDUS_RHI_BENCHMARK=1, with
# fewer samples. "Apple M5" is 4170706c65204d35 in hex.
ADAPTER_LINE = "R7ADAPTER,4170706c65204d35,0,0,0,1,0"
SOFTWARE_ADAPTER_LINE = "R7ADAPTER,6c6c766d70697065,65541,0,0,1,1"
RHI_OUTPUT = "\n".join([
    "Randomness seeded to: 1",
    ADAPTER_LINE,
    "R7PROFILE,1,1,1,64,256",
    "R5BACKEND,3",
    "R7,1,0,0,3,1200,41.6666667,64,24576,1,32768,32768,500,700,1,65536,65536",
    "R5,1,0,0,9000,4000,30000,2,3,512,1024,24576",
    "R7,1,0,1,4,0,0,0,24576,1,32768,32768,500,700,1,65536,65536",
    "R5,1,0,1,9500,4100,31000,2,3,512,1024,24576",
    "R7,1,1,0,3,1400,41.6666667,64,24576,1,32768,32768,500,700,1,65536,65536",
    "R5,1,1,0,8000,3000,29000,2,3,512,1024,24576",
    "R7,1,1,1,3,1300,41.6666667,64,24576,1,32768,32768,500,700,1,65536,65536",
    "R5,1,1,1,8500,3500,32000,2,3,512,1024,24576",
    "All tests passed",
]) + "\n"
R7_COLUMNS = ["layers", "sample", "merge", "state", "gpu_duration_ns", "tick_ns", "valid_bits", "logical_bytes",
              "api_bytes_available", "api_bytes", "peak_api_bytes", "arm_cpu_ns", "poll_and_memory_cpu_ns",
              "allocator_blocks_available", "allocator_block_bytes", "peak_allocator_block_bytes"]
R5_COLUMNS = ["layers", "sample", "merge", "setup_cpu_ns", "execute_cpu_ns", "readback_wait_cpu_ns", "raster_passes",
              "draws", "plan_bytes", "buffer_bytes", "texture_bytes"]
VULKAN_SUMMARY = """==========
VULKANINFO
==========

Devices:
========
GPU0:
\tapiVersion         = 1.3.280
\tvendorID           = 0x10de
\tdeviceID           = 0x2684
\tdeviceType         = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
\tdeviceName         = NVIDIA GeForce RTX 4090
\tdriverName         = NVIDIA
\tdriverInfo         = 550.54.14
GPU1:
\tdeviceType         = PHYSICAL_DEVICE_TYPE_CPU
\tdeviceName         = llvmpipe (LLVM 17.0.6, 256 bits)
"""


def rhi_parser(**table_overrides):
    table = {"tag": "R5", "columns": R5_COLUMNS, "key": ["layers", "merge"],
             "metrics": ["setup_cpu_ns", "execute_cpu_ns"], "where": {}, "rows": 4}
    table.update(table_overrides)
    return {"kind": "tagged-rows", "tables": [table], "observe": ["R7ADAPTER", "R7PROFILE", "R5BACKEND"]}


def trace_document(tick_microseconds=1.0):
    events = []
    for frame in range(3):
        start = frame * 10.0
        events += [
            {"pid": 1, "tid": 0, "ts": start, "ph": "B", "name": "Tick"},
            {"pid": 1, "tid": 0, "ts": start + 1, "ph": "B", "name": "Motion"},
            {"pid": 1, "tid": 0, "ts": start + 1 + tick_microseconds, "ph": "E", "name": "Motion"},
            {"pid": 1, "tid": 0, "ts": start + 4, "ph": "E", "name": "Tick"},
            {"pid": 1, "tid": 0, "ts": start + 4, "ph": "i", "name": "FrameMark", "s": "t"},
        ]
    return {"traceEvents": events, "displayTimeUnit": "ns"}


def scenario_entry(**overrides):
    entry = {
        "id": "sample", "owner": "modules/sample", "description": "Sample scenario.", "target": "ludus_sample",
        "executable": "bin/sample", "arguments": [], "cmake_options": {}, "environment": {}, "requires": [],
        "trace": False,
        "parser": {"kind": "key-value", "identity": ["workers"], "metrics": ["median_ns"]},
        "warmup": 0, "repetitions": 3, "timeout_seconds": 10,
    }
    entry.update(overrides)
    return entry


def write_manifest(directory, *entries):
    path = Path(directory) / "manifest.json"
    path.write_text(json.dumps({"schema": 1, "scenarios": list(entries)}))
    return path


class Manifest(unittest.TestCase):
    def test_repository_manifest_is_valid_and_builds_declared_executables(self):
        scenarios = runtime_profile.load_manifest(ROOT / runtime_profile.MANIFEST_PATH)
        self.assertTrue(scenarios)
        hosted = [scenario for scenario in scenarios if not set(scenario.requires) - {"shader-tools"}]
        self.assertTrue(any(scenario.trace for scenario in hosted), "hosted lanes need a trace-producing scenario")
        listed = subprocess.run(["git", "ls-files", "*CMakeLists.txt"], cwd=ROOT, stdout=subprocess.PIPE, text=True,
                                check=True).stdout.split()
        cmake_text = "\n".join((ROOT / path).read_text() for path in listed)
        for scenario in scenarios:
            with self.subTest(scenario=scenario.id):
                self.assertIn(f"add_executable({scenario.target}", cmake_text)
        device = [scenario for scenario in scenarios if runtime_profile.GPU_CAPABILITY in scenario.requires]
        self.assertTrue(device, "device lanes need GPU scenarios")
        read = set(subprocess.run(["git", "grep", "-h", "-o", "-E", 'getenv\\("LUDUS_[A-Z0-9_]+"\\)', "--", "modules"],
                                  cwd=ROOT, stdout=subprocess.PIPE, text=True, check=True).stdout.split())
        for scenario in scenarios:
            with self.subTest(scenario=scenario.id):
                for name, _ in scenario.environment:
                    self.assertIn(f'getenv("{name}")', read, f"{name} is not read by any module")
        for scenario in device:
            with self.subTest(scenario=scenario.id):
                self.assertEqual(scenario.parser["kind"], "tagged-rows")
                self.assertIn("shader-tools", scenario.requires)

    def test_repository_gpu_scenarios_match_their_benchmark_scripts(self):
        """Row counts and columns agree with scripts/benchmark-rhi and scripts/benchmark-renderer."""
        manifest = {scenario.id: scenario for scenario in runtime_profile.load_manifest(ROOT / runtime_profile.MANIFEST_PATH)}
        rhi = (ROOT / "scripts/benchmark-rhi").read_text()
        renderer = (ROOT / "scripts/benchmark-renderer").read_text()
        tables = {table["tag"]: table for scenario in ("rhi-ordered-graph", "rhi-ordered-graph-gpu-timing")
                  for table in manifest[scenario].parser["tables"]}
        self.assertIn(f"len(rows) != {tables['R5']['rows']}", rhi)
        self.assertIn(f"len(row) != {len(tables['R7']['columns'])}", rhi)
        self.assertEqual(tables["R7"]["where"], {"state": "3"}, "only available GPU timings are durations")
        self.assertIn("row[3] == 3", rhi)
        l5 = manifest["renderer-clustered-lighting"].parser["tables"][0]
        self.assertIn(f"len(samples) != {l5['rows']}", renderer)
        self.assertEqual(manifest["rhi-ordered-graph"].arguments, manifest["rhi-ordered-graph-gpu-timing"].arguments)
        self.assertNotIn("LUDUS_RHI_GPU_TIMING", dict(manifest["rhi-ordered-graph"].environment),
                         "the control run must stay uninstrumented")

    def test_rejects_unknown_or_missing_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            entry = scenario_entry(extra=True)
            with self.assertRaisesRegex(runtime_profile.ProfileError, "unknown \\['extra'\\]"):
                runtime_profile.load_manifest(write_manifest(directory, entry))
            entry = scenario_entry()
            del entry["timeout_seconds"]
            with self.assertRaisesRegex(runtime_profile.ProfileError, "missing \\['timeout_seconds'\\]"):
                runtime_profile.load_manifest(write_manifest(directory, entry))

    def test_rejects_invalid_values(self):
        cases = {
            "id": scenario_entry(id="Bad_ID"),
            "relative": scenario_entry(executable="../escape"),
            "arguments": scenario_entry(arguments=""),
            "option": scenario_entry(cmake_options={"CMAKE_BUILD_TYPE": "Debug"}),
            "repetitions": scenario_entry(repetitions=0),
            "parser": scenario_entry(parser={"kind": "unknown"}),
            "trace": scenario_entry(trace=False, parser={"kind": "perfetto-scopes"}),
            "csv": scenario_entry(parser={"kind": "csv", "header": "a,b", "key": ["a"], "metrics": ["c"], "ignore": []}),
            "regex": scenario_entry(parser={"kind": "regex", "pattern": "(?P<label>.*)"}),
            "environment": scenario_entry(environment={"PATH": "/tmp"}),
            "empty environment value": scenario_entry(environment={"LUDUS_RHI_BENCHMARK": ""}),
            "table keys": scenario_entry(parser={"kind": "tagged-rows", "observe": [],
                                                 "tables": [{"tag": "R5", "columns": ["a"]}]}),
            "table metric": scenario_entry(parser=rhi_parser(metrics=["missing_ns"])),
            "table key is metric": scenario_entry(parser=rhi_parser(key=["setup_cpu_ns"])),
            "table rows": scenario_entry(parser=rhi_parser(rows=0)),
            "table tag": scenario_entry(parser=rhi_parser(tag="r5")),
            "observe repeats table": scenario_entry(parser={**rhi_parser(), "observe": ["R5"]}),
        }
        with tempfile.TemporaryDirectory() as directory:
            for name, entry in cases.items():
                with self.subTest(case=name), self.assertRaises(runtime_profile.ProfileError):
                    runtime_profile.load_manifest(write_manifest(directory, entry))
            with self.assertRaisesRegex(runtime_profile.ProfileError, "repeats a scenario id"):
                runtime_profile.load_manifest(write_manifest(directory, scenario_entry(), scenario_entry()))


class Workflow(unittest.TestCase):
    def test_device_lane_is_opt_in_and_runs_only_trusted_main_code(self):
        workflow = (ROOT / ".github/workflows/performance.yml").read_text()
        job = workflow[workflow.index("\n  device:\n"):]
        self.assertIn("vars.LUDUS_PERF_DEVICE_LANES != ''", job)
        self.assertIn("github.ref == 'refs/heads/main'", job)
        self.assertIn("github.repository == 'The-Ludus-Project/Ludus'", job)
        self.assertIn("(github.event_name == 'schedule' || github.event_name == 'workflow_dispatch')", job)
        self.assertIn("runs-on: [self-hosted, ludus-perf,", job)
        self.assertIn("persist-credentials: false", job)
        self.assertIn('--capability "$GPU_CLASS" --device "$DEVICE"', job)
        self.assertIn("ARTIFACT: runtime-profile-device-${{ matrix.lane }}", job)
        self.assertIn("name: runtime-profile-device-${{ matrix.lane }}", job)
        self.assertNotIn("secrets.", workflow, "profiling jobs need no secrets")
        self.assertNotIn("pull_request_target", workflow)
        self.assertNotIn("write", workflow[workflow.index("permissions:"):workflow.index("concurrency:")])

    def test_runner_admission_hook_accepts_only_scheduled_or_dispatched_main_runs(self):
        hook = ROOT / "tools/ci/perf-runner-job-started.sh"
        self.assertTrue(os.access(hook, os.X_OK), "the hook must be executable")
        accepted = {"GITHUB_REPOSITORY": "The-Ludus-Project/Ludus", "GITHUB_REF": "refs/heads/main",
                    "GITHUB_EVENT_NAME": "schedule",
                    "GITHUB_WORKFLOW_REF": "The-Ludus-Project/Ludus/.github/workflows/performance.yml@refs/heads/main"}
        cases = {
            "schedule": ({}, 0),
            "dispatch": ({"GITHUB_EVENT_NAME": "workflow_dispatch"}, 0),
            "pull request": ({"GITHUB_EVENT_NAME": "pull_request", "GITHUB_REF": "refs/pull/7/merge",
                              "GITHUB_WORKFLOW_REF": "The-Ludus-Project/Ludus/.github/workflows/performance.yml"
                                                     "@refs/pull/7/merge"}, 1),
            "pull request event only": ({"GITHUB_EVENT_NAME": "pull_request_target"}, 1),
            "branch": ({"GITHUB_REF": "refs/heads/feature",
                        "GITHUB_WORKFLOW_REF": "The-Ludus-Project/Ludus/.github/workflows/performance.yml"
                                               "@refs/heads/feature"}, 1),
            "other workflow": ({"GITHUB_WORKFLOW_REF": "The-Ludus-Project/Ludus/.github/workflows/ci.yml"
                                                       "@refs/heads/main"}, 1),
            "fork": ({"GITHUB_REPOSITORY": "someone/Ludus"}, 1),
            "unset": ({"GITHUB_WORKFLOW_REF": None}, 1),
        }
        for name, (overrides, expected) in cases.items():
            with self.subTest(case=name):
                values = {**accepted, **overrides}
                environment = {key: value for key, value in os.environ.items() if not key.startswith("GITHUB_")}
                environment.update({key: value for key, value in values.items() if value is not None})
                completed = subprocess.run([str(hook)], env=environment, stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, text=True, check=False)
                self.assertEqual(completed.returncode, expected, completed.stderr)
                if expected:
                    self.assertIn("refusing job", completed.stderr)

    def test_hosted_macos_lane_runs_gpu_scenarios_on_a_declared_virtual_gpu(self):
        workflow = (ROOT / ".github/workflows/performance.yml").read_text()
        hosted = workflow[workflow.index("\n  hosted:\n"):workflow.index("\n  device:\n")]
        self.assertIn("gpu_class: gpu-apple-paravirtual", hosted)
        self.assertRegex(hosted, r'device: "[^"]*paravirtual[^"]*virtual machine[^"]*"')
        self.assertIn('arguments+=(--capability "$GPU_CLASS" --device "$DEVICE")', hosted)
        self.assertEqual(hosted.count('gpu_class: ""'), 1, "the Ubuntu lane declares no GPU")
        scenarios = runtime_profile.load_manifest(ROOT / runtime_profile.MANIFEST_PATH)
        macos = runtime_profile.resolve_capabilities({"shader-tools"}, ["presentation", "editor", "gpu-apple-paravirtual"],
                                                    "GitHub-hosted macos-15 arm64 runner, Apple paravirtual Metal GPU")
        self.assertTrue(all(reason is None for _, reason in runtime_profile.select_scenarios(scenarios, [], macos)))

    def test_scheduled_workflow_runs_the_runner_and_keeps_its_baseline_artifact_name(self):
        workflow = (ROOT / ".github/workflows/performance.yml").read_text()
        self.assertRegex(workflow, r"(?m)^  schedule:\n    - cron: ")
        self.assertIn("./scripts/profile-engine", workflow)
        self.assertIn("ARTIFACT: runtime-profile-${{ matrix.lane }}", workflow)
        self.assertIn("name: runtime-profile-${{ matrix.lane }}", workflow)
        self.assertIn("path: out/runtime-profile/${{ matrix.lane }}/", workflow)
        self.assertNotIn("pull_request", workflow, "timings must not become a pull-request gate")


class Selection(unittest.TestCase):
    def setUp(self):
        with tempfile.TemporaryDirectory() as directory:
            self.scenarios = runtime_profile.load_manifest(write_manifest(
                directory,
                scenario_entry(id="plain", cmake_options={"LUDUS_BUILD_A": "ON"}),
                scenario_entry(id="window", requires=["shader-tools", "presentation"],
                               cmake_options={"LUDUS_BUILD_A": "ON", "LUDUS_BUILD_B": "ON"})))

    def test_default_selection_skips_missing_capabilities(self):
        selection = runtime_profile.select_scenarios(self.scenarios, [], {"shader-tools"})
        self.assertEqual([(scenario.id, reason) for scenario, reason in selection],
                         [("plain", None), ("window", "missing capability presentation")])

    def test_explicit_selection_requires_capabilities_and_known_ids(self):
        with self.assertRaisesRegex(runtime_profile.ProfileError, "requires unavailable capability"):
            runtime_profile.select_scenarios(self.scenarios, ["window"], set())
        with self.assertRaisesRegex(runtime_profile.ProfileError, "unknown scenario"):
            runtime_profile.select_scenarios(self.scenarios, ["missing"], set())

    def test_configure_arguments_merge_options_and_shader_tools(self):
        self.assertEqual(runtime_profile.configure_arguments(self.scenarios[:1], ["-DLUDUS_SLANG_COMPILER=x"]),
                         ["-DLUDUS_BUILD_A=ON"])
        self.assertEqual(runtime_profile.configure_arguments(self.scenarios, ["-DLUDUS_SLANG_COMPILER=x"]),
                         ["-DLUDUS_BUILD_A=ON", "-DLUDUS_BUILD_B=ON", "-DLUDUS_SLANG_COMPILER=x"])
        conflicting = [self.scenarios[0], runtime_profile.Scenario(**{
            **self.scenarios[0].__dict__, "id": "other", "cmake_options": (("LUDUS_BUILD_A", "OFF"),)})]
        with self.assertRaisesRegex(runtime_profile.ProfileError, "disagree"):
            runtime_profile.configure_arguments(conflicting, None)


class Capabilities(unittest.TestCase):
    def test_gpu_class_implies_gpu_and_needs_a_device_identity(self):
        capabilities = runtime_profile.resolve_capabilities({"shader-tools"}, ["gpu-apple-m5", "presentation"],
                                                            "Apple M5 (8 GPU cores), Metal, macOS 26.6")
        self.assertEqual(capabilities, {"shader-tools", "gpu-apple-m5", "gpu", "presentation"})
        with self.assertRaisesRegex(runtime_profile.ProfileError, "requires --device"):
            runtime_profile.resolve_capabilities(set(), ["gpu-apple-m5"], None)
        self.assertEqual(runtime_profile.resolve_capabilities(set(), ["presentation"], None), {"presentation"})

    def test_rejects_bare_gpu_and_malformed_declarations(self):
        cases = {"bare gpu": (["gpu"], "Device"), "name": (["Presentation"], None),
                 "class": (["gpu-"], "Device"), "device text": (["gpu-x"], "line\nbreak"), "blank": ([], " ")}
        for name, (declared, device) in cases.items():
            with self.subTest(case=name), self.assertRaises(runtime_profile.ProfileError):
                runtime_profile.resolve_capabilities(set(), declared, device)

    def test_gpu_scenarios_are_skipped_without_a_gpu_class(self):
        scenarios = runtime_profile.load_manifest(ROOT / runtime_profile.MANIFEST_PATH)
        hosted = runtime_profile.select_scenarios(scenarios, [], {"shader-tools", "presentation", "editor"})
        device = runtime_profile.select_scenarios(scenarios, [], runtime_profile.resolve_capabilities(
            {"shader-tools"}, ["gpu-apple-m5", "presentation", "editor"], "Apple M5"))
        gpu = {scenario.id for scenario in scenarios if runtime_profile.GPU_CAPABILITY in scenario.requires}
        self.assertEqual({scenario.id for scenario, reason in hosted if reason is not None}, gpu)
        self.assertTrue(all(reason is None for _, reason in device))


class Parsers(unittest.TestCase):
    def test_p95_uses_nearest_rank_in_the_runner_and_native_benchmarks(self):
        values = list(range(30))
        functions = [runtime_profile.percentile]
        functions += [runpy.run_path(str(ROOT / "scripts" / name), run_name=name.replace("-", "_"))["percentile"]
                      for name in ("benchmark-rhi", "benchmark-renderer")]
        for function in functions:
            with self.subTest(function=function.__module__):
                self.assertEqual(function(values, 0.95), 28)
                self.assertEqual(function(values, 0.5), 14)
                self.assertEqual(function([7], 0.95), 7)

    def test_key_value_uses_identity_paths(self):
        metrics = runtime_profile.parse_key_value(THREADING_OUTPUT, ["iterations", "workers"], ["median_ns"])
        self.assertEqual(metrics, {
            "iterations=0/workers=0/median_ns": 1833.0,
            "iterations=0/workers=4/median_ns": 27167.0,
            "iterations=10000/workers=4/median_ns": 803750.0,
        })

    def test_csv_reads_the_declared_table_and_ignores_rows(self):
        metrics = runtime_profile.parse_csv(STRINGS_OUTPUT, "case,ns_per_operation", ["case"], ["ns_per_operation"],
                                            ["checksum"])
        self.assertEqual(metrics, {"case=unique_construct_assign/ns_per_operation": 26.0,
                                   "case=xxh3_64/ns_per_operation": 3.0})
        containers = runtime_profile.parse_csv(CONTAINER_OUTPUT, CONTAINER_OUTPUT.splitlines()[0], ["count"],
                                               ["hash_lookup_ns"], [])
        self.assertEqual(containers, {"count=1/hash_lookup_ns": 7.35, "count=16/hash_lookup_ns": 6.03})
        with self.assertRaisesRegex(runtime_profile.ProfileError, "header"):
            runtime_profile.parse_csv("other\n", "case,value", ["case"], ["value"], [])

    def test_regex_labels_lines(self):
        manifest = {scenario.id: scenario for scenario in runtime_profile.load_manifest(ROOT / runtime_profile.MANIFEST_PATH)}
        metrics = runtime_profile.parse_regex(INPUT_OUTPUT, manifest["input-reducer"].parser["pattern"])
        self.assertEqual(metrics, {
            "idle step/median_ns": 42.0, "idle step/p95_ns": 42.0,
            "256 transitions/256 bindings (map=Ok)/median_ns": 20292.0,
            "256 transitions/256 bindings (map=Ok)/p95_ns": 20791.0,
        })

    def test_repeated_or_non_finite_metrics_are_rejected(self):
        with self.assertRaisesRegex(runtime_profile.ProfileError, "repeats metric"):
            runtime_profile.parse_key_value("workers=1 median_ns=1\nworkers=1 median_ns=2\n", ["workers"], ["median_ns"])
        with self.assertRaisesRegex(runtime_profile.ProfileError, "non-finite"):
            runtime_profile.parse_key_value("workers=1 median_ns=nan\n", ["workers"], ["median_ns"])

    def test_tagged_rows_group_samples_and_filter_unavailable_timings(self):
        metrics = runtime_profile.parse_tagged_rows(RHI_OUTPUT, [
            {"tag": "R5", "columns": R5_COLUMNS, "key": ["layers", "merge"], "metrics": ["execute_cpu_ns"],
             "where": {}, "rows": 4},
            {"tag": "R7", "columns": R7_COLUMNS, "key": ["layers", "merge"], "metrics": ["gpu_duration_ns"],
             "where": {"state": "3"}, "rows": 4},
        ])
        self.assertEqual(metrics["R5/layers=1/merge=0/execute_cpu_median_ns"], 3000.0)
        self.assertEqual(metrics["R5/layers=1/merge=0/execute_cpu_p95_ns"], 4000.0)
        self.assertEqual(metrics["R5/layers=1/merge=1/samples"], 2.0)
        self.assertEqual(metrics["R7/layers=1/merge=0/gpu_duration_median_ns"], 1200.0)
        self.assertEqual(metrics["R7/layers=1/merge=0/gpu_duration_ns_minimum"], 1200.0)
        self.assertEqual(metrics["R7/layers=1/merge=0/gpu_duration_ns_maximum"], 1400.0)
        self.assertEqual(metrics["R7/layers=1/merge=1/samples"], 1.0, "pending/unavailable samples are not durations")
        self.assertEqual(metrics["R7/layers=1/merge=1/gpu_duration_median_ns"], 1300.0)

    def test_tagged_rows_reject_incomplete_or_malformed_sample_sets(self):
        table = {"tag": "R5", "columns": R5_COLUMNS, "key": ["layers"], "metrics": ["setup_cpu_ns"], "where": {},
                 "rows": 5}
        with self.assertRaisesRegex(runtime_profile.ProfileError, "expected 5 rows, found 4"):
            runtime_profile.parse_tagged_rows(RHI_OUTPUT, [table])
        with self.assertRaisesRegex(runtime_profile.ProfileError, "fields"):
            runtime_profile.parse_tagged_rows("R5,1,2\n", [{**table, "rows": 1}])
        with self.assertRaisesRegex(runtime_profile.ProfileError, "no rows match"):
            runtime_profile.parse_tagged_rows(RHI_OUTPUT, [{**table, "rows": 4, "where": {"merge": "2"}}])
        with self.assertRaisesRegex(runtime_profile.ProfileError, "not a number"):
            runtime_profile.parse_tagged_rows("R5,1,0,0,x,1,1,1,1,1,1,1\n", [{**table, "rows": 1}])

    def test_observations_and_adapter_identity(self):
        manifest = write_manifest(tempfile.mkdtemp(), scenario_entry(parser=rhi_parser()))
        scenario = runtime_profile.load_manifest(manifest)[0]
        observations = runtime_profile.parse_observations(scenario, RHI_OUTPUT)
        self.assertEqual(observations["R5BACKEND"], ["3"])
        self.assertEqual(runtime_profile.decode_adapter(observations["R7ADAPTER"]), {
            "name": "Apple M5", "vendor_id": 0, "device_id": 0, "driver_version_raw": None, "software": False})
        self.assertIsNone(runtime_profile.decode_adapter(["00", "0", "0", "0", "0", "1"])["software"],
                          "software state is unknown unless the backend reports it")
        with self.assertRaisesRegex(runtime_profile.ProfileError, "one R5BACKEND"):
            runtime_profile.parse_observations(scenario, RHI_OUTPUT.replace("R5BACKEND,3\n", ""))
        with self.assertRaisesRegex(runtime_profile.ProfileError, "malformed"):
            runtime_profile.decode_adapter(["zz", "0", "0", "0", "1", "0"])

    def test_system_gpu_reports(self):
        self.assertEqual(runtime_profile.parse_vulkan_summary(VULKAN_SUMMARY), [
            {"apiVersion": "1.3.280", "vendorID": "0x10de", "deviceID": "0x2684",
             "deviceType": "PHYSICAL_DEVICE_TYPE_DISCRETE_GPU", "deviceName": "NVIDIA GeForce RTX 4090",
             "driverName": "NVIDIA", "driverInfo": "550.54.14"},
            {"deviceType": "PHYSICAL_DEVICE_TYPE_CPU", "deviceName": "llvmpipe (LLVM 17.0.6, 256 bits)"},
        ])
        profile = json.dumps({"SPDisplaysDataType": [{
            "_name": "Apple M5", "sppci_model": "Apple M5", "sppci_cores": "8", "spdisplays_vendor": "sppci_vendor_Apple",
            "spdisplays_mtlgpufamilysupport": "spdisplays_metal4",
            "spdisplays_ndrvs": [{"_spdisplays_display-serial-number": "private"}]}]})
        self.assertEqual(runtime_profile.parse_display_profile(profile), [
            {"model": "Apple M5", "vendor": "sppci_vendor_Apple", "cores": "8", "metal_family": "spdisplays_metal4"}])
        self.assertEqual(runtime_profile.parse_display_profile("not json"), [])
        failing = FakeRunner([""], returncode=1)
        self.assertEqual(runtime_profile.observe_system_gpus("Darwin", failing), [])

    def test_perfetto_scopes_and_frames(self):
        metrics = runtime_profile.parse_perfetto_scopes(trace_document())
        self.assertEqual(metrics["scope=Tick/count"], 3.0)
        self.assertEqual(metrics["scope=Tick/median_ns"], 4000.0)
        self.assertEqual(metrics["scope=Motion/median_ns"], 1000.0)
        self.assertEqual(metrics["frames/count"], 3.0)
        self.assertEqual(metrics["frames/interval_median_ns"], 10000.0)
        complete = runtime_profile.parse_perfetto_scopes([{"pid": 1, "tid": 0, "ts": 0, "ph": "X", "dur": 2, "name": "Job"}])
        self.assertEqual(complete["scope=Job/median_ns"], 2000.0)

    def test_perfetto_rejects_misnested_or_empty_traces(self):
        document = trace_document()
        document["traceEvents"][2], document["traceEvents"][3] = document["traceEvents"][3], document["traceEvents"][2]
        with self.assertRaisesRegex(runtime_profile.ProfileError, "innermost"):
            runtime_profile.parse_perfetto_scopes(document)
        with self.assertRaisesRegex(runtime_profile.ProfileError, "no scopes"):
            runtime_profile.parse_perfetto_scopes({"traceEvents": []})


class FakeRunner:
    """Stands in for subprocess.run; writes requested traces and returns scripted outputs."""

    def __init__(self, outputs, returncode=0, trace=None, timeout=False):
        self.outputs = list(outputs)
        self.returncode = returncode
        self.trace = trace
        self.timeout = timeout
        self.commands = []
        self.environments = []

    def __call__(self, command, **options):
        self.commands.append(command)
        self.environments.append(options.get("env"))
        if self.timeout:
            raise subprocess.TimeoutExpired(command, options["timeout"], output="partial")
        if "--trace-output" in command and self.trace is not None:
            Path(command[command.index("--trace-output") + 1]).write_text(json.dumps(self.trace))
        output = self.outputs.pop(0) if len(self.outputs) > 1 else self.outputs[0]
        return subprocess.CompletedProcess(command, self.returncode, stdout=output)


class Execution(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.build = self.directory / "build"
        (self.build / "bin").mkdir(parents=True)
        (self.build / "bin/sample").write_text("")
        self.output = self.directory / "report"

    def scenario(self, **overrides):
        manifest = write_manifest(self.directory, scenario_entry(**overrides))
        return runtime_profile.load_manifest(manifest)[0]

    def test_measured_repetitions_are_summarized(self):
        scenario = self.scenario(warmup=1)
        runner = FakeRunner(["workers=1 median_ns=10\n", "workers=1 median_ns=30\n", "workers=1 median_ns=20\n",
                             "workers=1 median_ns=40\n"])
        result = runtime_profile.run_scenario(scenario, self.build, self.output, runner=runner)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(len(runner.commands), 4)
        metric = result["metrics"]["workers=1/median_ns"]
        self.assertEqual(metric["samples"], [30.0, 20.0, 40.0], "warmup output must not be measured")
        self.assertEqual((metric["median"], metric["min"], metric["max"]), (30.0, 20.0, 40.0))
        self.assertIn("process/wall_ns", result["metrics"])
        self.assertEqual(len(result["logs"]), 4)

    def test_windows_scenarios_resolve_the_native_executable_suffix(self):
        scenario = self.scenario(warmup=0, repetitions=1)
        (self.build / "bin/sample.exe").touch()
        runner = FakeRunner(["workers=1 median_ns=10\n"])
        with patch.object(runtime_profile, "os", SimpleNamespace(name="nt", environ={})):
            result = runtime_profile.run_scenario(scenario, self.build, self.output, runner=runner)
        self.assertEqual(result["status"], "passed", result.get("reason"))
        self.assertTrue(runner.commands[0][0].endswith("sample.exe"))

    def test_trace_scenarios_pass_an_output_path_and_keep_one_trace(self):
        scenario = self.scenario(trace=True, parser={"kind": "perfetto-scopes"}, arguments=["--headless"])
        runner = FakeRunner([""], trace=trace_document())
        result = runtime_profile.run_scenario(scenario, self.build, self.output, runner=runner)
        self.assertEqual(result["status"], "passed", result.get("reason"))
        self.assertEqual(runner.commands[0][1:3], ["--headless", "--trace-output"])
        self.assertEqual(result["trace"], "traces/sample.json")
        self.assertTrue((self.output / "traces/sample.json").is_file())

    def test_failures_are_reported_not_raised(self):
        cases = {
            "exited with 3": (self.scenario(), FakeRunner(["workers=1 median_ns=1\n"], returncode=3)),
            "exceeded 10s": (self.scenario(), FakeRunner([""], timeout=True)),
            "no metrics parsed": (self.scenario(), FakeRunner(["unrelated\n"])),
            "requested trace": (self.scenario(trace=True, parser={"kind": "perfetto-scopes"}), FakeRunner([""])),
            "different metric set": (self.scenario(), FakeRunner(["workers=1 median_ns=1\n",
                                                                  "workers=2 median_ns=1\n"])),
        }
        for reason, (scenario, runner) in cases.items():
            with self.subTest(reason=reason):
                result = runtime_profile.run_scenario(scenario, self.build, self.output, runner=runner)
                self.assertEqual(result["status"], "failed")
                self.assertIn(reason, result["reason"])
        (self.build / "bin/sample").unlink()
        result = runtime_profile.run_scenario(self.scenario(), self.build, self.output, runner=FakeRunner([""]))
        self.assertIn("was not built", result["reason"])

    def test_tagged_samples_keep_a_valid_trace_without_unstable_scope_sets(self):
        scenario = self.scenario(trace=True, parser=rhi_parser())
        runner = FakeRunner([RHI_OUTPUT], trace=trace_document())
        result = runtime_profile.run_scenario(scenario, self.build, self.output, runner=runner)
        self.assertEqual(result["status"], "passed", result.get("reason"))
        self.assertEqual(result["trace"], "traces/sample.json")
        self.assertFalse(any(name.startswith("scope=") for name in result["metrics"]))
        for trace in (None, {"traceEvents": []}):
            with self.subTest(trace=trace):
                result = runtime_profile.run_scenario(scenario, self.build, self.output,
                                                     runner=FakeRunner([RHI_OUTPUT], trace=trace))
                self.assertEqual(result["status"], "failed")

    def test_environment_is_declared_and_controlled_variables_do_not_leak(self):
        scenario = self.scenario(environment={"LUDUS_RHI_BENCHMARK": "1"})
        runner = FakeRunner(["workers=1 median_ns=10\n"])
        previous = dict(os.environ)
        os.environ["LUDUS_RHI_GPU_TIMING"] = "1"
        os.environ["LUDUS_RHI_BENCHMARK"] = "inherited"
        try:
            result = runtime_profile.run_scenario(scenario, self.build, self.output, runner=runner,
                                                  controlled_environment=frozenset({"LUDUS_RHI_GPU_TIMING"}))
        finally:
            os.environ.clear()
            os.environ.update(previous)
        self.assertEqual(result["status"], "passed", result.get("reason"))
        self.assertEqual(result["environment"], {"LUDUS_RHI_BENCHMARK": "1"})
        environment = runner.environments[0]
        self.assertEqual(environment["LUDUS_RHI_BENCHMARK"], "1")
        self.assertNotIn("LUDUS_RHI_GPU_TIMING", environment, "a timing switch must not reach the control run")

    def test_gpu_scenarios_record_the_engine_adapter_and_reject_software_adapters(self):
        scenario = self.scenario(requires=["gpu"], parser=rhi_parser())
        result = runtime_profile.run_scenario(scenario, self.build, self.output, runner=FakeRunner([RHI_OUTPUT]))
        self.assertEqual(result["status"], "passed", result.get("reason"))
        self.assertEqual(result["adapter"]["name"], "Apple M5")
        self.assertEqual(result["observations"]["R5BACKEND"], ["3"])
        self.assertIn("R5/layers=1/merge=1/setup_cpu_median_ns", result["metrics"])
        software = RHI_OUTPUT.replace(ADAPTER_LINE, SOFTWARE_ADAPTER_LINE)
        result = runtime_profile.run_scenario(scenario, self.build, self.output, runner=FakeRunner([software]))
        self.assertEqual(result["status"], "failed")
        self.assertIn("software adapter", result["reason"])
        changed = FakeRunner([RHI_OUTPUT, RHI_OUTPUT.replace("R5BACKEND,3", "R5BACKEND,0")])
        result = runtime_profile.run_scenario(scenario, self.build, self.output, runner=changed)
        self.assertIn("different adapter, profile or backend", result["reason"])

    def test_report_records_declared_and_observed_device_identity(self):
        scenario = self.scenario(requires=["gpu"], parser=rhi_parser())
        system = [{"model": "Apple M5", "cores": "8"}]
        report = runtime_profile.run_scenarios(
            ROOT, "test-profile", self.build, self.output, [(scenario, None)], {"gpu", "gpu-apple-m5"},
            compiler="clang", device="Apple M5 (8 GPU cores), Metal, macOS 26.6", runner=FakeRunner([RHI_OUTPUT]),
            system_gpus=lambda: system)
        self.assertEqual(report["device"]["declared"], "Apple M5 (8 GPU cores), Metal, macOS 26.6")
        self.assertEqual(report["device"]["gpu_classes"], ["gpu-apple-m5"])
        self.assertEqual(report["device"]["system_reported"], system)
        self.assertEqual([adapter["name"] for adapter in report["device"]["engine_adapters"]], ["Apple M5"])
        summary = (self.output / "summary.md").read_text()
        self.assertIn("Device: Apple M5 (8 GPU cores), Metal, macOS 26.6 (`gpu-apple-m5`)", summary)
        self.assertIn("Engine-selected adapter: Apple M5.", summary)

    def test_reports_and_refuses_foreign_output_directories(self):
        scenario = self.scenario()
        report = runtime_profile.run_scenarios(
            ROOT, "test-profile", self.build, self.output, [(scenario, None)], set(), compiler="clang",
            runner=FakeRunner(["workers=1 median_ns=5\n"]), system_gpus=list)
        self.assertEqual(report["timing_policy"], "report-only")
        self.assertIsNone(report["device"]["declared"])
        written = json.loads((self.output / "report.json").read_text())
        self.assertEqual(written["scenarios"][0]["status"], "passed")
        self.assertIn("`sample` | passed", (self.output / "summary.md").read_text())
        self.assertFalse((self.output / "work").exists())
        foreign = self.directory / "foreign"
        foreign.mkdir()
        (foreign / "keep.txt").write_text("user data")
        with self.assertRaisesRegex(runtime_profile.ProfileError, "not written by profile-engine"):
            runtime_profile.run_scenarios(ROOT, "test-profile", self.build, foreign, [(scenario, None)], set(),
                                          compiler="clang", runner=FakeRunner(["workers=1 median_ns=5\n"]),
                                          system_gpus=list)
        self.assertTrue((foreign / "keep.txt").is_file())


class Comparison(unittest.TestCase):
    @staticmethod
    def report(metrics):
        return {"schema": 1, "host": {}, "scenarios": [{"id": "sample", "status": "passed", "metrics": {
            name: runtime_profile.summarize(samples) for name, samples in metrics.items()}}]}

    def test_notable_requires_threshold_and_separated_ranges(self):
        baseline = self.report({"a_ns": [100, 101, 102], "b_ns": [100, 130, 160], "c_ns": [100, 101, 102]})
        current = self.report({"a_ns": [150, 151, 152], "b_ns": [140, 150, 160], "c_ns": [104, 105, 106]})
        deltas = runtime_profile.compare(current, baseline)["sample"]
        self.assertTrue(deltas["a_ns"]["notable"])
        self.assertFalse(deltas["b_ns"]["notable"], "overlapping repetition ranges are noise")
        self.assertFalse(deltas["c_ns"]["notable"], "below the relative notice threshold")

    def test_legacy_percentiles_are_not_compared_to_corrected_percentiles(self):
        metrics = {"scope=Tick/p95_ns": [100], "EDVIEW/cpu_p95_ns": [100], "EDVIEW/cpu_median_ns": [100]}
        baseline = self.report(metrics)
        current = self.report(metrics)
        current["percentile_method"] = runtime_profile.PERCENTILE_METHOD
        self.assertEqual(set(runtime_profile.compare(current, baseline)["sample"]), {"EDVIEW/cpu_median_ns"})
        baseline["percentile_method"] = runtime_profile.PERCENTILE_METHOD
        self.assertEqual(set(runtime_profile.compare(current, baseline)["sample"]), set(metrics))

    def test_trace_metrics_need_a_change_above_clock_resolution(self):
        baseline = self.report({"scope=Tick/median_ns": [208, 209, 209], "scope=Frame/median_ns": [1000, 1000, 1000]})
        current = self.report({"scope=Tick/median_ns": [250, 250, 250], "scope=Frame/median_ns": [2000, 2000, 2000]})
        deltas = runtime_profile.compare(current, baseline)["sample"]
        self.assertFalse(deltas["scope=Tick/median_ns"]["notable"])
        self.assertTrue(deltas["scope=Frame/median_ns"]["notable"])

    def test_baselines_from_another_device_are_marked(self):
        report = {"host": {"system": "Darwin"}, "device": {"declared": "Apple M5"}}
        self.assertTrue(runtime_profile.same_host(report, copy.deepcopy(report)))
        self.assertFalse(runtime_profile.same_host(report, {"host": {"system": "Darwin"}, "device": {"declared": "M4"}}))
        hosted = {"host": {"system": "Linux"}, "device": {"declared": None}}
        self.assertTrue(runtime_profile.same_host(hosted, {"host": {"system": "Linux"}}),
                        "reports written before device identity count as undeclared")

    def test_failed_scenarios_are_not_compared(self):
        baseline = self.report({"a_ns": [100]})
        current = copy.deepcopy(baseline)
        current["scenarios"][0]["status"] = "failed"
        self.assertEqual(runtime_profile.compare(current, baseline), {})


if __name__ == "__main__":
    unittest.main()
