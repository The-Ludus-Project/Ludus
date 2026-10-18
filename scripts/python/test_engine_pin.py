"""Engine pin drift: detection, automatic refresh, validated selection and init refresh.

Covers ludus_tools.engine_pin and its callers (project setup, SDK resolution,
the ``ludus project engine`` CLI, the Editor adapter frame and init's refresh of
this checkout's own installs). No engine is built; preparation is faked.
"""
import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from ludus_tools import engine_pin
from ludus_tools import project_setup as setup
from ludus_tools.cli import main as cli_main
from ludus_tools.create import create_project
from ludus_tools.errors import ToolingError
from ludus_tools.lockfile import parse_local_settings_file
from test_ludus_tools import _manifest_json, _write_sdk_prefix

OLD = "0361c59" + "1" * 33
NEW = "5c74c12" + "2" * 33
PROFILE = "linux-clang-development"


def write_install(prefix: Path, revision: str, **fields) -> Path:
    return _write_sdk_prefix(prefix, _manifest_json(source_revision=revision, **fields))


def write_build_tree(folder: Path) -> Path:
    (folder / "cmake").mkdir(parents=True)
    (folder / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n")
    (folder / "cmake/LudusSdkManifest.json").write_text(json.dumps(_manifest_json()))
    return folder


class EnginePinTestCase(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.tools = self.root / "tools"
        (self.tools / "out/install").mkdir(parents=True)
        (self.tools / "CMakeLists.txt").write_text("project(Ludus VERSION 0.1.0 LANGUAGES CXX)\n")
        self.owned = write_install(self.tools / "out/install" / f"{PROFILE}-0123456789abcdef", OLD)
        self.external = write_install(self.root / "external-sdk", OLD)

    def make_project(self, prefix: Path, name: str = "Game") -> Path:
        project = self.root / name
        create_project(project, name=name, template_id="minimal", engine_version="",
                       local_sdk_prefix=prefix, preset=PROFILE)
        return project

    def write_override(self, project: Path, prefix: str) -> None:
        (project / ".ludus/local.json").write_text(json.dumps({
            "schema_version": 1,
            "sdk_overrides": [{"target": "x86_64-linux-gnu", "flavor": "Development", "prefix": prefix}],
        }))


class RevisionAndPrefixTests(EnginePinTestCase):
    def test_revisions_match_by_prefix_and_unknown_never_matches(self):
        self.assertTrue(engine_pin.are_revisions_equal(NEW, NEW[:7]))
        self.assertTrue(engine_pin.are_revisions_equal(NEW.upper(), NEW))
        self.assertFalse(engine_pin.are_revisions_equal(NEW, OLD))
        self.assertFalse(engine_pin.are_revisions_equal(NEW, NEW[:4]))
        for unknown in ("", "unknown", None):
            self.assertFalse(engine_pin.are_revisions_equal(unknown, unknown))

    def test_build_folder_and_empty_folder_are_named(self):
        build = write_build_tree(self.tools / "out/build" / PROFILE)
        self.assertIn("CMake build folder", engine_pin.describe_prefix_problem(build))
        (self.root / "empty").mkdir()
        self.assertIn("LudusSdkManifest.json is missing", engine_pin.describe_prefix_problem(self.root / "empty"))
        self.assertIn("does not exist", engine_pin.describe_prefix_problem(self.root / "absent"))
        self.assertEqual("", engine_pin.describe_prefix_problem(self.owned))
        with self.assertRaises(ToolingError) as raised:
            engine_pin.validate_install_prefix(build)
        self.assertEqual(engine_pin.NOT_AN_INSTALL, raised.exception.code)

    def test_ownership_is_limited_to_direct_children_of_out_install(self):
        self.assertTrue(engine_pin.is_editor_owned(self.owned, self.tools))
        self.assertFalse(engine_pin.is_editor_owned(self.external, self.tools))
        self.assertFalse(engine_pin.is_editor_owned(self.tools / "out/install", self.tools))


class DiagnosisTests(EnginePinTestCase):
    def diagnose(self, project: Path, revision: str = NEW):
        return engine_pin.diagnose_engine_pin(project, tooling_root=self.tools, editor_revision=revision)

    def test_current_stale_and_unknown_pins(self):
        project = self.make_project(self.owned)
        before = {p: p.read_bytes() for p in project.rglob("*") if p.is_file()}
        self.assertEqual(engine_pin.PIN_CURRENT, self.diagnose(project, OLD).state)
        stale = self.diagnose(project)
        self.assertEqual(engine_pin.PIN_STALE, stale.state)
        self.assertTrue(stale.is_editor_owned)
        self.assertIn("built from 0361c59", stale.message)
        self.assertIn("this editor runs 5c74c12", stale.message)
        self.assertTrue(engine_pin.should_refresh_automatically(stale))
        self.assertEqual(engine_pin.PIN_UNKNOWN, self.diagnose(project, "").state)
        # Diagnosis is read-only.
        self.assertEqual(before, {p: p.read_bytes() for p in project.rglob("*") if p.is_file()})

    def test_explicit_external_install_is_reported_but_not_refreshed(self):
        stale = self.diagnose(self.make_project(self.external))
        self.assertEqual(engine_pin.PIN_STALE, stale.state)
        self.assertFalse(stale.is_editor_owned)
        self.assertIn("left unchanged", stale.message)
        self.assertFalse(engine_pin.should_refresh_automatically(stale))

    def test_hand_edited_build_folder_override_is_invalid_with_a_fix(self):
        project = self.make_project(self.owned)
        build = write_build_tree(self.tools / "out/build" / PROFILE)
        self.write_override(project, str(build))
        pin = self.diagnose(project)
        self.assertEqual(engine_pin.PIN_INVALID, pin.state)
        self.assertIn("CMake build folder", pin.message)
        self.assertIn("Use this editor's engine", pin.message)
        # Saved resolution names the mistake instead of "no SDK manifest".
        from ludus_tools.operations import resolve_project
        from ludus_tools.sdkstore import SdkStore
        with self.assertRaisesRegex(ToolingError, "build folder, not an engine install"):
            resolve_project(project, store=SdkStore(self.root / "store"), enforce_host_toolchain=False)

    def test_no_local_selection_reports_nothing(self):
        project = self.make_project(self.owned)
        (project / ".ludus/local.json").unlink()
        self.assertIsNone(self.diagnose(project))
        self.assertIsNone(self.diagnose(self.root / "missing-project"))

    def test_legacy_alias_target_override_is_still_found(self):
        project = self.make_project(self.owned)
        (project / ".ludus/local.json").write_text(json.dumps({
            "schema_version": 1,
            "sdk_overrides": [{"target": "linux-x64", "flavor": "Development", "prefix": str(self.owned)}],
        }))
        self.assertEqual(self.owned, engine_pin.selected_override_prefix(project))

    def test_adapter_frame_is_typed_and_bounded(self):
        import editor_tool
        frames = []
        writer = editor_tool.ProtocolWriter("0000000000000001", frames.append)
        pin = self.diagnose(self.make_project(self.owned)).to_json()
        writer.engine_pin(pin)
        frame = json.loads(frames[0])
        self.assertEqual("engine_pin", frame["type"])
        self.assertEqual("stale", frame["state"])
        self.assertTrue(frame["editor_owned"])
        with self.assertRaises(editor_tool.ProtocolError):
            writer.engine_pin(dict(pin, state="drifted"))
        with self.assertRaises(editor_tool.ProtocolError):
            writer.engine_pin(dict(pin, message="m" * editor_tool.ENGINE_PIN_MAX_BYTES))


class AutomaticRepairTests(EnginePinTestCase):
    def select(self, project: Path, prepared: list):
        def prepare(prefix):
            prepared.append(prefix)
            return write_install(prefix, NEW)
        with patch("ludus_tools.engine_pin.checkout_revision", return_value=NEW), \
             patch("ludus_tools.creation_engine.source_revision", return_value=NEW), \
             contextlib.redirect_stdout(io.StringIO()):
            return setup.select_repair_sdk(project, tooling_root=self.tools, prepare=prepare)

    def test_stale_editor_install_is_rebuilt_and_selected(self):
        prepared = []
        selected = self.select(self.make_project(self.owned), prepared)
        self.assertEqual([self.tools / "out/install" / PROFILE], prepared)
        self.assertEqual(self.tools / "out/install" / PROFILE, selected)

    def test_current_editor_install_is_reused_without_preparation(self):
        write_install(self.owned, NEW)
        prepared = []
        self.assertEqual(self.owned, self.select(self.make_project(self.owned), prepared))
        self.assertEqual([], prepared)

    def test_explicit_external_install_keeps_precedence(self):
        prepared = []
        self.assertEqual(self.external, self.select(self.make_project(self.external), prepared))
        self.assertEqual([], prepared)

    def test_broken_editor_owned_selection_is_replaced(self):
        project = self.make_project(self.owned)
        self.write_override(project, str(write_build_tree(self.tools / "out/build" / PROFILE)))
        prepared = []
        self.assertEqual(self.tools / "out/install" / PROFILE, self.select(project, prepared))
        self.assertEqual(1, len(prepared))

    def test_repair_rejects_an_explicit_build_folder_before_writing(self):
        project = self.make_project(self.owned)
        before = {p: p.read_bytes() for p in project.rglob("*") if p.is_file()}
        with self.assertRaisesRegex(ToolingError, "build folder"):
            setup.repair_project(project, tooling_root=self.tools,
                                 sdk=write_build_tree(self.root / "build"), runner=lambda *a, **k: "")
        self.assertEqual(before, {p: p.read_bytes() for p in project.rglob("*") if p.is_file()})

    def test_check_warning_names_the_matching_action(self):
        with patch("ludus_tools.engine_pin.checkout_revision", return_value=NEW):
            owned = setup.stale_revision_warning(self.tools, OLD, self.owned)
            external = setup.stale_revision_warning(self.tools, OLD, self.external)
            current = setup.stale_revision_warning(self.tools, NEW[:7], self.owned)
        self.assertIn("Automatic repair also refreshes it", owned)
        self.assertIn("selected explicitly", external)
        self.assertEqual("", current)


class EngineCommandTests(EnginePinTestCase):
    def run_cli(self, *argv):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            code = cli_main(["--json", *argv])
        lines = [line for line in output.getvalue().splitlines() if line.startswith("{")]
        return code, json.loads(lines[-1]) if lines else {}

    def test_engine_command_rejects_build_folders_and_keeps_the_selection(self):
        project = self.make_project(self.owned)
        before = (project / ".ludus/local.json").read_bytes()
        code, _ = self.run_cli("project", "engine", str(project), "--sdk",
                               str(write_build_tree(self.root / "build")))
        self.assertNotEqual(0, code)
        self.assertEqual(before, (project / ".ludus/local.json").read_bytes())
        code, _ = self.run_cli("project", "engine", str(project), "--sdk", str(self.external))
        self.assertEqual(0, code)
        override = parse_local_settings_file(project / ".ludus/local.json").find("x86_64-linux-gnu", "Development")
        self.assertEqual(str(self.external), override.prefix)

    def test_status_and_list_report_drift_without_writing(self):
        project = self.make_project(self.owned)
        write_install(self.tools / "out/install" / PROFILE, NEW)
        write_install(self.tools / "out/install" / "linux-clang-release", NEW, build_flavor="Release")
        before = (project / ".ludus/local.json").read_bytes()
        with patch("ludus_tools.engine_pin.checkout_revision", return_value=NEW):
            code, status = self.run_cli("project", "engine", str(project), "--status", "--tools", str(self.tools))
            self.assertEqual(0, code)
            self.assertEqual("stale", status["engine_pin"]["state"])
            code, listing = self.run_cli("project", "engine", str(project), "--list", "--tools", str(self.tools))
        self.assertEqual(0, code)
        self.assertEqual({str(self.owned): False, str(self.tools / "out/install" / PROFILE): True},
                         {item["prefix"]: item["current"] for item in listing["installs"]})
        self.assertEqual(before, (project / ".ludus/local.json").read_bytes())
        code, _ = self.run_cli("project", "engine", str(project), "--status")
        self.assertNotEqual(0, code)


class InitRefreshTests(EnginePinTestCase):
    def fake_engine(self, calls: list):
        build = self.tools / "out/build" / PROFILE
        (build / "cmake").mkdir(parents=True, exist_ok=True)
        (build / "cmake/LudusSdkManifest.json").write_text(json.dumps(_manifest_json(source_revision=NEW)))
        return SimpleNamespace(
            repo_root=lambda: self.tools,
            build_dir_for_preset=lambda root, preset: root / "out/build" / preset,
            cmake_build=lambda root, preset, extra=(): calls.append(("build", preset)),
            cmake=lambda root: Path("cmake"),
            tool_env=lambda root: {},
            run=lambda argv, **kwargs: calls.append(("run", [str(a) for a in argv])),
            bundle_sdk_dependencies=lambda root, preset, prefix: calls.append(("bundle", prefix)),
        )

    def refresh(self, calls: list, **options):
        import init_editor
        args = SimpleNamespace(preset=PROFILE, **options)
        with contextlib.redirect_stdout(io.StringIO()):
            return init_editor.refresh_editor_engine_installs(args, self.fake_engine(calls))

    def test_init_reinstalls_only_stale_matching_installs_of_this_checkout(self):
        current = write_install(self.tools / "out/install" / PROFILE, NEW)
        other_flavor = write_install(self.tools / "out/install" / "linux-clang-release", OLD, build_flavor="Release")
        other_variant = write_install(self.tools / "out/install" / f"{PROFILE}-variant", OLD, sdk_variant="other")
        calls = []
        refreshed = self.refresh(calls)
        self.assertEqual([self.owned], refreshed)
        self.assertEqual(("build", PROFILE), calls[0])
        installs = [call[1] for call in calls if call[0] == "run"]
        self.assertEqual([["cmake", "--install", str(self.tools / "out/build" / PROFILE), "--prefix", str(self.owned)]],
                         installs)
        self.assertEqual([("bundle", self.owned)], [call for call in calls if call[0] == "bundle"])
        self.assertNotIn(str(self.external), json.dumps(calls, default=str))
        for untouched in (current, other_flavor, other_variant):
            self.assertNotIn(str(untouched), [install[-1] for install in installs])

    def test_init_skips_refresh_when_current_or_opted_out(self):
        calls = []
        self.assertEqual([], self.refresh(calls, skip_engine_refresh=True))
        write_install(self.owned, NEW)
        self.assertEqual([], self.refresh(calls))
        self.assertEqual([], calls)


if __name__ == "__main__":
    unittest.main()
