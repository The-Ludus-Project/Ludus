"""Android setup consent, package verification and retry behavior."""
from __future__ import annotations

import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import android_tools
import engine
import init_android
import init_progress
import init_ui


class AndroidSetupTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.pins = android_tools.read_android_pins(engine.repo_root())
        self.sdk = android_tools.managed_sdk(self.root)

    def test_android_is_opt_in_and_switches_are_forwarded_to_gui_worker(self):
        parser = engine.make_parser()
        default = init_ui.apply_defaults(parser.parse_args(["init"]))
        self.assertFalse(default.with_android)
        self.assertNotIn("android", [step.key for step in init_progress.plan(default)])
        selected = init_ui.apply_defaults(parser.parse_args(
            ["init", "--with-android", "--accept-android-sdk-license"]))
        self.assertIn("android", [step.key for step in init_progress.plan(selected)])
        forwarded = parser.parse_args(["init", *init_ui.selection_to_argv(selected)])
        self.assertTrue(forwarded.with_android and forwarded.accept_android_sdk_license)

    def test_android_only_uses_the_installer_without_desktop_setup(self):
        with patch.object(init_android, "host_key", return_value="Darwin-arm64"), \
                patch.object(init_android, "install_android") as install, \
                patch.object(engine, "run_init_steps") as native:
            self.assertEqual(engine.main(["init", "--android-only", "--accept-android-sdk-license"]), 0)
        self.assertTrue(install.call_args.kwargs["is_license_accepted"])
        native.assert_not_called()

    def test_declining_license_creates_no_sdk_and_never_downloads(self):
        with patch.object(init_android.sys.stdin, "isatty", return_value=False):
            with self.assertRaisesRegex(engine.EngineError, "requires acceptance"):
                init_android.confirm_license(self.sdk, self.pins["sdkLicense"], False, engine)
        self.assertFalse(self.sdk.exists())

    def test_consent_records_only_the_pinned_license_and_preserves_other_acceptance(self):
        pin = self.pins["sdkLicense"]
        path = init_android.license_path(self.sdk, pin)
        path.parent.mkdir(parents=True)
        old_hash = "a" * 40
        path.write_text(old_hash, encoding="utf-8")
        init_android.confirm_license(self.sdk, pin, True, engine)
        init_android.confirm_license(self.sdk, pin, True, engine)
        self.assertEqual(path.read_text(encoding="utf-8").splitlines(), [old_hash, pin["acceptanceSha1"]])

    def test_gui_cannot_display_changed_license_as_the_pinned_agreement(self):
        repository = b'<sdk><license id="android-sdk-license">changed text</license></sdk>'
        with patch.object(init_android.urllib.request, "urlopen", return_value=io.BytesIO(repository)):
            with self.assertRaisesRegex(ValueError, "differs from the setup pin"):
                init_android.load_license_text(self.pins["sdkLicense"])

    def test_setup_reports_missing_packages_even_when_sdkmanager_exits_zero(self):
        with patch.object(init_android, "read_android_pins", return_value=self.pins), \
                patch.object(init_android, "host_key", return_value="Darwin-arm64"), \
                patch.object(init_android, "install_java", return_value=Path("/java")), \
                patch.object(init_android, "install_sdkmanager", return_value=Path("/sdkmanager")), \
                patch.object(init_android, "install_packages") as install:
            with self.assertRaisesRegex(engine.EngineError, f"ndk;{self.pins['ndk']}"):
                init_android.install_android(self.root, engine, is_license_accepted=True)
        self.assertIn("platform-tools", install.call_args.args[4])

    def write_package(self, directory: Path, filename: str, properties: str) -> None:
        marker = directory / filename
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.touch()
        marker.chmod(0o755)
        (directory / "source.properties").write_text(properties, encoding="utf-8")

    def test_repairs_stale_checkout_packages_and_reuses_compatible_ones(self):
        platform_tools = self.sdk / "platform-tools"
        platform = self.sdk / "platforms" / self.pins["platform"]
        build_tools = self.sdk / "build-tools" / self.pins["buildTools"]
        ndk = self.sdk / "ndk" / self.pins["ndk"]
        compiler = "toolchains/llvm/prebuilt/darwin-x86_64/bin/clang++"
        self.write_package(platform_tools, "adb", "Pkg.Revision=34.0.0")
        self.write_package(platform, "android.jar", "AndroidVersion.ApiLevel=36")
        self.write_package(build_tools, "aapt2", "Pkg.Revision=36.0.0")
        self.write_package(ndk, compiler, "Pkg.Revision=28.0.0")
        pending = init_android.missing_packages(self.sdk, self.pins, "Darwin")
        self.assertEqual(pending, {"platform-tools": platform_tools, f"ndk;{self.pins['ndk']}": ndk})

        def install(_root, _sdk, _manager, _java, packages, _engine):
            self.assertEqual(packages, list(pending))
            self.assertFalse(platform_tools.exists())
            self.assertFalse(ndk.exists())
            self.assertTrue((platform / "android.jar").is_file())
            self.assertTrue((build_tools / "aapt2").is_file())
            self.write_package(platform_tools, "adb", "Pkg.Revision=37.1.0")
            self.write_package(ndk, compiler, f"Pkg.Revision = {self.pins['ndk']}")

        with patch.object(init_android, "install_packages", side_effect=install):
            init_android.repair_packages(self.root, self.sdk, Path("/manager"), Path("/java"), pending, engine)
        self.assertEqual(init_android.missing_packages(self.sdk, self.pins, "Darwin"), {})

    def test_failed_repair_restores_the_previous_package_if_sdkmanager_left_no_replacement(self):
        platform_tools = self.sdk / "platform-tools"
        self.write_package(platform_tools, "adb", "Pkg.Revision=34.0.0")
        with patch.object(init_android, "install_packages", side_effect=engine.EngineError("offline")):
            with self.assertRaisesRegex(engine.EngineError, "offline"):
                init_android.repair_packages(self.root, self.sdk, Path("/manager"), Path("/java"),
                                             {"platform-tools": platform_tools}, engine)
        self.assertEqual(android_tools.read_revision(platform_tools), "34.0.0")

    def test_managed_sdk_takes_precedence_over_an_older_external_sdk(self):
        external = self.root / "external-sdk"
        external.mkdir()
        environment = {"ANDROID_HOME": str(external)}
        self.assertEqual(android_tools.resolve_sdk(self.root, self.root, environment), external)
        self.write_package(self.sdk / "platform-tools", "adb", "Pkg.Revision=37.0.3")
        self.assertEqual(android_tools.resolve_sdk(self.root, self.root, environment), self.sdk)

    def test_mixed_setup_copies_compatible_sdk_packages_and_downloads_only_the_rest(self):
        source = self.root / "AndroidStudioSdk"
        sdkmanager = source / "cmdline-tools/latest"
        self.write_package(sdkmanager, "bin/sdkmanager", "Pkg.Revision=21.0")
        (sdkmanager / "lib").mkdir()
        (sdkmanager / "lib/sdkmanager-classpath.jar").touch()
        (sdkmanager / "package.xml").write_text('<localPackage path="cmdline-tools;latest"/>')
        self.write_package(source / "platform-tools", "adb", "Pkg.Revision=37.0.3")
        self.write_package(source / "platforms/android-36", "android.jar", "AndroidVersion.ApiLevel=36")
        self.write_package(source / "build-tools/36.0.0", "aapt2", "Pkg.Revision=35.0.0")
        (source / "platform-tools/adb-alias").symlink_to("adb")

        def install(root, sdk, manager, _java, packages, _engine):
            self.assertEqual(root, self.root)
            self.assertEqual(sdk, self.sdk)
            self.assertEqual(manager, self.sdk / "cmdline-tools/21.0/bin/sdkmanager")
            self.assertEqual(packages, ["build-tools;36.0.0", f"ndk;{self.pins['ndk']}"])
            self.write_package(sdk / "build-tools/36.0.0", "aapt2", "Pkg.Revision=36.0.0")
            self.write_package(sdk / "ndk" / self.pins["ndk"], "toolchains/llvm/prebuilt/darwin-x86_64/bin/clang++",
                               f"Pkg.Revision={self.pins['ndk']}")

        with patch.object(init_android, "host_key", return_value="Darwin-arm64"), \
                patch.object(init_android, "read_android_pins", return_value=self.pins), \
                patch.object(init_android, "existing_sdk_roots", return_value=[source]), \
                patch.object(init_android, "install_java", return_value=Path("/java")), \
                patch.object(init_android, "download_verified") as download, \
                patch.object(init_android, "install_packages", side_effect=install) as download_sdk:
            init_android.install_android(self.root, engine, is_license_accepted=True)
            init_android.install_android(self.root, engine, is_license_accepted=False)
        download.assert_not_called()
        download_sdk.assert_called_once()
        installed_tools = self.sdk / "platform-tools"
        self.assertEqual((installed_tools / "adb-alias").resolve(), installed_tools / "adb")
        (installed_tools / "adb").write_text("Ludus copy", encoding="utf-8")
        self.assertEqual((source / "platform-tools/adb").read_bytes(), b"")
        self.assertEqual(android_tools.read_revision(source / "build-tools/36.0.0"), "35.0.0")
        self.assertTrue((sdkmanager / "package.xml").is_file())
        self.assertFalse((self.sdk / "cmdline-tools/21.0/package.xml").exists())

    def test_external_package_links_never_escape_the_local_copy(self):
        source = self.root / "AndroidStudioSdk/platform-tools"
        self.write_package(source, "adb", "Pkg.Revision=37.0.3")
        (source / "outside").symlink_to("../elsewhere")
        (source.parent / "elsewhere").touch()
        self.sdk.parent.mkdir(parents=True)
        self.assertFalse(init_android.reuse_sdk_packages(self.sdk, self.pins, "Darwin", [source.parent]))
        self.assertFalse((self.sdk / "platform-tools").exists())
        self.assertTrue((source / "outside").exists())

    def test_setup_refuses_a_managed_sdk_link_to_an_external_sdk(self):
        source = self.root / "AndroidStudioSdk"
        source.mkdir()
        self.sdk.parent.mkdir(parents=True)
        self.sdk.symlink_to(source, target_is_directory=True)
        with patch.object(init_android, "host_key", return_value="Darwin-arm64"), \
                patch.object(init_android, "read_android_pins", return_value=self.pins), \
                patch.object(init_android, "download_verified") as download:
            with self.assertRaisesRegex(engine.EngineError, "checkout-local SDK"):
                init_android.install_android(self.root, engine, is_license_accepted=True)
        download.assert_not_called()
        self.assertEqual(list(source.iterdir()), [])

    def test_setup_refuses_a_managed_package_link_to_an_external_sdk(self):
        source = self.root / "AndroidStudioSdk/ndk" / self.pins["ndk"]
        source.mkdir(parents=True)
        destination = self.sdk / "ndk" / self.pins["ndk"]
        destination.parent.mkdir(parents=True)
        destination.symlink_to(source, target_is_directory=True)
        with patch.object(init_android, "host_key", return_value="Darwin-arm64"), \
                patch.object(init_android, "read_android_pins", return_value=self.pins):
            with self.assertRaisesRegex(engine.EngineError, "cannot write through the link"):
                init_android.install_android(self.root, engine, is_license_accepted=True)
        self.assertEqual(list(source.iterdir()), [])

    def test_java_checks_runtime_metadata_as_well_as_executables(self):
        home = self.root / "java"
        (home / "bin").mkdir(parents=True)
        for name in ("java", "keytool", "javac"):
            tool = home / "bin" / name
            tool.touch()
            tool.chmod(0o755)
        pinned = self.pins["jdk"]["version"]
        release = home / "release"
        release.write_text('JAVA_RUNTIME_VERSION="17.0.1+1-LTS"\n', encoding="utf-8")
        self.assertFalse(init_android.is_pinned_java(home, pinned))
        release.write_text(f'JAVA_RUNTIME_VERSION="{pinned}-LTS"\n', encoding="utf-8")
        self.assertTrue(init_android.is_pinned_java(home, pinned))

    def test_setup_reuses_installed_packages(self):
        with patch.object(init_android, "read_android_pins", return_value=self.pins), \
                patch.object(init_android, "host_key", return_value="Darwin-arm64"), \
                patch.object(init_android, "install_java"), \
                patch.object(init_android, "install_sdkmanager"), \
                patch.object(init_android, "missing_packages", return_value={}), \
                patch.object(init_android, "install_packages") as install:
            init_android.install_android(self.root, engine, is_license_accepted=True)
        install.assert_not_called()


if __name__ == "__main__":
    unittest.main()
