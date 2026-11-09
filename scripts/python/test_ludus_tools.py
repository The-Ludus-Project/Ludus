"""Unit tests for the ludus_tools host-tooling package.

Run with the pinned-or-newer Python (>=3.10):

    python3 -m unittest test_ludus_tools -v

These tests prove the schema/store/resolution logic that is implementable and
checkable without the native C++ toolchain. Native build/relocation/GUI gates
are tracked in docs/development/project-sdk-workflow-evidence.md.
"""

from __future__ import annotations

import io
import json
import os
import tarfile
import tempfile
import unittest
from pathlib import Path

from ludus_tools import descriptor, identity, lockfile, sdkstore
from ludus_tools.errors import ToolingError

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = REPO_ROOT / "apps" / "editor" / "tests" / "fixtures"


def _manifest_json(**overrides) -> dict:
    base = {
        "schema_version": 2,
        "name": "Ludus",
        "version": "0.1.0",
        "source_revision": "deadbeef",
        "target_triple": "x86_64-linux-gnu",
        "compiler_id": "Clang",
        "compiler_version": "18.1.8",
        "cxx_runtime_abi": "libstdc++-cxx11",
        "distro_baseline": "ubuntu-24.04",
        "build_flavor": "Development",
        "sdk_variant": "assert-v2-Development-RelWithDebInfo-dialogs-0-asan-0-ubsan-0-tsan-0",
        "cxx_standard": "C++23",
        "library_type": "static",
        "features": ["text", "rhi"],
        "assert_policy": {
            "policy_version": 2,
            "enable_asserts": True,
            "break_on_check": True,
            "dialogs_available": False,
        },
        "package_format": "ludus-sdk-tar-1",
        "components": ["FoundationBase", "GraphicsRhi"],
        "dependencies": [{"name": "volk", "version": "1.4.357.0"}],
        "system_prerequisites": ["vulkan-loader"],
    }
    base.update(overrides)
    return base


def _write_sdk_prefix(root: Path, manifest: dict) -> Path:
    prefix = root
    (prefix / "share" / "Ludus").mkdir(parents=True, exist_ok=True)
    (prefix / "share" / "Ludus" / "LudusSdkManifest.json").write_text(
        json.dumps(manifest, indent=2)
    )
    (prefix / "include" / "ludus").mkdir(parents=True, exist_ok=True)
    (prefix / "include" / "ludus" / "placeholder.h").write_text("#pragma once\n")
    (prefix / "lib").mkdir(parents=True, exist_ok=True)
    (prefix / "lib" / "libludus_foundation_base.a").write_bytes(b"\x00ar")
    return prefix


def _make_sdk_tar(dest_tar: Path, manifest: dict, *, wrap_dir: str = "") -> None:
    with tempfile.TemporaryDirectory() as td:
        root = Path(td) / "prefix"
        _write_sdk_prefix(root, manifest)
        with tarfile.open(dest_tar, "w:gz") as tf:
            base = wrap_dir or "."
            for path in sorted(root.rglob("*")):
                arcname = os.path.join(base, str(path.relative_to(root)))
                tf.add(path, arcname=arcname)


# ---------------------------------------------------------------------------


class DescriptorV1Compat(unittest.TestCase):
    """The v2-aware reader must agree with the shared v1 fixtures.

    The shared ``cases.json`` is the v1 contract fixture consumed by the legacy
    C++ ProjectStore and editor_project.py (whose byte-for-byte agreement with
    these cases is covered by test_editor_tool.py). ludus_tools.descriptor is the
    newer *version-2-aware* reader, so it must still produce the identical
    verdict for every v1 case — with one deliberate exception: a case whose only
    reason to be invalid under the old reader is "version == 2" is now a valid
    v2 descriptor. That one case (``invalid_bad_version.json``) is handled below
    by test_v2_formerly_invalid_version so the preserved-v1 intent stays explicit.
    """

    V2_ACCEPTED_FILES = {"invalid_bad_version.json"}

    def test_shared_fixture_cases(self) -> None:
        cases = json.loads((FIXTURES / "cases.json").read_text())
        for case in cases:
            if case["file"] in self.V2_ACCEPTED_FILES:
                continue
            data = (FIXTURES / case["file"]).read_bytes()
            if case["valid"]:
                d = descriptor.parse_descriptor_bytes(data)
                self.assertEqual(d.version, 1)
                self.assertEqual(d.name, case["name"])
                self.assertEqual(d.provider, case["provider"])
                self.assertEqual(d.run_args, case["run_args"])
            else:
                with self.assertRaises(ToolingError) as ctx:
                    descriptor.parse_descriptor_bytes(data)
                self.assertEqual(ctx.exception.code, case["code"])

    def test_v2_formerly_invalid_version(self) -> None:
        # The v1 fixture marks version==2 invalid (UnsupportedVersion). The
        # v2-aware reader accepts it. But that fixture declares provider "ludus"
        # which must NOT carry an engine object, and omits template — both legal.
        data = (FIXTURES / "invalid_bad_version.json").read_bytes()
        d = descriptor.parse_descriptor_bytes(data)
        self.assertEqual(d.version, 2)
        self.assertEqual(d.provider, "ludus")
        self.assertIsNone(d.engine)

    def test_shared_v2_fixture_cases(self) -> None:
        """The v2-aware reader must match the shared v2 fixtures (also consumed
        by the C++ ProjectStore v2 test, so the schema cannot drift)."""
        cases = json.loads((FIXTURES / "cases_v2.json").read_text())
        self.assertTrue(cases)
        for case in cases:
            data = (FIXTURES / case["file"]).read_bytes()
            if case["valid"]:
                d = descriptor.parse_descriptor_bytes(data)
                self.assertEqual(d.version, case["version"])
                self.assertEqual(d.name, case["name"])
                self.assertEqual(d.provider, case["provider"])
                self.assertEqual(d.preset, case["preset"])
                self.assertEqual(d.run_args, case["run_args"])
                if case.get("has_engine", True) and case["provider"] == "cmake":
                    self.assertIsNotNone(d.engine)
                    self.assertEqual(d.engine.version, case["engine_version"])
                    self.assertEqual(d.engine.components, case.get("components", []))
                    self.assertEqual(d.engine.features, case.get("features", []))
                else:
                    self.assertIsNone(d.engine)
            else:
                with self.assertRaises(ToolingError) as ctx:
                    descriptor.parse_descriptor_bytes(data)
                self.assertEqual(ctx.exception.code, case["code"])

    def test_legacy_reader_still_rejects_v2(self) -> None:
        # editor_project.py (the v1-only reader) must keep rejecting v2 so old
        # Editor builds fail usefully (P09 "Old Editor versions reject v2").
        import editor_project

        data = (FIXTURES / "invalid_bad_version.json").read_bytes()
        with self.assertRaises(editor_project.ProjectError) as ctx:
            editor_project.parse_descriptor_bytes(data)
        self.assertEqual(ctx.exception.code, "UnsupportedVersion")


