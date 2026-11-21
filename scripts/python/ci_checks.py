#!/usr/bin/env python3
"""Run one CI job's checks from a candidate-owned plan and report every failure.

A batch runs main's workflow definitions against the candidate's sources, so the
commands of a job written into workflow YAML change only after a promotion, and
the step that fails first hides every later step. A job that calls this runner
keeps its commands in a plan module under scripts/ci/, which the candidate owns,
and runs every check whose prerequisites succeeded, so one run reports all of
its failures. See docs/development/continuous-integration.md#batch-job-check-plans.

Plan modules define CHECKS, a list of Check records, in run order.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import importlib.util
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from typing import Dict, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[2]
SUCCESS = "success"
FAILURE = "failure"
SKIPPED = "skipped"
SECONDS_PER_MINUTE = 60
# A check that ignores SIGTERM this long after its deadline is killed.
TERMINATION_GRACE_SECONDS = 30
# GitHub's own shell for `run:` steps, so plan commands behave as they did in YAML.
SHELL = ("bash", "--noprofile", "--norc", "-eo", "pipefail", "-c")


@dataclass(frozen=True)
class Check:
    """One command block of a job.

    run is a bash script run from the repository root. needs names checks that
    must succeed first; otherwise this check is skipped. env values may use
    $VARIABLES, which are expanded from the job environment. timeout_minutes
    bounds the check; None leaves only the job's own timeout.
    """

    id: str
    title: str
    run: str
    needs: Tuple[str, ...] = ()
    env: Dict[str, str] = field(default_factory=dict)
    timeout_minutes: Optional[float] = None


@dataclass
class Outcome:
    check: Check
    result: str
    seconds: float = 0.0
    reason: str = ""


def load_plan(path: Path) -> List[Check]:
    """Import a plan module and validate its CHECKS: unique ids, needs that refer to earlier checks."""
    spec = importlib.util.spec_from_file_location(f"ludus_ci_plan_{path.stem}", path)
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot load check plan {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    checks = list(getattr(module, "CHECKS", []))
    if len(checks) == 0:
        raise ValueError(f"{path} defines no CHECKS")
    seen = set()
    for check in checks:
        if not isinstance(check, Check):
            raise ValueError(f"{path}: {check!r} is not a Check")
        if check.id in seen:
            raise ValueError(f"{path}: duplicate check id {check.id}")
        missing = [need for need in check.needs if need not in seen]
        if missing:
            raise ValueError(f"{path}: {check.id} needs {', '.join(missing)}, which no earlier check provides")
        if "${{" in check.run or any("${{" in value for value in check.env.values()):
            raise ValueError(f"{path}: {check.id} uses a workflow expression; plans see only the environment")
        seen.add(check.id)
    return checks


def expand(value: str, environment: Dict[str, str]) -> str:
    """Replace $NAME and ${NAME} with the job environment's value; leave unknown names as written."""
    def replace(match: "re.Match[str]") -> str:
        name = match.group(1) or match.group(2)
        return environment.get(name, match.group(0))
    return re.sub(r"\$(?:\{([A-Za-z_][A-Za-z0-9_]*)\}|([A-Za-z_][A-Za-z0-9_]*))", replace, value)


def run_check(check: Check, environment: Dict[str, str], root: Path) -> Outcome:
    """Run one check in its own process group, bounded by its timeout."""
    merged = dict(environment)
    for name, value in check.env.items():
        merged[name] = expand(value, environment)
    started = time.monotonic()
    process = subprocess.Popen([*SHELL, check.run], cwd=root, env=merged, start_new_session=True)
    timeout = None if check.timeout_minutes is None else check.timeout_minutes * SECONDS_PER_MINUTE
    try:
        status = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        stop_group(process)
        return Outcome(check, FAILURE, time.monotonic() - started, f"timed out after {check.timeout_minutes:g} min")
    seconds = time.monotonic() - started
    if status != 0:
        return Outcome(check, FAILURE, seconds, f"exit status {status}")
    return Outcome(check, SUCCESS, seconds)


