"""Frame capture of one library configuration output of a published generation.

The viewport draws its root configuration. A library capture lets a frame capture render a library
configuration that the viewport does not show, such as a slow reference template inserted beside the
interactive graph. It is rebuilt from the published generation's saved sources, like
rebuild_capture_profile, so it renders exactly what that generation was built from. A generated root
graph wires the library's Scene, View and ColorTexture inputs like the built-in template roots do and
routes the chosen output through Accumulate and ToneMap, unless the library already accumulates it,
to one Export. The library capture has its own build tree and runtime package; the viewport's generation,
build tree and runtime package are never written. See docs/architecture/render-graph-frame-capture.md.
"""
from pathlib import Path
import shutil

from . import render_graph as graph, render_graph_build as build, render_graph_execution as execution
from . import render_graph_reload as reload

CAPTURE_ROOT_NAME = 'capture'
# The generated root's ID and captured Export; the editor names "<ROOT_ID>/<CAPTURE_OUTPUT>" as
# LIBRARY_CAPTURE_EXPORT (apps/editor/src/internal/render_graph_library_capture.h).
ROOT_ID = 'LibraryCapture'
CAPTURE_OUTPUT = 'output'
# Library outputs whose producer already accumulates go straight to the Export.
ACCUMULATED_KINDS = ('Accumulate', 'ToneMap', 'Denoise')
# The root nodes that supply each library input type, as in the built-in template roots.
INPUT_SOURCES = {'Scene': 'scene.scene', 'View': 'camera.view', 'ColorTexture': 'color.texture'}
REPAIR = ('Choose another output, or capture the viewport output after routing this one to an Export on the '
          'root canvas.')


def _fail(where, message):
    build.fail('Capture', where, message)


def list_capture_targets(graph_document):
    """Return {configuration: [output, ...]} for the configurations of one library graph document."""
    targets = {}
    for configuration in graph_document.get('configurations', []):
        outputs = [item['id'] for item in configuration.get('outputs', []) if item.get('type') == 'ColorTexture']
        if outputs:
            targets[configuration['id']] = outputs
    return targets


def make_capture_root(library_document, library_label, configuration, output):
    """Return the root graph document that captures configuration.output of a library graph.

    library_label is the library's package-relative path, written as the Subgraph asset. The root is
    pure data: no files are read or written.
    """
    where = f'{library_label}#{configuration}'
    record = next((item for item in library_document.get('configurations', []) if item.get('id') == configuration),
                  None)
    if record is None:
        _fail(where, f'{library_label} has no configuration {configuration}. Choose one of: '
              + ', '.join(item.get('id', '?') for item in library_document.get('configurations', [])) + '.')
    outputs = {item['id']: item for item in record.get('outputs', [])}
    if output not in outputs:
        _fail(where, f'configuration {configuration} has no output {output}. Choose one of: '
              + ', '.join(outputs) + '.')
    if outputs[output].get('type') != 'ColorTexture':
        _fail(where, f'output {output} is a {outputs[output].get("type")}; a frame capture reads a ColorTexture '
              f'output. {REPAIR}')
    edges = []
    for item in record.get('inputs', []):
        source = INPUT_SOURCES.get(item.get('type'))
        if source is None:
            _fail(where, f'input {item["id"]} needs a {item.get("type")}, but a capture supplies only Scene, View '
                  f'and ColorTexture inputs. {REPAIR}')
        edges.append({'from': source, 'to': 'transport.' + item['id']})
    producer = outputs[output].get('source', '').split('.')[0]
    producer_kind = next((node.get('kind') for node in record.get('nodes', []) if node.get('id') == producer), None)
    nodes = [
        {'id': 'scene', 'kind': 'Scene'},
        {'id': 'camera', 'kind': 'Camera', 'role': 'Offscreen', 'target_type': 'ColorTexture', 'active': True},
        {'id': 'color', 'kind': 'Target', 'type': 'ColorTexture', 'format': 'RGBA8Unorm', 'extent': 'RenderSize',
         'samples': 1, 'imported': False, 'initialized': False, 'presentation': False},
        {'id': 'renderSize', 'kind': 'RenderSize', 'extent': {'reference': 'View', 'scale': [1, 1]}},
        {'id': 'transport', 'kind': 'Subgraph', 'asset': library_label, 'configuration': configuration},
        {'id': CAPTURE_OUTPUT, 'kind': 'Export', 'type': 'ColorTexture'},
    ]
    edges += [{'from': 'color.texture', 'to': 'camera.target'}, {'from': 'renderSize.size', 'to': 'color.size'}]
    # Every other required output needs a consumer for the graph to validate, and only a consumer that
    # reaches an Export counts. Execution order can put these Exports first, so the editor names the
    # captured one (ROOT_ID/CAPTURE_OUTPUT) instead of relying on the first exported colour.
    for item in record.get('outputs', []):
        if item['id'] != output and item.get('required') is True:
            nodes.append({'id': 'required_' + item['id'], 'kind': 'Export', 'type': item.get('type')})
            edges.append({'from': 'transport.' + item['id'], 'to': 'required_' + item['id'] + '.texture'})
    if producer_kind in ACCUMULATED_KINDS:
        edges.append({'from': 'transport.' + output, 'to': CAPTURE_OUTPUT + '.texture'})
    else:
        nodes += [{'id': 'accumulate', 'kind': 'Accumulate', 'samples': graph.MAX_ACCUMULATION_SAMPLES},
                  {'id': 'tonemap', 'kind': 'ToneMap', 'exposure': 1}]
        edges += [{'from': 'transport.' + output, 'to': 'accumulate.color'},
                  {'from': 'accumulate.color', 'to': 'tonemap.color'},
                  {'from': 'tonemap.color', 'to': CAPTURE_OUTPUT + '.texture'}]
    return {'version': 2, 'id': ROOT_ID, 'default': 'Default',
            'configurations': [{'id': 'Default', 'inputs': [], 'outputs': [], 'nodes': nodes, 'edges': edges}]}


