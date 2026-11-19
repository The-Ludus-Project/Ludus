"""Cook the visibility resolve inserted by the canvas alongside the original raster graph."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    editor, tools_directory, *shader_tools = sys.argv[1:]
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        subprocess.run([editor, 'Visibility configurations apply from the canvas*', '-c', 'opaque material resolve'],
                       env={**os.environ, 'LUDUS_VISIBILITY_CONFIGURATION_FIXTURE': str(root)}, check=True)
        manifest = root / 'opaque-material-resolve' / 'main.lushader.json'
        graph = [sys.executable, '-m', 'ludus_tools', 'render-graph']
        subprocess.run([*graph, 'build', manifest, root / 'build', *shader_tools,
                        '--runtime-package', root / 'resolve.lurgrt'], cwd=tools_directory, check=True)
        subprocess.run([*graph, 'cook-runtime', root / 'build', root / 'original.lurgrt',
                        '--configuration', 'Original'], cwd=tools_directory, check=True)


if __name__ == '__main__':
    main()
