"""Compiler-owned Slang editing contexts; never parse shader symbols.

Thanks to the Slang contributors, The Slang Shading Language VSCode Extension,
Features and Additional Search Path, for the semantic-service integration model:
https://github.com/shader-slang/slang-vscode-extension. Generated source uses
render_graph_build.prepare_sources/shader_source, the compilation owners.
"""
from pathlib import Path
from contextlib import contextmanager
import base64
import binascii
import re
import shutil

from . import render_graph_build as build

INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"\n]+)"[^\n]*(?:\n|$)', re.M)
MAX_CONTEXT_BYTES = build.MAX_BYTES * 2
# Captured input has its own wire budget, separate from decoded source ownership.
MAX_DRAFT_INPUT_BYTES = build.MAX_BYTES * 2
CONTEXT_DIRECTORY_PREFIX = 'context-'
CONTEXT_OWNER_FILE = '.ludus-shader-context'
CONTEXT_OWNER = b'Ludus shader assistance context version 1\n'


@contextmanager
def capture_draft(stream, root):
    """Decode a bounded immutable editor capture into a new private stage.

    Validate all paths/bytes before writing, never replace an existing directory,
    and remove the stage on success/failure. Project Save is never involved.
    """
    data = stream.read(MAX_DRAFT_INPUT_BYTES + 1)
    build.require(len(data) <= MAX_DRAFT_INPUT_BYTES, 'Capacity', 'shader draft', 'captured input exceeds its wire budget')
    value = build.document(data, 'shader draft')
    build.keys(value, ('version', 'files'), 'shader draft')
    build.require(type(value['version']) is int and value['version'] == 1, 'Schema', 'shader draft', 'version 1 required')
    files = value['files']
    build.require(type(files) is dict and 0 < len(files) <= build.MAX_FILES, 'Capacity', 'shader draft', 'captured file count exceeds the package limit')
    directory = Path(root).resolve()
    build.require(not directory.exists(), 'Source', str(directory), 'draft staging directory must be new; existing files are retained')
    snapshot = build.Snapshot(directory)
    decoded = {}
    total = 0
    for relative, encoded in files.items():
        snapshot.path(relative)
        build.require(type(encoded) is str, 'Schema', relative, 'base64 source bytes required')
        try:
            source = base64.b64decode(encoded, validate=True)
        except (binascii.Error, ValueError) as error:
            build.fail('Source', relative, str(error))
        total += len(source)
        build.require(len(source) <= build.MAX_FILE and total <= build.MAX_BYTES, 'Capacity', relative, 'captured source bytes exceed the package limit')
        decoded[relative] = source
    directory.mkdir(parents=True, exist_ok=False)
    try:
        for relative, source in decoded.items():
            path = snapshot.path(relative)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(source)
        yield directory
    finally:
        shutil.rmtree(directory, ignore_errors=True)



def find_includes(text):
    """Literal include locations only; leave preprocessing/semantics to Slang."""
    masked = re.sub(r'/\*.*?\*/|//[^\n]*', lambda match: re.sub(r'[^\n]', ' ', match[0]), text, flags=re.S)
    return INCLUDE.finditer(masked)


