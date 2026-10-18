"""The installed ``ludus`` CLI: SDK and project commands (P07).

This is one client of the shared backend; the Editor is the other. The CLI works
without Qt or a Ludus checkout. No command string is ever passed through a shell.
Human output uses stable exit statuses; ``--json`` emits a versioned structured
result for scripting (the Editor does not parse human text — it uses the private
streaming protocol in editor_tool.py).

Commands:

    ludus tools install-butler <destination>
    ludus sdk list
    ludus sdk install --archive <sdk-archive> [--repair] [--store <dir>]
    ludus sdk remove --digest <sha256>
    ludus project create <destination> --name <name> --template minimal
                         (--engine <release> | --sdk <prefix>)
    ludus project configure <project> [--profile <preset>] [--sdk <prefix>]
    ludus project build     <project> [--profile <preset>] [--sdk <prefix>]
    ludus project run       <project> [--profile <preset>] [--sdk <prefix>]
    ludus project engine    <project> (--sdk <prefix> | --clear-override)
                                       [--profile <preset>]
    ludus project engine    <project> --status --tools <checkout> [--profile <preset>]
    ludus project engine    <project> --list   --tools <checkout> [--profile <preset>]
    ludus project migrate   <project> --engine <release>
"""

from __future__ import annotations

import argparse
import json as _json
import sys
import os
from pathlib import Path
from typing import Optional

from . import __version__, create
from .errors import (
    EXIT_CANCELLED,
    EXIT_FAILED,
    EXIT_OK,
    EXIT_UNAVAILABLE,
    EXIT_USAGE,
    INSTALL_CANCELLED,
    SDK_NOT_FOUND,
    UNRESOLVED_LOCK,
    ToolingError,
)
from .lockfile import parse_local_settings_file
from .sdkstore import SdkStore


def _emit(args, payload: dict) -> None:
    if getattr(args, "json", False):
        print(_json.dumps({"schema": 1, **payload}, ensure_ascii=False))


def _store(args) -> SdkStore:
    root = Path(args.store) if getattr(args, "store", None) else None
    return SdkStore(root)


def _exit_for_code(code: str) -> int:
    if code == INSTALL_CANCELLED:
        return EXIT_CANCELLED
    if code in (SDK_NOT_FOUND, UNRESOLVED_LOCK, "MissingTools"):
        return EXIT_UNAVAILABLE
    return EXIT_FAILED


# --- sdk commands ------------------------------------------------------------


def cmd_sdk_list(args) -> int:
    store = _store(args)
    installed = store.list_installed()
    _emit(args, {"sdks": [
        {"flavor": s.identity.flavor, "variant": s.identity.sdk_variant,
         "version": s.identity.engine_version, "revision": s.identity.source_revision,
         "digest": s.digest, "prefix": str(s.prefix)}
        for s in installed
    ]})
    if not getattr(args, "json", False):
        if not installed:
            print(f"No SDKs installed under {store.sdk_root}")
        for s in installed:
            print(f"{s.identity.flavor:12} {s.identity.engine_version:10} "
                  f"rev={s.identity.source_revision or '?':12} {s.digest[:16]}  {s.prefix}")
    return EXIT_OK


def cmd_sdk_install(args) -> int:
    store = _store(args)
    if args.archive:
        installed = store.install_archive(
            Path(args.archive), expected_digest=args.digest, repair=args.repair
        )
    elif args.version:
        from .catalog import download_entry, parse_catalog_file

        if not args.catalog:
            raise ToolingError("MissingTools", "--version install requires --catalog <catalog.json>")
        catalog = parse_catalog_file(Path(args.catalog))
        entry = catalog.find(args.version, args.target, args.flavor)
        if entry is None:
            raise ToolingError(
                "SdkNotFound",
                f"catalog has no {args.version}/{args.target}/{args.flavor} package",
            )
        archive = download_entry(entry, store.tmp_root)
        try:
            installed = store.install_archive(archive, expected_digest=entry.sha256, repair=args.repair)
        finally:
            Path(archive).unlink(missing_ok=True)
    else:
        raise ToolingError("InvalidProject", "provide --archive <path> or --version <release>")
    _emit(args, {"installed": {"flavor": installed.identity.flavor,
                               "digest": installed.digest, "prefix": str(installed.prefix)}})
    if not getattr(args, "json", False):
        print(f"Installed {installed.identity.flavor} SDK at {installed.prefix}")
    return EXIT_OK


