"""Source/installed CMake adapter for the shared offline graph services."""
import argparse
import sys

from ludus_tools import render_graph_build, render_graph_execution
from ludus_tools.errors import ToolingError


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('manifest', 'root', 'output', 'header', 'name', 'configuration', 'helper', 'compiler'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--worker-socket')
    parser.add_argument('--validator')
    parser.add_argument('--spirv-cross')
    parser.add_argument('--runtime-package')
    parser.add_argument('--ray-path-capture', action='store_true')
    parser.add_argument('--ray-path-capture-package')
    args = parser.parse_args()
    try:
        report = render_graph_build.build_package(args.manifest, args.output, root=args.root,
                    helper=args.helper, compiler=args.compiler, validator=args.validator, spirv_cross=args.spirv_cross, worker_socket=args.worker_socket,
                    ray_path_capture=args.ray_path_capture)
        render_graph_execution.emit_runtime(report['package'], args.header,
                    configuration=args.configuration, name=args.name)
        if args.runtime_package:
            render_graph_execution.cook_runtime(report['package'], args.runtime_package,
                    configuration=args.configuration)
        if args.ray_path_capture_package:
            render_graph_execution.cook_ray_path_capture(report['package'], args.ray_path_capture_package,
                    configuration=args.configuration)
    except (ToolingError, OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
