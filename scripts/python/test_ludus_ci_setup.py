"""Generated GitHub Actions build/test workflow: content, placement and no-overwrite setup."""
from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from ludus_tools import ci_setup, cli, create, workflow_steps
from ludus_tools.errors import ToolingError
from ludus_tools.release_setup import RELEASE_TOOLS_VENV, setup_release
from ludus_tools.release_setup import workflow_text as release_workflow_text

try:
    import yaml
except ImportError:  # The host-tooling venv does not require PyYAML.
    yaml = None

TOOLS_REF = "a" * 40
OTHER_REF = "b" * 40


def make_project(root: Path, **options) -> Path:
    create.create_project(root, name="game", template_id="minimal", engine_version="0.1.0",
                          preset="linux-clang-development", **options)
    return root


class WorkflowTextTests(unittest.TestCase):
    def test_linux_workflow_builds_release_profile_without_secrets(self):
        text = ci_setup.workflow_text("linux-x64")
        self.assertIn("runs-on: ubuntu-24.04", text)
        self.assertIn("permissions:\n  contents: read\n", text)
        self.assertIn("ludus project build . --profile linux-clang-release", text)
        self.assertIn("ctest --test-dir out/build/linux-clang-release", text)
        self.assertIn(f"CXX: {workflow_steps.pinned_compiler()}", text)
        self.assertIn(workflow_steps.CHECKOUT_STEP, text)
        for forbidden in ("secrets.", "pull_request_target", "BUTLER_API_KEY", "TOKEN"):
            self.assertNotIn(forbidden, text)

    def test_macos_workflow_uses_native_runner_and_homebrew_llvm(self):
        text = ci_setup.workflow_text("macos-arm64")
        self.assertIn("runs-on: macos-15\n", text)
        self.assertIn("--profile macos-clang-release", text)
        self.assertNotIn("apt.llvm.org", text)
        self.assertNotIn("CXX:", text)
        self.assertLess(text.index("brew update --quiet"), text.index("brew install python@3.12"))

    def test_macos_x64_is_refused_without_a_hosted_toolchain(self):
        self.assertEqual(workflow_steps.HOSTED_PLATFORMS, ("linux-x64", "macos-arm64"))
        for platforms in (("macos-x64",), ("linux-x64", "macos-x64")):
            with self.subTest(platforms=platforms), self.assertRaisesRegex(ToolingError, "Intel macOS bottles"):
                ci_setup.workflow_text(*platforms)

    def test_release_and_ci_workflows_share_the_pinned_toolchain(self):
        for platform in workflow_steps.HOSTED_PLATFORMS:
            self.assertIn(workflow_steps.build_prerequisites(platform, ci_setup.CI_TOOLS_VENV),
                          ci_setup.workflow_text(platform))
        self.assertIn(workflow_steps.build_prerequisites("linux-x64", RELEASE_TOOLS_VENV),
                      release_workflow_text("linux-release", "linux", "linux-x64"))
        self.assertIn(workflow_steps.build_prerequisites("macos-arm64", RELEASE_TOOLS_VENV),
                      release_workflow_text("macos-release", "macos", "macos-arm64"))

    def test_missing_sdk_variables_skip_the_toolchain(self):
        text = ci_setup.workflow_text("linux-x64")
        notice, build = text.split("  build-and-test:\n", 1)
        self.assertIn("if: vars.LUDUS_RELEASE_SDK_URL == '' && vars.LUDUS_RELEASE_SDK_SHA256 == ''", notice)
        self.assertIn("::notice", notice)
        self.assertNotIn("apt-get", notice)
        self.assertTrue(build.startswith("    if: vars.LUDUS_RELEASE_SDK_URL != '' || vars.LUDUS_RELEASE_SDK_SHA256 != ''"))

    @unittest.skipIf(yaml is None, "requires PyYAML")
    def test_workflow_is_valid_yaml(self):
        for platform in workflow_steps.HOSTED_PLATFORMS:
            document = yaml.safe_load(ci_setup.workflow_text(platform))
            self.assertEqual(document["permissions"], {"contents": "read"})
            self.assertEqual(set(document["jobs"]), {"sdk-not-configured", "build-and-test"})
            # PyYAML reads the bare `on:` key as boolean true (YAML 1.1).
            self.assertIn("pull_request", document[True])

    def test_rejects_unknown_platform(self):
        with self.assertRaises(ToolingError):
            ci_setup.workflow_text("web")
        with self.assertRaises(ToolingError):
            ci_setup.workflow_text("linux-x64", "web")
        with self.assertRaises(ToolingError):
            ci_setup.workflow_text()

    def test_sdk_install_failure_keeps_its_diagnostic(self):
        text = ci_setup.workflow_text("linux-x64")
        self.assertIn("stdout=subprocess.PIPE", text)
        self.assertNotIn("capture_output", text)