def cmd_sdk_remove(args) -> int:
    store = _store(args)
    removed = store.remove(args.digest)
    _emit(args, {"removed": removed})
    if not getattr(args, "json", False):
        print("Removed" if removed else "No matching SDK")
    return EXIT_OK if removed else EXIT_FAILED


# --- project commands --------------------------------------------------------


def cmd_project_setup(args) -> int:
    from .project_setup import check_project, repair_project
    options = dict(tooling_root=Path(args.tools).resolve(), profile=args.profile)
    if args.cmd == "check":
        message = check_project(Path(args.project), **options)
    else:
        message = repair_project(Path(args.project), sdk=Path(args.sdk) if args.sdk else None,
                                 web_sdk=Path(args.web_sdk) if args.web_sdk else None,
                                 disable_web=args.no_web, **options)
    _emit(args, {"setup": message})
    if not getattr(args, "json", False):
        print(message)
    return 0


def cmd_project_create(args) -> int:
    components = [c for c in (args.components or "FoundationBase").split(",") if c]
    local_prefix: Optional[Path] = Path(args.sdk) if args.sdk else None
    if Path(args.destination).exists():
        raise ToolingError("DestinationExists", "New Project destination already exists")
    if args.tools and not args.engine:
        from .creation_engine import select_creation_sdk, prepare_creation_sdk
        from .project_setup import run_command
        root = Path(args.tools).resolve()
        local_prefix = select_creation_sdk(root, explicit=local_prefix,
            profile=args.profile,
            prepare=lambda prefix: prepare_creation_sdk(root, prefix, runner=run_command, profile=args.profile))
    if not args.engine and not local_prefix:
        raise ToolingError("InvalidProject", "create requires --engine <release> or --sdk <prefix>")
    verifier = None
    if args.tools:
        from .project_setup import repair_project
        verifier = lambda staged: repair_project(staged, tooling_root=Path(args.tools).resolve(), sdk=local_prefix)
    result = create.create_project(
        Path(args.destination),
        name=args.name,
        template_id=args.template,
        engine_version=args.engine or "",
        components=components,
        preset=args.profile,
        local_sdk_prefix=local_prefix,
        verify_staged=verifier,
        release=args.release,
        itch_target=args.itch_target,
    )
    _emit(args, {"created": str(result.destination), "resolved": result.lock.resolved})
    if not getattr(args, "json", False):
        print(f"Created project at {result.destination}")
        if not result.lock.resolved:
            if local_prefix:
                print("Using the selected local engine; the release lock remains unresolved until a release is published.")
            else:
                print("Lock is unresolved (no published release yet); select a local engine before building.")
    return EXIT_OK


def _resolve(args):
    from . import operations

    store = _store(args)
    return operations.resolve_project(
        Path(args.project),
        store=store,
        cli_sdk_prefix=Path(args.sdk) if getattr(args, "sdk", None) else None,
        profile=getattr(args, "profile", None),
    )


def cmd_project_configure(args) -> int:
    from . import operations

    resolved = _resolve(args)
    return EXIT_OK if operations.op_configure(resolved) == 0 else EXIT_FAILED


def cmd_project_build(args) -> int:
    from . import operations

    resolved = _resolve(args)
    return EXIT_OK if operations.op_build(resolved) == 0 else EXIT_FAILED


def cmd_project_run(args) -> int:
    from . import operations

    resolved = _resolve(args)
    code = operations.op_run(resolved)
    return EXIT_OK if code == 0 else EXIT_FAILED


