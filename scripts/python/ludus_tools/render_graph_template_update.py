"""Explicit, conservative upgrades of copied template shaders; authored edits are preserved."""
from pathlib import Path
import hashlib
import os
import tempfile

from . import render_graph_build as build
from . import render_graph_templates as templates

# Original initial RIS source shipped before batched emitter selection. Exact bytes,
# rather than a filename or callback-name heuristic, establish an unedited template.
RESTIR_INITIAL_V1_SHA256 = '9e731a473fb2ff1a140b44b10c857732237040a1a67000b577204c42bc5a8d87'


def publish_new(path, data):
    """Publish complete same-directory bytes exclusively; retain any existing destination."""
    descriptor, temporary = tempfile.mkstemp(prefix='.template-update-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def update_restir_sampling(manifest_path, *, apply=False):
    """Preview/apply a copied-template upgrade, backing up originals and preserving custom sources.

    Save and close the editor graph first. Package-confined snapshots detect disk changes;
    the include is published before its callback. A failed write rolls back this operation's
    bytes when they have not changed externally. Backups remain under the package's out/.
    """
    from . import render_graph as graph
    manifest_path = Path(manifest_path).resolve()
    snapshot = build.Snapshot(manifest_path.parent)
    manifest = build.document(snapshot.load(manifest_path.name), str(manifest_path))
    build.validate_manifest(manifest)
    canonical = templates.template_files('restir-di', manifest['profiles'], graph)
    initial = canonical['main_shaders/initial.slang'].encode()
    helper = canonical['main_shaders/initial_emitter_sampling.slang'].encode()
    changes = {}
    custom = []
    unchanged = []
    visited = set()
    for program in manifest['programs']:
        source = program['source']
        if program['stage'] != 'closest_hit' or source in visited:
            continue
        visited.add(source)
        previous = snapshot.load(source)
        is_current = previous == initial
        if not is_current and hashlib.sha256(previous).hexdigest() != RESTIR_INITIAL_V1_SHA256:
            if program['id'].endswith('InitialClosest'):
                custom.append(source)
            continue
        helper_path = (Path(source).parent / 'initial_emitter_sampling.slang').as_posix()
        target = snapshot.path(helper_path)
        if target.exists():
            build.require(snapshot.load(helper_path) == helper, 'Conflict', helper_path,
                          'existing helper differs; no files changed')
        else:
            changes[helper_path] = (None, helper)
        if is_current:
            unchanged.append(source)
        else:
            changes[source] = (previous, initial)
    result = {'template': 'restir-di', 'applied': False, 'updates': sorted(changes),
              'custom_sources_preserved': sorted(custom), 'unchanged': sorted(unchanged)}
    if not apply or not changes:
        return result
    lock = snapshot.root / '.ludus-editor-render-graph.lock'
    build.require(not lock.exists(), 'Conflict', str(lock), 'editor Save is active; retry after closing the graph')
    snapshot.verify()
    backup_root = snapshot.path('out/template-updates/restir-initial-sampling-v2')
    # Prepare every backup before changing any authored bytes. Never replace another backup.
    for relative, (previous, _) in changes.items():
        if previous is None:
            continue
        backup = snapshot.path((backup_root.relative_to(snapshot.root) / relative).as_posix())
        backup.parent.mkdir(parents=True, exist_ok=True)
        if backup.exists():
            build.require(build.read(backup) == previous, 'Conflict', str(backup), 'backup already has different bytes')
        else:
            publish_new(backup, previous)
    written = []
    try:
        for relative, (previous, replacement) in changes.items():
            target = snapshot.path(relative)
            if previous is None:
                # Do not replace a file created after the snapshot.
                publish_new(target, replacement)
            else:
                build.require(build.read(target) == previous, 'Conflict', relative, 'source changed during upgrade')
                build.atomic(target, replacement)
            written.append(relative)
    except BaseException:
        for relative in reversed(written):
            target = snapshot.path(relative)
            previous, replacement = changes[relative]
            if target.exists() and build.read(target) == replacement:
                if previous is None:
                    target.unlink()
                else:
                    build.atomic(target, previous)
        raise
    result['applied'] = True
    result['backup'] = str(backup_root)
    return result
