"""Headless development rebuild/watch coordinator over the shared native compiler.

Build/cook owns source snapshots; application-side activation remains a separate
transaction. A compiler or source failure never replaces the published runtime file.
"""
from pathlib import Path
import hashlib
import os
import time

from . import render_graph_build as build, render_graph_execution as execution
from .buildlock import BuildTreeLock
from .errors import ToolingError


def fingerprint(root):
    files = {}
    total = 0
    for directory, names, entries in os.walk(root):
        names[:] = sorted(name for name in names if not name.startswith('.') and name != 'out')
        for name in sorted(entries):
            path = Path(directory) / name
            if name.startswith('.') or path.suffix not in ('.json', '.slang'):
                continue
            relative = path.relative_to(root)
            build.require(not path.is_symlink(), 'Source', str(relative), 'symlink sources are not admitted')
            build.require(len(files) < build.MAX_FILES, 'Capacity', str(root), 'source file count exceeded')
            data = build.read(path)
            total += len(data)
            build.require(total <= build.MAX_BYTES, 'Capacity', str(root), 'source byte count exceeded')
            files[relative.as_posix()] = hashlib.sha256(data).hexdigest()
    return files


def rebuild(manifest, output, runtime_package, *, configuration='Default', **options):
    manifest = Path(manifest).resolve()
    output = Path(output).resolve()
    root = Path(options.get('root') or manifest.parent).resolve()
    destination = Path(runtime_package).absolute()
    build.require(not output.is_relative_to(root) and not destination.is_relative_to(root),
                  'Source', 'reload', 'source and generated output roots must be separate')
    destination.parent.mkdir(parents=True, exist_ok=True)
    before = fingerprint(root)
    # Distinct publication lock spans compilation and cooking without recursively
    # acquiring the builder's own output lock. Two reload publishers cannot race.
    with BuildTreeLock(destination.parent / ('.' + destination.name + '.publication')):
        report = build.build_package(manifest, output, **options)
        build.checkpoint(options.get('cancel'))
        staged = output / 'runtime-publication' / 'candidate.lurgrt'
        cooked = execution.cook_runtime(report['package'], staged, configuration=configuration)
        build.require(fingerprint(root) == before, 'Stale', 'reload', 'sources changed before runtime publication')
        build.checkpoint(options.get('cancel'))
        # Names first, so a reader that sees the new package also finds its matching names.
        names = execution.resource_names_path(destination)
        build.atomic(names, execution.resource_names_path(staged).read_bytes())
        build.atomic(destination, staged.read_bytes())
        cooked['runtime_package'] = str(destination)
        cooked['resource_names'] = str(names)
        return {'build': report, 'runtime': cooked}


def ray_path_capture_sources(generation):
    """Return the verified generation, its saved source root and saved build manifest."""
    generation = execution.resolve_generation(generation)
    package = build.verify_package(generation)
    sources = generation / 'sources'
    manifest = sources / package['source_map']['build_manifest']
    build.require(manifest.resolve().is_relative_to(sources.resolve()) and manifest.is_file(), 'Package',
                  str(generation), 'the generation carries no saved build manifest')
    return generation, sources, manifest


def rebuild_ray_path_capture(generation, output, destination, *, configuration='Default', **options):
    """Compile and cook the ray path capture variant of one published generation.

    The variant is linked from the generation's own saved source snapshot, never the
    live project sources, so it traces exactly what the runtime package cooked from
    that generation traces. output is the capture build tree, separate from the
    generation's tree; destination receives the editor-only diagnostic package
    (cook_ray_path_capture). A failure preserves the previous diagnostic package.
    """
    generation, sources, manifest = ray_path_capture_sources(generation)
    options = {key: value for key, value in options.items() if key != 'root'}
    report = build.build_package(manifest, output, root=sources, ray_path_capture=True, **options)
    build.checkpoint(options.get('cancel'))
    cooked = execution.cook_ray_path_capture(report['package'], destination, configuration=configuration)
    return {'build': report, 'ray_path_capture': cooked, 'source_generation': generation.name}


def watch(manifest, output, runtime_package, emit, *, interval=0.2, configuration='Default', **options):
    """Watch explicit source roots; emit only build/error events, never unchanged polls."""
    root = Path(options.get('root') or Path(manifest).parent).resolve()
    previous = None
    while True:
        try:
            current = fingerprint(root)
            if current != previous:
                time.sleep(interval)
                stable = fingerprint(root)
                if stable != current:
                    continue
                previous = current
                try:
                    emit(rebuild(manifest, output, runtime_package, configuration=configuration, **options))
                except (ToolingError, OSError, ValueError, RuntimeError) as error:
                    emit({'error': {'code': getattr(error, 'code', 'RenderGraphReload'), 'message': str(error)}})
        except (ToolingError, OSError, ValueError) as error:
            message = {'error': {'code': getattr(error, 'code', 'RenderGraphReload'), 'message': str(error)}}
            if previous != message:
                emit(message)
                previous = message
        time.sleep(interval)