def export_context(manifest, output, source, *, helper, context=None, root=None):
    """Stage one exact compiler context and expose a single editable source span.

    Inputs must be an immutable draft snapshot, output a separate owned temporary
    directory. All dependencies are captured before any output write. The selected
    UTF-8 source is verbatim, including BOM/CRLF; surrounding includes become
    absolute paths inside this snapshot. No compiler/tool process or project write.
    Unmappable compatibility rewrites/repeated source occurrences fail explicitly.
    """
    snapshot, value, schemas, programs, _, _, closures, _, _, _ = build.prepare_sources(manifest, helper, root=root)
    snapshot.path(source)
    build.require(source in snapshot.files, 'Source', source, 'source is outside the selected build closure')
    output = Path(output).resolve()
    build.require(not snapshot.root.is_relative_to(output) and not output.is_relative_to(snapshot.root),
                  'Source', str(output), 'language output must be separate from source snapshot')
    candidates = []
    for identity, program in sorted(programs.items()):
        if source in closures[identity]:
            candidates.append({'id': identity, 'stage': program['stage'],
                               'programs': [p['id'] for p in value['programs'] if p['source'] == source]})
    build.require(candidates, 'Source', source, 'no executable program context includes this source; assign it to a graph pipeline')
    selected = context or candidates[0]['id']
    build.require(any(item['id'] == selected for item in candidates), 'Source', selected, 'context no longer includes this source')
    program = programs[selected]
    overrides = program.get('source_overrides', {})
    build.require(source not in overrides or overrides[source].encode() == snapshot.files[source],
                  'Source', source, 'legacy compatibility rewrites cannot map semantic edits; migrate this shader first')
    directory = output / (CONTEXT_DIRECTORY_PREFIX + build.identity({'program': program, 'files': {p: build.digest(snapshot.files[p]) for p in closures[selected] if p != source}}))
    compiled = {p: overrides[p].encode() if p in overrides else snapshot.files[p] for p in closures[selected]}
    source_text = snapshot.files[source].decode('utf-8')
    occurrences = []

    # A bounded reverse include graph finds ancestors without recursively scanning
    # the SDK for every include. This is source-location plumbing, never semantics.
    ancestors = {source}
    dependencies = {relative: {(Path(relative).parent / match[1]).as_posix()
                              for match in find_includes(data.decode())} for relative, data in compiled.items()}
    for _ in range(len(compiled)):
        additions = {relative for relative, children in dependencies.items() if children & ancestors}
        if additions <= ancestors:
            break
        ancestors.update(additions)

    def expand(text, relative, active=()):
        build.require(len(active) <= build.MAX_SHADER_INCLUDE_DEPTH, 'Source', relative, 'language include nesting exceeds limit')
        parts = []
        cursor = 0
        for match in find_includes(text):
            parts.append(text[cursor:match.start()])
            child = (Path(relative).parent / match[1]).as_posix()
            build.require(child in compiled, 'Source', child, 'generated include is not in the compiler closure')
            if child == source:
                occurrences.append(True)
                parts.append(None)  # exact authored span; resolved below, never rewritten
            elif child in ancestors:
                build.require(child not in active, 'Source', source, 'cannot map an editable source through an include cycle')
                # Only ancestors of the editable file are inlined. Everything else
                # retains its own file/pragma/include semantics in the snapshot.
                parts.extend(expand(compiled[child].decode(), child, (*active, child)))
            else:
                parts.append('#include "' + (directory / child).as_posix() + '"\n')
            cursor = match.end()
        parts.append(text[cursor:])
        return parts

    parts = expand(build.shader_source(program, schemas), 'wrapper.slang')
    build.require(len(occurrences) == 1, 'Source', source,
                  'this linked context includes the source more than once; choose a single-source program context')
    split = parts.index(None)
    prefix = ''.join(parts[:split])
    suffix = ''.join(parts[split + 1:])
    unit = prefix + source_text + '\n' + suffix
    build.require(len(unit.encode()) <= MAX_CONTEXT_BYTES, 'Capacity', source, 'language translation unit exceeds limit')
    unit_relative = (Path(source).parent / '__ludus_editor_context.slang').as_posix()
    build.require(unit_relative not in compiled, 'Source', source, 'reserved language context filename')
    snapshot.verify()
    if directory.exists():
        build.require(not directory.is_symlink() and (directory / CONTEXT_OWNER_FILE).is_file() and
                      (directory / CONTEXT_OWNER_FILE).read_bytes() == CONTEXT_OWNER,
                      'Source', str(directory), 'context output directory is not owned by shader assistance')
    directory.mkdir(parents=True, exist_ok=True)
    (directory / CONTEXT_OWNER_FILE).write_bytes(CONTEXT_OWNER)
    destination = build.Snapshot(directory)
    for relative, data in compiled.items():
        path = destination.path(relative)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    unit_path = destination.path(unit_relative)
    unit_path.write_text(unit, encoding='utf-8', newline='')
    # Cleanup belongs to the background owner too. Never remove unrelated output
    # directories or follow symlinks; only marked contexts created by this helper.
    for previous in output.iterdir():
        owner = previous / CONTEXT_OWNER_FILE
        if (previous != directory and not previous.is_symlink() and previous.is_dir() and
                previous.name.startswith(CONTEXT_DIRECTORY_PREFIX) and owner.is_file() and
                not owner.is_symlink() and owner.read_bytes() == CONTEXT_OWNER):
            shutil.rmtree(previous, ignore_errors=True)
    return {'version': 1, 'contexts': candidates, 'context': selected, 'profiles': value['profiles'],
            'manifest': Path(manifest).resolve().relative_to(snapshot.root).as_posix(),
            'uri': unit_path.as_uri(), 'prefix': prefix, 'suffix': '\n' + suffix}
