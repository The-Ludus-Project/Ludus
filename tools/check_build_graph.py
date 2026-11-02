"""Reject sources that several build targets compile separately.

A source listed by several targets is compiled once per target. When every
compilation is the same, move the source into a library and link it instead;
an OBJECT library keeps each executable's link unchanged. A source whose
compilations must differ (other definitions, flags or a test double) is a
variant, and config/build_graph.json records why. Existing duplication that has
not been migrated yet is recorded there as baseline debt, which may only shrink.

The check reads a configured build tree's compile_commands.json, so it sees
exactly what the generator will compile. It is load-independent: it counts
compilations instead of timing them.
"""

import argparse
import fnmatch
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = ROOT / "config" / "build_graph.json"
BUILD_PREFIX = "<build>/"
TARGET_DIRECTORY = re.compile(r"CMakeFiles/([^/]+)\.dir/")
KINDS = ("variant", "baseline")


def object_target(entry):
    """Returns the CMake target that owns a compile_commands.json entry's object file."""
    output = entry.get("output")
    if output is None:
        arguments = entry.get("arguments") or entry.get("command", "").split()
        if "-o" in arguments and arguments.index("-o") + 1 < len(arguments):
            output = arguments[arguments.index("-o") + 1]
    match = TARGET_DIRECTORY.search(output or "")
    return match.group(1) if match is not None else None


def source_name(file_path, source_root, build_root):
    """Names a source relative to the checkout, or to the build tree for generated sources."""
    path = Path(file_path).resolve()
    if path.is_relative_to(build_root):
        return BUILD_PREFIX + path.relative_to(build_root).as_posix()
    if path.is_relative_to(source_root):
        return path.relative_to(source_root).as_posix()
    return path.as_posix()


def collect_targets(entries, source_root, build_root):
    """Maps each compiled source to the set of targets that compile it."""
    targets = {}
    for entry in entries:
        file_path = Path(entry["file"])
        if file_path.is_absolute() is False:
            file_path = Path(entry["directory"]) / file_path
        target = object_target(entry)
        if target is None:
            continue
        targets.setdefault(source_name(file_path, source_root, build_root), set()).add(target)
    return targets


def load_rules(config_path):
    """Reads the variant and baseline rules; returns (rules, problems)."""
    document = json.loads(config_path.read_text(encoding="utf-8"))
    rules = []
    problems = []
    for index, rule in enumerate(document.get("rules", [])):
        patterns = rule.get("sources")
        kind = rule.get("kind")
        reason = rule.get("reason", "").strip()
        owners = rule.get("targets", [])
        if isinstance(patterns, list) is False or len(patterns) == 0 or kind not in KINDS or reason == "":
            problems.append(f"{config_path.name} rule {index}: needs non-empty sources, a kind of "
                            f"{' or '.join(KINDS)}, and a reason")
            continue
        if kind == "baseline" and (isinstance(owners, list) is False or len(owners) < 2):
            problems.append(f"{config_path.name} rule {index}: a baseline rule names the targets that compile "
                            "its sources, so a tree containing all of them can show the debt resolved")
            continue
        rules.append({"sources": patterns, "kind": kind, "reason": reason, "targets": owners, "index": index})
    return rules, problems


def matching_rule(source, rules):
    for rule in rules:
        if any(fnmatch.fnmatchcase(source, pattern) for pattern in rule["sources"]):
            return rule
    return None


def count_lines(source, source_root, build_root):
    path = build_root / source[len(BUILD_PREFIX):] if source.startswith(BUILD_PREFIX) else source_root / source
    try:
        with path.open("rb") as stream:
            return sum(1 for _ in stream)
    except OSError:
        return 0