def cmd_project_engine(args) -> int:
    # Explicit local override set/clear. Never rewrites the committed lock.
    from . import operations

    paths = operations.locate_project(Path(args.project))
    settings = parse_local_settings_file(paths.local_settings_path)
    from .descriptor import parse_descriptor_file
    from .native import target_for_profile, validate_target
    profile = args.profile or parse_descriptor_file(paths.descriptor_path).preset
    flavor = operations.PRESET_FLAVOR.get(profile)
    if flavor is None:
        raise ToolingError("InvalidProject", f"unknown profile {profile!r}")
    # target triple derived from the lock or defaulted.
    from .lockfile import parse_lock_file

    lock = parse_lock_file(paths.lock_path)
    target = operations._target_triple_for_lock(lock, flavor, target_for_profile(profile)) or target_for_profile(profile)
    if args.clear_override:
        changed = settings.clear_override(target, flavor)
        paths.local_settings_path.parent.mkdir(parents=True, exist_ok=True)
        paths.local_settings_path.write_bytes(settings.serialize())
        if not getattr(args, "json", False):
            print("Cleared override" if changed else "No override to clear")
        _emit(args, {"cleared": changed})
        return EXIT_OK
    if args.status or args.list:
        return _report_engine_pin(args, paths, profile, target, flavor)
    if not args.sdk:
        raise ToolingError("InvalidProject", "engine requires --sdk <prefix>, --clear-override, --status or --list")
    from .engine_pin import validate_install_prefix
    from .identity import require_compatible

    # Rejects build folders and folders without an installed manifest by name.
    identity = validate_install_prefix(Path(args.sdk).resolve())
    require_compatible(identity, required_flavor=flavor)
    validate_target(identity, profile)
    descriptor = parse_descriptor_file(paths.descriptor_path)
    if identity.engine_version != descriptor.engine.version:
        raise ToolingError("SdkIncompatible", "SDK engine version does not match the project requirement")
    settings.set_override(target, flavor, str(Path(args.sdk).resolve()))
    paths.local_settings_path.parent.mkdir(parents=True, exist_ok=True)
    paths.local_settings_path.write_bytes(settings.serialize())
    if not getattr(args, "json", False):
        print(f"Set local override for {flavor}: {Path(args.sdk).resolve()}")
        print("The committed lock is unchanged.")
    _emit(args, {"override": str(Path(args.sdk).resolve()), "flavor": flavor})
    return EXIT_OK


def _report_engine_pin(args, paths, profile: str, target: str, flavor: str) -> int:
    """Show the selected install versus the tooling checkout, or list this checkout's installs."""
    from .engine_pin import (are_revisions_equal, checkout_revision, diagnose_engine_pin, editor_install_root,
                             is_known_revision)
    from .identity import load_prefix_manifest
    if not args.tools:
        raise ToolingError("InvalidProject", "--status and --list require --tools <Ludus checkout>")
    tools = Path(args.tools).resolve()
    revision = checkout_revision(tools)
    if args.status:
        pin = diagnose_engine_pin(paths.descriptor_path, tooling_root=tools, profile=profile, editor_revision=revision)
        payload = pin.to_json() if pin is not None else {"state": "none", "editor_revision": revision,
                                                         "message": "No local engine install is selected."}
        _emit(args, {"engine_pin": payload})
        if not getattr(args, "json", False):
            print(payload["message"])
            if pin is not None:
                print(f"Installation: {pin.installation}")
        return EXIT_OK
    installs = []
    root = editor_install_root(tools)
    for candidate in sorted(p for p in root.iterdir() if p.is_dir()) if root.is_dir() else []:
        try:
            identity = load_prefix_manifest(candidate)
        except ToolingError:
            continue
        if identity.target_triple != target or identity.flavor != flavor:
            continue
        installs.append({"prefix": str(candidate), "revision": identity.source_revision,
                         "version": identity.engine_version,
                         "current": is_known_revision(revision) and are_revisions_equal(identity.source_revision, revision)})
    _emit(args, {"installs": installs, "editor_revision": revision})
    if not getattr(args, "json", False):
        if not installs:
            print(f"No {flavor} engine installs for {target} under {root}")
        for item in installs:
            marker = "current" if item["current"] else "older"
            print(f"{item['revision'][:7] or 'unknown':8} {marker:8} {item['prefix']}")
    return EXIT_OK


