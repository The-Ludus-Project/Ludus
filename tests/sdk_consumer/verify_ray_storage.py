"""Check the SDK fixture's physical layouts and its response to a wrong lowered field read."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch

# ray_storage.cpp's exit status when the adapter has no native ray queries.
CONSUMER_SKIPPED = 77


def check_layout(package, profile):
    generation = json.loads((package / 'current.json').read_text())['generation']
    cooked = json.loads((package / 'generations' / generation / 'package.json').read_text())
    kernel, = [p['descriptor'] for p in cooked['programs'] if p['id'] == 'GraphRayCompute' and p['profile'] == profile]
    fields = [{'name': name, 'offset': offset, 'scalar': 'float32', 'width': 2}
              for name, offset in [('First', 0), ('Second', 8)]]
    expected = [{'name': name, 'binding': slot, 'kind': kind, 'size': 16, 'fields': fields}
                for name, slot, kind in [('Reference', 6, 'StorageRead'), ('Written', 7, 'StorageReadWrite')]]
    actual = sorted((entry for entry in kernel['interface']['entries'] if entry['binding'] in (6, 7)),
                    key=lambda entry: entry['binding'])
    if actual != expected:
        raise ValueError(f'{profile} reflected storage layout {actual!r}, expected {expected!r}')


def check_sensitivity(manifest, helper, compiler, cross, consumer):
    from ludus_tools import render_graph_build, render_graph_execution, render_graph_language

    # Run the real installed compiler, then redirect just the consumer's red channel in its output.
    # Keep normal inputs, the writer, reflection, the SDK runtime and the CPU oracle intact.
    lower = render_graph_language._lower

    def lower_with_wrong_field(*args, **kwargs):
        module, report = lower(*args, **kwargs)
        if module.count(b'neighbor.Second.y') != 1:
            raise ValueError('expected one lowered read of the second field for the red channel')
        return module.replace(b'neighbor.Second.y', b'neighbor.First.y'), report

    with tempfile.TemporaryDirectory(prefix='ray-storage-') as temporary:
        root = Path(temporary)
        with patch.object(render_graph_language, '_lower', lower_with_wrong_field):
            built = render_graph_build.build_package(manifest, root / 'package', helper=helper, compiler=compiler,
                                                     spirv_cross=cross)
        runtime = root / 'perturbed.lurgrt'
        render_graph_execution.cook_runtime(built['package'], runtime)
        result = subprocess.run([consumer, root, runtime.name], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, check=False)
        print(result.stdout, end='')
        if result.returncode == CONSUMER_SKIPPED:
            # The adapter has no native ray queries, so the perturbed package cannot run either.
            sys.exit(CONSUMER_SKIPPED)
        if result.returncode != 1 or 'Pixel 0 returned (6, 6, 10) after 2 samples, expected (14, 6, 10)' not in result.stdout:
            raise ValueError(f'perturbed package failed to demonstrate the expected field mismatch (exit {result.returncode})')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    layout = commands.add_parser('layout')
    layout.add_argument('package', type=Path)
    layout.add_argument('profile')
    sensitivity = commands.add_parser('sensitivity')
    for name in ('manifest', 'helper', 'compiler', 'cross', 'consumer'):
        sensitivity.add_argument(name, type=Path)
    arguments = parser.parse_args()
    if arguments.command == 'layout':
        check_layout(arguments.package, arguments.profile)
    else:
        check_sensitivity(arguments.manifest, arguments.helper, arguments.compiler, arguments.cross, arguments.consumer)