def check(targets, rules, source_root, build_root):
    """Classifies duplicated sources; returns a report dictionary."""
    unexpected = []
    allowed = {kind: [] for kind in KINDS}
    exercised = set()
    resolved = set()
    for source, owners in sorted(targets.items()):
        rule = matching_rule(source, rules)
        if len(owners) < 2:
            if rule is not None:
                resolved.add(rule["index"])
            continue
        record = {
            "source": source,
            "targets": sorted(owners),
            "extra_compilations": len(owners) - 1,
            "lines": count_lines(source, source_root, build_root),
        }
        if rule is None:
            unexpected.append(record)
        else:
            exercised.add(rule["index"])
            allowed[rule["kind"]].append(record)
    # A baseline rule is resolved debt when this tree contains every target it names, yet
    # compiles each of its sources once; remove it so the duplication cannot return
    # unnoticed. A tree without one of those targets (tests or the editor off) proves nothing.
    present = set().union(*targets.values()) if targets else set()
    stale = [rule for rule in rules
             if rule["kind"] == "baseline" and rule["index"] in resolved and rule["index"] not in exercised
             and set(rule["targets"]) <= present]
    return {
        "compilations": sum(len(owners) for owners in targets.values()),
        "sources": len(targets),
        "unexpected": unexpected,
        "variant": allowed["variant"],
        "baseline": allowed["baseline"],
        "stale_baseline": [{"sources": rule["sources"], "reason": rule["reason"]} for rule in stale],
    }


def extra(records):
    return sum(record["extra_compilations"] for record in records)


def format_summary(report):
    lines = [
        f"Build graph: {report['compilations']} compilations of {report['sources']} sources.",
        f"  variants (intended):        {extra(report['variant'])} extra compilations",
        f"  baseline (to migrate):      {extra(report['baseline'])} extra compilations",
        f"  unexpected:                 {extra(report['unexpected'])} extra compilations",
    ]
    for record in report["unexpected"]:
        lines.append(f"error: {record['source']} ({record['lines']} lines) is compiled separately by "
                     f"{', '.join(record['targets'])}")
    if report["unexpected"]:
        lines.append("Link one library (an OBJECT library keeps each link unchanged) instead of listing the source "
                     "in several targets. If the compilations must differ, add a variant rule with its reason to "
                     "config/build_graph.json.")
    for rule in report["stale_baseline"]:
        lines.append(f"error: baseline rule for {', '.join(rule['sources'])} no longer matches a duplicated "
                     "source; remove it from config/build_graph.json")
    return "\n".join(lines)


def format_markdown(report):
    rows = [
        "### Build graph",
        "",
        "| Compilations | Sources | Variant extra | Baseline extra | Unexpected extra |",
        "| ---: | ---: | ---: | ---: | ---: |",
        f"| {report['compilations']} | {report['sources']} | {extra(report['variant'])} | "
        f"{extra(report['baseline'])} | {extra(report['unexpected'])} |",
    ]
    for record in report["unexpected"]:
        rows.append(f"- Unexpected: `{record['source']}` in {', '.join(record['targets'])}")
    return "\n".join(rows) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("build_directory", type=Path, help="configured build tree containing compile_commands.json")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--report", type=Path, help="write the classification as JSON")
    parser.add_argument("--summary", type=Path, help="append a Markdown table (for example $GITHUB_STEP_SUMMARY)")
    arguments = parser.parse_args(argv)

    build_root = arguments.build_directory.resolve()
    database = build_root / "compile_commands.json"
    if database.is_file() is False:
        print(f"error: {database} does not exist; configure the tree with CMAKE_EXPORT_COMPILE_COMMANDS=ON",
              file=sys.stderr)
        return 2
    rules, problems = load_rules(arguments.config)
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 2
    entries = json.loads(database.read_text(encoding="utf-8"))
    report = check(collect_targets(entries, arguments.source_root.resolve(), build_root), rules,
                   arguments.source_root.resolve(), build_root)
    print(format_summary(report))
    if arguments.report is not None:
        arguments.report.parent.mkdir(parents=True, exist_ok=True)
        arguments.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if arguments.summary is not None:
        with arguments.summary.open("a", encoding="utf-8") as stream:
            stream.write(format_markdown(report))
    return 1 if report["unexpected"] or report["stale_baseline"] else 0


if __name__ == "__main__":
    sys.exit(main())