class DescriptorV2(unittest.TestCase):
    def _v2(self, **over) -> bytes:
        base = {
            "version": 2,
            "name": "MyGame",
            "provider": "cmake",
            "source_dir": ".",
            "preset": "linux-clang-development",
            "target": "my_game",
            "run": {"cwd": ".", "args": []},
            "engine": {"version": "0.1.0", "components": ["FoundationBase"], "features": []},
            "template": {"id": "minimal", "version": 1},
        }
        base.update(over)
        return json.dumps(base).encode("utf-8")

    def test_valid_v2(self) -> None:
        d = descriptor.parse_descriptor_bytes(self._v2())
        self.assertEqual(d.version, 2)
        self.assertIsNotNone(d.engine)
        self.assertEqual(d.engine.version, "0.1.0")
        self.assertEqual(d.engine.components, ["FoundationBase"])
        self.assertEqual(d.template.id, "minimal")
        self.assertEqual(d.template.version, 1)

    def test_release_preset_allowed_v2(self) -> None:
        d = descriptor.parse_descriptor_bytes(self._v2(preset="linux-clang-release"))
        self.assertEqual(d.preset, "linux-clang-release")

    def test_cmake_requires_engine(self) -> None:
        body = json.loads(self._v2())
        del body["engine"]
        with self.assertRaises(ToolingError) as ctx:
            descriptor.parse_descriptor_bytes(json.dumps(body).encode())
        self.assertEqual(ctx.exception.code, "InvalidProject")

    def test_ludus_provider_rejects_engine(self) -> None:
        with self.assertRaises(ToolingError):
            descriptor.parse_descriptor_bytes(self._v2(provider="ludus"))

    def test_floating_version_rejected(self) -> None:
        with self.assertRaises(ToolingError):
            descriptor.parse_descriptor_bytes(self._v2(engine={"version": "^1.0"}))

    def test_unknown_engine_field_rejected(self) -> None:
        with self.assertRaises(ToolingError):
            descriptor.parse_descriptor_bytes(
                self._v2(engine={"version": "1.0", "nope": 1})
            )

    def test_release_preset_rejected_in_v1(self) -> None:
        body = {
            "version": 1,
            "name": "x",
            "provider": "cmake",
            "source_dir": ".",
            "preset": "linux-clang-release",
            "target": "t",
            "run": {"cwd": ".", "args": []},
        }
        with self.assertRaises(ToolingError):
            descriptor.parse_descriptor_bytes(json.dumps(body).encode())

    def test_oversize_rejected(self) -> None:
        with self.assertRaises(ToolingError):
            descriptor.parse_descriptor_bytes(b"{" + b" " * (descriptor.MAX_FILE_BYTES + 1))


