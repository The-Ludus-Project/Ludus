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
        self.root = Path(temporary.name)
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

    def test_setup_reuses_installed_packages(self):
        with patch.object(init_android, "read_android_pins", return_value=self.pins), \
                patch.object(init_android, "host_key", return_value="Darwin-arm64"), \
                patch.object(init_android, "install_java"), \
                patch.object(init_android, "install_sdkmanager"), \
                patch.object(init_android, "missing_packages", return_value=[]), \
                patch.object(init_android, "install_packages") as install:
            init_android.install_android(self.root, engine, is_license_accepted=True)
        install.assert_not_called()


if __name__ == "__main__":
    unittest.main()
