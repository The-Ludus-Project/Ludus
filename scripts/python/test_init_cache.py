"""Tests for the stamps that let a warm ./init.sh skip unchanged work."""
from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import engine
import init_cache

PRESET = "linux-clang-development"
PINS = {"managed": {"cmake": "3.29.6", "ninja": "1.11.1.3", "conan": "2.33.0"}}


class StampTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def test_record_then_current_until_an_input_changes(self):
        path = init_cache.stamp_path(self.root, "step")
        inputs = {"profile": "abc", "presets": [PRESET]}
        self.assertFalse(init_cache.is_current(path, inputs, environment={}))
        init_cache.record(path, inputs)
        self.assertTrue(init_cache.is_current(path, dict(inputs), environment={}))
        self.assertFalse(init_cache.is_current(path, {**inputs, "profile": "changed"}, environment={}))
        self.assertFalse(path.with_suffix(".tmp").exists())

    def test_force_and_unreadable_stamps_rerun(self):
        path = init_cache.stamp_path(self.root, "step")
        init_cache.record(path, {})
        for value in ("1", "yes", "ON"):
            self.assertFalse(init_cache.is_current(path, {}, environment={init_cache.FORCE_ENVIRONMENT: value}))
        for value in ("", "0", "false", "off"):
            self.assertTrue(init_cache.is_current(path, {}, environment={init_cache.FORCE_ENVIRONMENT: value}))
        for broken in ("not json", "[]", json.dumps({"fingerprint": "other"})):
            path.write_text(broken, encoding="utf-8")
            self.assertFalse(init_cache.is_current(path, {}, environment={}))
        init_cache.forget([path, path])
        self.assertFalse(path.exists())

    def test_tree_digest_follows_names_and_contents_but_not_caches(self):
        tree = self.root / "recipe"
        (tree / "__pycache__").mkdir(parents=True)
        (tree / "conanfile.py").write_text("one", encoding="utf-8")
        first = init_cache.tree_digest(tree)
        (tree / "__pycache__" / "conanfile.pyc").write_bytes(b"compiled")
        self.assertEqual(init_cache.tree_digest(tree), first)
        (tree / "conanfile.py").write_text("two", encoding="utf-8")
        self.assertNotEqual(init_cache.tree_digest(tree), first)
        self.assertEqual(init_cache.tree_digest(self.root / "absent"), "missing")
        self.assertEqual(init_cache.file_digest(self.root / "absent"), "missing")


class ConfigureSkipTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.build = engine.build_dir_for_preset(self.root, PRESET)
        self.build.mkdir(parents=True)
        (self.build / "CMakeCache.txt").write_text(
            "LUDUS_BUILD_EDITOR:BOOL=ON\nLUDUS_USE_INIT_OPTIONS:BOOL=TRUE\nLUDUS_ENABLE_CCACHE:BOOL=OFF\n",
            encoding="utf-8")
        (self.build / "build.ninja").write_text("", encoding="utf-8")
        self.argv = ["cmake", "--preset", PRESET, "-DLUDUS_USE_INIT_OPTIONS=ON", "-DLUDUS_BUILD_EDITOR=ON"]
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        self.addCleanup(self.directory.cleanup)
        stack.enter_context(patch("init_options.read_options", return_value={}))
        stack.enter_context(patch.dict("os.environ", {}, clear=False))
        stack.enter_context(patch.object(engine, "configure_needs_fresh_cache", return_value=False))

    def record(self, argv=None):
        init_cache.record(self.build / engine.CONFIGURE_STAMP_NAME,
                          engine.configure_stamp_inputs(self.root, PRESET, argv or self.argv))

    def test_current_only_with_a_matching_stamp_and_cache(self):
        self.assertFalse(engine.configure_is_current(self.root, PRESET, self.argv))
        self.record()
        self.assertTrue(engine.configure_is_current(self.root, PRESET, self.argv))
        # The ccache choice is kept by the cache, so its definition does not change the stamp.
        self.assertTrue(engine.configure_is_current(self.root, PRESET, [*self.argv, "-DLUDUS_ENABLE_CCACHE=OFF"]))
        self.assertFalse(engine.configure_is_current(self.root, PRESET, [*self.argv, "-DLUDUS_ENABLE_CCACHE=ON"]))
        self.assertFalse(engine.configure_is_current(self.root, PRESET, [*self.argv, "-DLUDUS_EDITOR_RENDER_PREVIEW=ON"]))

    def test_changed_inputs_or_missing_build_files_reconfigure(self):
        self.record()
        (self.root / "CMakePresets.json").write_text("{}", encoding="utf-8")
        self.assertFalse(engine.configure_is_current(self.root, PRESET, self.argv))
        self.record()
        with patch.dict("os.environ", {"CXX": "clang++-19"}):
            self.assertFalse(engine.configure_is_current(self.root, PRESET, self.argv))
        with patch.object(engine, "configure_needs_fresh_cache", return_value=True):
            self.assertFalse(engine.configure_is_current(self.root, PRESET, self.argv))
        (self.build / "build.ninja").unlink()
        self.assertFalse(engine.configure_is_current(self.root, PRESET, self.argv))

    def test_unanswered_file_api_query_reconfigures(self):
        self.record()
        query = self.build / ".cmake" / "api" / "v1" / "query" / "client-ludus"
        query.mkdir(parents=True)
        self.assertFalse(engine.configure_is_current(self.root, PRESET, self.argv))
        reply = self.build / ".cmake" / "api" / "v1" / "reply"
        reply.mkdir(parents=True)
        (reply / "index-1.json").write_text(json.dumps({"reply": {"client-ludus": {}}}), encoding="utf-8")
        self.assertTrue(engine.configure_is_current(self.root, PRESET, self.argv))

    def test_boolean_cache_spellings_match(self):
        for actual, requested, expected in (("ON", "1", True), ("TRUE", "yes", True), ("OFF", "0", True),
                                            ("Foo-NOTFOUND", "OFF", True), ("ON", "OFF", False),
                                            ("/usr/bin/slangc", "/opt/slangc", False)):
            with self.subTest(actual=actual, requested=requested):
                self.assertEqual(engine.cache_value_matches(actual, requested), expected)

    def test_cmake_configure_skips_and_rewrites_the_stamp(self):
        runs = []
        with patch.object(engine, "run", side_effect=lambda args, **_: runs.append(args)), \
                patch.object(engine, "request_codemodel"), \
                patch("init_editor.compiler_cache_args", return_value=[]), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            engine.cmake_configure(self.root, PRESET, ["-DLUDUS_BUILD_EDITOR=ON"], reuse_current_cache=True)
            self.assertEqual(len(runs), 1)
            engine.cmake_configure(self.root, PRESET, ["-DLUDUS_BUILD_EDITOR=ON"], reuse_current_cache=True)
            self.assertEqual(len(runs), 1)
            self.assertIn("already matches this setup", output.getvalue())
            # Other callers always configure and drop the stamp, since they may use other arguments.
            engine.cmake_configure(self.root, PRESET, ["-DLUDUS_BUILD_EDITOR=ON"])
            self.assertEqual(len(runs), 2)
            self.assertFalse((self.build / engine.CONFIGURE_STAMP_NAME).exists())


class ManagedToolTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def install(self, installed, *, has_executables=True, environment=None):
        runs = []
        with patch.object(engine, "managed_package_versions", return_value=installed), \
                patch.object(engine, "managed_executable", return_value=Path(self.directory.name) if has_executables
                             else self.root / "absent"), \
                patch.object(engine, "run", side_effect=lambda args, **_: runs.append(args)), \
                patch.dict("os.environ", environment or {}), \
                contextlib.redirect_stdout(io.StringIO()):
            engine.install_managed_tools(self.root, PINS)
        return runs

    def test_pip_runs_only_when_a_pin_or_executable_is_missing(self):
        pinned = dict(PINS["managed"])
        self.assertEqual(self.install(pinned), [])
        self.assertEqual(len(self.install({**pinned, "conan": "2.32.0"})), 1)
        self.assertEqual(len(self.install({})), 1)
        self.assertEqual(len(self.install(pinned, has_executables=False)), 1)
        self.assertEqual(len(self.install(pinned, environment={init_cache.FORCE_ENVIRONMENT: "1"})), 1)

    def test_version_probe_is_reused_until_the_executable_changes(self):
        tool = self.root / "tool"
        tool.write_text("one", encoding="utf-8")
        calls = []

        def probe(argv, **_):
            calls.append(argv)
            return 0, f"tool {len(calls)}\n"
        with patch.object(engine, "capture_command_quiet", side_effect=probe):
            self.assertEqual(engine.executable_version(str(tool), ["--version"], self.root), "tool 1")
            self.assertEqual(engine.executable_version(str(tool), ["--version"], self.root), "tool 1")
            tool.write_text("a longer replacement", encoding="utf-8")
            self.assertEqual(engine.executable_version(str(tool), ["--version"], self.root), "tool 2")
        self.assertEqual(len(calls), 2)


class ConanSkipTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.profile = self.root / "profile"
        self.profile.write_text("[settings]\n", encoding="utf-8")
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        stack.enter_context(patch.object(engine, "conan_outputs_exist", return_value=True))
        stack.enter_context(patch.object(engine, "preset_bootstrap_fingerprint", return_value="bootstrap"))
        stack.enter_context(patch.object(engine, "conan_home", return_value=self.root / "home"))

    def test_current_until_the_profile_lock_mode_or_a_recipe_changes(self):
        presets = [PRESET]
        self.assertFalse(engine.conan_step_is_current(self.root, presets, self.profile, locked=True))
        with contextlib.redirect_stdout(io.StringIO()):
            engine.record_conan_step(self.root, presets, self.profile, locked=True)
        self.assertTrue(engine.conan_step_is_current(self.root, presets, self.profile, locked=True))
        self.assertFalse(engine.conan_step_is_current(self.root, presets, self.profile, locked=False))
        self.assertFalse(engine.conan_step_is_current(self.root, [*presets, "linux-clang-debug"], self.profile,
                                                      locked=True))
        recipe = self.root / engine.LOCAL_CONAN_RECIPES[0]
        recipe.mkdir(parents=True)
        (recipe / "conanfile.py").write_text("changed", encoding="utf-8")
        self.assertFalse(engine.conan_step_is_current(self.root, presets, self.profile, locked=True))

    def test_missing_outputs_rerun(self):
        with contextlib.redirect_stdout(io.StringIO()):
            engine.record_conan_step(self.root, [PRESET], self.profile, locked=True)
        with patch.object(engine, "conan_outputs_exist", return_value=False):
            self.assertFalse(engine.conan_step_is_current(self.root, [PRESET], self.profile, locked=True))


if __name__ == "__main__":
    unittest.main()
