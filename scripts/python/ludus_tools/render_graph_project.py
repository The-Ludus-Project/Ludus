"""Read-only project SDK/tool selection for graph builds; no setup or downloads."""
from pathlib import Path

from .errors import ToolingError
from .operations import locate_project, resolve_project
from .project_setup import executable, preset_for, read_object, spirv_validator
from .sdkstore import SdkStore
from .shader_tools import REPAIR_MESSAGE, required_shader_tools, verify_translator


def resolve_build_tools(project, tooling_root, manifest, *, helper=None, compiler=None,
                        validator=None, spirv_cross=None, store=None, assistance_only=False):
    """Resolve the current saved SDK and prepared tools for one immutable build.

    Explicit tool paths override detection for this operation. Project SDK
    precedence remains owned by resolve_project (local selection, locked store).
    Selection metadata is rechecked at the builder's cancellation checkpoints.
    """
    from .render_graph_build import HELPERS, document, read, validate_manifest

    project_paths = locate_project(Path(project))
    initial_paths = (project_paths.descriptor_path, project_paths.lock_path, project_paths.local_settings_path)
    def capture(paths):
        return {path: path.read_bytes() if path.exists() else None for path in paths}
    initial = capture(initial_paths)
    try:
        resolved = resolve_project(Path(project), store=store or SdkStore(), enforce_host_toolchain=False)
    except ToolingError as error:
        raise ToolingError(error.code, f"Cannot resolve this project's shader tools: {error.message} "
                           "Use Project > Repair Project Setup to check the selected engine.") from error
    paths = resolved.paths
    preset_path = paths.source_dir / "CMakeUserPresets.json"
    selection_paths = (paths.descriptor_path, paths.lock_path, paths.local_settings_path, preset_path,
                       resolved.resolution.prefix / "share/Ludus/LudusSdkManifest.json")
    def snapshot():
        return capture(selection_paths)
    if capture(initial_paths) != initial:
        raise ToolingError("RenderGraphProjectChanged", "Project SDK selection changed during resolution; "
                           "Build & Preview again.")
    before = snapshot()
    local = read_object(preset_path)
    selected = preset_for(paths.source_dir, resolved.descriptor.preset)
    preset = next((item for item in local.get("configurePresets", []) if item.get("name") == selected), {})
    cache = preset.get("cacheVariables", {})
    value = document(read(Path(manifest)), str(manifest))
    validate_manifest(value)
    required = set() if assistance_only else required_shader_tools(value)

    def configured_tool(key, fallback):
        name = cache.get(key)
        if isinstance(name, dict):
            name = name.get("value")
        if name is None:
            return fallback()
        if not isinstance(name, str) or not name or "$" in name:
            raise ToolingError("MissingTools", f"Cannot resolve {key} in the selected local preset. "
                               "Use Project > Repair Project Setup to refresh shader tools.")
        path = Path(name)
        return path if path.is_absolute() else paths.source_dir / path

    sdk = resolved.resolution.prefix.resolve()
    helper = Path(helper) if helper else sdk / "lib/cmake/Ludus/shaders"
    missing = [name for name in HELPERS if not (helper / name).is_file()]
    if missing:
        raise ToolingError("MissingTools", f"The selected SDK's shader helper is incomplete: {helper} "
                           f"(missing {', '.join(missing)}). Use Project > Repair Project Setup.")
    tooling_root = Path(tooling_root) if tooling_root else None

    def managed_tool(relative):
        if tooling_root is None:
            raise ToolingError("MissingTools", "Prepared shader tools are not recorded in this project's local "
                               "preset. Use Project > Repair Project Setup or supply a trusted tooling root.")
        return tooling_root / "out/shader-tools" / relative

    def prepared_executable(path):
        try:
            return executable(path)
        except ToolingError as error:
            raise ToolingError(error.code, REPAIR_MESSAGE) from error

    compiler = Path(compiler) if compiler else configured_tool("LUDUS_SLANG_COMPILER", lambda: managed_tool("slang/bin/slangc"))
    options = {"helper": helper, "compiler": prepared_executable(compiler)}
    if validator or "validator" in required:
        def managed_validator():
            if tooling_root is None:
                return managed_tool("spirv-tools/spirv-val")
            return spirv_validator(tooling_root)
        options["validator"] = prepared_executable(Path(validator) if validator else configured_tool("LUDUS_SPIRV_VALIDATOR", managed_validator))
    if spirv_cross or "spirv_cross" in required:
        options["spirv_cross"] = prepared_executable(Path(spirv_cross) if spirv_cross else configured_tool(
            "LUDUS_SPIRV_CROSS", lambda: managed_tool("spirv-cross/bin/spirv-cross")))
        verify_translator(options["spirv_cross"], helper / "spirv_cross_toolchain.json")

    targets = {path: path.resolve() for path in (resolved.resolution.prefix, *options.values())}
    def check_unchanged():
        if snapshot() != before or any(path.resolve() != target for path, target in targets.items()):
            raise ToolingError("RenderGraphProjectChanged", "Project SDK or shader-tool selection changed during "
                               "the build; the previous runtime is retained. Build & Preview again.")
        return False
    options["cancel"] = check_unchanged
    check_unchanged()
    metadata = {"sdk": str(sdk), "sdk_source": resolved.resolution.source,
                "profile": resolved.descriptor.preset,
                **{name: str(path) for name, path in options.items() if name != "cancel"}}
    return options, metadata
