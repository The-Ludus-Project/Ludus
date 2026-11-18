"""Generated GitHub Actions build/test workflow: content, placement and no-overwrite setup."""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from ludus_tools import ci_setup, cli, create, workflow_steps
from ludus_tools.errors import ToolingError
from ludus_tools.release_setup import setup_release
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
    def test_linux_workflow_provisions_and_uses_the_pin_without_secrets(self):
        text = ci_setup.workflow_text("linux-x64", preset="linux-clang-development")
        self.assertIn("runs-on: ubuntu-24.04", text)
        self.assertIn("permissions:\n  contents: read\n", text)
        self.assertIn("LUDUS_CI_PROFILE: linux-clang-development", text)
        self.assertIn("repository: The-Ludus-Project/Ludus", text)
        self.assertIn("ref: ${{ steps.ludus-revision.outputs.revision }}", text)
        self.assertIn("revision=$(cat config/ludus-tools-revision.txt)", text)
        self.assertLess(text.index("./scripts/shader-probe bootstrap"),
                        text.index('./scripts/install-sdk "$LUDUS_CI_PROFILE"'))
        self.assertIn('ludus project repair . --profile "$LUDUS_CI_PROFILE"', text)
        self.assertIn('--tools "$GITHUB_WORKSPACE/out/ludus-ci-engine" --sdk "$LUDUS_CI_SDK_PREFIX"', text)
        self.assertIn(workflow_steps.CHECKOUT_STEP, text)
        # Only the digest-checked archive download gets the built-in read-only job token.
        self.assertEqual(text.count("GH_TOKEN: ${{ github.token }}\n"), 1)
        for forbidden in ("secrets.", "pull_request_target", "BUTLER_API_KEY", "TOKEN"):
            self.assertNotIn(forbidden, text.replace("GH_TOKEN: ${{ github.token }}\n", ""))

    def test_macos_workflow_builds_selected_flavor(self):
        for flavor in ("debug", "development", "release"):
            with self.subTest(flavor=flavor):
                preset = f"macos-clang-{flavor}"
                text = ci_setup.workflow_text("macos-arm64", preset=preset)
                self.assertIn("runs-on: macos-15\n", text)
                self.assertIn(f"LUDUS_CI_PROFILE: {preset}", text)
                self.assertIn(f"vars.LUDUS_{flavor.upper()}_SDK_URL", text)

    def test_macos_x64_is_refused_without_a_hosted_toolchain(self):
        self.assertEqual(workflow_steps.HOSTED_PLATFORMS, ("linux-x64", "macos-arm64"))
        for platforms in (("macos-x64",), ("linux-x64", "macos-x64")):
            with self.subTest(platforms=platforms), self.assertRaisesRegex(ToolingError, "Intel macOS bottles"):
                ci_setup.workflow_text(*platforms, preset="macos-clang-development")

    @unittest.skipIf(yaml is None, "requires PyYAML")
    def test_workflow_keeps_the_build_job_and_selects_one_sdk_step(self):
        document = yaml.safe_load(ci_setup.workflow_text("linux-x64", preset="linux-clang-release"))
        self.assertEqual(document["permissions"], {"contents": "read"})
        self.assertEqual(set(document["jobs"]), {"build-and-test"})
        # PyYAML reads the bare on key as boolean true (YAML 1.1).
        self.assertIn("pull_request", document[True])
        job = document["jobs"]["build-and-test"]
        self.assertNotIn("if", job)
        steps = {step.get("name"): step for step in job["steps"]}
        self.assertEqual(steps["Prepare pinned Ludus tooling"]["working-directory"], "out/ludus-ci-engine")
        self.assertEqual(steps["Acquire digest-pinned Release SDK"]["if"],
                         "vars.LUDUS_RELEASE_SDK_URL != '' || vars.LUDUS_RELEASE_SDK_SHA256 != ''")
        fallback = steps["Build and install the pinned Ludus SDK"]
        self.assertEqual(fallback["if"], "vars.LUDUS_RELEASE_SDK_URL == '' && vars.LUDUS_RELEASE_SDK_SHA256 == ''")
        self.assertEqual(fallback["working-directory"], "out/ludus-ci-engine")
        self.assertNotIn("if", steps["Build and test the game"])

    def test_rejects_unknown_platform_or_preset(self):
        for platforms in (("web",), ("linux-x64", "web"), ()):
            with self.subTest(platforms=platforms), self.assertRaises(ToolingError):
                ci_setup.workflow_text(*platforms, preset="linux-clang-development")
        with self.assertRaisesRegex(ToolingError, "unsupported CI preset"):
            ci_setup.workflow_text("linux-x64", preset="windows-msvc-development")

    def test_sdk_install_failure_keeps_its_diagnostic(self):
        text = ci_setup.workflow_text("linux-x64", preset="linux-clang-development")
        self.assertIn("stdout=subprocess.PIPE", text)
        self.assertNotIn("capture_output", text)


