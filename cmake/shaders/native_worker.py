"""Bounded client of the headless native Slang worker; no engine/editor startup."""
import json
import hashlib
from pathlib import Path
import socket
import struct

MAX_MESSAGE_BYTES = 16 * 1024 * 1024


def receive(stream, count):
    result = bytearray()
    while len(result) < count:
        data = stream.recv(count - len(result))
        if not data:
            raise RuntimeError('Native shader worker disconnected')
        result.extend(data)
    return bytes(result)


def compile_stage(endpoint, args, lock, artifact, reflection, depfile):
    root = Path(args.source).parent
    files = {}
    for path in sorted(root.rglob('*')):
        if path.is_file():
            if path.is_symlink():
                raise RuntimeError('Native shader snapshot cannot contain symlinks')
            files[path.relative_to(root).as_posix()] = path.read_text(encoding='utf-8')
    request = {'version': 1, 'identity': 1, 'source': Path(args.source).name, 'entry': args.name,
               'stage': {'vertex': 0, 'fragment': 1, 'compute': 2}[args.graph_stage],
               'target': {'spirv': 0, 'glsl': 0, 'wgsl': 1, 'metal': 2}[args.graph_target], 'files': files}
    if getattr(args, 'graph_software_rays', False):
        request['target'] = 0  # Ordinary SPIR-V compute; no ray capability.
    if getattr(args, 'native_ray_queries', False):
        request['target'] = 3  # Native SPIR-V 1.5 query profile, also used before Metal translation.
    result = exchange(endpoint, request)
    if result.get('compiler') != lock['slang']['version']:
        raise RuntimeError('Native shader worker version differs from SDK pin')
    if args.worker_identity and result.get('implementation') != args.worker_identity:
        raise RuntimeError('Native shader worker changed during build')
    if result['status'] != 0:
        raise RuntimeError('\n' + (result.get('diagnostics') or 'Native shader worker rejected request'))
    binary = bytes.fromhex(result['artifact'])
    reflected = bytes.fromhex(result['reflection'])
    if not binary or not reflected or max(len(binary), len(reflected)) > 2 * 1024 * 1024:
        raise RuntimeError('Native shader worker returned invalid artifact bounds')
    artifact.write_bytes(binary)
    reflection.write_bytes(reflected)
    # All possible compiler inputs were supplied through its closed filesystem.
    # Reporting the captured closure is conservative and includes every real input.
    escaped = [str(root / path).replace('\\', '\\\\').replace(' ', '\\ ') for path in files]
    depfile.write_text('artifact: ' + ' '.join(escaped) + '\n')


def exchange(endpoint, request):
    payload = json.dumps(request).encode('utf-8')
    if len(payload) > MAX_MESSAGE_BYTES:
        raise RuntimeError('Native shader snapshot exceeds protocol limit')
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stream:
        stream.settimeout(120)
        stream.connect(endpoint)
        stream.sendall(struct.pack('<I', len(payload)) + payload)
        size, = struct.unpack('<I', receive(stream, 4))
        if size == 0 or size > MAX_MESSAGE_BYTES:
            raise RuntimeError('Native shader worker returned an invalid frame size')
        result = json.loads(receive(stream, size))
    if result.get('version') != 1 or result.get('identity') != request['identity']:
        raise RuntimeError('Native shader worker protocol mismatch')
    return result


def compile_raster_stage(endpoint, args, lock, stage, entry, target, artifact, reflection, depfile, extra):
    """Capture bounded renderer/custom include roots, then use the same native API.

    Explicit roots retain their relative names; ambiguous names are rejected.
    No caller working directory or uncaptured include can enter the worker.
    """
    roots = [Path(args.source).resolve().parent, *(Path(p).resolve() for p in args.include)]
    files = {}
    origins = {}
    total = 0
    for root in roots:
        for path in sorted(root.rglob('*')):
            if path.suffix not in ('.slang', '.h') or not path.is_file():
                continue
            relative = path.relative_to(root)
            if any(part.startswith('.') or part == 'out' for part in relative.parts):
                continue
            if path.is_symlink():
                raise RuntimeError('Native shader include roots cannot contain symlinks')
            text = path.read_text(encoding='utf-8')
            name = relative.as_posix()
            if name in files:
                if files[name] != text:
                    raise RuntimeError('Ambiguous native shader include: ' + name)
                continue
            total += len(text.encode('utf-8'))
            if len(files) >= 63 or total > 4 * 1024 * 1024:
                raise RuntimeError('Native shader include snapshot exceeds bounds; use dedicated include roots')
            files[name] = text
            origins[name] = path
    source = Path(args.source).name
    if source not in files or '__ludus_defines.slang' in files:
        raise RuntimeError('Native shader root is missing or uses a reserved filename')
    defines = []
    for argument in extra:
        if argument.startswith('-D'):
            name, separator, value = argument[2:].partition('=')
            if not name or any(c in name + value for c in '\n\r\0'):
                raise RuntimeError('Invalid native shader preprocessor definition')
            defines.append('#define ' + name + ' ' + (value if separator else '1'))
    files['__ludus_defines.slang'] = '\n'.join(defines) + '\n#include "' + source + '"\n'
    request = {'version': 1, 'identity': 1, 'source': '__ludus_defines.slang', 'entry': entry,
               'stage': {'vertex': 0, 'fragment': 1, 'compute': 2}[stage],
               'target': {'spirv': 0, 'wgsl': 1, 'metal': 2}[target], 'files': files}
    result = exchange(endpoint, request)
    if result.get('compiler') != lock['slang']['version']:
        raise RuntimeError('Native shader worker version differs from SDK pin')
    if result['status'] != 0:
        raise RuntimeError('\n' + (result.get('diagnostics') or 'Native shader worker rejected request'))
    artifact.write_bytes(bytes.fromhex(result['artifact']))
    reflection.write_bytes(bytes.fromhex(result['reflection']))
    mask = bytes.fromhex(result['source_read_mask'])
    if len(mask) != len(files) or any(value not in (0, 1) for value in mask):
        raise RuntimeError('Invalid native shader dependency mask')
    escaped = [str(origins[name]).replace('\\', '\\\\').replace(' ', '\\ ')
               for index, name in enumerate(files) if name in origins and mask[index]]
    depfile.write_text('artifact: ' + ' '.join(escaped) + '\n')

    return {'implementation': result['implementation'], 'library_inventory_sha256': result['library_inventory_sha256'],
            'entry': entry, 'stage': stage, 'target': target,
            'sources': {name: hashlib.sha256(text.encode()).hexdigest() for name, text in files.items()}}