def cmd_project_migrate(args) -> int:
    components = [c for c in (args.components or "FoundationBase").split(",") if c]
    result = create.migrate_v1_to_v2(Path(args.project), engine_version=args.engine, components=components)
    _emit(args, {"migrated": str(result.destination)})
    if not getattr(args, "json", False):
        print(f"Migrated {result.destination} to version 2 (lock unresolved; select an SDK to build).")
    return EXIT_OK


def cmd_package_dispatch(args) -> int:
    if args.project == "verify":
        if not args.package or args.profile or args.version or args.sdk:
            raise ToolingError("InvalidRelease", "use project package verify <package-directory>")
        return cmd_package_verify(args)
    if args.package or not args.profile or not args.version:
        raise ToolingError("InvalidRelease", "packaging requires --profile and --version")
    return cmd_project_package(args)


def cmd_project_package(args) -> int:
    from .release import package_project

    package = package_project(Path(args.project), profile=args.profile, version=args.version,
                              store=_store(args), sdk=Path(args.sdk) if args.sdk else None)
    _emit(args, {"package": str(package), "archiveSha256": package.name})
    if not getattr(args, "json", False):
        print(f"Package: {package}\nSHA256: {package.name}")
    return EXIT_OK


def cmd_package_verify(args) -> int:
    from .package_verify import verify_package

    result = verify_package(Path(args.package))
    _emit(args, {"verified": result})
    if not getattr(args, "json", False):
        print(f"Verified {result['profile']}: {result['archiveSha256']}")
    return EXIT_OK


def cmd_publish_plan(args) -> int:
    from .release import publish_plan

    result = publish_plan(Path(args.project), Path(args.package), destination=args.destination,
                          allow_local_inputs=args.allow_local_inputs)
    _emit(args, {"plan": result})
    if not getattr(args, "json", False):
        print(_json.dumps(result, indent=2))
    return EXIT_OK


# --- parser ------------------------------------------------------------------


def cmd_release_init(args) -> int:
    from .release_setup import setup_release
    files = setup_release(Path(args.project), platform=args.platform, itch_target=args.itch_target,
                          tools_ref=args.tools_ref, build_command=args.build_command_json)
    _emit(args, {"files": files})
    if not args.json:
        print("Release setup saved: " + ", ".join(files))
    return EXIT_OK


def cmd_publish_upload(args) -> int:
    from .itch import upload
    result = upload(Path(args.project), Path(args.package), destination=args.destination,
                    allow_local_inputs=args.allow_local_inputs, expected_digest=args.expected_digest, target_override=args.itch_target or None)
    _emit(args, result)
    if not args.json:
        print("Upload submitted; receipt: " + result['receipt'])
    return EXIT_OK


def cmd_scripts_cook(args) -> int:
    from . import behavior_cook
    import subprocess
    try:
        result = behavior_cook.cook(*(Path(getattr(args, name)) for name in
            ("contract", "package", "output", "profile", "compiler", "analyzer")))
    except (KeyError, TypeError, RecursionError, subprocess.SubprocessError) as error:
        detail = getattr(error, "stderr", b"") or b""
        raise ToolingError("BehaviorCookFailed", str(error) + "\n" + detail[-8192:].decode(errors="replace")) from error
    _emit(args, {"cooked": str(result)})
    if not getattr(args, "json", False):
        print(f"Cooked behavior package: {result}")
    return EXIT_OK


def cmd_install_butler(args) -> int:
    from .butler import BUTLER_VERSION, host_artifact, install_butler
    artifact = host_artifact()
    binary = install_butler(Path(args.destination))
    _emit(args, {"tool": "butler", "version": BUTLER_VERSION, "platform": artifact.channel, "executable": str(binary)})
    if not getattr(args, "json", False):
        print(f"Installed butler {BUTLER_VERSION}: {binary}")
    return EXIT_OK


