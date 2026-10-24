"""Separate, pinned browser dependency path; never installs native Conan packages."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import runpy
import shlex
import shutil
import subprocess
import urllib.request

from formatting import format_source
import init_progress
import managed_venv
from analysis_inventory import inventory, request_codemodel, SOURCE_ROOTS

PRESETS = ("web-emscripten-development", "web-emscripten-release")


def analysis_commands(root: Path, entries: list[dict], tidy: str, sysroot: Path,
                      owned_generated: tuple[Path, ...] = ()) -> list[list[str]]:
    """Analyze owned sources and probes, leaving pinned vendor implementations alone."""
    root = root.resolve()
    owned_roots = tuple(root / directory for directory in SOURCE_ROOTS)
    generated = {path.resolve() for path in owned_generated}
    commands = []
    for entry in entries:
        source = Path(entry["file"])
        if not source.is_absolute():
            source = Path(entry.get("directory", root)) / source
        source = source.resolve()
        if source not in generated and not any(source.is_relative_to(directory) for directory in owned_roots):
            continue
        command_line = entry.get("arguments") or shlex.split(entry["command"])
        flags = []
        index = 1
        while index < len(command_line):
            flag = command_line[index]
            if flag == "-o":
                index += 2
                continue
            if flag not in ("-c", entry["file"]) and not flag.startswith("--use-port="):
                flags.append(flag)
            index += 1
        # Thanks to LLVM contributors, Clang 19 Release Notes, C++ language
        # changes (P0522/CWG2398): both the pinned SDK and reference analyzer now
        # enable standard relaxed template matching by default. LLVM 23 removes
        # the old opt-in flag; keep C++23 and error checks without that override.
        # https://releases.llvm.org/19.1.0/tools/clang/docs/ReleaseNotes.html#c-language-changes
        flags += ["-I" + str(sysroot.parent / "ports/emdawnwebgpu/emdawnwebgpu_pkg/webgpu/include"),
                  "--target=wasm32-unknown-emscripten", f"--sysroot={sysroot}", "-DEMSCRIPTEN",
                  "-isystem", str(sysroot / "include/c++/v1"),
                  "-isystem", str(sysroot / "include/compat")]
        commands.append([tidy, "--warnings-as-errors=*", str(source), "--", *flags])
    return commands


def command(args, engine) -> int:
    # Native workflows never invoke this handler. Reuse the calling engine module
    # so errors retain its identity when the CLI runs as __main__.
    EngineError = engine.EngineError
    repo_root = engine.repo_root
    run = engine.run
    run_format_check = engine.run_format_check
    run_foundational_includes = engine.run_foundational_includes

    root = repo_root()
    if args.command == "check" and getattr(args, "tooling", False):
        from tooling_quality import check
        try:
            check(root)
        except RuntimeError as error:
            raise EngineError(str(error)) from error
        if not any((args.format, args.tidy, args.all, args.include_cleaner,
                    getattr(args, "analyzer", False), getattr(args, "inventory", False))):
            return 0
    if args.preset not in PRESETS:
        raise EngineError("Unknown browser preset: " + args.preset)
    if args.command not in ("init", "bootstrap", "doctor", "build", "test", "check"):
        raise EngineError("Browser SDK installation/profiling is not supported by this stage")
    if args.command == "init" and (args.all_presets or args.validate or args.ci):
        raise EngineError("Use web init --preset-only, then web build/test/check separately")
    if args.command == "test":
        from init_options import read_options
        if not read_options(root, args.preset).get("LUDUS_BUILD_WEB_PROBES", True):
            raise EngineError("Browser probes are disabled; opt in with ./init.sh --cli " + args.preset + " --with-web-probes")
    lock = json.loads((root / "config/web_toolchain.json").read_text())
    sdk = root / "out/host-tools/emsdk"
    host_bin = root / "out/host-tools/venv/bin"

    if args.command == "init":
        init_progress.start(init_progress.browser_plan(args))
    try:
        if args.command == "init":
            from init_options import save_options
            from init_denoiser import UNSUPPORTED_NOTE
            save_options(root, (args.preset,), args)
            print(UNSUPPORTED_NOTE)
        if args.command in ("init", "bootstrap"):
            init_progress.begin("host_tools")
            native = json.loads((root / "config/tool_versions.json").read_text())["managed"]
            try:
                managed_venv.ensure(host_bin.parent, notify=init_progress.note)
            except managed_venv.ManagedEnvironmentError as exc:
                raise EngineError(str(exc)) from exc
            run([host_bin / "python", "-m", "pip", "install",
                 f"cmake=={native['cmake']}", f"ninja=={native['ninja']}"], cwd=root)
            init_progress.begin("emsdk")
            if not (sdk / "emsdk").exists():
                if sdk.exists() and any(sdk.iterdir()):
                    raise EngineError("Incomplete SDK directory; preserve it and repair before retrying")
                archive = root / "out/host-tools/emsdk.tar.gz"
                urllib.request.urlretrieve(lock["emsdk_archive"], archive)
                if hashlib.sha256(archive.read_bytes()).hexdigest() != lock["emsdk_archive_sha256"]:
                    raise EngineError("SDK source archive checksum mismatch; extraction refused")
                sdk.mkdir(parents=True, exist_ok=True)
                run(["tar", "-xzf", archive, "--strip-components=1", "-C", sdk], cwd=root)
            run([sdk / "emsdk", "install", lock["emscripten"]], cwd=root)
            run([sdk / "emsdk", "activate", lock["emscripten"]], cwd=root)

        if args.command == "init":
            init_progress.begin("verify")
        # Reuse W0's SDK version and vendored-port integrity validation.
        probe_tools = runpy.run_path(str(root / "scripts/webgpu-probe"))
        emscripten, env = probe_tools["verify_sdk"](sdk, lock)
        for name in ("cmake", "ninja"):
            if not (host_bin / name).is_file():
                raise EngineError("Missing pinned host tool; run ./init.sh " + args.preset + " --preset-only")
        native = json.loads((root / "config/tool_versions.json").read_text())["managed"]
        for name in ("cmake", "ninja"):
            actual = subprocess.check_output([host_bin / name, "--version"], text=True)
            expected = native[name] if name == "cmake" else native[name].rsplit(".", 1)[0]
            if expected not in actual.splitlines()[0]:
                raise EngineError(f"Pinned host {name} differs: {actual.splitlines()[0]}")
        if args.command == "doctor":
            print("Browser tools verified; no Conan, Volk, Wayland, or native helper dependency.")
            return 0

        cmake = host_bin / "cmake"
        from init_options import read_options
        shader_args = []
        if read_options(root, args.preset).get("LUDUS_BUILD_SMOKE_APP", True):
            if args.command in ("init", "bootstrap"):
                init_progress.begin("shader_tools")
                run([root / "scripts/shader-probe", "bootstrap"], cwd=root)
                run([root / "scripts/bootstrap-spirv-cross"], cwd=root)
            tools = root / "out/shader-tools"
            for variable, relative in (
                ("LUDUS_SLANG_COMPILER", "slang/bin/slangc"),
                ("LUDUS_SPIRV_VALIDATOR", "spirv-tools/usr/bin/spirv-val"),
                ("LUDUS_SPIRV_CROSS", "spirv-cross/bin/spirv-cross"),
            ):
                path = tools / relative
                if not path.is_file():
                    raise EngineError("Missing shader tool; run ./init.sh " + args.preset + " --preset-only")
                shader_args.append("-D" + variable + "=" + str(path))
        init_progress.begin("configure")
        request_codemodel(root / "out/build" / args.preset)
        run([emscripten / "emcmake", cmake, "--preset", args.preset, *shader_args], cwd=root, env=env)
        if args.command in ("init", "bootstrap"):
            print("Browser configure complete; next: ./scripts/build " + args.preset)
            return 0
        build = root / "out/build" / args.preset
        if args.command in ("build", "test"):
            extra = getattr(args, "extra", [])
            run([cmake, "--build", "--preset", args.preset, *extra], cwd=root, env=env)
            if args.command == "test":
                options = ["-L", args.label] if args.label else []
                run([host_bin / "ctest", "--preset", args.preset, *options], cwd=root, env=env)
            return 0

        analyzer = getattr(args, "analyzer", False)
        inventory_only = getattr(args, "inventory", False)
        all_checks = args.all or not (args.format or args.tidy or args.include_cleaner or analyzer or inventory_only or getattr(args, "tooling", False))
        if args.include_cleaner:
            raise EngineError("Browser include-cleaner is not implemented; use format/tidy checks")
        if args.format or all_checks:
            run_format_check(root, fix=args.fix)
            run_foundational_includes(root)
            for probe in ("web-foundation-probe", "web-observability-probe", "web-platform-probe", "web-rhi-probe", "web-frame-probe"):
                path = root / "tools" / probe / "main.cpp"
                original = path.read_text()
                formatted = format_source(original, path, engine.pinned_llvm_tool(root, "clang-format"))
                if args.fix:
                    path.write_text(formatted)
                elif formatted != original:
                    raise EngineError(f"Browser {probe} needs repository formatting")
        if args.tidy or all_checks or analyzer or inventory_only:
            entries, report = inventory(root, build)
            directory = engine.analysis_directory(root)
            (directory / "inventory.json").write_text(json.dumps(report, indent=2) + "\n")
            if inventory_only and not (args.tidy or all_checks or analyzer):
                return 0
            llvm_major = engine.pinned_llvm_major(root)
            tidy = shutil.which(f"clang-tidy-{llvm_major}")
            if not tidy:
                raise EngineError(f"clang-tidy {llvm_major} is required")
            actual = engine.executable_version(tidy, ("--version",), root)
            if not engine.version_tuple(actual) or engine.version_tuple(actual)[0] != llvm_major:
                raise EngineError(f"Browser analysis requires reference clang-tidy {llvm_major}")
            policy = root / ("config/clang-analyzer.yml" if analyzer and not (args.tidy or all_checks) else ".clang-tidy")
            report["analysis"] = {"tool_version": actual, "tool_sha256": engine.file_sha256(Path(tidy)),
                                  "policy": str(policy.relative_to(root)), "policy_sha256": engine.file_sha256(policy)}
            (directory / "inventory.json").write_text(json.dumps(report, indent=2) + "\n")
            sysroot = emscripten / "cache/sysroot"
            commands = analysis_commands(root, entries, tidy, sysroot)
            if not commands or len(commands) != len(entries):
                raise EngineError("Browser analysis omitted required translation units")
            if args.tidy or all_checks:
                engine.run_analysis_commands(root, commands, env=env, directory=directory)
            if analyzer:
                if args.tidy or all_checks:
                    directory = engine.analysis_directory(root)
                    policy = root / "config/clang-analyzer.yml"
                    report["analysis"].update(policy=str(policy.relative_to(root)), policy_sha256=engine.file_sha256(policy))
                    (directory / "inventory.json").write_text(json.dumps(report, indent=2) + "\n")
                for command in commands:
                    command.insert(1, f"--config-file={root / 'config/clang-analyzer.yml'}")
                engine.run_analysis_commands(root, commands, env=env, directory=directory)
        return 0
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        raise EngineError(str(error)) from error
    finally:
        init_progress.stop()
