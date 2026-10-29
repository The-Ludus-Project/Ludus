"""Exercise locked dependency preparation and bounded analysis failure handling."""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

import engine
import web_build

LLVM_MAJOR = engine.pinned_llvm_major(engine.repo_root())
CLANG_TIDY = engine.pinned_llvm_tool(engine.repo_root(), "clang-tidy")


class WindowsClosureTests(unittest.TestCase):
    """The Windows configure adds a fixed module list; every Ludus:: link in it must resolve."""
    ROOT = Path(__file__).resolve().parents[2]

    def windows_modules(self) -> list[str]:
        top = (self.ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        branch = re.search(r'if\(CMAKE_SYSTEM_NAME STREQUAL "Windows"\)(.*?)\nendif\(\)', top, re.S)
        self.assertIsNotNone(branch, "Windows configure branch not found")
        modules = re.search(r"foreach\(module IN ITEMS ([^)]*)\)", branch.group(1))
        self.assertIsNotNone(modules, "Windows module list not found")
        # FoundationBase is added before the platform branch; the Direct3D 12
        # closure adds Input, Platform and the RHI after the Foundation list.
        foundation = [f"modules/foundation/{name}" for name in ["base", *modules.group(1).split()]]
        return [*foundation, *re.findall(r"add_subdirectory\((modules/[\w/]+)\)", branch.group(1))]

    def test_windows_module_list_is_closed_over_its_links(self):
        defined = set()
        referenced = {}
        for module in self.windows_modules():
            text = (self.ROOT / module / "CMakeLists.txt").read_text(encoding="utf-8")
            defined.update(re.findall(r"add_library\((Ludus::\w+) ALIAS", text))
            referenced[module] = set(re.findall(r"Ludus::\w+", text))
        for module, targets in referenced.items():
            self.assertEqual(targets - defined, set(),
                             f"{module} links targets the Windows configure does not add")

    def test_windows_closure_includes_the_direct3d12_backends(self):
        modules = self.windows_modules()
        for module in ("modules/input", "modules/platform", "modules/graphics/rhi"):
            self.assertIn(module, modules)
        # Dependencies are configured first: Platform links Input, and the RHI links Platform.
        self.assertLess(modules.index("modules/input"), modules.index("modules/platform"))
        self.assertLess(modules.index("modules/platform"), modules.index("modules/graphics/rhi"))


class ToolchainPinTests(unittest.TestCase):
    """Copies of the LLVM pin that cannot read config/tool_versions.json must agree with it."""
    ROOT = Path(__file__).resolve().parents[2]

    def test_llvm_tool_minimums_share_the_pinned_major(self):
        minimum = engine.load_tool_versions(self.ROOT)["minimum"]
        for tool in ("clang", "lld", "clang-format", "clang-tidy"):
            self.assertEqual(engine.version_tuple(minimum[tool])[0], LLVM_MAJOR, tool)

    def test_redistributable_tooling_mirrors_the_pin(self):
        from ludus_tools import toolchain

        self.assertEqual(toolchain.PINNED_LLVM_MAJOR, LLVM_MAJOR)

    def test_conan_profiles_and_example_presets_use_the_pinned_compiler(self):
        for profile in sorted((self.ROOT / "config/conan/profiles").glob("*-clang-*")):
            with self.subTest(profile=profile.name):
                self.assertIn(f"\ncompiler.version={LLVM_MAJOR}\n", profile.read_text())
        compilers = set()
        for presets in sorted((self.ROOT / "examples").glob("*/CMakePresets.json")):
            for preset in json.loads(presets.read_text()).get("configurePresets", []):
                compiler = preset.get("cacheVariables", {}).get("CMAKE_CXX_COMPILER", "")
                if compiler.startswith("clang++-"):
                    compilers.add(compiler)
        self.assertEqual(compilers, {f"clang++-{LLVM_MAJOR}"})

    def test_generated_project_files_and_setup_follow_a_changed_pin(self):
        from ludus_tools import project_setup, release_setup, templates, toolchain, workflow_steps
        from ludus_tools.errors import ToolingError

        with patch.object(toolchain, "PINNED_LLVM_MAJOR", 23), patch.object(workflow_steps, "PINNED_LLVM_MAJOR", 23):
            linux = release_setup.workflow_text("linux-release", "game:linux", "linux-x64")
            macos = release_setup.workflow_text("macos-release", "game:mac", "macos-arm64")
            rendered = templates.render_files(templates.get_template("minimal"),
                                              templates.ProjectInputs("Game", "game", "1", []))
        self.assertIn("clang-23 lld-23", linux)
        self.assertIn("Suites: llvm-toolchain-noble-23\\n", linux)
        self.assertIn(f'[[ "$fingerprint" == {toolchain.LLVM_APT_KEY_FINGERPRINT} ]]', linux)
        self.assertNotIn("apt.llvm.org", macos)
        self.assertNotIn("LLVM_APT_KEY_TOKEN", linux)
        self.assertIn("brew install python@3.12 llvm@23\n", macos)
        self.assertIn('"$(brew --prefix llvm@23)"', macos)
        presets = next(item.content for item in rendered if item.relpath == "CMakePresets.json")
        self.assertIn('"CMAKE_CXX_COMPILER": "clang++-23"', presets)
        self.assertNotIn("LLVM_MAJOR_TOKEN", linux + macos)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "config").mkdir()
            (root / "config/tool_versions.json").write_text(json.dumps(
                {"managed": {"cmake": "3.29.6", "ninja": "1.11.1.3"}, "minimum": {"clang": "23.0.0"}}))
            configured = {"cmake": Path("cmake"), "ninja": Path("ninja"), "compiler": Path("clang++")}

            def runner(compiler_version):
                replies = {"cmake": "cmake version 3.29.6", "ninja": "1.11.1",
                           "clang++": f"clang version {compiler_version}"}
                return lambda command, **_: replies[command[0]]

            project_setup.verify_tool_versions(configured, root, runner("23.1.4"), {})
            with self.assertRaisesRegex(ToolingError, "pinned version 23"):
                project_setup.verify_tool_versions(configured, root, runner("18.1.8"), {})

    def test_ci_and_devcontainer_install_the_pinned_llvm_from_its_apt_source(self):
        import re

        files = [*sorted((self.ROOT / ".github").rglob("*.yml")), self.ROOT / ".devcontainer/Dockerfile"]
        tools = r"(?:clang|clang\+\+|clang-format|clang-tidy|clang-tools|lld|llvm|libclang-rt)"
        versioned = re.compile(rf"\b{tools}[-@](\d+)\b")
        apt_package = re.compile(rf"(?<![/\w-]){tools}-\d+\b")
        for path in files:
            text = path.read_text()
            with self.subTest(path=path.relative_to(self.ROOT)):
                self.assertLessEqual({int(major) for major in versioned.findall(text)}, {LLVM_MAJOR})
                # Ubuntu 24.04 does not package the pinned LLVM.
                if "apt-get" in text and apt_package.search(text):
                    self.assertIn("llvm_apt_source.py", text)

    def test_windows_lanes_install_the_same_pinned_llvm(self):
        import re

        pins = set()
        for name in ("threading-platforms.yml", "windows-direct3d12.yml"):
            with self.subTest(workflow=name):
                workflow = (self.ROOT / ".github/workflows" / name).read_text()
                version = re.search(r"LLVM_WINDOWS_VERSION: (\d+)\.(\d+)\.(\d+)\n", workflow)
                digest = re.search(r"LLVM_WINDOWS_SHA256: ([0-9a-f]{64})\n", workflow)
                self.assertIsNotNone(version)
                self.assertIsNotNone(digest)
                self.assertEqual(int(version.group(1)), LLVM_MAJOR)
                self.assertIn("Get-FileHash -Algorithm SHA256", workflow)
                pins.add((version.group(0), digest.group(1)))
        self.assertEqual(len(pins), 1, "Windows lanes must share one LLVM installer pin")

    def test_committed_llvm_apt_key_matches_the_published_fingerprint(self):
        import llvm_apt_source

        from ludus_tools import toolchain

        key = (self.ROOT / llvm_apt_source.KEY_SOURCE).read_text(encoding="ascii")
        self.assertEqual(llvm_apt_source.openpgp_fingerprint(key), llvm_apt_source.KEY_FINGERPRINT)
        self.assertEqual(toolchain.LLVM_APT_KEY_FINGERPRINT, llvm_apt_source.KEY_FINGERPRINT)
        tampered = key.replace(key.splitlines()[3], key.splitlines()[4], 1)
        self.assertNotEqual(llvm_apt_source.openpgp_fingerprint(tampered), llvm_apt_source.KEY_FINGERPRINT)

    def test_llvm_apt_source_registers_only_the_pinned_suite_with_the_committed_key(self):
        import llvm_apt_source

        staged = {}

        def record(command, **_):
            if command[-1] == str(llvm_apt_source.SOURCE_LIST):
                staged["source"] = Path(command[-2]).read_text()
            return subprocess.CompletedProcess(command, 0)

        with patch.object(llvm_apt_source.subprocess, "run", side_effect=record) as run, \
                contextlib.redirect_stdout(io.StringIO()):
            llvm_apt_source.register(self.ROOT, "noble", LLVM_MAJOR, ["sudo"])
        commands = [call.args[0] for call in run.call_args_list]
        self.assertTrue(all(command[0] == "sudo" for command in commands))
        self.assertIn(["sudo", "install", "-m", "0644", str(self.ROOT / llvm_apt_source.KEY_SOURCE),
                       str(llvm_apt_source.KEYRING)], commands)
        self.assertIn(f"Suites: llvm-toolchain-noble-{LLVM_MAJOR}\n", staged["source"])
        self.assertIn(f"Signed-By: {llvm_apt_source.KEYRING}\n", staged["source"])
        with self.assertRaisesRegex(llvm_apt_source.SourceError, "codename"):
            llvm_apt_source.register(self.ROOT, "", LLVM_MAJOR, [])

    def test_ubuntu_setup_adds_the_llvm_source_only_when_the_archive_lacks_llvm(self):
        import llvm_apt_source

        clang = engine.AptPackageGroup("Clang", (f"clang-{LLVM_MAJOR}",))
        git = engine.AptPackageGroup("Git", ("git",))
        for missing, registered in (([clang], True), ([git], False)):
            with self.subTest(missing=missing[0].purpose), \
                    patch.object(engine, "configure_system_tool_shims"), \
                    patch.object(engine, "system_prerequisites_need_install", return_value=True), \
                    patch.object(engine, "host_supports_apt_install", return_value=True), \
                    patch.object(engine, "host_is_ubuntu", return_value=True), \
                    patch.object(engine, "sudo_command_prefix", return_value=["sudo"]), \
                    patch.object(engine, "required_ubuntu_package_groups", return_value=missing), \
                    patch.object(engine, "enable_ubuntu_universe"), \
                    patch.object(engine, "run"), \
                    patch.object(engine, "print_missing_apt_packages"), \
                    patch.object(engine, "resolve_apt_package_groups", return_value=([], missing)), \
                    patch.object(llvm_apt_source, "os_release_codename", return_value="noble"), \
                    patch.object(llvm_apt_source, "register") as register, \
                    contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(engine.EngineError):
                    engine.install_ubuntu_system_prerequisites(self.ROOT, engine.load_tool_versions(self.ROOT))
                self.assertEqual(register.called, registered)
                if registered:
                    register.assert_called_once_with(self.ROOT, "noble", LLVM_MAJOR, ["sudo"])

    def test_ubuntu_sources_mirror_file_form_counts_as_ubuntu(self):
        source = """Types: deb
URIs: mirror+file:/etc/apt/apt-mirrors.txt
Suites: noble noble-updates noble-backports
Components: main universe restricted multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
"""
        self.assertTrue(engine.ubuntu_stanza_has_component(source, "universe"))
        with tempfile.TemporaryDirectory() as temporary:
            source_path = Path(temporary) / "ubuntu.sources"
            source_path.write_text(source, encoding="utf-8")
            with patch.object(engine, "ubuntu_sources_file", return_value=source_path):
                self.assertTrue(engine.ubuntu_apt_component_enabled("universe"))

    def test_missing_clang_pin_is_an_explicit_error(self):
        with self.assertRaisesRegex(engine.EngineError, "minimum.clang"):
            engine.llvm_major_version({"minimum": {"clang": ""}})


