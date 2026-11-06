"""Record executed S6 evidence. Build admission never implies device qualification."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CASES = (
    'explicit-frozen-environment', 'readonly-numeric-forged-value-boundaries',
    'uncatchable-interrupt-discards-effects', 'stack-fault-discards-effects',
    'retained-state-facade-expires', 'debug-resumes-share-one-safepoint-budget',
    'metadata-rejection-retains-active-package', 'all-entity-identity-boundaries',
    'native-service-resources-finish-before-vm-recovery', 'production-invoke-allocation-sweep',
    'production-debug-allocation-sweep', 'failed-candidate-allocation-retains-active-provider',
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_output(output):
    executed = re.findall(r'\bS6 PASS ([\w-]+)(?=\s|$)', output)
    if sorted(executed) != sorted(CASES):
        raise ValueError('missing, duplicated or unknown executed safety case')
    if 'S6 FAIL' in output:
        raise ValueError('provider reported a safety failure')
    targets = list(re.finditer(r'S6 TARGET os=(\S+) arch=(\S+) pointer_bits=(\d+)', output))
    if len(targets) != 1:
        raise ValueError('missing or ambiguous compiled target identity')
    target = targets[0]
    admitted = {(system, arch, '64') for system in ('Linux', 'macOS') for arch in ('x86_64', 'arm64')}
    admitted.add(('Emscripten', 'wasm32', '32'))
    # config/behavior_profiles.json admits Windows x86_64 with MSVC 19.44.
    admitted.add(('Windows', 'x86_64', '64'))
    if (target[1], target[2], target[3]) not in admitted:
        raise ValueError('unadmitted compiled target identity')
    points = re.findall(r'S6 METRIC (\S+)-allocation-points=(\d+)', output)
    metrics = dict(points)
    if len(points) != 3 or set(metrics) != {'invoke', 'debug', 'replacement'} or any(int(n) == 0 for n in metrics.values()):
        raise ValueError('allocation sweeps must execute nonzero failure points')
    return {'system': target[1], 'architecture': target[2], 'pointer_bits': int(target[3]),
            'allocation_points': {key: int(value) for key, value in metrics.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--cook', type=Path, required=True)
    parser.add_argument('--runner', nargs='+', default=[])
    args = parser.parse_args()
    build, executable = args.build.resolve(), args.exe.resolve()
    destination = executable.parent/'safety-evidence.json'
    destination.unlink(missing_ok=True)
    result = subprocess.run([*args.runner, str(executable)], capture_output=True, text=True, timeout=90)
    output = result.stdout + result.stderr
    print(output, end='')
    if result.returncode:
        raise ValueError(f'provider safety executable exited {result.returncode}')
    record = inspect_output(output)
    commands = json.loads((build/'compile_commands.json').read_text())
    groups = ('/modules/runtime/behavior/', '/modules/runtime/scripting/',
              '/out/luau-probe/source/VM/', '/out/luau-probe/source/Common/')
    for group in groups:
        if not any(group in entry['file'] for entry in commands):
            raise ValueError('missing production compile commands: '+group)
    owned = [entry for entry in commands if any(part in entry['file'] for part in groups)]
    for entry in owned:
        flags = shlex.split(entry['command']) if 'command' in entry else entry['arguments']
        exception_flags = [flag for flag in flags if flag in ('-fexceptions', '-fno-exceptions')]
        if not exception_flags or exception_flags[-1] != '-fno-exceptions':
            raise ValueError('production interpreter must be exception-free: ' + entry['file'])
    sources = [ROOT/'config/luau_toolchain.json', ROOT/'config/behavior_profiles.json',
               ROOT/'scripts/luau-probe', ROOT/'scripts/script-provider',
               ROOT/'scripts/python/ludus_tools/behavior_cook.py',
               ROOT/'cmake/LudusBehaviorProfile.cmake']
    for directory in ('modules/runtime/behavior', 'modules/runtime/scripting', 'tools/script-provider'):
        sources += [path for path in (ROOT/directory).rglob('*') if path.suffix in ('.cpp', '.h', '.luau', '.py', '.mjs', '.json') or path.name == 'CMakeLists.txt']
    sources += [ROOT/'tools/script-provider/shell.html']
    safety = ROOT/'out/script-provider/safety'
    pointer = json.loads((safety/'current.json').read_text())
    if args.cook.resolve() != (safety/pointer['key']).resolve():
        raise ValueError('stale configured safety cook; configure and build again')
    cooked = json.loads((args.cook/'evidence.json').read_text())
    for name, expected in cooked['outputs'].items():
        if digest(safety/pointer['key']/name) != expected:
            raise ValueError('modified executed cook: '+name)
    artifacts = [executable]
    if executable.suffix == '.js':
        artifacts.append(executable.with_suffix('.wasm'))
    record.update(version=1, execution='node-wasm' if args.runner else 'native-process',
                  qualification='partial; physical-device and six-platform acceptance pending',
                  cases=list(CASES), cook=cooked, inputs={str(p.relative_to(ROOT)): digest(p) for p in sorted(sources)},
                  artifacts={p.name: digest(p) for p in artifacts},
                  compile_commands_sha256=digest(build/'compile_commands.json'),
                  production_translation_units=len(owned), output=output)
    temporary = destination.with_suffix('.tmp')
    temporary.write_text(json.dumps(record, indent=2)+'\n')
    temporary.replace(destination)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print('S6 safety acceptance failed: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
