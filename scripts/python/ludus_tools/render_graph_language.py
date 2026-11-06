"""LUSL program sources in render-graph builds.

A program whose `source` ends in `.lusl` is lowered once per build with `luslc`, the
command-line front of Ludus::ShaderLanguage, from the same captured snapshot the rest of the
build uses. The generated Slang module joins the snapshot at `<source>.slang`, and the program
then names it, so wrappers, closures, cache keys and diagnostics treat it like authored Slang.
Each lowering also reports its template specializations and `groupshared` clears; the package's
optional `budgets.specializations` bounds the specializations of all its LUSL sources together.
See docs/modules/tooling/shader-language.md, "Render-graph programs".
"""
from __future__ import annotations

import os
import posixpath
import re
import subprocess
import tempfile
from pathlib import Path

LANGUAGE_SUFFIX = '.lusl'
GENERATED_SUFFIX = '.slang'
# SDK headers are staged where ray wrappers already stage engine helpers.
SDK_DIRECTORY = '_ludus_ray'
STYLES = ('strict', 'warn', 'off')
TIMEOUT_SECONDS = 120
# Paths LUSL names: `#include "x"` and `extern slang "x"`. The scan is lexical and conservative.
NAMED_PATH = re.compile(r'(?:#[ \t]*include|\bextern[ \t\r\n]+slang)[ \t\r\n]*"([^"\n]+)"')
LANGUAGE_FILES = ('.lusl', '.luslh')


def is_language_source(source):
    return source.endswith(LANGUAGE_SUFFIX)


def find_luslc(helper, luslc=None):
    """The luslc executable: the explicit one, $LUDUS_LUSLC, or the installed SDK's bin/luslc."""
    for candidate in (luslc, os.environ.get('LUDUS_LUSLC')):
        if candidate:
            return Path(candidate)
    # An installed SDK keeps its helpers in lib/cmake/Ludus/shaders and luslc in bin.
    helper = Path(helper).resolve()
    if len(helper.parents) >= 4:
        installed = helper.parents[3] / 'bin' / ('luslc.exe' if os.name == 'nt' else 'luslc')
        if installed.is_file():
            return installed
    return None


def _load(snapshot, helper, build, relative):
    """The bytes of a snapshot path, capturing it on first use; None when no such file exists."""
    if relative in snapshot.files:
        return snapshot.files[relative]
    if relative.startswith(SDK_DIRECTORY + '/'):
        name = relative[len(SDK_DIRECTORY) + 1:]
        path = Path(helper) / name
        if '/' in name or not path.is_file():
            return None
        data = build.read(path)
        snapshot.capture(relative, data, external=path)
        return data
    if relative.startswith('../') or relative == '..' or posixpath.isabs(relative):
        return None
    try:
        path = snapshot.path(relative)
    except build.ToolingError:
        return None
    return snapshot.load(relative) if path.is_file() else None


def _collect(snapshot, helper, build, source):
    """Every file LUSL may read for `source`, resolved as the preprocessor resolves them:
    beside the naming file, from the snapshot root, then below SDK_DIRECTORY."""
    files = {}
    pending = [source]
    while pending:
        relative = pending.pop()
        if relative in files:
            continue
        data = _load(snapshot, helper, build, relative)
        if data is None:
            continue
        files[relative] = data
        if not relative.endswith(LANGUAGE_FILES):
            continue
        directory = posixpath.dirname(relative)
        for name in NAMED_PATH.findall(data.decode('utf-8', 'replace')):
            for candidate in (posixpath.join(directory, name), name, posixpath.join(SDK_DIRECTORY, name)):
                pending.append(posixpath.normpath(candidate))
    return files


def _parse_report(text, build, source):
    """The records luslc wrote with --report: tab-separated `specialization` and
    `groupshared-clear` lines."""
    report = {'specializations': [], 'groupshared_clears': []}
    for line in text.splitlines():
        fields = line.split('\t')
        if fields[0] == 'specialization' and len(fields) == 4:
            report['specializations'].append({'template': fields[1], 'name': fields[2], 'generated': fields[3]})
        elif fields[0] == 'groupshared-clear' and len(fields) == 4 and fields[3].isdigit():
            report['groupshared_clears'].append({'entry': fields[1], 'variable': fields[2], 'bytes': int(fields[3])})
        else:
            build.fail('Tools', source, f'luslc wrote an unknown report line: {line[:200]}')
    return report


def _lower(files, build, tool, source, style, specialization_budget):
    with tempfile.TemporaryDirectory(prefix='ludus-lusl-') as directory:
        root = Path(directory) / 'source'
        for relative, data in files.items():
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        output = Path(directory) / 'generated.slang'
        report = Path(directory) / 'report.txt'
        command = [str(tool), 'lower', '--root', str(root), '--include-directory', SDK_DIRECTORY,
                   '--style', style, '--output', str(output), '--report', str(report)]
        if specialization_budget is not None:
            command += ['--max-specializations', str(specialization_budget)]
        command.append(source)
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=TIMEOUT_SECONDS, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            build.fail('Tools', source, f'luslc could not run: {error}')
        build.require(result.returncode == 0, 'Source', source,
                      result.stderr.strip() or f'luslc failed with exit code {result.returncode}')
        return output.read_bytes(), _parse_report(report.read_text(encoding='utf-8'), build, source)


def lower_sources(snapshot, programs, helper, build, *, luslc=None, style='strict', specialization_budget=None):
    """Lower every `.lusl` program source once and point its programs at the generated module.

    Each program keeps its authored path as `language_source`. `specialization_budget` is the
    manifest's budgets.specializations, or None for the engine limit per source. Returns the
    luslc that ran, for the build's tool identity, and the report of each lowered source; the
    tool is None and the reports empty when no program is written in LUSL.
    """
    sources = sorted({program['source'] for program in programs.values() if is_language_source(program['source'])})
    if not sources:
        return None, {}
    build.require(style in STYLES, 'Configuration', 'lusl_style',
                  f"LUSL style '{style}' is not one of {', '.join(STYLES)}")
    tool = find_luslc(helper, luslc)
    build.require(tool is not None and tool.is_file(), 'Tools', sources[0],
                  'LUSL program sources need luslc: build with an SDK that installs bin/luslc, '
                  'or set LUDUS_LUSLC to its path')
    reports = {}
    for source in sources:
        generated = source + GENERATED_SUFFIX
        build.require(generated not in snapshot.files, 'Source', generated,
                      'the generated module of ' + source + ' would replace an authored file; rename one of them')
        data, reports[source] = _lower(_collect(snapshot, helper, build, source), build, tool, source, style,
                                       specialization_budget)
        snapshot.capture(generated, data, generated=True)
    if specialization_budget is not None:
        total = sum(len(report['specializations']) for report in reports.values())
        largest = max(sources, key=lambda source: len(reports[source]['specializations']))
        build.require(total <= specialization_budget, 'Capacity', 'budgets.specializations',
                      f'the LUSL sources need {total} template specializations together, over budgets.specializations '
                      f'{specialization_budget}; increase budgets.specializations to at least {total}, or constrain '
                      f'templates by interfaces only so they lower to one Slang generic ({largest} has the most)')
    for program in programs.values():
        if is_language_source(program['source']):
            program['language_source'] = program['source']
            program['source'] = program['source'] + GENERATED_SUFFIX
    return tool, reports
