"""Read-only project script metadata and explicit shared cook planning.

Loading never configures, builds, cooks, downloads or rewrites project settings.
The Editor adapter owns subprocess cancellation; this module supplies only paths.
"""
import os
from pathlib import Path
import re
from . import behavior_cook as cook


# Installed host tools carry the host executable suffix (.exe on Windows).
TOOL_SUFFIX = '.exe' if os.name == 'nt' else ''


def load(project):
    root = Path(project).resolve()
    path = root/'ludus.scripts.json'
    cook.require(path.is_file() and path.stat().st_size <= 65536, 'missing or oversized ludus.scripts.json')
    value = cook.read_json(path)
    cook.closed(value, ('version', 'name', 'contract', 'package'))
    cook.integer(value['version'], 1, 1)
    cook.require(type(value['name']) is str and re.fullmatch('[A-Za-z][A-Za-z0-9_]{0,63}',value['name']), 'invalid cook target name')
    for key in ('contract', 'package'):
        relative = value[key]
        cook.require(type(relative) is str and not Path(relative).is_absolute(), 'project-relative script metadata required')
        selected = (root/relative).resolve()
        cook.require(selected.is_relative_to(root) and selected.is_file() and selected.stat().st_size <= 131072, 'script metadata escapes project or is missing')
        value[key] = selected
    return value


def arguments(project, build, sdk):
    value = load(project)
    resource = Path(sdk)/'share/Ludus/behavior'
    tools = {'contract':value['contract'],'package':value['package'],'output':Path(build)/value['name'],
             'profile':resource/'profile.json','compiler':resource/('bin/luau-compile'+TOOL_SUFFIX),
             'analyzer':resource/('bin/luau-analyze'+TOOL_SUFFIX)}
    cook.require((resource/'behavior_cook.py').is_file() and (resource/'behavior_graph.py').is_file(), 'Behavior SDK cook modules missing')
    for key in ('profile','compiler','analyzer'):
        cook.require(tools[key].is_file(), 'Behavior SDK host tools missing; explicitly install/prepare the optional component')
    return [argument for key,value in tools.items() for argument in ('--'+key,str(value))]