def stop_group(process: subprocess.Popen) -> None:
    """Terminate a timed-out check and everything it started, then kill what remains."""
    for stop in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(process.pid, stop)
        except ProcessLookupError:
            return
        try:
            process.wait(timeout=TERMINATION_GRACE_SECONDS)
            return
        except subprocess.TimeoutExpired:
            continue


def run_plan(checks: Sequence[Check], environment: Dict[str, str], root: Path,
             selected: Optional[Sequence[str]] = None) -> List[Outcome]:
    """Run each check whose needs succeeded, in order, and continue past failures."""
    outcomes: List[Outcome] = []
    results: Dict[str, str] = {}
    for check in checks:
        if selected and check.id not in selected:
            continue
        blocked = [need for need in check.needs if results.get(need, SUCCESS if selected else SKIPPED) != SUCCESS]
        if blocked:
            outcome = Outcome(check, SKIPPED, reason=f"needs {', '.join(blocked)}")
            print(f"Skipped {check.title}: {outcome.reason}.", flush=True)
        else:
            print(f"::group::{check.title}", flush=True)
            outcome = run_check(check, environment, root)
            print("::endgroup::", flush=True)
            if outcome.result == FAILURE:
                print(f"::error title={check.title}::{check.title} failed: {outcome.reason}", flush=True)
        results[check.id] = outcome.result
        outcomes.append(outcome)
    return outcomes


def write_outputs(outcomes: Sequence[Outcome], output: Optional[str]) -> None:
    """Publish each check's result as a step output named by its id."""
    if not output:
        return
    with open(output, "a", encoding="utf-8") as stream:
        for outcome in outcomes:
            stream.write(f"{outcome.check.id}={outcome.result}\n")


def write_summary(title: str, outcomes: Sequence[Outcome], summary: Optional[str]) -> str:
    """Return, and append to the job summary, a table of every check's result."""
    lines = [f"### {title}", "", "| Check | Result | Time | Detail |", "| --- | --- | --- | --- |"]
    for outcome in outcomes:
        seconds = "" if outcome.result == SKIPPED else f"{outcome.seconds:.0f} s"
        lines.append(f"| {outcome.check.title} | {outcome.result} | {seconds} | {outcome.reason} |")
    text = "\n".join(lines) + "\n"
    if summary:
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write(text + "\n")
    return text


def main(arguments: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("plan", type=Path, help="plan module under scripts/ci/")
    parser.add_argument("--only", action="append", default=[],
                        help="run only this check id (repeatable); its needs are assumed to have succeeded")
    parser.add_argument("--list", action="store_true", help="print the plan's check ids and titles, then exit")
    options = parser.parse_args(arguments)
    plan = options.plan.resolve() if options.plan.exists() else ROOT / options.plan
    checks = load_plan(plan)
    if options.list:
        for check in checks:
            needs = f" (needs {', '.join(check.needs)})" if check.needs else ""
            print(f"{check.id}: {check.title}{needs}")
        return 0
    unknown = [identifier for identifier in options.only if identifier not in {check.id for check in checks}]
    if unknown:
        parser.error(f"unknown check id: {', '.join(unknown)}")
    environment = dict(os.environ)
    environment.setdefault("GITHUB_WORKSPACE", str(ROOT))
    outcomes = run_plan(checks, environment, ROOT, options.only or None)
    write_outputs(outcomes, os.environ.get("GITHUB_OUTPUT"))
    report = write_summary(f"{plan.stem} checks", outcomes, os.environ.get("GITHUB_STEP_SUMMARY"))
    print(report, flush=True)
    failed = [outcome.check.title for outcome in outcomes if outcome.result == FAILURE]
    if failed:
        print(f"{len(failed)} of {len(outcomes)} checks failed: {'; '.join(failed)}", flush=True)
        return 1
    return 0


if __name__ == "__main__":
    # Plans import Check from ci_checks; let them share this module's class.
    sys.modules.setdefault("ci_checks", sys.modules[__name__])
    sys.exit(main())