def cmd_render_graph(args) -> int:
    from . import render_graph

    if args.graph_action == "watch":
        from . import render_graph_reload
        if not args.runtime_package or not (args.worker_socket or os.environ.get('LUDUS_SHADER_WORKER_SOCKET')):
            raise ToolingError("RenderGraphReload", "watch requires --runtime-package and a native --worker-socket")
        def emit_event(payload):
            _emit(args, payload)
            sys.stdout.flush()
        try:
            render_graph_reload.watch(args.manifest, args.output, args.runtime_package, emit_event,
                configuration=args.configuration, helper=args.helper, compiler=args.compiler,
                validator=args.validator, spirv_cross=args.spirv_cross, root=args.root, worker_socket=args.worker_socket)
        except KeyboardInterrupt:
            pass
        return EXIT_OK
    if args.graph_action in ("schema-diff", "migrate"):
        from . import render_graph_authoring
        if args.graph_action == "schema-diff":
            payload = {"schema_diff": render_graph_authoring.compare_packages(Path(args.old), Path(args.new))}
        else:
            values = _read_json_argument(args.values, "--values")
            defaults = _read_json_argument(args.defaults, "--defaults") if args.defaults else None
            payload = {"migration": render_graph_authoring.migrate_values(Path(args.old), Path(args.new), values, defaults)}
        human = _json.dumps(payload, indent=2)
    elif args.graph_action in ("new-build", "build", "verify-package", "emit-runtime", "cook-runtime"):
        from . import render_graph_build
        if args.graph_action == "cook-runtime":
            from . import render_graph_execution
            payload = {"runtime": render_graph_execution.cook_runtime(Path(args.package), Path(args.output),
                        configuration=args.configuration)}
        elif args.graph_action == "emit-runtime":
            from . import render_graph_execution
            payload = {"execution": render_graph_execution.emit_runtime(Path(args.package), Path(args.output),
                        configuration=args.configuration, name=args.name)}
        elif args.graph_action == "new-build":
            created = render_graph_build.new_build_sources(Path(args.graph), profiles=args.profiles, root=args.root, renderer_static=args.renderer_static)
            payload = {"created": str(created), "stage": "RG1"}
        elif args.graph_action == "verify-package":
            payload = {"package": render_graph_build.verify_package(Path(args.package))}
        else:
            options = dict(helper=args.helper, compiler=args.compiler, validator=args.validator,
                           spirv_cross=args.spirv_cross, root=args.root, worker_socket=args.worker_socket)
            if args.runtime_package:
                from . import render_graph_reload
                payload = render_graph_reload.rebuild(args.manifest, args.output, args.runtime_package,
                            configuration=args.configuration, **options)
            else:
                payload = {"build": render_graph_build.build_package(args.manifest, args.output, **options)}
        human = _json.dumps(payload, indent=2)
    elif args.graph_action == "new":
        path = render_graph.new_workspace(Path(args.destination), output=args.output)
        payload = {"created": str(path), "stage": "RG0"}
        human = f"Saved graph sources at {path}; RG0 validates contracts, not GPU execution."
    elif getattr(args, "library", False):
        if args.configuration is not None:
            raise ToolingError("RenderGraphConfiguration", "--library inspects all configurations; omit --configuration")
        payload = {"library": render_graph.load_library(Path(args.graph), root=args.root)}
        human = _json.dumps(payload["library"], indent=2)
    else:
        plan = render_graph.compile_graph(Path(args.graph), configuration=args.configuration, root=args.root)
        payload = {"plan": plan}
        human = render_graph.dump_plan(plan) if args.graph_action == "dump" else (
            f"Validated {plan['asset']}#{plan['configuration']}: {len(plan['passes'])} serial operations; "
            f"{len(plan['techniques'])} authored techniques. RG0 contract only.")
    _emit(args, payload)
    if not args.json:
        print(human)
    # A rejected migration is reported in full but is not a successful result.
    return EXIT_FAILED if payload.get("migration", {}).get("status") == "Rejected" else EXIT_OK