class MultiPlatformWorkflowTests(unittest.TestCase):
    PLATFORMS = ("macos-arm64", "linux-x64", "linux-x64")

    def test_each_platform_gets_a_job_gated_on_its_own_sdk_variables(self):
        text = ci_setup.workflow_text(*self.PLATFORMS)
        self.assertNotIn("  build-and-test:\n", text)
        for platform in workflow_steps.HOSTED_PLATFORMS:
            url, sha = ci_setup.sdk_variables(platform, workflow_steps.HOSTED_PLATFORMS)
            self.assertEqual((url, sha), ("LUDUS_RELEASE_SDK_URL_" + platform.upper().replace("-", "_"),
                                          "LUDUS_RELEASE_SDK_SHA256_" + platform.upper().replace("-", "_")))
            job = text.split(f"  build-and-test-{platform}:\n", 1)[1].split("\n  build-and-test-", 1)[0]
            self.assertTrue(job.startswith(f"    if: vars.{url} != '' || vars.{sha} != ''\n"))
            self.assertIn(f"runs-on: {workflow_steps.runner_for(platform)}\n", job)
            self.assertIn("SDK_URL: ${{ vars." + url + " }}", job)
            self.assertIn("SDK_SHA256: ${{ vars." + sha + " }}", job)
            self.assertIn(workflow_steps.build_prerequisites(platform, ci_setup.CI_TOOLS_VENV), job)
            self.assertNotIn("vars.LUDUS_RELEASE_SDK_URL }}", job)
        self.assertEqual(text.count("  build-and-test-"), 2)
        # Jobs follow NATIVE_PLATFORMS order whatever order the platforms were requested in.
        self.assertLess(text.index("build-and-test-linux-x64:"), text.index("build-and-test-macos-arm64:"))

    def test_notice_reports_each_unconfigured_platform(self):
        text = ci_setup.workflow_text(*self.PLATFORMS)
        notice = text.split("  sdk-not-configured:\n", 1)[1].split("\n  build-and-test-", 1)[0]
        conditions = []
        for platform in workflow_steps.HOSTED_PLATFORMS:
            url, sha = ci_setup.sdk_variables(platform, workflow_steps.HOSTED_PLATFORMS)
            unset = f"vars.{url} == '' && vars.{sha} == ''"
            conditions.append(f"({unset})")
            self.assertIn(f"        if: {unset}\n", notice)
            self.assertIn(f"Set repository variables {url} and {sha} to build and test on {platform}.", notice)
        self.assertTrue(notice.startswith("    if: " + " || ".join(conditions) + "\n"))
        self.assertNotIn("apt-get", notice)

    def test_a_single_platform_keeps_the_shared_release_variables(self):
        self.assertEqual(ci_setup.workflow_text("linux-x64", "linux-x64"), ci_setup.workflow_text("linux-x64"))
        self.assertEqual(ci_setup.sdk_variables("linux-x64", ("linux-x64",)),
                         ("LUDUS_RELEASE_SDK_URL", "LUDUS_RELEASE_SDK_SHA256"))

    @unittest.skipIf(yaml is None, "requires PyYAML")
    def test_multi_platform_workflow_is_valid_yaml(self):
        document = yaml.safe_load(ci_setup.workflow_text(*self.PLATFORMS))
        self.assertEqual(set(document["jobs"]), {"sdk-not-configured", "build-and-test-linux-x64",
                                                 "build-and-test-macos-arm64"})
        self.assertEqual(len(document["jobs"]["sdk-not-configured"]["steps"]), 2)