class LockSchema(unittest.TestCase):
    def test_unresolved_roundtrip(self) -> None:
        lock = lockfile.unresolved_lock("0.1.0", template_version=1)
        parsed = lockfile.parse_lock_bytes(lock.serialize())
        self.assertFalse(parsed.resolved)
        self.assertEqual(parsed.engine_version, "0.1.0")
        self.assertEqual(parsed.packages, [])

    def test_resolved_requires_packages(self) -> None:
        bad = {
            "schema_version": 1,
            "resolved": True,
            "engine": {"version": "0.1.0", "revision": "abc"},
            "template_version": 1,
            "packages": [],
        }
        with self.assertRaises(ToolingError):
            lockfile.parse_lock_bytes(json.dumps(bad).encode())

    def test_unresolved_forbids_packages(self) -> None:
        bad = {
            "schema_version": 1,
            "resolved": False,
            "engine": {"version": "0.1.0", "revision": ""},
            "template_version": 1,
            "packages": [{"target": "t", "flavor": "Debug", "sdk_variant": "v", "digest": "d", "size": 1}],
        }
        with self.assertRaises(ToolingError):
            lockfile.parse_lock_bytes(json.dumps(bad).encode())

    def test_descriptor_lock_agreement(self) -> None:
        lock = lockfile.unresolved_lock("0.1.0", 1)
        lockfile.validate_descriptor_lock_agreement("0.1.0", lock)
        with self.assertRaises(ToolingError):
            lockfile.validate_descriptor_lock_agreement("9.9.9", lock)

    def test_resolved_roundtrip(self) -> None:
        lock = lockfile.Lock(
            resolved=True,
            engine_version="0.1.0",
            engine_revision="abc",
            template_version=1,
            packages=[
                lockfile.PackageEntry("linux-x64", "Development", "variant", "a" * 64, 1234)
            ],
        )
        parsed = lockfile.parse_lock_bytes(lock.serialize())
        self.assertTrue(parsed.resolved)
        self.assertEqual(len(parsed.packages), 1)
        self.assertEqual(parsed.packages[0].digest, "a" * 64)