def _read_json_argument(path, flag):
    """Read a bounded UTF-8 JSON input file without following it into a project write."""
    try:
        with open(path, "rb") as stream:
            data = stream.read(256 * 1024 + 1)
    except OSError as error:
        raise ToolingError("RenderGraphMigration", f"{flag}: {error}") from error
    if len(data) > 256 * 1024:
        raise ToolingError("RenderGraphCapacity", f"{flag}: input exceeds 256 KiB")
    try:
        return _json.loads(data.decode("utf-8"), parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ToolingError("RenderGraphMigration", f"{flag}: {error}") from error


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="ludus", description="Ludus host tooling CLI")
    p.add_argument("--version", action="version", version=f"ludus {__version__}")
    p.add_argument("--json", action="store_true", help="emit a versioned JSON result")
    p.add_argument("--store", help="SDK store root (default: $LUDUS_SDK_STORE or the native user data directory)")
    sub = p.add_subparsers(dest="group", required=True)
    graphs = sub.add_parser("render-graph", help="graph contracts and offline shader packages (no GPU execution)").add_subparsers(dest="graph_action", required=True)
    new_graph = graphs.add_parser("new", help="save RasterizeGraph and a root graph in a new directory")
    new_graph.add_argument("destination")
    new_graph.add_argument("--output", choices=("presentation", "offscreen", "depth"), default="presentation")
    new_graph.set_defaults(func=cmd_render_graph)
    for action in ("validate", "dump"):
        graph = graphs.add_parser(action)
        graph.add_argument("graph", help="saved .lurender.json root graph")
        graph.add_argument("--root", help="graph package root; imports are relative to it (default: root file directory)")
        graph.add_argument("--configuration", help="select an explicitly declared root configuration")
        if action == "validate":
            graph.add_argument("--library", action="store_true", help="inspect all library interfaces; bindings require a containing graph")
        graph.set_defaults(func=cmd_render_graph)
    new_build = graphs.add_parser("new-build", help="save explicit RG1 build sources next to a root graph")
    new_build.add_argument("graph")
    new_build.add_argument("--profiles", nargs="+", choices=("spirv", "wgsl", "metal", "glsl"), required=True)
    new_build.add_argument("--root", help="containing graph/source package root")
    new_build.add_argument("--renderer-static", action="store_true", help="author the six-location Renderer static/instance profile")
    new_build.set_defaults(func=cmd_render_graph)
    for action in ("build", "watch"):
        build_graph = graphs.add_parser(action, help="headless shader package build and optional runtime publication")
        build_graph.add_argument("manifest", help=".lushader.json manifest; sibling .lurender.json root")
        build_graph.add_argument("output", help="owned build tree, cache and immutable generations")
        build_graph.add_argument("--root", help="source package root (default: manifest directory)")
        build_graph.add_argument("--helper", required=True, help="installed SDK lib/cmake/Ludus/shaders directory")
        build_graph.add_argument("--compiler", required=True, help="pinned Slang executable")
        build_graph.add_argument("--validator", help="pinned spirv-val; required for spirv/glsl")
        build_graph.add_argument("--spirv-cross", help="verified SPIRV-Cross; required for glsl")
        build_graph.add_argument("--worker-socket", help="existing headless native shader worker endpoint")
        build_graph.set_defaults(func=cmd_render_graph)
        build_graph.add_argument("--runtime-package", help="atomically cook/publish a reloadable .lurgrt generation")
        build_graph.add_argument("--configuration", default="Default")
    verify_graph = graphs.add_parser("verify-package", help="verify complete generation inventory without shader tools")
    verify_graph.add_argument("package")
    verify_graph.set_defaults(func=cmd_render_graph)
    emit_graph = graphs.add_parser("emit-runtime", help="emit a numeric Renderer header from a verified static package")
    emit_graph.add_argument("package", help="immutable generation or build tree with current.json")
    emit_graph.add_argument("output", help="generated .h destination")
    emit_graph.add_argument("--configuration", default="Default")
    emit_graph.add_argument("--name", default="DefaultGraph")
    emit_graph.set_defaults(func=cmd_render_graph)
    cook_graph = graphs.add_parser("cook-runtime", help="write a runtime-loadable generation for atomic activation (RG3)")
    cook_graph.add_argument("package", help="immutable generation or build tree with current.json")
    cook_graph.add_argument("output", help="generated .lurgrt destination")
    cook_graph.add_argument("--configuration", default="Default")
    cook_graph.set_defaults(func=cmd_render_graph)
    diff_graph = graphs.add_parser("schema-diff", help="compare schemas/layouts of two cooked generations (RG3)")
    diff_graph.add_argument("old")
    diff_graph.add_argument("new")
    diff_graph.set_defaults(func=cmd_render_graph)
    migrate_graph = graphs.add_parser("migrate", help="report migrated Uniform values; never writes sources (RG3)")
    migrate_graph.add_argument("old")
    migrate_graph.add_argument("new")
    migrate_graph.add_argument("--values", required=True, help="JSON schema -> field -> numbers")
    migrate_graph.add_argument("--defaults", help="JSON explicit defaults for added/changed fields")
    migrate_graph.set_defaults(func=cmd_render_graph)
    tools = sub.add_parser("tools", help="explicitly acquire optional host tools").add_subparsers(dest="cmd", required=True)
    butler = tools.add_parser("install-butler", help="download the pinned host butler executable; never logs in or uploads")
    butler.add_argument("destination", help="directory for butler; an existing executable is never replaced")
    butler.set_defaults(func=cmd_install_butler)
    scripts = sub.add_parser("scripts", help="cook project-owned behavior assets").add_subparsers(dest="cmd", required=True)
    cook = scripts.add_parser("cook", help="explicit paired-tool cook; never downloads tools")
    for flag in ("contract", "package", "output", "profile", "compiler", "analyzer"):
        cook.add_argument("--" + flag, required=True)
    cook.set_defaults(func=cmd_scripts_cook)

    sdk = sub.add_parser("sdk", help="manage installed SDKs").add_subparsers(dest="cmd", required=True)
    sdk.add_parser("list").set_defaults(func=cmd_sdk_list)
    inst = sdk.add_parser("install")
    inst.add_argument("--archive", help="install a local SDK archive")
    inst.add_argument("--version", help="install a release from --catalog")
    inst.add_argument("--catalog", help="release catalog JSON (required with --version)")
    inst.add_argument("--target", default="linux-x64")
    inst.add_argument("--flavor", default="development")
    inst.add_argument("--digest", help="expected sha256 of the archive")
    inst.add_argument("--repair", action="store_true")
    inst.set_defaults(func=cmd_sdk_install)
    rm = sdk.add_parser("remove")
    rm.add_argument("--digest", required=True)
    rm.set_defaults(func=cmd_sdk_remove)

    proj = sub.add_parser("project", help="create and operate projects").add_subparsers(dest="cmd", required=True)
    cr = proj.add_parser("create")
    cr.add_argument("destination")
    cr.add_argument("--name", required=True)
    cr.add_argument("--template", default="minimal")
    cr.add_argument("--engine", help="exact engine release")
    cr.add_argument("--sdk", help="local SDK prefix (recorded only in ignored local settings)")
    cr.add_argument("--components", help="comma-separated public modules to link")
    cr.add_argument("--profile", help="default preset")
    cr.add_argument("--tools", help="verify setup/build/tests using this prepared tooling checkout before publishing")
    cr.add_argument("--release", action="store_true", help="include native release configuration/install rules")
    cr.add_argument("--itch-target", help="optional username/game for offline release planning")
    cr.set_defaults(func=cmd_project_create)

    for name, func in (("configure", cmd_project_configure), ("build", cmd_project_build), ("run", cmd_project_run)):
        sp = proj.add_parser(name)
        sp.add_argument("project")
        sp.add_argument("--profile")
        sp.add_argument("--sdk", help="per-operation local SDK override")
        sp.set_defaults(func=func)

    for action in ("check", "repair", "update"):
        setup = proj.add_parser(action, help="check or verify local CMake setup")
        setup.add_argument("project")
        setup.add_argument("--tools", required=True, help="trusted prepared Ludus tooling checkout")
        setup.add_argument("--sdk")
        setup.add_argument("--profile", help="native profile to check/repair (defaults to the project descriptor)")
        setup.add_argument("--web-sdk")
        if action != "check":
            setup.add_argument("--no-web", action="store_true", help="repair desktop builds only; clear saved browser setup")
        setup.set_defaults(func=cmd_project_setup)

    eng = proj.add_parser("engine")
    eng.add_argument("project")
    eng.add_argument("--sdk")
    eng.add_argument("--clear-override", action="store_true")
    eng.add_argument("--status", action="store_true", help="compare the selected install with --tools' revision")
    eng.add_argument("--list", action="store_true", help="list engine installs owned by the --tools checkout")
    eng.add_argument("--tools", help="trusted Ludus tooling checkout (for --status and --list)")
    eng.add_argument("--profile")
    eng.set_defaults(func=cmd_project_engine)

    mig = proj.add_parser("migrate")
    mig.add_argument("project")
    mig.add_argument("--engine", required=True)
    mig.add_argument("--components")
    mig.set_defaults(func=cmd_project_migrate)

    pkg = proj.add_parser("package", help="build a native Release package, or verify an existing package")
    pkg.add_argument("project", help="project path, or the literal 'verify'")
    pkg.add_argument("package", nargs="?")
    pkg.add_argument("--profile")
    pkg.add_argument("--version")
    pkg.add_argument("--sdk", help="explicit Release SDK override")
    pkg.set_defaults(func=cmd_package_dispatch)

    setup = proj.add_parser("release", help="set up release files for an existing project").add_subparsers(dest="release_action", required=True)
    init = setup.add_parser("init")
    init.add_argument("project")
    init.add_argument("--platform", choices=("linux-x64", "macos", "web"), default="macos" if sys.platform == "darwin" else "linux-x64")
    init.add_argument("--itch-target")
    init.add_argument("--tools-ref")
    init.add_argument("--build-command-json", type=_json.loads, help="project bootstrap argv as a JSON array, without a shell")
    init.set_defaults(func=cmd_release_init)

    publish = proj.add_parser("publish", help="inspect an offline upload plan").add_subparsers(dest="action", required=True)
    plan = publish.add_parser("plan")
    plan.add_argument("project")
    plan.add_argument("--package", required=True)
    plan.add_argument("--destination", required=True)
    plan.add_argument("--allow-local-inputs", action="store_true")
    plan.set_defaults(func=cmd_publish_plan)

    upload = publish.add_parser("upload", help="explicitly upload a verified package to itch.io")
    upload.add_argument("project")
    upload.add_argument("--package", required=True)
    upload.add_argument("--destination", required=True)
    upload.add_argument("--allow-local-inputs", action="store_true")
    upload.add_argument("--expected-digest")
    upload.add_argument("--itch-target", help="destination for projects configured through a CI variable")
    upload.set_defaults(func=cmd_publish_upload)
    return p


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except ToolingError as exc:
        if getattr(args, "json", False):
            error = {"code": exc.code, "message": exc.message}
            if exc.location is not None:
                error["location"] = exc.location
            if exc.diagnostics is not None:
                error["diagnostics"] = exc.diagnostics
            print(_json.dumps({"schema": 1, "error": error}))
        else:
            print(f"error [{exc.code}]: {exc.message}", file=sys.stderr)
        return _exit_for_code(exc.code)
    except (OSError, ValueError) as exc:
        if getattr(args, "json", False):
            print(_json.dumps({"schema": 1, "error": {"code": "OperationFailed", "message": str(exc)}}))
        else:
            print(f"error [OperationFailed]: {exc}", file=sys.stderr)
        return EXIT_FAILED
    except KeyboardInterrupt:
        return EXIT_CANCELLED


if __name__ == "__main__":
    raise SystemExit(main())
