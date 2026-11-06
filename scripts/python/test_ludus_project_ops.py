"""Unit tests for project creation, templates, resolution and migration.

    python3 -m unittest test_ludus_project_ops -v
"""

from __future__ import annotations

import json
import os
import tarfile
import tempfile
import threading
import unittest
from pathlib import Path

from ludus_tools import create, descriptor, lockfile, resolve, templates
from ludus_tools.errors import ToolingError
from ludus_tools.identity import load_prefix_manifest
from ludus_tools.sdkstore import SdkStore

from test_ludus_tools import _make_sdk_tar, _manifest_json, _write_sdk_prefix


class Templates(unittest.TestCase):
    def test_render_minimal(self) -> None:
        tmpl = templates.get_template("minimal")
        files = templates.render_files(
            tmpl,
            templates.ProjectInputs(
                name="My Game", target="my_game", engine_version="0.1.0",
                components=["FoundationBase", "GraphicsRhi"],
            ),
        )
        by = {f.relpath: f.content for f in files}
        self.assertIn("CMakeLists.txt", by)
        self.assertIn("project(my_game", by["CMakeLists.txt"])
        self.assertIn("Ludus::FoundationBase Ludus::GraphicsRhi", by["CMakeLists.txt"])
        self.assertIn("linux-clang-release", by["CMakePresets.json"])
        self.assertNotIn("{{", by["CMakeLists.txt"])

    def test_minimal_template_launches_debugs_and_installs_from_ides(self) -> None:
        files = templates.render_files(
            templates.get_template("minimal"),
            templates.ProjectInputs(name="My Game", target="my_game", engine_version="0.1.0", components=[]),
        )
        by = {f.relpath: f.content for f in files}
        cmake = by["CMakeLists.txt"]
        self.assertIn("set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT my_game)", cmake)
        self.assertIn('VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"', cmake)
        self.assertIn('install(TARGETS my_game RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" BUNDLE DESTINATION .)', cmake)
        self.assertIn('ludus_install_d3d12_agility_runtime(my_game "${CMAKE_INSTALL_BINDIR}")', cmake)
        # The tracked launch configuration holds no machine path: CMake Tools
        # resolves the selected preset's executable.
        launch = json.loads(by[".vscode/launch.json"])
        (configuration,) = launch["configurations"]
        self.assertEqual(configuration["type"], "cppvsdbg")
        self.assertEqual(configuration["program"], "${command:cmake.launchTargetPath}")
        self.assertEqual(configuration["cwd"], "${workspaceFolder}")
        self.assertIn("ms-vscode.cmake-tools", json.loads(by[".vscode/extensions.json"])["recommendations"])
        self.assertNotIn("/.vscode/launch.json", by[".gitignore"])

    def test_unknown_placeholder_rejected(self) -> None:
        tmpl = templates.Template("x", 1, [templates.TemplateFile("f.txt", "{{ NOPE }}")])
        with self.assertRaises(ToolingError):
            templates.render_files(tmpl, templates.ProjectInputs("n", "t", "1", []))

    def test_stage_rejects_existing_destination(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            dest = Path(td) / "proj"
            dest.mkdir()
            with self.assertRaises(ToolingError) as ctx:
                templates.stage_project([templates.TemplateFile("a.txt", "x")], dest)
            self.assertEqual(ctx.exception.code, "DestinationExists")

    def test_stage_rejects_symlink_destination(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            real = Path(td) / "real"
            real.mkdir()
            link = Path(td) / "proj"
            os.symlink(real, link)
            with self.assertRaises(ToolingError) as ctx:
                templates.stage_project([templates.TemplateFile("a.txt", "x")], link)
            self.assertEqual(ctx.exception.code, "DestinationExists")

    def test_publish_no_replace_race(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            dest = Path(td) / "proj"
            staged = templates.stage_project([templates.TemplateFile("a.txt", "x")], dest)
            # Destination appears between staging and publish.
            dest.mkdir()
            (dest / "preexisting").write_text("keep me")
            with self.assertRaises(ToolingError) as ctx:
                templates.publish_project(staged)
            self.assertEqual(ctx.exception.code, "DestinationExists")
            # The unrelated file must be intact and staging cleaned up.
            self.assertEqual((dest / "preexisting").read_text(), "keep me")
            self.assertFalse(staged.staging.exists())

    def test_publish_rejects_empty_dir_racing_in(self) -> None:
        # Regression: os.rename silently replaces an EMPTY dir on Linux, so an
        # empty directory appearing between the pre-check and the rename must not
        # be clobbered. publish_project claims the destination with exclusive
        # os.mkdir, so an empty dir already present is rejected, not overwritten.
        with tempfile.TemporaryDirectory() as td:
            dest = Path(td) / "proj"
            staged = templates.stage_project([templates.TemplateFile("a.txt", "x")], dest)
            dest.mkdir()  # empty dir races in
            with self.assertRaises(ToolingError) as ctx:
                templates.publish_project(staged)
            self.assertEqual(ctx.exception.code, "DestinationExists")
            # Nothing from the staged project leaked into the pre-existing dir.
            self.assertEqual(list(dest.iterdir()), [])
            self.assertFalse(staged.staging.exists())


class CreateProject(unittest.TestCase):
    def test_create_minimal_unresolved(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            dest = Path(td) / "MyGame"
            result = create.create_project(
                dest, name="MyGame", template_id="minimal", engine_version="0.1.0",
                components=["FoundationBase"],
            )
            self.assertTrue((dest / "ludus.project.json").is_file())
            self.assertTrue((dest / "ludus.lock.json").is_file())
            self.assertTrue((dest / "CMakeLists.txt").is_file())
            self.assertTrue((dest / "src" / "main.cpp").is_file())
            self.assertTrue((dest / ".gitignore").is_file())
            d = descriptor.parse_descriptor_file(dest / "ludus.project.json")
            self.assertEqual(d.version, 2)
            self.assertEqual(d.engine.version, "0.1.0")
            self.assertEqual(d.template.id, "minimal")
            lock = lockfile.parse_lock_file(dest / "ludus.lock.json")
            self.assertFalse(lock.resolved)
            lockfile.validate_descriptor_lock_agreement(d.engine.version, lock)

    def test_create_rejects_existing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            dest = Path(td) / "MyGame"
            dest.mkdir()
            with self.assertRaises(ToolingError) as ctx:
                create.create_project(dest, name="MyGame", template_id="minimal", engine_version="0.1.0")
            self.assertEqual(ctx.exception.code, "DestinationExists")

    def test_create_cancelled_leaves_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            dest = Path(td) / "MyGame"

            calls = {"n": 0}

            def cancel_check():
                calls["n"] += 1
                if calls["n"] >= 2:
                    raise ToolingError("InstallCancelled", "cancelled")

            with self.assertRaises(ToolingError):
                create.create_project(
                    dest, name="MyGame", template_id="minimal", engine_version="0.1.0",
                    cancel_check=cancel_check,
                )
            self.assertFalse(dest.exists())
            # No leftover staging dirs either.
            leftovers = [p for p in Path(td).iterdir() if p.name.startswith(".MyGame")]
            self.assertEqual(leftovers, [])

    def test_create_with_local_sdk_writes_override(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            prefix = Path(td) / "sdk"
            _write_sdk_prefix(prefix, _manifest_json(engine_version="0.1.0"))
            dest = Path(td) / "MyGame"
            create.create_project(
                dest, name="MyGame", template_id="minimal", engine_version="",
                local_sdk_prefix=prefix,
            )
            local = lockfile.parse_local_settings_file(dest / ".ludus" / "local.json")
            self.assertEqual(len(local.overrides), 1)
            self.assertEqual(local.overrides[0].prefix, str(prefix.resolve()))
            # committed lock stays unresolved (no fabricated hashes)
            lock = lockfile.parse_lock_file(dest / "ludus.lock.json")
            self.assertFalse(lock.resolved)
            self.assertEqual(lock.engine_version, "0.1.0")

    def test_unicode_and_spaces(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            dest = Path(td) / "My Game 日本語"
            create.create_project(dest, name="My Game 日本語", template_id="minimal", engine_version="0.1.0")
            self.assertTrue((dest / "ludus.project.json").is_file())
            d = descriptor.parse_descriptor_file(dest / "ludus.project.json")
            self.assertEqual(d.name, "My Game 日本語")
            # derived target is a valid identifier
            self.assertTrue(descriptor.TARGET_NAME_RE.fullmatch(d.target))


class Migration(unittest.TestCase):
    def _write_v1(self, project_dir: Path, provider="cmake") -> None:
        project_dir.mkdir(parents=True, exist_ok=True)
        v1 = {
            "version": 1, "name": "Legacy", "provider": provider, "source_dir": ".",
            "preset": "linux-clang-development", "target": "legacy_app",
            "run": {"cwd": ".", "args": ["--x"]},
        }
        (project_dir / "ludus.project.json").write_text(json.dumps(v1, indent=2))

    def test_migrate_preserves_and_locks(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            proj = Path(td) / "legacy"
            self._write_v1(proj)
            result = create.migrate_v1_to_v2(proj, engine_version="0.1.0")
            d = descriptor.parse_descriptor_file(proj / "ludus.project.json")
            self.assertEqual(d.version, 2)
            self.assertEqual(d.target, "legacy_app")
            self.assertEqual(d.run_args, ["--x"])
            self.assertEqual(d.engine.version, "0.1.0")
            self.assertTrue((proj / "ludus.lock.json").is_file())

    def test_migrate_requires_engine(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            proj = Path(td) / "legacy"
            self._write_v1(proj)
            with self.assertRaises(ToolingError) as ctx:
                create.migrate_v1_to_v2(proj, engine_version="")
            self.assertEqual(ctx.exception.code, "MigrationFailed")

    def test_migrate_ludus_provider_refused(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            proj = Path(td) / "legacy"
            self._write_v1(proj, provider="ludus")
            with self.assertRaises(ToolingError):
                create.migrate_v1_to_v2(proj, engine_version="0.1.0")

    def test_recover_partial_migration_discards_temporaries(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            proj = Path(td) / "legacy"
            proj.mkdir()
            (proj / "ludus.project.json.migrating").write_text("partial")
            (proj / "ludus.lock.json.migrating").write_text("partial")
            actions = create.recover_partial_migration(proj)
            # The .migrating temporaries are discarded (order-independent).
            self.assertTrue(any("ludus.lock.json.migrating" in a for a in actions))
            self.assertTrue(any("ludus.project.json.migrating" in a for a in actions))
            self.assertFalse((proj / "ludus.project.json.migrating").exists())
            self.assertFalse((proj / "ludus.lock.json.migrating").exists())

    def test_recover_rolls_back_interrupted_commit(self) -> None:
        # Simulate a crash AFTER the descriptor flipped to v2 but BEFORE the lock
        # landed: a backup of the original v1 descriptor exists, the descriptor is
        # v2, and no lock is present. Recovery must roll the descriptor back to v1.
        with tempfile.TemporaryDirectory() as td:
            proj = Path(td) / "legacy"
            proj.mkdir()
            v1 = {
                "version": 1, "name": "Legacy", "provider": "cmake", "source_dir": ".",
                "preset": "linux-clang-development", "target": "legacy_app",
                "run": {"cwd": ".", "args": []},
            }
            v2 = {**v1, "version": 2, "engine": {"version": "0.1.0", "components": [], "features": []}}
            # On-disk mid-crash state:
            (proj / "ludus.project.json").write_text(json.dumps(v2))              # flipped to v2
            (proj / "ludus.project.json.premigrate").write_text(json.dumps(v1))   # backup of v1
            # (no ludus.lock.json -> commit was interrupted)
            actions = create.recover_partial_migration(proj)
            restored = descriptor.parse_descriptor_file(proj / "ludus.project.json")
            self.assertEqual(restored.version, 1, actions)
            self.assertFalse((proj / "ludus.project.json.premigrate").exists())

    def test_recover_rolls_forward_completed_commit(self) -> None:
        # Crash AFTER both files landed but before the backup was cleared:
        # descriptor is v2 and the lock is present -> roll forward (keep v2).
        with tempfile.TemporaryDirectory() as td:
            proj = Path(td) / "legacy"
            proj.mkdir()
            v2 = {
                "version": 2, "name": "Legacy", "provider": "cmake", "source_dir": ".",
                "preset": "linux-clang-development", "target": "legacy_app",
                "run": {"cwd": ".", "args": []}, "engine": {"version": "0.1.0"},
            }
            (proj / "ludus.project.json").write_text(json.dumps(v2))
            (proj / "ludus.lock.json").write_text(lockfile.unresolved_lock("0.1.0", 1).serialize().decode())
            (proj / "ludus.project.json.premigrate").write_text(json.dumps({**v2, "version": 1}))
            create.recover_partial_migration(proj)
            kept = descriptor.parse_descriptor_file(proj / "ludus.project.json")
            self.assertEqual(kept.version, 2)
            self.assertFalse((proj / "ludus.project.json.premigrate").exists())


class Resolution(unittest.TestCase):
    def setUp(self) -> None:
        self.td = tempfile.TemporaryDirectory()
        self.store = SdkStore(Path(self.td.name) / "store")

    def tearDown(self) -> None:
        self.td.cleanup()

    def _install(self, **over):
        tar = Path(self.td.name) / "sdk.tar.gz"
        _make_sdk_tar(tar, _manifest_json(**over))
        return self.store.install_archive(tar)

    def test_cli_override_precedence(self) -> None:
        installed = self._install()
        lock = lockfile.unresolved_lock("0.1.0", 1)
        res = resolve.resolve_sdk(
            target="x86_64-linux-gnu", flavor="Development", lock=lock,
            local_settings=lockfile.LocalSettings(), store=self.store,
            cli_prefix=installed.prefix,
        )
        self.assertEqual(res.source, resolve.SOURCE_OPTION)
        self.assertTrue(res.is_override)

    def test_unresolved_lock_without_override(self) -> None:
        lock = lockfile.unresolved_lock("0.1.0", 1)
        with self.assertRaises(ToolingError) as ctx:
            resolve.resolve_sdk(
                target="x86_64-linux-gnu", flavor="Development", lock=lock,
                local_settings=lockfile.LocalSettings(), store=self.store,
            )
        self.assertEqual(ctx.exception.code, "UnresolvedLock")

    def test_saved_override(self) -> None:
        installed = self._install()
        settings = lockfile.LocalSettings()
        settings.set_override("x86_64-linux-gnu", "Development", str(installed.prefix))
        res = resolve.resolve_sdk(
            target="x86_64-linux-gnu", flavor="Development",
            lock=lockfile.unresolved_lock("0.1.0", 1),
            local_settings=settings, store=self.store,
        )
        self.assertEqual(res.source, resolve.SOURCE_LOCAL)

    def test_flavor_mismatch_rejected(self) -> None:
        installed = self._install(build_flavor="Development")
        with self.assertRaises(ToolingError) as ctx:
            resolve.resolve_sdk(
                target="x86_64-linux-gnu", flavor="Release",
                lock=lockfile.unresolved_lock("0.1.0", 1),
                local_settings=lockfile.LocalSettings(), store=self.store,
                cli_prefix=installed.prefix,
            )
        self.assertEqual(ctx.exception.code, "SdkIncompatible")

    def test_resolved_lock_uses_store(self) -> None:
        installed = self._install()
        lock = lockfile.Lock(
            resolved=True, engine_version="0.1.0", engine_revision="deadbeef",
            template_version=1,
            packages=[lockfile.PackageEntry("x86_64-linux-gnu", "Development", installed.identity.sdk_variant, installed.digest, 1)],
        )
        res = resolve.resolve_sdk(
            target="x86_64-linux-gnu", flavor="Development", lock=lock,
            local_settings=lockfile.LocalSettings(), store=self.store,
        )
        self.assertEqual(res.source, resolve.SOURCE_LOCKED)
        self.assertFalse(res.is_override)

    def test_stamp_detects_same_size_same_second_rebuild(self) -> None:
        # Regression: compute_stamp must hash library CONTENT, so an in-place
        # rebuild that lands a same-size .a within the same integer-second mtime
        # (the exact mutable-local-prefix case) still changes the stamp.
        installed = self._install()
        lib = installed.prefix / "lib" / "libludus_foundation_base.a"
        lib.write_bytes(b"A" * 128)
        st = lib.stat()
        s1 = resolve.compute_stamp(installed.prefix)
        lib.write_bytes(b"B" * 128)  # same size
        os.utime(lib, (st.st_mtime, st.st_mtime))  # restore mtime (same second)
        s2 = resolve.compute_stamp(installed.prefix)
        self.assertNotEqual(s1, s2, "content change with identical size+mtime must change the stamp")

    def test_stamp_change_detection(self) -> None:
        installed = self._install()
        with tempfile.TemporaryDirectory() as bd:
            build_dir = Path(bd)
            res = resolve.resolve_sdk(
                target="x86_64-linux-gnu", flavor="Development",
                lock=lockfile.unresolved_lock("0.1.0", 1),
                local_settings=lockfile.LocalSettings(), store=self.store,
                cli_prefix=installed.prefix,
            )
            self.assertTrue(resolve.needs_reconfigure(build_dir, res))
            resolve.write_stamp(build_dir, res)
            self.assertFalse(resolve.needs_reconfigure(build_dir, res))
            # Mutate the installed prefix (simulate a local engine rebuild).
            (installed.prefix / "lib" / "libludus_foundation_base.a").write_bytes(b"\x00ar-changed")
            res2 = resolve.resolve_sdk(
                target="x86_64-linux-gnu", flavor="Development",
                lock=lockfile.unresolved_lock("0.1.0", 1),
                local_settings=lockfile.LocalSettings(), store=self.store,
                cli_prefix=installed.prefix,
            )
            self.assertTrue(resolve.needs_reconfigure(build_dir, res2))


if __name__ == "__main__":
    unittest.main()