def write_capture_sources(sources, manifest, library, configuration, output):
    """Add the capture root and its build manifest beside a package's sources; return the manifest path.

    sources is a package root to write into, never a published generation. manifest is the package's
    package-relative build manifest, copied as the capture root's sibling manifest so the build reads
    the same programs, techniques and settings.
    """
    sources = Path(sources)
    library_path = (sources / library).resolve()
    build.require(library_path.is_relative_to(sources.resolve()) and library_path.is_file(), 'Capture', library,
                  f'{library} is not part of the package')
    root = make_capture_root(build.document(build.read(library_path), library), library, configuration, output)
    capture_manifest = sources / (CAPTURE_ROOT_NAME + '.lushader.json')
    shutil.copyfile(sources / manifest, capture_manifest)
    (sources / (CAPTURE_ROOT_NAME + '.lurender.json')).write_bytes(graph.canonical_asset(root))
    return capture_manifest


def rebuild_library_capture(generation, output, destination, *, library, library_configuration, library_output,
                            **options):
    """Compile and cook the library capture of one library output of a published generation.

    generation is a generation or a build tree with current.json, as for rebuild_capture_profile.
    output is the library capture's owned build tree. destination receives the runtime package (`.lurgrt`);
    when the saved build declares a larger capture budget, the capture profile is written beside it
    with the `.lurgcp` suffix. A failure preserves both previous files.
    """
    generation, sources, manifest = reload.ray_path_capture_sources(generation)
    library_path = (sources / library).resolve()
    build.require(library_path.is_relative_to(sources.resolve()) and library_path.is_file(), 'Capture', library,
                  f'generation {generation.name} carries no {library}. Only libraries the root graph imports are '
                  'saved with a generation; insert the library on the root canvas and build the graph again.')
    # Validate the target before copying the sources.
    make_capture_root(build.document(build.read(library_path), library), library, library_configuration,
                      library_output)
    output = Path(output).resolve()
    destination = Path(destination).absolute()
    capture_sources = output / 'sources'
    if capture_sources.exists():
        shutil.rmtree(capture_sources)
    shutil.copytree(sources, capture_sources)
    capture_manifest = write_capture_sources(capture_sources, manifest.relative_to(sources), library,
                                             library_configuration, library_output)
    options = {key: value for key, value in options.items() if key != 'root'}
    report = build.build_package(capture_manifest, output / 'build', root=capture_sources, **options)
    build.checkpoint(options.get('cancel'))
    destination.parent.mkdir(parents=True, exist_ok=True)
    runtime = execution.cook_runtime(report['package'], destination)
    profile = None
    if reload.declares_capture_budget(capture_manifest, capture_sources):
        profiled = build.build_package(capture_manifest, output / 'profile', root=capture_sources,
                                       capture_profile=True, **options)
        build.checkpoint(options.get('cancel'))
        profile = execution.cook_capture_profile(profiled['package'], destination.with_suffix('.lurgcp'))
    return {'build': report, 'runtime': runtime, 'capture_profile': profile, 'source_generation': generation.name,
            'library': library, 'library_configuration': library_configuration, 'library_output': library_output,
            'export': ROOT_ID + '/' + CAPTURE_OUTPUT}
