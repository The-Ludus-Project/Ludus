"""Compare the Editor and a relocated SDK app to the projected cube in cube.world.json."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from PIL import Image


FIXTURE = Path(__file__).resolve().parent
ROOT = FIXTURE.parents[2]
EXTENT = (96, 64)
# The camera sees 12 x 8 metres. The cube covers [-2,0] x [0,2] relative to
# the optical axis, so its boundaries fall between pixel centres on every side.
CUBE = (32, 16, 48, 32)  # right and bottom exclusive


def write_expectation(path, color):
    image = Image.new('RGB', EXTENT)
    image.paste(color, CUBE)
    image.save(path)


def capture(command, package, evidence, name, environment):
    path = package / 'capture.png'
    path.unlink(missing_ok=True)
    with (evidence / f'{name}.log').open('w') as log:
        process = subprocess.run(command, cwd=package, env=environment, stdout=log,
                                 stderr=subprocess.STDOUT, timeout=150)
    if process.returncode or not path.is_file():
        raise RuntimeError(f'{name} failed to capture. Inspect {log.name}')
    shutil.move(path, evidence / f'{name}.png')


def compare(evidence, label, candidates, expected_code):
    report = evidence / f'{label}.json'
    with report.open('x') as output:
        process = subprocess.run([sys.executable, ROOT / 'scripts/compare-render', 'expected.png',
                                  *candidates, '--max-rmse', '0', '-o', f'{label}.html'],
                                 cwd=evidence, stdout=output)
    if process.returncode != expected_code:
        raise RuntimeError(f'{label}: expected exit {expected_code}, got {process.returncode}. See {report}')


def verify(editor, executable, graph, evidence, driver):
    evidence.mkdir(parents=True, exist_ok=True)
    environment = {'PATH': '/usr/bin:/bin', 'QT_QPA_PLATFORM': 'offscreen',
                   'VK_DRIVER_FILES': str(driver), 'LANG': 'C.UTF-8'}
    if 'GALLIVM_PERF' in os.environ:
        environment['GALLIVM_PERF'] = os.environ['GALLIVM_PERF']
    version = subprocess.check_output(['dpkg-query', '-W', '-f=${Version}', 'mesa-vulkan-drivers'], text=True)
    write_expectation(evidence / 'expected.png', 'red')
    with tempfile.TemporaryDirectory() as workspace:
        package = Path(workspace) / 'moved' / 'app'
        authoring = package / 'authoring'
        authoring.mkdir(parents=True)
        # The installed executable and its packaged libraries travel together.
        shutil.copytree(executable.parent, package, dirs_exist_ok=True)
        linked = subprocess.check_output(['ldd', './' + executable.name], cwd=package, env=environment, text=True)
        (evidence / 'sdk-libraries.txt').write_text(linked)
        for line in linked.splitlines():
            if 'not found' in line:
                raise RuntimeError(f'Missing packaged SDK dependency: {line}')
            if ' => /' in line:
                library = Path(line.split(' => ', 1)[1].split(' (', 1)[0]).resolve()
                if not any(library.is_relative_to(root) for root in (package, Path('/usr/lib'), Path('/lib'))):
                    raise RuntimeError(f'SDK runtime dependency outside package/system libraries: {library}')
        shutil.copyfile(graph, package / 'graph.lurgrt')
        names = Path(str(graph) + '.resources.json')
        shutil.copyfile(names, package / 'graph.lurgrt.resources.json')
        shutil.copyfile(names, evidence / 'graph.resources.json')
        for asset in ('cube.mesh.json', 'world.schema.json', 'render.preview.json', 'ludus.png'):
            shutil.copyfile(ROOT / 'examples/live-edit-game/authoring' / asset, authoring / asset)
        world_path = package / 'cube.world.json'
        shutil.copyfile(FIXTURE / world_path.name, world_path)
        metadata = {'driver_package': 'mesa-vulkan-drivers', 'driver_version': version,
                    'driver_manifest': json.loads(driver.read_text()), 'export': 'Main/output',
                    'camera': 'preview.camera', 'extent': EXTENT, 'configuration': 'Default',
                    'runtime_sha256': hashlib.sha256(graph.read_bytes()).hexdigest(),
                    'world_sha256': hashlib.sha256(world_path.read_bytes()).hexdigest(),
                    'max_rmse': 0, 'encoding': 'normalized 8-bit display RGB'}
        (evidence / 'fixture.json').write_text(json.dumps(metadata, indent=2) + '\n')
        routes = {'editor': [editor, '[.sdk-raster-baseline]'], 'sdk': ['./' + executable.name]}
        positives = []
        for attempt in ('first', 'repeat'):
            for host, command in routes.items():
                name = f'{host}-{attempt}'
                capture(command, package, evidence, name, environment)
                positives.append(f'{name}.png')
        compare(evidence, 'baseline', positives, 0)

        # Change the material in the saved World. Both processes reopen the file.
        world = json.loads(world_path.read_bytes())
        cube = next(entity for entity in world['entities'] if entity['id'] == 'starter.cube')
        cube['components']['renderable'].update(r=0, g=1)
        world_path.write_text(json.dumps(world, indent=2) + '\n')
        shutil.copyfile(world_path, evidence / 'green.world.json')
        for host, command in routes.items():
            capture(command, package, evidence, f'{host}-green', environment)
        compare(evidence, 'deliberate-failure', ['editor-green.png', 'sdk-green.png'], 1)
        report = json.loads((evidence / 'deliberate-failure.json').read_bytes())
        # A crash or an arbitrary wrong image is not the negative control.
        expected_rmse = math.sqrt(2 * 16 * 16 / (3 * 96 * 64))
        for comparison in report['comparisons']:
            if comparison['regression']['passed'] or not math.isclose(comparison['metrics']['rmse'], expected_rmse):
                raise RuntimeError('The deliberate color change did not produce its predicted error')
        green = Image.new('RGB', EXTENT)
        green.paste((0, 255, 0), CUBE)
        for host in routes:
            with Image.open(evidence / f'{host}-green.png') as result:
                if result.convert('RGB').tobytes() != green.tobytes():
                    raise RuntimeError(f'{host}: the changed pixels must be the green cube')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('editor', 'executable', 'graph', 'evidence', 'driver'):
        parser.add_argument('--' + option, type=Path, required=True)
    verify(**{name: path.resolve() for name, path in vars(parser.parse_args()).items()})