class ToolchainBootstrapTests(unittest.TestCase):
    def test_macos_consumers_keep_the_configured_homebrew_compiler(self):
        import re

        root = Path(__file__).resolve().parents[2]
        workflow = (root / ".github/workflows/ci.yml").read_text()
        jobs = re.split(r"(?m)^  [a-z][a-z0-9_-]*:\n", workflow)[1:]
        checked = 0
        for job in jobs:
            if re.search(r"runs-on: macos-", job) and "-DCMAKE_CXX_COMPILER=" in job:
                self.assertNotIn("/usr/bin/clang++-", job)
                self.assertIn('-DCMAKE_CXX_COMPILER="$PWD/out/host-tools/bin/clang++"', job)
                checked += 1
        self.assertGreater(checked, 0)

    def test_trusted_workflow_installers_follow_the_checked_out_source_pin(self):
        root = Path(__file__).resolve().parents[2]
        installers = (
            ("ci.yml", "Install pinned formatter and GUI test dependencies", ("clang-format", "clang-tidy")),
            ("webgpu-probe.yml", "Prepare host tools and pinned SDK", ("clang", "clang-format", "clang-tidy")),
            ("wiki.yml", "Install native tools for precompiled web samples", ("clang", "lld")),
            ("project-sdk.yml", "Install native package inspection tools", ("clang",)),
        )
        for workflow, step_name, packages in installers:
            chunks = (root / ".github/workflows" / workflow).read_text().split(f"      - name: {step_name}\n")[1:]
            self.assertTrue(chunks, step_name)
            for index, chunk in enumerate(chunks):
                body = []
                for line in chunk.split("        run: |\n", 1)[1].splitlines():
                    if line.strip() and not line.startswith("          "):
                        break
                    body.append(line[10:])
                # The browser step continues with SDK downloads after host tools.
                script = "\n".join(body).split("python3 - <<", 1)[0]
                for major in (18, 23):
                    with self.subTest(workflow=workflow, step=index, major=major), \
                            tempfile.TemporaryDirectory() as temporary:
                        checkout = Path(temporary)
                        (checkout / "config").mkdir()
                        (checkout / "config/tool_versions.json").write_text(json.dumps(
                            {"minimum": {"clang": f"{major}.1.3"}}))
                        binaries = checkout / "bin"
                        binaries.mkdir()
                        (binaries / "python3").symlink_to(sys.executable)
                        sudo = binaries / "sudo"
                        sudo.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$APT_LOG"\n')
                        sudo.chmod(0o755)
                        if major == 23:
                            helper = checkout / "scripts/python/llvm_apt_source.py"
                            helper.parent.mkdir(parents=True)
                            helper.write_text('import os\nfrom pathlib import Path\n'
                                              'with Path(os.environ["APT_LOG"]).open("a") as stream:\n'
                                              '    stream.write("register-source\\n")\n')
                        log = checkout / "apt.log"
                        github_environment = checkout / "github.env"
                        environment = {**os.environ, "PATH": str(binaries) + os.pathsep + os.defpath,
                                       "APT_LOG": str(log), "GITHUB_ENV": str(github_environment)}
                        subprocess.run(["/bin/bash", "-e", "-c", script], cwd=checkout, env=environment,
                                       check=True, capture_output=True, text=True)
                        commands = log.read_text().splitlines()
                        installed = next(line.split() for line in commands if line.startswith("apt-get install "))
                        for package in packages:
                            self.assertIn(f"{package}-{major}", installed)
                            self.assertNotIn(f"{package}-{23 if major == 18 else 18}", installed)
                        self.assertEqual("register-source" in commands, major == 23)
                        if major == 23:
                            self.assertEqual(commands[0], "register-source")
                        if workflow == "ci.yml":
                            self.assertIn(f"LUDUS_CI_LLVM_MAJOR={major}\n", github_environment.read_text())