class RepositoryPlacementTests(unittest.TestCase):
    def test_nested_project_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            repository = Path(temp) / "repository"
            (repository / ".git").mkdir(parents=True)
            with self.assertRaisesRegex(ToolingError, "repository root"):
                ci_setup.require_workflow_root(repository / "games" / "game")
            ci_setup.require_workflow_root(repository)

    def test_linked_worktree_file_counts_as_a_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / ".git").write_text("gitdir: /elsewhere\n")
            self.assertEqual(ci_setup.find_git_root(root / "missing" / "child"), root.resolve())

    def test_describe_repository(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.assertIn("Not a Git repository", ci_setup.describe_repository(root))
            (root / ".git").mkdir()
            (root / ".git/config").write_text("[core]\n\tbare = false\n")
            self.assertIn("No origin remote", ci_setup.describe_repository(root))
            (root / ".git/config").write_text('[remote "origin"]\n\turl = https://gitlab.com/me/game.git\n')
            self.assertIn("not on github.com", ci_setup.describe_repository(root))
            (root / ".git/config").write_text('[remote "origin"]\n\turl = git@github.com:me/game.git\n')
            self.assertIn("runs on GitHub Actions", ci_setup.describe_repository(root))


class SetupTests(unittest.TestCase):
    def test_setup_adds_workflow_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as temp:
            root = make_project(Path(temp) / "game")
            source = (root / "src/main.cpp").read_bytes()
            files = ci_setup.setup_ci(root, tools_ref=TOOLS_REF)
            self.assertEqual(files, [ci_setup.WORKFLOW_RELPATH, workflow_steps.TOOLS_REVISION_RELPATH])
            self.assertEqual((root / workflow_steps.TOOLS_REVISION_RELPATH).read_text(), TOOLS_REF + "\n")
            self.assertEqual((root / ci_setup.WORKFLOW_RELPATH).read_text(), ci_setup.workflow_text("linux-x64"))
            self.assertEqual(source, (root / "src/main.cpp").read_bytes())
            (root / ci_setup.WORKFLOW_RELPATH).write_text("edited\n")
            with self.assertRaisesRegex(ToolingError, "overwrite"):
                ci_setup.setup_ci(root, tools_ref=TOOLS_REF)
            self.assertEqual((root / ci_setup.WORKFLOW_RELPATH).read_text(), "edited\n")
            self.assertEqual([path.name for path in root.iterdir() if path.name.startswith(".ci-setup-")], [])

    def test_setup_requires_an_immutable_tools_commit(self):
        with tempfile.TemporaryDirectory() as temp:
            root = make_project(Path(temp) / "game")
            for ref in (None, "main", "A" * 40):
                with self.subTest(ref=ref), self.assertRaises(ToolingError):
                    ci_setup.setup_ci(root, tools_ref=ref)
            self.assertFalse((root / ".github").exists())

    def test_macos_x64_workflows_are_refused_without_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = make_project(Path(temp) / "game")
            with self.assertRaisesRegex(ToolingError, "Intel macOS bottles"):
                ci_setup.setup_ci(root, platform=["linux-x64", "macos-x64"], tools_ref=TOOLS_REF)
            with self.assertRaisesRegex(ToolingError, "Intel macOS bottles"):
                setup_release(root, platform="macos-x64", tools_ref=TOOLS_REF)
            self.assertFalse((root / ".github").exists())
            self.assertFalse((root / workflow_steps.TOOLS_REVISION_RELPATH).exists())

    def test_setup_shares_the_release_tools_pin(self):
        with tempfile.TemporaryDirectory() as temp:
            root = make_project(Path(temp) / "game")
            setup_release(root, platform="linux-x64", tools_ref=TOOLS_REF)
            self.assertEqual(ci_setup.setup_ci(root, tools_ref=TOOLS_REF), [ci_setup.WORKFLOW_RELPATH])
            other = make_project(Path(temp) / "other")
            ci_setup.setup_ci(other, tools_ref=TOOLS_REF)
            setup_release(other, platform="linux-x64", tools_ref=TOOLS_REF)
            self.assertTrue((other / ".github/workflows/itch-release.yml").is_file())

    def test_setup_refuses_a_different_tools_pin_without_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = make_project(Path(temp) / "game")
            (root / workflow_steps.TOOLS_REVISION_RELPATH).write_text(OTHER_REF + "\n")
            with self.assertRaisesRegex(ToolingError, "different Ludus commit"):
                ci_setup.setup_ci(root, tools_ref=TOOLS_REF)
            self.assertFalse((root / ".github").exists())
            with self.assertRaisesRegex(ToolingError, "different Ludus commit"):
                setup_release(root, platform="linux-x64", tools_ref=TOOLS_REF)

    def test_setup_refuses_a_project_nested_in_a_repository(self):
        with tempfile.TemporaryDirectory() as temp:
            (Path(temp) / ".git").mkdir()
            root = make_project(Path(temp) / "game")
            with self.assertRaisesRegex(ToolingError, "repository root"):
                ci_setup.setup_ci(root, tools_ref=TOOLS_REF)
            self.assertFalse((root / ".github").exists())


class CreationTests(unittest.TestCase):
    def test_create_stages_the_workflow_with_the_project(self):
        with tempfile.TemporaryDirectory() as temp:
            root = make_project(Path(temp) / "game", ci="github", tools_ref=TOOLS_REF)
            self.assertEqual((root / ci_setup.WORKFLOW_RELPATH).read_text(), ci_setup.workflow_text("linux-x64"))
            self.assertEqual((root / workflow_steps.TOOLS_REVISION_RELPATH).read_text(), TOOLS_REF + "\n")

    def test_create_validates_ci_options_before_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            (Path(temp) / "repository/.git").mkdir(parents=True)
            for destination, options in (
                (Path(temp) / "repository/game", {"ci": "github", "tools_ref": TOOLS_REF}),
                (Path(temp) / "no-ref", {"ci": "github"}),
                (Path(temp) / "no-ci", {"tools_ref": TOOLS_REF}),
                (Path(temp) / "gitlab", {"ci": "gitlab", "tools_ref": TOOLS_REF}),
            ):
                with self.subTest(destination=destination.name), self.assertRaises(ToolingError):
                    make_project(destination, **options)
                self.assertFalse(destination.exists())

    def test_cli_create_and_init(self):
        with tempfile.TemporaryDirectory() as temp:
            created = Path(temp) / "created"
            with contextlib.redirect_stdout(io.StringIO()):
                code = cli.main(["project", "create", str(created), "--name", "game", "--engine", "0.1.0",
                                 "--profile", "linux-clang-development", "--ci", "github", "--tools-ref", TOOLS_REF])
            self.assertEqual(code, 0)
            self.assertTrue((created / ci_setup.WORKFLOW_RELPATH).is_file())
            existing = make_project(Path(temp) / "existing")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = cli.main(["--json", "project", "ci", "init", str(existing), "--tools-ref", TOOLS_REF])
            self.assertEqual(code, 0)
            result = json.loads(output.getvalue())
            self.assertEqual(result["files"], [ci_setup.WORKFLOW_RELPATH, workflow_steps.TOOLS_REVISION_RELPATH])
            self.assertIn("Not a Git repository", result["repository"])
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertNotEqual(cli.main(["project", "ci", "init", str(existing), "--tools-ref", TOOLS_REF]), 0)

    def test_cli_init_accepts_several_platforms(self):
        with tempfile.TemporaryDirectory() as temp:
            root = make_project(Path(temp) / "game")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = cli.main(["project", "ci", "init", str(root), "--tools-ref", TOOLS_REF,
                                 "--platform", "macos-arm64", "--platform", "linux-x64"])
            self.assertEqual(code, 0)
            self.assertEqual((root / ci_setup.WORKFLOW_RELPATH).read_text(),
                             ci_setup.workflow_text("linux-x64", "macos-arm64"))
            self.assertIn("LUDUS_RELEASE_SDK_URL_LINUX_X64 and LUDUS_RELEASE_SDK_SHA256_LINUX_X64", output.getvalue())
            self.assertIn("LUDUS_RELEASE_SDK_URL_MACOS_ARM64 and LUDUS_RELEASE_SDK_SHA256_MACOS_ARM64", output.getvalue())


if __name__ == "__main__":
    unittest.main()