@unittest.skipIf(os.name == "nt", "runs POSIX stand-ins for gh and ludus")
class ReleaseSdkDownloadTests(unittest.TestCase):
    """Run the generated SDK step's script against stand-in ``gh`` and ``ludus`` commands."""

    REPOSITORY = "owner/game"
    ARCHIVE = b"sdk archive"

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.runner_temp = self.root / "runner"
        self.runner_temp.mkdir()
        self.github_env = self.root / "github-env"
        self.github_env.write_text("")
        # gh records its arguments and writes the archive as the requested asset.
        self.write_command("gh", f"""
            import json, pathlib, sys
            arguments = sys.argv[1:]
            pathlib.Path({str(self.root / "gh-argv.json")!r}).write_text(json.dumps(arguments))
            directory = pathlib.Path(arguments[arguments.index("--dir") + 1])
            directory.mkdir(parents=True, exist_ok=True)
            (directory / arguments[arguments.index("--pattern") + 1]).write_bytes({self.ARCHIVE!r})
        """)
        self.write_command("ludus", """
            import json, sys
            print(json.dumps({"installed": {"prefix": "/sdk/" + sys.argv[sys.argv.index("--digest") + 1]}}))
        """)

    def write_command(self, name: str, body: str) -> None:
        path = self.bin / name
        path.write_text(f"#!{sys.executable}\n" + textwrap.dedent(body))
        path.chmod(0o755)

    def run_step(self, url: str, digest: str) -> subprocess.CompletedProcess:
        step = workflow_steps.release_sdk_step("Acquire SDK", "invalid SDK variables")
        script = textwrap.dedent(step.split("<<'PYTHON'\n", 1)[1].split("          PYTHON\n", 1)[0])
        env = {"PATH": str(self.bin) + os.pathsep + os.environ.get("PATH", os.defpath), "SDK_URL": url,
               "SDK_SHA256": digest, "GITHUB_REPOSITORY": self.REPOSITORY, "RUNNER_TEMP": str(self.runner_temp),
               "GITHUB_ENV": str(self.github_env), "GH_TOKEN": "job-token",
               # Direct downloads go through an unreachable proxy so tests never use the network.
               "HTTPS_PROXY": "http://127.0.0.1:1", "https_proxy": "http://127.0.0.1:1"}
        return subprocess.run([sys.executable, "-I", "-"], input=script, env=env, text=True, capture_output=True,
                              timeout=60)

    def test_step_passes_the_job_token_to_gh(self):
        step = workflow_steps.release_sdk_step("Acquire SDK", "invalid SDK variables")
        self.assertIn("          GH_TOKEN: ${{ github.token }}\n        run: |\n", step)
        release_text = release_workflow_text("macos-release", "macos", "macos-arm64")
        self.assertIn(step.split("SystemExit('invalid SDK variables')\n", 1)[1], release_text)
        ci_text = ci_setup.workflow_text("linux-x64", preset="linux-clang-development")
        self.assertIn("          GH_TOKEN: ${{ github.token }}\n", ci_text)
        self.assertIn("output.write('LUDUS_CI_SDK_PREFIX=' + prefix", ci_text)

    def test_own_release_asset_is_downloaded_with_gh_and_digest_checked(self):
        digest = hashlib.sha256(self.ARCHIVE).hexdigest()
        url = f"https://github.com/{self.REPOSITORY}/releases/download/ludus-sdk-1/ludus-sdk.tar"
        result = self.run_step(url, digest)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((self.root / "gh-argv.json").read_text()),
                         ["release", "download", "ludus-sdk-1", "--repo", self.REPOSITORY, "--pattern",
                          "ludus-sdk.tar", "--dir", str(self.runner_temp / "ludus-release-sdk-download"), "--clobber"])
        self.assertEqual((self.runner_temp / "ludus-release-sdk.tar").read_bytes(), self.ARCHIVE)
        self.assertEqual(self.github_env.read_text(), f"LUDUS_RELEASE_SDK_PREFIX=/sdk/{digest}\n")

    def test_own_release_asset_with_another_digest_is_not_installed(self):
        url = f"https://github.com/{self.REPOSITORY}/releases/download/ludus-sdk-1/ludus-sdk.tar"
        result = self.run_step(url, "0" * 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SDK archive digest mismatch", result.stderr)
        self.assertEqual(self.github_env.read_text(), "")

    def test_other_urls_are_downloaded_directly_without_gh(self):
        digest = hashlib.sha256(self.ARCHIVE).hexdigest()
        # Another repository's release, and a lookalike repository name, never reach gh and its token.
        for url in ("https://github.com/other/game/releases/download/ludus-sdk-1/ludus-sdk.tar",
                    f"https://github.com/{self.REPOSITORY}-fork/releases/download/ludus-sdk-1/ludus-sdk.tar"):
            with self.subTest(url=url):
                result = self.run_step(url, digest)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("urlopen", result.stderr)
                self.assertFalse((self.root / "gh-argv.json").exists())
                self.assertEqual(self.github_env.read_text(), "")

    def test_invalid_variables_fail_before_any_download(self):
        digest = hashlib.sha256(self.ARCHIVE).hexdigest()
        for url, checksum in (("http://example.invalid/sdk.tar", digest),
                              ("", digest), ("https://example.invalid/sdk.tar", "")):
            with self.subTest(url=url, checksum=checksum):
                result = self.run_step(url, checksum)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("invalid SDK variables", result.stderr)
                self.assertFalse((self.root / "gh-argv.json").exists())



class MultiPlatformWorkflowTests(unittest.TestCase):
    PLATFORMS = ("macos-arm64", "linux-x64", "linux-x64")

    def test_each_platform_gets_the_selected_flavor_and_its_own_sdk_variables(self):
        text = ci_setup.workflow_text(*self.PLATFORMS, preset="macos-clang-development")
        self.assertNotIn("  build-and-test:\n", text)
        for platform in workflow_steps.HOSTED_PLATFORMS:
            suffix = platform.upper().replace("-", "_")
            url = "LUDUS_DEVELOPMENT_SDK_URL_" + suffix
            sha = "LUDUS_DEVELOPMENT_SDK_SHA256_" + suffix
            job = text.split(f"  build-and-test-{platform}:\n", 1)[1].split("\n  build-and-test-", 1)[0]
            family = platform.split("-", 1)[0]
            self.assertIn(f"LUDUS_CI_PROFILE: {family}-clang-development", job)
            self.assertIn(f"runs-on: {workflow_steps.runner_for(platform)}", job)
            self.assertIn("SDK_URL: ${{ vars." + url + " }}", job)
            self.assertIn("SDK_SHA256: ${{ vars." + sha + " }}", job)
            self.assertIn(f"if: vars.{url} == '' && vars.{sha} == ''", job)
        self.assertEqual(text.count("  build-and-test-"), 2)
        self.assertLess(text.index("build-and-test-linux-x64:"), text.index("build-and-test-macos-arm64:"))

    def test_single_release_platform_keeps_the_shared_archive_variables(self):
        preset = "linux-clang-release"
        self.assertEqual(ci_setup.workflow_text("linux-x64", "linux-x64", preset=preset),
                         ci_setup.workflow_text("linux-x64", preset=preset))
        self.assertEqual(ci_setup.sdk_variables("linux-x64", ("linux-x64",), "Release"),
                         ("LUDUS_RELEASE_SDK_URL", "LUDUS_RELEASE_SDK_SHA256"))


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
            self.assertEqual((root / ci_setup.WORKFLOW_RELPATH).read_text(), ci_setup.workflow_text("linux-x64", preset="linux-clang-development"))
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
            self.assertEqual((root / ci_setup.WORKFLOW_RELPATH).read_text(), ci_setup.workflow_text("linux-x64", preset="linux-clang-development"))
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
                             ci_setup.workflow_text("linux-x64", "macos-arm64", preset="linux-clang-development"))
            self.assertIn("CI builds the selected SDK from the commit", output.getvalue())


if __name__ == "__main__":
    unittest.main()