class NativePrerequisiteTests(unittest.TestCase):
    ROOT = Path(__file__).resolve().parents[2]

    def test_rendered_editor_setup_installs_software_vulkan_before_initialization(self):
        action = (self.ROOT / ".github/actions/native-setup/action.yml").read_text()
        step = action.split("    - name: Install native prerequisites\n", 1)[1]
        command = step.split("      run: |\n", 1)[1].split("    - name:", 1)[0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, body in (("timeout", 'shift 3; exec "$@"'), ("sudo", 'exec "$@"'),
                               ("apt-get", 'printf "%s\n" "$@"'), ("python3", 'printf "python3 %s\n" "$*"')):
                tool = root / name
                tool.write_text("#!/bin/sh\n" + body + "\n")
                tool.chmod(0o755)
            for editor in ("false", "true"):
                environment = {**os.environ, "PATH": str(root), "WITH_EDITOR": editor}
                result = subprocess.run(["/bin/bash", "-e", "-c", command], env=environment,
                                        capture_output=True, text=True, check=True)
                arguments = result.stdout.splitlines()
                # The pinned LLVM suite is registered before apt resolves clang-<major>.
                self.assertEqual(arguments[0], "python3 scripts/python/llvm_apt_source.py")
                self.assertIn(f"clang-{LLVM_MAJOR}", arguments)
                for package in ("qt6-base-dev", "qt6-declarative-dev", "qt6-wayland", "libvulkan1", "mesa-vulkan-drivers"):
                    self.assertEqual(package in arguments, editor == "true", package)

    @unittest.skipUnless(shutil.which("timeout"), "GNU timeout is provided by the Linux CI runner")
    def test_silent_package_update_has_a_wall_clock_deadline(self):
        action = (self.ROOT / ".github/actions/native-setup/action.yml").read_text()
        command = next(line.strip() for line in action.splitlines()
                       if "timeout --signal=TERM" in line and "update -qq" in line)
        command = command.replace("5m", "0.1s").replace("--kill-after=30s", "--kill-after=0.1s")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, body in (("sudo", 'exec "$@"'), ("apt-get", "exec /bin/sleep 30")):
                tool = root / name
                tool.write_text("#!/bin/sh\n" + body + "\n")
                tool.chmod(0o755)
            environment = {**os.environ, "PATH": str(root) + os.pathsep + os.environ["PATH"]}
            result = subprocess.run(["/bin/bash", "-c", "apt_options=();\n" + command],
                                    env=environment, capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 124, result.stdout + result.stderr)

    def test_macos_cache_launchers_survive_restricted_consumer_path(self):
        action = (self.ROOT / ".github/actions/macos-setup/action.yml").read_text()
        command = action.split("    - name: Configure compiler caching\n", 1)[1]
        command = command.split("      run: |\n", 1)[1].split("    - name:", 1)[0]
        with tempfile.TemporaryDirectory(prefix="cache launcher ") as temporary:
            root = Path(temporary)
            tool = root / "ccache"
            tool.write_text('#!/bin/sh\ncase "$1" in --max-size=*|--zero-stats) exit 0;; esac\nexec "$@"\n')
            tool.chmod(0o755)
            environment_file = root / "github-env"
            environment = {**os.environ, "PATH": str(root), "GITHUB_ENV": str(environment_file),
                           "GITHUB_WORKSPACE": str(root)}
            subprocess.run(["/bin/bash", "-e", "-c", command], env=environment, check=True)
            exported = dict(line.split("=", 1) for line in environment_file.read_text().splitlines())
            for language in ("C", "CXX", "OBJCXX"):
                launcher = exported[f"CMAKE_{language}_COMPILER_LAUNCHER"]
                self.assertEqual(launcher, str(tool))
                # Release/relocation tests deliberately exclude Homebrew's bin
                # directory. The launcher must work without searching PATH.
                result = subprocess.run([launcher, "/bin/echo", "compiler-ran"],
                                        env={"PATH": "/usr/bin:/bin"}, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), "compiler-ran")

    def test_macos_profile_can_disable_editor_packages_and_initialization(self):
        action = (self.ROOT / ".github/actions/macos-setup/action.yml").read_text()
        install = action.split(f"    - name: Install upstream Clang {LLVM_MAJOR} and native prerequisites\n", 1)[1]
        install = install.split("      run: |\n", 1)[1].split("    - name:", 1)[0]
        initialize = action.split("    - name: Prepare pinned native dependencies\n", 1)[1]
        initialize = initialize.split("      run: |\n", 1)[1].split("    - name:", 1)[0]
        self.assertRegex(action, r"editor:\n    description: [^\n]+\n    default: 'true'")
        workflow = (self.ROOT / ".github/workflows/performance.yml").read_text()
        profile_setup = workflow.split("      - name: Prepare macOS tools and the Profile preset\n", 1)[1]
        self.assertIn("editor: 'false'", profile_setup.split("      - name:", 1)[0])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, body in (("brew", 'printf "%s\\n" "$@"'), ("git", "exit 0"),
                               ("init.sh", 'printf "%s\\n" "$@" > "$INVOCATION_LOG"')):
                tool = root / name
                tool.write_text("#!/bin/sh\n" + body + "\n")
                tool.chmod(0o755)
            invocation = root / "initialization"
            for editor, preset in (("false", "macos-clang-profile"), ("true", "macos-clang-development")):
                with self.subTest(editor=editor, preset=preset):
                    environment = {**os.environ, "PATH": str(root), "WITH_EDITOR": editor,
                                   "PRESET": preset, "INVOCATION_LOG": str(invocation)}
                    result = subprocess.run(["/bin/bash", "-e", "-c", install], cwd=root,
                                            env=environment, check=True, capture_output=True, text=True)
                    packages = result.stdout.splitlines()
                    for package in ("qtbase", "qtdeclarative"):
                        self.assertEqual(package in packages, editor == "true")
                    for package in (f"llvm@{LLVM_MAJOR}", "ccache"):
                        self.assertIn(package, packages)
                    subprocess.run(["/bin/bash", "-e", "-c", initialize], cwd=root,
                                   env=environment, check=True, capture_output=True)
                    arguments = invocation.read_text().splitlines()
                    self.assertEqual("--with-editor" in arguments, editor == "true")
                    self.assertEqual(arguments[arguments.index("--preset") + 1], preset)
                    for argument in ("--locked", "--with-tests", "--no-system-install", "--preset-only"):
                        self.assertIn(argument, arguments)

    def test_cache_report_survives_missing_prerequisite_and_reports_when_available(self):
        workflow = (self.ROOT / ".github/workflows/ci.yml").read_text()
        commands = [line.strip().removeprefix("run: ") for line in workflow.splitlines()
                    if "ccache --show-stats" in line]
        self.assertTrue(commands)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            summary = root / "summary"
            environment = {**os.environ, "PATH": str(root), "GITHUB_STEP_SUMMARY": str(summary)}
            for command in commands:
                result = subprocess.run(["/bin/bash", "-c", command], env=environment, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(summary.exists())
            tool = root / "ccache"
            tool.write_text('#!/bin/sh\n[ "$1" = --show-stats ] || exit 1\necho cache-statistics\n')
            tool.chmod(0o755)
            result = subprocess.run(["/bin/bash", "-c", commands[0]], env=environment, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(summary.read_text(), "cache-statistics\n")


class LockedSetupTests(unittest.TestCase):
    def test_missing_system_tools_fail_before_venv_or_network(self):
        with tempfile.TemporaryDirectory() as temporary, contextlib.ExitStack() as stack:
            root = Path(temporary)
            stack.enter_context(patch.object(engine, "check_current_python"))
            shims = stack.enter_context(patch.object(engine, "configure_system_tool_shims"))
            stack.enter_context(patch.object(engine, "validate_required_system_tools",
                                            side_effect=engine.EngineError("missing Clang")))
            setup = stack.enter_context(patch.object(engine, "create_or_update_venv"))
            install = stack.enter_context(patch.object(engine, "install_managed_tools"))
            run = stack.enter_context(patch.object(engine, "run"))
            versions = {"minimum": {"python": "3.10"}}
            with self.assertRaisesRegex(engine.EngineError, "missing Clang"):
                engine.prepare_conan_artifacts(root, versions, (engine.DEFAULT_PRESET,))
            shims.assert_called_once_with(root, versions)
            setup.assert_not_called()
            install.assert_not_called()
            run.assert_not_called()

    def test_unlocked_setup_preserves_other_hosts_and_failed_refresh_preserves_lock(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock = root / "conan.lock"
            original = b'{"version":"0.5","requires":["volk/linux-pin"]}\n'
            lock.write_bytes(original)
            merged = b'{"version":"0.5","requires":["volk/linux-pin","freetype/mac-pin"]}\n'

            def execute(command, **kwargs):
                self.assertEqual(lock.read_bytes(), original)
                if command[2] == "create":
                    self.assertIn("--lockfile=", command)
                    Path(command[command.index("--lockfile-out") + 1]).write_text("new graph")
                else:
                    self.assertEqual(command[2], "merge")
                    Path(command[command.index("--lockfile-out") + 1]).write_bytes(merged)

            with patch.object(engine, "tool_env", return_value={}), patch.object(engine, "run", side_effect=execute):
                engine.create_conan_lock(root, root / "profile")
            self.assertEqual(lock.read_bytes(), merged)
            for fail_on in ("create", "merge"):
                lock.write_bytes(original)

                def fail(command, **kwargs):
                    if command[2] == fail_on:
                        raise engine.EngineError("refresh failed")
                    execute(command, **kwargs)

                with self.subTest(command=fail_on), patch.object(engine, "tool_env", return_value={}), \
                        patch.object(engine, "run", side_effect=fail):
                    with self.assertRaisesRegex(engine.EngineError, "refresh failed"):
                        engine.create_conan_lock(root, root / "profile")
                self.assertEqual(lock.read_bytes(), original)

    def test_locked_setup_preserves_lock_and_prepares_only_selected_preset(self):
        with tempfile.TemporaryDirectory() as temporary, contextlib.ExitStack() as stack:
            root = Path(temporary)
            lock = root / "conan.lock"
            lock.write_bytes(b'{"version": "0.5", "requires": []}\n')
            before = lock.read_bytes()
            for name in ("check_current_python", "create_or_update_venv", "install_managed_tools",
                         "validate_managed_tools", "configure_system_tool_shims", "validate_required_system_tools",
                         "export_local_recipes", "run"):
                stack.enter_context(patch.object(engine, name))
            stack.enter_context(patch.object(engine, "install_conan_profile", return_value=root / "profile"))
            regenerate = stack.enter_context(patch.object(engine, "create_conan_lock"))
            install = stack.enter_context(patch.object(engine, "conan_install_for_preset"))
            engine.prepare_conan_artifacts(root, {"minimum": {"python": "3.10"}},
                                           (engine.DEFAULT_PRESET,), locked=True)
            regenerate.assert_not_called()
            install.assert_called_once_with(root, root / "profile", engine.DEFAULT_PRESET, locked=True)
            self.assertEqual(lock.read_bytes(), before)

    def test_missing_lock_fails_before_tools_or_network(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(engine, "run") as run, \
                patch.object(engine, "create_or_update_venv") as setup:
            with self.assertRaisesRegex(engine.EngineError, "committed conan.lock"):
                engine.prepare_conan_artifacts(Path(temporary), {}, (engine.DEFAULT_PRESET,), locked=True)
            setup.assert_not_called()
            run.assert_not_called()

    def test_locked_install_does_not_allow_unlocked_dependencies(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(engine, "run") as run, \
                patch.object(engine, "tool_env", return_value={}):
            root = Path(temporary)
            engine.conan_install_for_preset(root, root / "profile", engine.DEFAULT_PRESET, locked=True)
            command = run.call_args.args[0]
            self.assertIn("--lockfile", command)
            self.assertNotIn("--lockfile-partial", command)
            self.assertEqual(command[command.index("--lockfile") + 1], str(root / "conan.lock"))


class SharedConanHomeTests(unittest.TestCase):
    def make_root(self, temporary: str) -> Path:
        root = Path(temporary) / "checkout"
        (root / "config").mkdir(parents=True)
        (root / "config/tool_versions.json").write_text('{"managed": {"conan": "2.8.1"}, "minimum": {}}')
        return root

    def test_checkout_home_is_default_and_shared_home_is_versioned_and_absolute(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_root(temporary)
            with patch.dict(os.environ, {engine.SHARED_CONAN_HOME_VARIABLE: ""}):
                self.assertEqual(engine.conan_home(root), root / "out/conan/home")
                self.assertEqual(engine.tool_env(root)["CONAN_HOME"], str(root / "out/conan/home"))
            shared = Path(temporary) / "shared"
            with patch.dict(os.environ, {engine.SHARED_CONAN_HOME_VARIABLE: str(shared)}):
                self.assertEqual(engine.conan_home(root), shared / "conan-2.8.1")
                self.assertEqual(engine.tool_env(root)["CONAN_HOME"], str(shared / "conan-2.8.1"))
            with patch.dict(os.environ, {engine.SHARED_CONAN_HOME_VARIABLE: "relative/home"}):
                with self.assertRaisesRegex(engine.EngineError, "absolute path"):
                    engine.conan_home(root)

    def test_profile_stays_in_checkout_when_home_is_shared(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.dict(os.environ, {engine.SHARED_CONAN_HOME_VARIABLE: str(Path(temporary) / "shared")}):
            root = self.make_root(temporary)
            source = root / "config/conan/profiles" / engine.native_profile_name()
            source.parent.mkdir(parents=True)
            source.write_text("[settings]\n")
            installed = engine.install_conan_profile(root)
            self.assertEqual(installed, root / "out/conan/profiles" / engine.native_profile_name())
            self.assertFalse((Path(temporary) / "shared").exists())

    def test_shared_home_lock_excludes_other_checkouts_until_released(self):
        import fcntl

        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_root(temporary)
            shared = Path(temporary) / "shared"
            with patch.dict(os.environ, {engine.SHARED_CONAN_HOME_VARIABLE: str(shared)}):
                lock_path = engine.conan_home(root) / ".ludus-cache.lock"
                with engine.conan_cache_lock(root):
                    with lock_path.open("a") as other:
                        with self.assertRaises(BlockingIOError):
                            fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with lock_path.open("a") as other:
                    fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with patch.dict(os.environ, {engine.SHARED_CONAN_HOME_VARIABLE: ""}):
                with engine.conan_cache_lock(root):
                    pass
            self.assertFalse((root / "out").exists())

    def test_conan_cache_writes_run_inside_the_shared_lock(self):
        with tempfile.TemporaryDirectory() as temporary, contextlib.ExitStack() as stack:
            root = Path(temporary)
            (root / "conan.lock").write_text('{"version": "0.5", "requires": []}\n')
            held = []

            @contextlib.contextmanager
            def lock(_root):
                held.append(True)
                yield
                held.append(False)

            def record(*args, **kwargs):
                self.assertEqual(held, [True])

            for name in ("check_current_python", "create_or_update_venv", "install_managed_tools",
                         "validate_managed_tools", "configure_system_tool_shims", "validate_required_system_tools"):
                stack.enter_context(patch.object(engine, name))
            stack.enter_context(patch.object(engine, "install_conan_profile", return_value=root / "profile"))
            stack.enter_context(patch.object(engine, "tool_env", return_value={}))
            stack.enter_context(patch.object(engine, "conan_cache_lock", side_effect=lock))
            for name in ("run", "export_local_recipes", "create_conan_lock", "conan_install_for_preset"):
                stack.enter_context(patch.object(engine, name, side_effect=record))
            engine.prepare_conan_artifacts(root, {"minimum": {"python": "3.10"}}, (engine.DEFAULT_PRESET,))
            self.assertEqual(held, [True, False])

    def test_locked_export_rejects_recipe_revision_missing_from_lock(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for recipe in engine.LOCAL_CONAN_RECIPES:
                (root / recipe).mkdir(parents=True)
                (root / recipe / "conanfile.py").write_text("")
            (root / "conan.lock").write_text(json.dumps({
                "version": "0.5",
                "requires": ["harfbuzz/14.5.1#pinned%1790885077.9", "catch2/3.4.0#catch"],
            }))
            exported = {"third_party/harfbuzz": "harfbuzz/14.5.1#pinned", "third_party/catch2": "catch2/3.4.0#catch"}

            def export(argv, **kwargs):
                recipe = Path(argv[2]).relative_to(root).as_posix()
                return subprocess.CompletedProcess(argv, 0, json.dumps({"reference": exported[recipe]}))

            with patch.object(engine, "tool_env", return_value={}), \
                    patch.object(engine.subprocess, "run", side_effect=export):
                engine.export_local_recipes(root, locked=True)
                exported["third_party/harfbuzz"] = "harfbuzz/14.5.1#edited"
                engine.export_local_recipes(root)
                with self.assertRaisesRegex(engine.EngineError, "pins harfbuzz/14.5.1#pinned.*#edited"):
                    engine.export_local_recipes(root, locked=True)


class AnalysisTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    @staticmethod
    def result(code, text, log):
        from quality_process import ProcessResult
        log.write_text(text)
        return ProcessResult(code, log, len(text.encode()), False, False)

    def test_shards_execute_every_translation_unit_once(self):
        commands = [["tidy", str(i)] for i in range(11)]
        visited = []

        def analyze(command, **kwargs):
            visited.append(command[1])
            return self.result(0, "", kwargs["log"])

        with patch.dict(os.environ, {"LUDUS_TIDY_JOBS": "2", "LUDUS_TIDY_SHARD_COUNT": "3"}), \
                patch.object(engine, "run_bounded", side_effect=analyze), \
                contextlib.redirect_stdout(io.StringIO()):
            for index in range(3):
                with patch.dict(os.environ, {"LUDUS_TIDY_SHARD_INDEX": str(index)}):
                    engine.run_analysis_commands(self.root, commands)
        self.assertCountEqual(visited, [str(i) for i in range(11)])

    def test_invalid_or_empty_shards_fail_before_launching_analysis(self):
        for count, index in (("0", "0"), ("2", "2"), ("2", "-1"), ("two", "0"), ("2", "one"), ("2", "1")):
            with self.subTest(count=count, index=index), \
                    patch.dict(os.environ, {"LUDUS_TIDY_SHARD_COUNT": count, "LUDUS_TIDY_SHARD_INDEX": index}), \
                    patch.object(engine, "run_bounded") as run:
                with self.assertRaises(engine.EngineError):
                    engine.run_analysis_commands(self.root, [["tidy", "source.cpp"]])
                run.assert_not_called()

    def test_failed_shard_keeps_diagnostics_and_fails_the_gate(self):
        commands = [[sys.executable, "-c", "print('other shard')"],
                    [sys.executable, "-c", "import sys; print('failed shard'); sys.exit(1)"]]
        output = io.StringIO()
        with patch.dict(os.environ, {"LUDUS_TIDY_SHARD_COUNT": "2", "LUDUS_TIDY_SHARD_INDEX": "1"}), \
                contextlib.redirect_stdout(output):
            with self.assertRaisesRegex(engine.EngineError, "1 translation unit"):
                engine.run_analysis_commands(self.root, commands)
        self.assertIn("failed shard\n", output.getvalue())
        self.assertNotIn("other shard", output.getvalue())

    def test_empty_database_cannot_pass_a_sharded_gate(self):
        with patch.dict(os.environ, {"LUDUS_TIDY_SHARD_COUNT": "2", "LUDUS_TIDY_SHARD_INDEX": "0"}), \
                patch.object(engine, "run_bounded") as run:
            with self.assertRaisesRegex(engine.EngineError, "no translation units"):
                engine.run_analysis_commands(self.root, [])
            run.assert_not_called()

    def test_concurrency_is_bounded_and_all_failures_are_reported(self):
        barrier = threading.Barrier(2)
        mutex = threading.Lock()
        active = 0
        peak = 0
        visited = []

        def analyze(command, **kwargs):
            nonlocal active, peak
            with mutex:
                active += 1
                peak = max(peak, active)
                visited.append(command[1])
            barrier.wait(timeout=5)
            with mutex:
                active -= 1
            return self.result(1 if command[1] in ("0", "3") else 0,
                               f"diagnostic {command[1]}\n", kwargs["log"])

        output = io.StringIO()
        with patch.dict(os.environ, {"LUDUS_TIDY_JOBS": "2"}), \
                patch.object(engine, "run_bounded", side_effect=analyze), contextlib.redirect_stdout(output):
            with self.assertRaisesRegex(engine.EngineError, "2 translation unit"):
                engine.run_analysis_commands(self.root, [["tidy", str(i)] for i in range(4)])
        self.assertEqual(peak, 2)
        self.assertCountEqual(visited, ["0", "1", "2", "3"])
        diagnostics = [line for line in output.getvalue().splitlines() if line.startswith("diagnostic")]
        self.assertEqual(diagnostics, [f"diagnostic {i}" for i in range(4)])

    def test_invalid_worker_count_fails_without_launching_analysis(self):
        for value in ("0", "-1", "two"):
            with self.subTest(value=value), patch.dict(os.environ, {"LUDUS_TIDY_JOBS": value}), \
                    patch.object(engine, "run_bounded") as run:
                with self.assertRaisesRegex(engine.EngineError, "positive integer"):
                    engine.run_analysis_commands(self.root, [["tidy", "source.cpp"]])
                run.assert_not_called()

    def test_real_process_diagnostics_survive_a_failed_command(self):
        commands = [
            [sys.executable, "-c", "import sys; print('analysis error', file=sys.stderr); sys.exit(2)"],
            [sys.executable, "-c", "print('analysis completed')"],
        ]
        output = io.StringIO()
        with patch.dict(os.environ, {"LUDUS_TIDY_JOBS": "2"}), contextlib.redirect_stdout(output):
            with self.assertRaisesRegex(engine.EngineError, "1 translation unit"):
                engine.run_analysis_commands(self.root, commands)
        self.assertIn("analysis error\n", output.getvalue())
        self.assertIn("analysis completed\n", output.getvalue())
        self.assertIn("Analysis exited with code 2", output.getvalue())

    def test_missing_analyzer_reports_actionable_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(engine.EngineError, "Cannot launch analysis"):
                engine.run_analysis_commands(Path(temporary), [[Path(temporary) / "missing-analyzer"]])

    def listed_source_fixture(self, preset="linux-clang-development"):
        build = self.root / "out/build" / preset
        build.mkdir(parents=True)
        sources = []
        for name in ("apps/editor/src/controller.cpp", "apps/editor/tests/controller_tests.cpp"):
            source = self.root / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("int main() { return 0; }\n")
            sources.append(name)
        (build / "compile_commands.json").write_text(json.dumps([
            {"directory": str(build), "file": str(self.root / name), "arguments": ["clang++", "-c", name]}
            for name in sources]))
        patches = [patch.object(engine, "load_tool_versions", return_value={"minimum": {"clang": f"{LLVM_MAJOR}.0.0"}}),
                   patch.object(engine, "find_system_tool", return_value="/pinned/clang-tidy"),
                   patch.object(engine, "executable_version", return_value=f"{LLVM_MAJOR}.1.8")]
        for item in patches:
            self.addCleanup(item.stop)
            item.start()
        return build, sources

    def test_listed_sources_run_through_the_bounded_concurrent_runner(self):
        build, sources = self.listed_source_fixture()
        with patch.object(engine, "run_analysis_commands") as runner, contextlib.redirect_stdout(io.StringIO()):
            engine.run_tidy_sources(self.root, "linux-clang-development", sources)
        runner.assert_called_once()
        root, commands = runner.call_args.args
        self.assertEqual(root, self.root)
        # One ordinary database lookup per listed source: the same command a
        # serial workflow step ran, so diagnostics and header filtering match.
        self.assertEqual(commands, [["/pinned/clang-tidy", "-p", build, "--warnings-as-errors=*",
                                     str((self.root / name).resolve())] for name in sources])

    def test_unlisted_duplicate_or_uncompiled_sources_fail_before_analysis(self):
        _, sources = self.listed_source_fixture()
        (self.root / "apps/editor/src/orphan.cpp").write_text("")
        cases = {"at least one": [], "listed twice": [sources[0], sources[0]],
                 "not compiled": [sources[0], "apps/editor/src/orphan.cpp"]}
        for message, listed in cases.items():
            with self.subTest(message=message), patch.object(engine, "run_analysis_commands") as runner:
                with self.assertRaisesRegex(engine.EngineError, message):
                    engine.run_tidy_sources(self.root, "linux-clang-development", listed)
                runner.assert_not_called()

    def test_listed_sources_require_built_analysis_inputs(self):
        self.listed_source_fixture()
        with patch.object(engine, "run_analysis_commands") as runner:
            with self.assertRaisesRegex(engine.EngineError, "build analysis inputs"):
                engine.run_tidy_sources(self.root, "macos-clang-development", ["apps/editor/src/controller.cpp"])
            runner.assert_not_called()

    def test_listed_source_process_failure_preserves_all_diagnostics(self):
        self.listed_source_fixture()
        analyzer = self.root / "fake-tidy"
        analyzer.write_text(
            f"#!{sys.executable}\n"
            "import sys\n"
            "source = sys.argv[-1]\n"
            "print('checked ' + source, flush=True)\n"
            "sys.exit(1 if source.endswith('/controller.cpp') else 0)\n")
        analyzer.chmod(0o755)
        sources = ["apps/editor/src/controller.cpp", "apps/editor/tests/controller_tests.cpp"]
        output = io.StringIO()
        with patch.object(engine, "find_system_tool", return_value=str(analyzer)), \
                patch.dict(os.environ, {"LUDUS_TIDY_JOBS": "2", "LUDUS_TIDY_SHARD_COUNT": "1",
                                        "LUDUS_TIDY_SHARD_INDEX": "0"}), contextlib.redirect_stdout(output):
            with self.assertRaisesRegex(engine.EngineError, "1 translation unit"):
                engine.run_tidy_sources(self.root, "linux-clang-development", sources)
        for source in sources:
            self.assertIn("checked " + str((self.root / source).resolve()), output.getvalue())
        reports = list((self.root / "out/quality/analysis").glob("*/results.json"))
        self.assertEqual(len(reports), 1)
        report = json.loads(reports[0].read_text())
        self.assertEqual(report["completed"], 2)
        self.assertEqual([result["state"] for result in report["results"]], ["fail", "pass"])

    def test_editor_source_lists_use_bounded_analysis_entrypoint(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        for job, preset, workers, step in (
            ("macos-compile", "macos-clang-development", 3, "Analyze native Editor production and regression sources"),
            ("editor", "linux-clang-development", 4, "Analyze document interactions and regressions"),
        ):
            with self.subTest(job=job):
                # Read the job until the next two-space job key, preserving step indentation.
                section = re.split(r"\n  [a-z][a-z0-9-]*:\n", workflow.split(f"\n  {job}:\n", 1)[1], 1)[0]
                self.assertIn(f'LUDUS_TIDY_JOBS: "{workers}"', section)
                command = section.split(f"- name: {step}\n", 1)[1].split("\n      - ", 1)[0]
                self.assertIn(f"./scripts/check {preset} --tidy-sources", command)
                self.assertNotIn("out/host-tools/bin/clang-tidy", command)

    def test_check_routes_listed_sources_without_running_other_checks(self):
        args = engine.make_parser().parse_args(
            ["check", "macos-clang-development", "--tidy-sources", "apps/editor/src/a.cpp", "apps/editor/src/b.cpp"])
        self.assertEqual(args.tidy_sources, ["apps/editor/src/a.cpp", "apps/editor/src/b.cpp"])
        with patch.object(engine, "repo_root", return_value=self.root), \
                patch.object(engine, "ensure_bootstrap_for_preset") as bootstrap, \
                patch.object(engine, "run_format_check") as formatting, \
                patch.object(engine, "run_tidy") as tidy, \
                patch.object(engine, "run_tidy_sources") as listed:
            self.assertEqual(engine.command_check(args), 0)
        bootstrap.assert_called_once_with(self.root, "macos-clang-development")
        listed.assert_called_once_with(self.root, "macos-clang-development",
                                       ["apps/editor/src/a.cpp", "apps/editor/src/b.cpp"])
        formatting.assert_not_called()
        tidy.assert_not_called()

    def test_missing_worker_result_cannot_pass(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(engine, "ThreadPoolExecutor") as pool:
            pool.return_value.__enter__.return_value.map.return_value = []
            with self.assertRaisesRegex(engine.EngineError, "omitted required"):
                engine.run_analysis_commands(self.root, [["tidy", "source.cpp"]])
            pool.assert_called_once_with(max_workers=1)


class BrowserAnalysisTests(unittest.TestCase):
    def test_only_explicit_owned_generated_file_is_analyzed(self):
        root = Path("/tmp/browser-analysis").resolve()
        sources = [root / "out/script-interaction/cooked/bindings.cpp", root / "out/vendor.cpp"]
        entries = [{"file": str(source), "command": f"em++ -c {source} -o object.o"}
                   for source in sources]
        commands = web_build.analysis_commands(root, entries, CLANG_TIDY, root / "sysroot",
                                              owned_generated=(sources[0],))
        self.assertEqual([command[2] for command in commands], [str(sources[0])])

    def test_owns_engine_apps_and_probes_but_not_vendor_or_generated_sources(self):
        root = Path("/tmp/browser-analysis").resolve()
        sources = ["modules/audio/src/audio.cpp", "apps/smoke/main.cpp", "tools/web-rhi-probe/main.cpp",
                   "tests/sdk_consumer/strings.cpp", "third_party/yyjson/yyjson.c", "out/generated.cpp",
                   "modules-other/file.cpp"]
        entries = [{"file": name, "directory": str(root),
                    "command": f"em++ -std=c++23 --use-port=emdawnwebgpu -DWEB=1 -c {name} -o object.o"}
                   for name in sources]
        commands = web_build.analysis_commands(root, entries, CLANG_TIDY, root / "sysroot")
        self.assertEqual([command[2] for command in commands], [str(root / source) for source in sources[:4]])
        for command in commands:
            self.assertIn("--warnings-as-errors=*", command)
            self.assertIn("--target=wasm32-unknown-emscripten", command)
            self.assertIn("-DWEB=1", command)
            self.assertIn("-std=c++23", command)
            self.assertNotIn("-frelaxed-template-template-args", command)
            self.assertNotIn("object.o", command)
            self.assertNotIn("-c", command)
            self.assertNotIn("--use-port=emdawnwebgpu", command)

    def test_argument_arrays_and_paths_with_spaces_are_preserved(self):
        root = Path("/tmp/browser project").resolve()
        source = str(root / "modules/base/src/main.cpp")
        entries = [{"file": source, "arguments": ["em++", "-I", str(root / "include"), "-c", source, "-o", "main.o"]}]
        command = web_build.analysis_commands(root, entries, CLANG_TIDY, root / "sysroot")[0]
        self.assertEqual(command[2], source)
        self.assertIn(str(root / "include"), command)


class BuildAnalyzerBootstrapTests(unittest.TestCase):
    def test_include_profile_preserves_bodies_and_rejects_changed_anchors(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            files = (
                ("enkiTS/TaskScheduler.cpp", '#include "TaskScheduler.h"\n'),
                ("llvm-Demangle/include/Utility.h", '#include "StringView.h"\n'),
                ("llvm-Demangle/include/ItaniumDemangle.h", '#include "DemangleConfig.h"\n'),
            )
            for name, anchor in files:
                path = source / "src/external" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("// upstream license\n" + anchor + "// unchanged body\n")
            engine.prepare_clang_build_analyzer_includes(source)
            patched = {name: (source / "src/external" / name).read_text() for name, _ in files}
            engine.prepare_clang_build_analyzer_includes(source)
            for name, anchor in files:
                text = (source / "src/external" / name).read_text()
                self.assertEqual(text, patched[name])
                self.assertTrue(text.startswith("// upstream license\n" + anchor))
                self.assertTrue(text.endswith("// unchanged body\n"))
            path = source / "src/external" / files[0][0]
            for changed in ("unrecognized source", files[0][1] * 2,
                            patched[files[0][0]].replace("#include <cstdlib>\n", "")):
                path.write_text(changed)
                with self.assertRaisesRegex(engine.EngineError, "include profile no longer matches"):
                    engine.prepare_clang_build_analyzer_includes(source)

    def test_an_older_cached_binary_cannot_satisfy_the_current_pin(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old = root / "out/host-tools/clang-build-analyzer/build/ClangBuildAnalyzer"
            old.parent.mkdir(parents=True)
            old.write_bytes(b"old analyzer")
            source = engine.clang_build_analyzer_dir(root)
            source.mkdir(parents=True)
            (source / "CMakeLists.txt").touch()
            binary = engine.clang_build_analyzer_binary(root)

            def build(command, **kwargs):
                if "--build" in command:
                    binary.write_bytes(b"current analyzer")

            with patch.object(engine, "run", side_effect=build) as run, \
                    patch.object(engine, "prepare_clang_build_analyzer_includes"):
                self.assertEqual(engine.ensure_clang_build_analyzer(root), binary)
                self.assertEqual(binary.read_bytes(), b"current analyzer")
                self.assertEqual(old.read_bytes(), b"old analyzer")
                self.assertEqual(run.call_count, 2)
                self.assertEqual(engine.ensure_clang_build_analyzer(root), binary)
                self.assertEqual(run.call_count, 2)


class BuildBudgetRunnerSpeedTests(unittest.TestCase):
    """The budget gate compares timings at the reference run's speed (ADR 0005)."""

    PRESET = "linux-clang-development"

    @staticmethod
    def analysis(root: Path, frontend_s: float, standard: tuple[int, int, int, int], header_ms: int) -> str:
        # Include counts and frontend totals from CI runs 40639240762 and 40735340923.
        string_ms, span_ms, view_ms, array_ms = standard
        return "\n".join((
            "**** Time summary:",
            "Compilation (576 times):",
            f"  Parsing (frontend):          {frontend_s:.1f} s",
            "",
            "**** Expensive headers:",
            f"{string_ms * 348} ms: /usr/include/c++/14/string (included 348 times, avg {string_ms} ms), included via:",
            f"{span_ms * 424} ms: /usr/include/c++/14/span (included 424 times, avg {span_ms} ms), included via:",
            f"{header_ms * 46} ms: {root}/modules/graphics/renderer/include/ludus/graphics/renderer/environment.hpp "
            f"(included 46 times, avg {header_ms} ms), included via:",
            f"{array_ms * 425} ms: /usr/include/c++/14/array (included 425 times, avg {array_ms} ms), included via:",
            f"{view_ms * 455} ms: /usr/include/c++/14/string_view (included 455 times, avg {view_ms} ms), included via:",
            "",
            "**** Done",
            "",
        ))

    def check(self, frontend_s: float, standard: tuple[int, int, int, int], header_ms: int = 600) -> str:
        committed = engine.load_build_budget(engine.repo_root())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "config").mkdir()
            (root / "config/build_budget.json").write_text(json.dumps(committed), encoding="utf-8")
            profile = root / "out/profile"
            profile.mkdir(parents=True)
            (profile / f"{self.PRESET}-analysis.txt").write_text(
                self.analysis(root, frontend_s, standard, header_ms), encoding="utf-8")
            (profile / f"{self.PRESET}-profile.json").write_text("{}\n", encoding="utf-8")
            output = io.StringIO()
            with patch.object(engine, "repo_root", return_value=root), contextlib.redirect_stdout(output):
                try:
                    engine.command_check_build_budget(engine.argparse.Namespace(preset=None, profile=False))
                except engine.EngineError:
                    return "FAILED\n" + output.getvalue()
            return "PASSED\n" + output.getvalue()

    def test_a_slow_runner_passes_the_unchanged_graph(self):
        # Run 40735340923 measured 490.5 s on a runner about 2.1x slower than the baseline.
        result = self.check(490.5, (217, 90, 53, 60))
        self.assertTrue(result.startswith("PASSED"), result)
        self.assertIn("budgets compare timings divided by 2.0", result)
        self.assertRegex(result, r"\[OK\] total frontend parsing: 490\.5 s, 23\d\.\d s normalized")

    def test_growth_at_baseline_speed_still_fails(self):
        result = self.check(330.0, (111, 40, 25, 26))
        self.assertTrue(result.startswith("FAILED"), result)
        self.assertIn("[OVER] total frontend parsing: 330.0 s", result)
        self.assertIn("measured) exceeds budget 290 s", result)

    def test_a_heavy_header_fails_after_normalization(self):
        result = self.check(240.0, (111, 40, 25, 26), header_ms=2400)
        self.assertTrue(result.startswith("FAILED"), result)
        self.assertIn("[OVER] modules/graphics/renderer/include/ludus/graphics/renderer/environment.hpp", result)

    def test_missing_or_implausible_reference_uses_raw_timings(self):
        reference = engine.load_build_budget(engine.repo_root())["runner_speed_reference"]
        headers = engine.parse_expensive_headers(
            self.analysis(Path("/work"), 240.0, (111, 40, 25, 26), 300))
        factor, _note = engine.measure_runner_speed(headers, reference)
        self.assertAlmostEqual(factor, 1.0, delta=0.01)
        without_array = [entry for entry in headers if not entry[0].endswith("/array")]
        factor, note = engine.measure_runner_speed(without_array, reference)
        self.assertIsNone(factor)
        self.assertIn("/usr/include/c++/14/array", note)
        implausible = engine.parse_expensive_headers(self.analysis(Path("/work"), 240.0, (999, 400, 250, 260), 300))
        factor, note = engine.measure_runner_speed(implausible, reference)
        self.assertIsNone(factor)
        self.assertIn("outside", note)
        result = self.check(490.5, (999, 400, 250, 260))
        self.assertTrue(result.startswith("FAILED"), result)
        self.assertIn("comparing raw timings", result)


class ShaderBootstrapCacheTests(unittest.TestCase):
    def test_relocated_verified_tool_needs_no_download_or_cmake_build(self):
        script = Path(engine.__file__).resolve().parents[2] / "scripts/bootstrap-spirv-cross"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            copy = root / "scripts/bootstrap-spirv-cross"
            copy.parent.mkdir()
            copy.write_bytes(script.read_bytes())
            binary = root / "out/shader-tools/spirv-cross/bin/spirv-cross"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"verified translator")
            (binary.parent.parent / "LICENSE").touch()
            archive = root / "out/shader-tools/spirv-cross-src/spirv-cross.tar.gz"
            archive.parent.mkdir()
            archive.write_bytes(b"verified source archive")
            pin = {"tag": "pinned-tag", "commit": "pinned-revision",
                   "source_sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
            (root / "config").mkdir()
            (root / "config/spirv_cross_toolchain.json").write_text(json.dumps({"spirv_cross": pin}))
            shutil.copyfile(engine.repo_root() / "config/tool_versions.json", root / "config/tool_versions.json")
            compiler = f"clang version {LLVM_MAJOR}.1.8"
            manifest = {**pin, "compiler": compiler, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
            Path(str(binary) + ".build.json").write_text(json.dumps(manifest))
            namespace = runpy.run_path(str(copy))
            with patch.object(subprocess, "check_output", return_value=compiler + "\n"), \
                    patch.object(subprocess, "run") as build, \
                    patch("urllib.request.urlretrieve") as download, contextlib.redirect_stdout(io.StringIO()):
                namespace["main"]()
            build.assert_not_called()
            download.assert_not_called()
            check = namespace["cached_translator"]
            self.assertFalse(check(binary.parent.parent, {**pin, "commit": "different"}, "clang version 18.1.8"))
            self.assertFalse(check(binary.parent.parent, pin, "Clang 19"))
            binary.write_bytes(b"modified translator")
            self.assertFalse(check(binary.parent.parent, pin, "clang version 18.1.8"))


if __name__ == "__main__":
    unittest.main()