class LocalSettings(unittest.TestCase):
    def test_override_set_clear(self) -> None:
        s = lockfile.LocalSettings()
        s.set_override("linux-x64", "Development", "/opt/sdk")
        self.assertEqual(s.find("linux-x64", "Development").prefix, "/opt/sdk")
        parsed = lockfile.parse_local_settings_bytes(s.serialize())
        self.assertEqual(parsed.find("linux-x64", "Development").prefix, "/opt/sdk")
        self.assertTrue(parsed.clear_override("linux-x64", "Development"))
        self.assertIsNone(parsed.find("linux-x64", "Development"))

    def test_missing_file_is_empty(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            s = lockfile.parse_local_settings_file(Path(td) / "local.json")
            self.assertEqual(s.overrides, [])


class Identity(unittest.TestCase):
    def test_compat_flavor_mismatch(self) -> None:
        ident = identity.SdkIdentity.from_json(_manifest_json())
        mism = identity.check_compatible(ident, required_flavor="Release")
        self.assertEqual([m.field_name for m in mism], ["flavor"])

    def test_compat_ok(self) -> None:
        ident = identity.SdkIdentity.from_json(_manifest_json())
        self.assertEqual(identity.check_compatible(ident, required_flavor="Development"), [])

    def test_feature_missing(self) -> None:
        ident = identity.SdkIdentity.from_json(_manifest_json())
        mism = identity.check_compatible(ident, required_features=["audio"])
        self.assertEqual([m.field_name for m in mism], ["features"])

    def test_abi_reference_mismatch(self) -> None:
        a = identity.SdkIdentity.from_json(_manifest_json())
        b = identity.SdkIdentity.from_json(_manifest_json(compiler_version="15.0.0"))
        mism = identity.check_compatible(a, reference=b)
        self.assertIn("compiler_version", [m.field_name for m in mism])

    def test_legacy_flat_manifest(self) -> None:
        legacy = {
            "name": "Ludus",
            "version": "0.1.0",
            "build_flavor": "Development",
            "sdk_variant": "variant",
            "assert_policy_version": 2,
            "enable_asserts": 1,
            "break_on_check": 1,
            "assert_dialogs_available": 0,
        }
        ident = identity.SdkIdentity.from_json(legacy)
        self.assertEqual(ident.assert_policy.policy_version, 2)
        self.assertTrue(ident.assert_policy.enable_asserts)


class ManifestTemplate(unittest.TestCase):
    """The extended SDK manifest template renders to valid, schema-conformant JSON.

    A real CMake configure is unavailable in this environment (pinned CMake 3.29
    absent), so this test substitutes representative @VAR@ values exactly as
    configure_file(@ONLY) would and asserts the result parses into the identity
    model and keeps the legacy flat fields that engine.verify_sdk_install and
    CheckSdkVariant.cmake.in still read.
    """

    SUBS = {
        "LUDUS_ENGINE_PACKAGE_NAME": "Ludus",
        "PROJECT_VERSION": "0.1.0",
        "LUDUS_GIT_REVISION": "abc123def456",
        "LUDUS_ENGINE_EXPORT_NAMESPACE": "Ludus::",
        "LUDUS_TARGET_TRIPLE": "x86_64-linux-gnu",
        "CMAKE_SYSTEM_NAME": "Linux",
        "CMAKE_SYSTEM_PROCESSOR": "x86_64",
        "LUDUS_COMPILER_ID": "Clang",
        "LUDUS_COMPILER_VERSION": "18.1.8",
        "LUDUS_CXX_RUNTIME_ABI": "libstdc++-cxx11",
        "LUDUS_DISTRO_BASELINE": "ubuntu-24.04",
        "LUDUS_SDK_VARIANT": "assert-v2-Development-RelWithDebInfo-dialogs-0-asan-0-ubsan-0-tsan-0",
        "LUDUS_BUILD_FLAVOR": "Development",
        "LUDUS_BUILD_FLAVOR_ID": "2",
        "CMAKE_BUILD_TYPE": "RelWithDebInfo",
        "LUDUS_ENABLE_ASAN_JSON": "0",
        "LUDUS_ENABLE_UBSAN_JSON": "0",
        "LUDUS_ENABLE_TSAN_JSON": "0",
        "LUDUS_ASSERT_POLICY_VERSION": "2",
        "LUDUS_ENABLE_ASSERTS": "1",
        "LUDUS_BREAK_ON_CHECK": "1",
        "LUDUS_ASSERT_DIALOGS_AVAILABLE": "0",
        "LUDUS_SDK_FEATURES_JSON": '"GraphicsRhi", "Text"',
        "LUDUS_SDK_COMPONENTS_JSON": '"FoundationBase", "GraphicsRhi", "Text"',
        "LUDUS_SDK_DEPENDENCIES_JSON": (
            '{"name": "volk", "version": "1.4.357.0", "licenses": "MIT", "kind": "bundled"}, '
            '{"name": "freetype", "version": "2.14.3", "licenses": "FTL-or-GPLv2", "kind": "bundled"}'
        ),
        "LUDUS_SDK_SYSTEM_PREREQS_JSON": '"Vulkan loader", "POSIX threads"',
    }

    def _render(self) -> dict:
        import re

        tmpl = (REPO_ROOT / "cmake" / "LudusSdkManifest.json.in").read_text()

        def repl(m):
            key = m.group(1)
            self.assertIn(key, self.SUBS, f"unsubstituted @{key}@ in manifest template")
            return self.SUBS[key]

        return json.loads(re.sub(r"@([A-Z0-9_]+)@", repl, tmpl))

    def test_renders_valid_json(self) -> None:
        d = self._render()
        self.assertEqual(d["schema_version"], 2)
        ident = identity.SdkIdentity.from_json(d)
        self.assertEqual(ident.target_triple, "x86_64-linux-gnu")
        self.assertEqual(ident.components, ["FoundationBase", "GraphicsRhi", "Text"])
        self.assertEqual([dep["name"] for dep in ident.dependencies], ["volk", "freetype"])

    def test_keeps_legacy_flat_fields(self) -> None:
        d = self._render()
        for key in ("enable_asserts", "break_on_check", "assert_policy_version",
                    "assert_dialogs_available", "build_flavor_id", "sdk_variant"):
            self.assertIn(key, d, f"legacy flat field {key} must remain for verify_sdk_install")


class Store(unittest.TestCase):
    def setUp(self) -> None:
        self.td = tempfile.TemporaryDirectory()
        self.store = sdkstore.SdkStore(Path(self.td.name) / "store")

    def tearDown(self) -> None:
        self.td.cleanup()

    def test_install_and_reuse(self) -> None:
        tar = Path(self.td.name) / "sdk.tar.gz"
        _make_sdk_tar(tar, _manifest_json())
        installed = self.store.install_archive(tar)
        self.assertTrue(installed.prefix.is_dir())
        self.assertTrue((installed.prefix / "share" / "Ludus" / "LudusSdkManifest.json").is_file())
        # Second install of the identical archive reuses the same directory.
        again = self.store.install_archive(tar)
        self.assertEqual(installed.prefix, again.prefix)
        self.assertEqual(len(self.store.list_installed()), 1)

    def test_install_wrapped_dir(self) -> None:
        tar = Path(self.td.name) / "wrapped.tar.gz"
        _make_sdk_tar(tar, _manifest_json(), wrap_dir="ludus-sdk")
        installed = self.store.install_archive(tar)
        self.assertTrue((installed.prefix / "share" / "Ludus" / "LudusSdkManifest.json").is_file())

    def test_digest_mismatch_rejected(self) -> None:
        tar = Path(self.td.name) / "sdk.tar.gz"
        _make_sdk_tar(tar, _manifest_json())
        with self.assertRaises(ToolingError) as ctx:
            self.store.install_archive(tar, expected_digest="0" * 64)
        self.assertEqual(ctx.exception.code, "DigestMismatch")

    def test_resolve_missing(self) -> None:
        with self.assertRaises(ToolingError) as ctx:
            self.store.resolve_installed("f" * 64)
        self.assertEqual(ctx.exception.code, "SdkNotFound")

    def test_traversal_archive_rejected(self) -> None:
        tar = Path(self.td.name) / "evil.tar"
        with tarfile.open(tar, "w") as tf:
            info = tarfile.TarInfo("../escape.txt")
            data = b"nope"
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
        with self.assertRaises(ToolingError) as ctx:
            self.store.validate_archive(tar)
        self.assertEqual(ctx.exception.code, "ArchiveInvalid")

    def test_absolute_name_rejected(self) -> None:
        tar = Path(self.td.name) / "abs.tar"
        with tarfile.open(tar, "w") as tf:
            info = tarfile.TarInfo("/etc/passwd")
            info.size = 0
            tf.addfile(info, io.BytesIO(b""))
        with self.assertRaises(ToolingError) as ctx:
            self.store.validate_archive(tar)
        self.assertEqual(ctx.exception.code, "ArchiveInvalid")

    def test_absolute_symlink_rejected(self) -> None:
        tar = Path(self.td.name) / "link.tar"
        with tarfile.open(tar, "w") as tf:
            info = tarfile.TarInfo("link")
            info.type = tarfile.SYMTYPE
            info.linkname = "/etc/passwd"
            tf.addfile(info)
        with self.assertRaises(ToolingError) as ctx:
            self.store.validate_archive(tar)
        self.assertEqual(ctx.exception.code, "ArchiveInvalid")

    def test_cancellation(self) -> None:
        tar = Path(self.td.name) / "sdk.tar.gz"
        _make_sdk_tar(tar, _manifest_json())
        token = sdkstore.CancelToken()
        token.cancel()
        with self.assertRaises(ToolingError) as ctx:
            self.store.install_archive(tar, cancel=token)
        self.assertEqual(ctx.exception.code, "InstallCancelled")


class BundleDeps(unittest.TestCase):
    def setUp(self) -> None:
        from ludus_tools import bundle_deps

        self.bundle_deps = bundle_deps

    def test_rewrite_producer_paths(self) -> None:
        text = (
            'set(volk_INCLUDE_DIR "/home/u/.conan2/p/volka1b2/p/include")\n'
            'set(volk_LIB "/home/u/.conan2/p/volka1b2/p/lib/libvolk.a")\n'
        )
        out = self.bundle_deps.rewrite_producer_paths(
            text, [("/home/u/.conan2/p/volka1b2/p", "${CMAKE_CURRENT_LIST_DIR}/../packages/volk")]
        )
        self.assertNotIn("/home/u/.conan2", out)
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../packages/volk/include", out)
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../packages/volk/lib/libvolk.a", out)

    def test_rewrite_longest_root_first(self) -> None:
        # A nested root must not be partially rewritten by a shorter one.
        text = 'set(x "/a/b/c/p/include")\n'
        out = self.bundle_deps.rewrite_producer_paths(text, [("/a", "/SHORT"), ("/a/b/c/p", "/LONG")])
        self.assertEqual(out.strip(), 'set(x "/LONG/include")')

    def test_find_producer_roots(self) -> None:
        text = (
            'target "/home/runner/.conan2/p/freetf0add/p/lib/libfreetype.a"\n'
            'build "/w/out/conan/b/harfb12/p/include"\n'
        )
        roots = self.bundle_deps.find_producer_roots(text)
        self.assertIn("/home/runner/.conan2/p/freetf0add/p", roots)
        self.assertIn("/w/out/conan/b/harfb12/p", roots)

    def test_find_producer_roots_keeps_windows_drive_letters(self) -> None:
        text = 'set(freetype_PACKAGE_FOLDER_RELWITHDEBINFO "C:/Users/runner/.conan2/p/b/freet12ab/p")\n'
        self.assertEqual(self.bundle_deps.find_producer_roots(text), ["C:/Users/runner/.conan2/p/b/freet12ab/p"])

    def test_path_spellings_cover_native_and_cmake_separators(self) -> None:
        self.assertEqual(self.bundle_deps.path_spellings("C:\\deps\\p"), ["C:\\deps\\p", "C:/deps/p"])
        self.assertEqual(self.bundle_deps.path_spellings("/deps/p"), ["/deps/p"])

    def test_windows_spelled_package_roots_are_rewritten_and_audited(self) -> None:
        # CMakeDeps writes forward slashes on Windows; Path uses backslashes there.
        native = "C:\\cache\\p\\freet12ab\\p"
        text = 'set(freetype_INCLUDE "C:/cache/p/freet12ab/p/include")\n'
        replacements = self.bundle_deps._spelled_replacements(native, "${CMAKE_CURRENT_LIST_DIR}/../packages/freetype")
        self.assertEqual(self.bundle_deps.rewrite_producer_paths(text, replacements),
                         'set(freetype_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../packages/freetype/include")\n')
        with tempfile.TemporaryDirectory() as td:
            cmake_dir = Path(td) / "lib" / "cmake" / "Ludus" / "dependencies" / "cmake"
            cmake_dir.mkdir(parents=True)
            (cmake_dir / "freetype-config.cmake").write_text(text)
            self.assertEqual(len(self.bundle_deps.audit_no_producer_paths(Path(td), [native])), 1)

    def test_conan_package_dirs_reads_cmakedeps_data_files(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            package = root / "cache" / "freetype"
            package.mkdir(parents=True)
            generators = root / "deps"
            generators.mkdir()
            (generators / "freetype-relwithdebinfo-x86_64-data.cmake").write_text(
                f'set(freetype_PACKAGE_FOLDER_RELWITHDEBINFO "{package.as_posix()}")\n'
                'set(missing_PACKAGE_FOLDER_RELWITHDEBINFO "/no/such/package")\n')
            self.assertEqual(self.bundle_deps.conan_package_dirs(generators), {"freetype": package})
            self.assertEqual(self.bundle_deps.conan_package_dirs(root / "absent"), {})

    def test_bundle_from_conan_layout_and_audit(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            # Fake a volk Conan package.
            pkg = root / "cache" / "volk" / "p"
            (pkg / "lib").mkdir(parents=True)
            (pkg / "include").mkdir(parents=True)
            (pkg / "lib" / "libvolk.a").write_bytes(b"\x00ar")
            # Fake the Conan generators output folder with the real find_package
            # entry points, referencing the package folder by absolute path.
            gen = root / "conan-out"
            gen.mkdir()
            (gen / "volk-config.cmake").write_text(
                'include("${CMAKE_CURRENT_LIST_DIR}/volkTargets.cmake")\n'
            )
            (gen / "volkTargets.cmake").write_text(
                f'set_target_properties(volk::volk PROPERTIES '
                f'INTERFACE_INCLUDE_DIRECTORIES "{pkg}/include")\n'
                f'set_property(TARGET volk::volk PROPERTY IMPORTED_LOCATION "{pkg}/lib/libvolk.a")\n'
            )
            (gen / "cmakedeps_macros.cmake").write_text("# macros\n")
            prefix = root / "prefix"
            prefix.mkdir()
            bundled = self.bundle_deps.bundle_from_conan(
                prefix=prefix, generators_dir=gen, package_dirs={"volk": pkg}, dependencies=("volk",),
            )
            self.assertEqual(bundled, ["volk"])
            dep_root = prefix / "lib" / "cmake" / "Ludus" / "dependencies"
            # find_package entry point is present under dependencies/cmake.
            self.assertTrue((dep_root / "cmake" / "volk-config.cmake").is_file())
            self.assertTrue((dep_root / "cmake" / "cmakedeps_macros.cmake").is_file())
            # Package payload copied under dependencies/packages/volk.
            self.assertTrue((dep_root / "packages" / "volk" / "lib" / "libvolk.a").is_file())
            # Targets file now points at the bundled package, not the cache.
            targets = (dep_root / "cmake" / "volkTargets.cmake").read_text()
            self.assertNotIn(str(pkg), targets)
            self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../packages/volk/include", targets)
            self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../packages/volk/lib/libvolk.a", targets)
            # Audit: no producer path survives anywhere in the bundle.
            self.assertEqual(
                self.bundle_deps.audit_no_producer_paths(prefix, [str(pkg), str(gen)]), []
            )

    def test_excludes_build_and_test_only_generator_files(self) -> None:
        # Regression: copying the whole generators folder dragged in Catch2 /
        # conan_toolchain files that embed producer paths and are not part of the
        # runtime link closure. Only the bundled deps' configs must be copied.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            pkg = root / "cache" / "volk" / "p"
            (pkg / "lib").mkdir(parents=True)
            (pkg / "lib" / "libvolk.a").write_bytes(b"\x00ar")
            gen = root / "conan-out"
            gen.mkdir()
            (gen / "volk-config.cmake").write_text("# volk\n")
            (gen / "VulkanHeaders-release-x86_64-data.cmake").write_text("# vh\n")
            (gen / "cmakedeps_macros.cmake").write_text("# macros\n")
            # These must NOT be copied (they embed producer paths):
            (gen / "Catch2-release-x86_64-data.cmake").write_text(f'set(c "{root}/leak")\n')
            (gen / "conan_toolchain.cmake").write_text(f'set(t "{root}/leak")\n')
            prefix = root / "prefix"
            prefix.mkdir()
            self.bundle_deps.bundle_from_conan(
                prefix=prefix, generators_dir=gen, package_dirs={"volk": pkg}, dependencies=("volk",),
            )
            cmake_dir = prefix / "lib" / "cmake" / "Ludus" / "dependencies" / "cmake"
            names = {p.name for p in cmake_dir.iterdir()}
            self.assertIn("volk-config.cmake", names)
            self.assertIn("VulkanHeaders-release-x86_64-data.cmake", names)
            self.assertIn("cmakedeps_macros.cmake", names)
            self.assertNotIn("Catch2-release-x86_64-data.cmake", names)
            self.assertNotIn("conan_toolchain.cmake", names)
            # And the audit for the broad producer root is clean.
            self.assertEqual(self.bundle_deps.audit_no_producer_paths(prefix, [str(root)]), [])

    def test_residual_transitive_root_is_bundled_not_rejected(self) -> None:
        # Regression (CI "Development and SDK"): a kept generator config can
        # reference a transitive, header-only package root (e.g. VulkanHeaders
        # pulled by volk) that is NOT in package_dirs. The bundler must copy that
        # payload FROM the referenced root and rewrite to it — not fail, and not
        # leave a producer path.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            # volk package (in package_dirs) ...
            volk = root / "cache" / "volk" / "p"
            (volk / "include").mkdir(parents=True)
            (volk / "include" / "volk.h").write_text("// volk\n")
            # ... and a VulkanHeaders package root that is NOT in package_dirs but
            # is referenced by volk's generated config, under a Conan /p/<tok>/p.
            vh = root / "cache" / "p" / "vulka2b5ed468eaac1" / "p"
            (vh / "include" / "vulkan").mkdir(parents=True)
            (vh / "include" / "vulkan" / "vulkan.h").write_text("// vulkan\n")
            gen = root / "conan-out"
            gen.mkdir()
            (gen / "volk-config.cmake").write_text("# volk\n")
            (gen / "volkTargets.cmake").write_text(
                f'set_property(TARGET volk::volk_headers PROPERTY '
                f'INTERFACE_INCLUDE_DIRECTORIES "{vh}/include")\n'
            )
            prefix = root / "prefix"
            prefix.mkdir()
            bundled = self.bundle_deps.bundle_from_conan(
                prefix=prefix, generators_dir=gen, package_dirs={"volk": volk}, dependencies=("volk",),
            )
            self.assertEqual(bundled, ["volk"])
            dep_root = prefix / "lib" / "cmake" / "Ludus" / "dependencies"
            targets = (dep_root / "cmake" / "volkTargets.cmake").read_text()
            # The VulkanHeaders root was rewritten to a bundled payload...
            self.assertNotIn(str(vh), targets)
            self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../packages/vulka2b5ed468eaac1/include", targets)
            # ...and its payload was actually copied so the include dir exists.
            self.assertTrue((dep_root / "packages" / "vulka2b5ed468eaac1" / "include" / "vulkan" / "vulkan.h").is_file())
            # No producer path survives anywhere in the bundle.
            self.assertEqual(self.bundle_deps.audit_no_producer_paths(prefix, [str(vh), str(volk), str(gen)]), [])

    def test_residual_root_missing_on_disk_fails(self) -> None:
        # If a config references a Conan root that no longer exists, the bundler
        # cannot copy the payload and must fail rather than ship a broken path.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            gen = root / "conan-out"
            gen.mkdir()
            (gen / "volk-config.cmake").write_text(
                'set(x "/nonexistent/.conan2/p/ghost00000000/p/include")\n'
            )
            prefix = root / "prefix"
            prefix.mkdir()
            with self.assertRaises(ToolingError) as ctx:
                self.bundle_deps.bundle_from_conan(
                    prefix=prefix, generators_dir=gen, package_dirs={}, dependencies=(),
                )
            self.assertEqual(ctx.exception.code, "ArchiveInvalid")

    def test_mac_framework_link_uses_consumer_sdk_after_relocation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "producer with spaces"
            package = root / "cache/harfbuzz/p"
            config = package / "lib/cmake/harfbuzz/harfbuzzConfig.cmake"
            config.parent.mkdir(parents=True)
            framework = root / "out/host-tools/macos-sdk/System/Library/Frameworks/ApplicationServices.framework"
            config.write_text('set_target_properties(harfbuzz::harfbuzz PROPERTIES\n'
                              f'  INTERFACE_LINK_LIBRARIES "Threads::Threads;Freetype::Freetype;{framework};freetype")\n')
            generators = root / "generators"
            generators.mkdir()
            (generators / "harfbuzz-config.cmake").write_text("# entry point\n")
            prefix = root / "sdk"
            self.bundle_deps.bundle_from_conan(prefix=prefix, generators_dir=generators,
                package_dirs={"harfbuzz": package}, dependencies=("harfbuzz",))
            bundled = prefix / "lib/cmake/Ludus/dependencies/packages/harfbuzz/lib/cmake/harfbuzz/harfbuzzConfig.cmake"
            self.assertIn('Threads::Threads;Freetype::Freetype;$<LINK_LIBRARY:FRAMEWORK,ApplicationServices>;freetype', bundled.read_text())
            self.assertEqual([], self.bundle_deps.audit_no_producer_paths(prefix, [str(root)]))

    def test_mac_framework_rewrite_preserves_non_link_properties_and_private_paths(self) -> None:
        text = ('INTERFACE_INCLUDE_DIRECTORIES "/sdk/System/Library/Frameworks/Cocoa.framework"\n'
                'INTERFACE_LINK_LIBRARIES "/private/My.framework;Threads::Threads;/sdk/System/Library/Frameworks/Cocoa.framework"')
        result = self.bundle_deps.relocate_apple_framework_links(text)
        self.assertIn('INTERFACE_INCLUDE_DIRECTORIES "/sdk/System/Library/Frameworks/Cocoa.framework"', result)
        self.assertIn('/private/My.framework;Threads::Threads;$<LINK_LIBRARY:FRAMEWORK,Cocoa>', result)

    def test_audit_detects_leak(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            prefix = Path(td) / "prefix"
            dep = prefix / "lib" / "cmake" / "Ludus" / "dependencies" / "cmake"
            dep.mkdir(parents=True)
            (dep / "volk-config.cmake").write_text('set(x "/producer/cache/p/abc/p/lib/x.a")\n')
            leaks = self.bundle_deps.audit_no_producer_paths(prefix, ["/producer/cache/p/abc/p"])
            self.assertEqual(len(leaks), 1)


class CatalogDownload(unittest.TestCase):
    def setUp(self) -> None:
        from ludus_tools import catalog

        self.catalog = catalog
        self.td = tempfile.TemporaryDirectory()
        self.root = Path(self.td.name)
        self.tar = self.root / "sdk.tar.gz"
        _make_sdk_tar(self.tar, _manifest_json())
        self.digest = sdkstore.sha256_file(self.tar)
        self.size = self.tar.stat().st_size

    def tearDown(self) -> None:
        self.td.cleanup()

    def _opener(self, url):
        return open(self.tar, "rb")

    def _entry(self, **over):
        base = dict(release="0.1.0", revision="deadbeef", target="linux-x64",
                    flavor="development", url="https://example.test/sdk.tar.gz",
                    size=self.size, sha256=self.digest)
        base.update(over)
        return self.catalog.CatalogEntry.from_json(base)

    def test_catalog_parse_requires_https(self) -> None:
        bad = {"schema_version": 1, "sdks": [
            {"release": "0.1.0", "target": "linux-x64", "flavor": "development",
             "url": "http://insecure/x.tar.gz", "size": 1, "sha256": "a" * 64}]}
        with self.assertRaises(ToolingError):
            self.catalog.parse_catalog_bytes(json.dumps(bad).encode())

    def test_download_ok_then_install(self) -> None:
        entry = self._entry()
        archive = self.catalog.download_entry(entry, self.root / "dl", opener=self._opener)
        store = sdkstore.SdkStore(self.root / "store")
        installed = store.install_archive(archive, expected_digest=entry.sha256)
        self.assertTrue(installed.prefix.is_dir())

    def test_download_digest_mismatch(self) -> None:
        entry = self._entry(sha256="0" * 64)
        with self.assertRaises(ToolingError) as ctx:
            self.catalog.download_entry(entry, self.root / "dl", opener=self._opener)
        self.assertEqual(ctx.exception.code, "DigestMismatch")

    def test_download_truncated(self) -> None:
        entry = self._entry(size=self.size + 100)
        with self.assertRaises(ToolingError) as ctx:
            self.catalog.download_entry(entry, self.root / "dl", opener=self._opener)
        self.assertEqual(ctx.exception.code, "ArchiveInvalid")

    def test_download_oversize_rejected(self) -> None:
        entry = self._entry(size=self.catalog.MAX_DOWNLOAD_BYTES + 1)
        with self.assertRaises(ToolingError):
            self.catalog.download_entry(entry, self.root / "dl", opener=self._opener)



# Shared with apps/editor/tests/project_setup_dialog_tests.cpp, which checks the
# editor's DeriveProjectTarget against the same names; change both together.
SESSION_TARGET_CASES = (
    ("New Game", "New_Game"),
    ("Jeux Vidéo 日本語", "Jeux_Vid_o"),
    ("  game  ", "game"),
    ("-.+_", "app"),
    ("日本", "app"),
    ("a\U0001F600b", "a_b"),
    ("my.game+1-x", "my.game+1-x"),
    ("x" * 130, "x" * 128),
)


class SessionTemplate(unittest.TestCase):
    """The browser editor's embedded template renders exactly what create_project writes."""

    @staticmethod
    def _render(text: str, name: str, target: str) -> str:
        escaped = json.dumps(name, ensure_ascii=False)[1:-1]
        return text.replace("{{ NAME }}", escaped).replace("{{ TARGET }}", target)

    def test_targets_match_creation(self) -> None:
        from ludus_tools import create

        for name, target in SESSION_TARGET_CASES:
            with self.subTest(name=name):
                self.assertEqual(create._derive_target(name), target)

    def test_substituted_template_equals_created_project(self) -> None:
        from ludus_tools import create

        for name in ("New Game", 'Quote "Game"', "Jeux Vidéo 日本語"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as td:
                created = create.create_project(Path(td) / "project", name=name, template_id="minimal",
                                                engine_version="0.1.0", preset=create.SESSION_TEMPLATE_PRESET)
                expected = {path.relative_to(created.destination).as_posix(): path.read_bytes()
                            for path in created.destination.rglob("*") if path.is_file()}
                target = create._derive_target(name)
                rendered = {file.relpath: self._render(file.content, name, target).encode("utf-8")
                            for file in create.session_template_files("0.1.0")}
                self.assertEqual(rendered, expected)
                parsed = descriptor.parse_descriptor_bytes(rendered["ludus.project.json"])
                self.assertEqual((parsed.name, parsed.target), (name, target))

    def test_only_identity_placeholders_remain(self) -> None:
        import re
        from ludus_tools import create

        keys = set()
        for file in create.session_template_files("0.1.0"):
            keys.update(re.findall(r"\{\{\s*([A-Z0-9_]+)\s*\}\}", file.content))
            if "{{ NAME }}" in file.content:
                self.assertEqual(file.relpath, "ludus.project.json")
        self.assertEqual(keys, set(create.SESSION_TEMPLATE_PLACEHOLDERS))

    def test_write_keeps_unchanged_files_and_removes_stale_ones(self) -> None:
        from ludus_tools import create

        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / "minimal"
            paths = create.write_session_template(root, "0.1.0")
            self.assertIn("ludus.project.json", paths)
            self.assertIn(".gitignore", paths)
            stale = root / "old" / "removed.txt"
            stale.parent.mkdir()
            stale.write_text("stale")
            kept = root / "CMakeLists.txt"
            os.utime(kept, (1, 1))
            self.assertEqual(create.write_session_template(root, "0.1.0"), paths)
            self.assertFalse(stale.parent.exists())
            self.assertEqual(kept.stat().st_mtime, 1)

    def test_rejects_an_invalid_engine_version(self) -> None:
        from ludus_tools import create

        with self.assertRaises(ToolingError):
            create.session_template_files("")


if __name__ == "__main__":
    unittest.main()
