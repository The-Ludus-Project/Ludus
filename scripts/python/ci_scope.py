#!/usr/bin/env python3
"""Route documentation-only changes and enforce the required native CI gate."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import urllib.error
import urllib.request


# Explicitly exempt documentation tooling only; unknown paths require native CI.
DOCUMENTATION_FILES = frozenset({
    "README.md",
    "mkdocs.yml",
    ".github/workflows/wiki.yml",
    "config/doxygen_toolchain.json",
    "scripts/build-api",
    "scripts/check-wiki",
    "scripts/python/api_reference.py",
    "scripts/python/api_markdown.py",
    "scripts/python/wiki_api_hook.py",
    "scripts/python/test_api_reference.py",
    "scripts/python/test_api_markdown.py",
    "scripts/python/test_wiki.py",
})
NATIVE_JOBS = (
    "macos-compile", "macos-sanitizers", "macos-analysis", "formatting", "debug-runtime", "development",
    "sanitizers", "filesystem-fuzz", "threading-races", "analysis", "editor",
    "assertion-policy", "pch", "build-budget", "scripting-qualification",
)
# Slow native jobs deferred from ordinary pull-request runs to the merge queue,
# pushes to main and manual runs. Every change still passes them before landing.
MERGE_QUEUE_JOBS = (
    "macos-compile", "macos-sanitizers", "sanitizers", "editor", "build-budget",
)
FULL_TIER = "full"
PULL_REQUEST_TIER = "pull-request"
INTEGRATION_TIER = "integration"
NIGHTLY_TIER = "nightly"
NIGHTLY_JOBS = ("macos-analysis",)
TIERS = frozenset({FULL_TIER, PULL_REQUEST_TIER, INTEGRATION_TIER, NIGHTLY_TIER})
# Integration PRs run source/tooling checks; compiled variants run once per batch.
INTEGRATION_JOBS = tuple(name for name in NATIVE_JOBS if name != "formatting")
# Older trusted main workflows still report this removed PR-only job as skipped
# during batch validation. Accept that exact legacy shape until main is promoted.
LEGACY_INTEGRATION_JOB = "integration-compile"
# A pull request carrying this label runs the full tier before entering the queue.
FULL_VALIDATION_LABEL = "ci:full"
# Bounded GitHub run metadata read by a cancelled pull-request gate.
MAX_RUN_METADATA_BYTES = 2 * 1024 * 1024


def documentation_only(paths: list[str]) -> bool:
    if not paths:
        return False
    for path in paths:
        parts = PurePosixPath(path).parts
        if path.startswith("/") or ".." in parts:
            return False
        if path not in DOCUMENTATION_FILES and not path.startswith("docs/"):
            return False
    return True


def changed_paths(event_name: str, event: dict, root: Path) -> list[str]:
    if event_name == "pull_request":
        base = event["pull_request"]["base"]["sha"]
        head = event["pull_request"]["head"]["sha"]
        separator = "..."  # Entire PR since its merge base, not just its last commit.
    elif event_name == "push":
        base, head = event["before"], event["after"]
        separator = ".."
    elif event_name == "merge_group":
        # The queue's candidate commit contains every PR ahead of it; compare
        # it with the branch commit the group was built on.
        base, head = event["merge_group"]["base_sha"], event["merge_group"]["head_sha"]
        separator = ".."
    else:
        raise ValueError("manual or unsupported event requires native validation")
    for sha in (base, head):
        if (
            not isinstance(sha, str)
            or not re.fullmatch(r"[0-9a-fA-F]{40}|[0-9a-fA-F]{64}", sha)
            or not sha.strip("0")
        ):
            raise ValueError("missing or invalid comparison commit")
    # Thanks to Git's git-diff documentation: disabling rename detection keeps
    # both paths, so moving an engine file into docs cannot qualify for exemption.
    # NUL-separated output also preserves filenames containing whitespace.
    # https://git-scm.com/docs/git-diff
    result = subprocess.run(
        ["git", "diff", "--name-only", "--no-renames", "-z", f"{base}{separator}{head}", "--"],
        cwd=root, check=True, capture_output=True, timeout=30,
    )
    return [os.fsdecode(path) for path in result.stdout.split(b"\0") if path]


def classify(event_name: str, event: dict, root: Path) -> tuple[bool, str]:
    try:
        paths = changed_paths(event_name, event, root)
    except (KeyError, TypeError, ValueError, OSError, subprocess.SubprocessError):
        return True, "Comparison unavailable or event requires full validation."
    if documentation_only(paths):
        return False, f"All {len(paths)} changed paths are documentation or documentation tooling."
    return True, "Engine, tooling, policy, unknown paths, or an empty diff require full validation."


def validation_tier(event_name: str, event: dict, *, is_batch: bool = False) -> str:
    """Use cheap checks for develop PRs, retain legacy main tiers, and fully validate batches."""
    if is_batch:
        return FULL_TIER
    if event_name in {"schedule", "workflow_dispatch"}:
        return NIGHTLY_TIER
    if event_name != "pull_request":
        return FULL_TIER
    try:
        if event["pull_request"].get("base", {}).get("ref") == "develop":
            return INTEGRATION_TIER
        names = {label["name"] for label in event["pull_request"].get("labels") or []}
    except (AttributeError, KeyError, TypeError):
        return FULL_TIER
    return FULL_TIER if FULL_VALIDATION_LABEL in names else PULL_REQUEST_TIER


def gate_failures(results: dict) -> list[str]:
    failures = []
    scope = results.get("changes", {})
    if scope.get("result") != "success":
        return ["Change classification did not succeed."]
    outputs = scope.get("outputs", {})
    required = outputs.get("native_required")
    if required not in {"true", "false"}:
        return ["Change classification did not report a valid native_required output."]
    tier = outputs.get("validation_tier")
    if tier not in TIERS:
        return ["Change classification did not report a valid validation_tier output."]
    inventory = {"changes", *NATIVE_JOBS}
    if LEGACY_INTEGRATION_JOB in results and tier != INTEGRATION_TIER:
        if results[LEGACY_INTEGRATION_JOB].get("result") != "skipped":
            failures.append("Legacy integration compile must be skipped outside integration PRs.")
        results = {name: job for name, job in results.items() if name != LEGACY_INTEGRATION_JOB}
    if set(results) != inventory:
        failures.append("Required native job inventory is incomplete or unexpected.")
    for name in NATIVE_JOBS:
        deferred = ((tier == PULL_REQUEST_TIER and name in MERGE_QUEUE_JOBS)
                    or (tier == INTEGRATION_TIER and name in INTEGRATION_JOBS)
                    or (tier != NIGHTLY_TIER and name in NIGHTLY_JOBS))
        expected = "success" if required == "true" and not deferred else "skipped"
        actual = results.get(name, {}).get("result")
        if actual != expected:
            failures.append(f"{name}: expected {expected}, received {actual}.")
    return failures


def has_cancelled_dependency(results: dict) -> bool:
    return any(isinstance(job, dict) and job.get("result") == "cancelled" for job in results.values())


def superseding_run(current: dict, runs: list[dict]) -> dict | None:
    """Return a newer, uncancelled run of the same workflow, event and head commit.

    Concurrency cancels only an older run, and the newer run publishes its own
    required gate on the same commit, so that gate owns the result. A replacement
    that was itself cancelled does not count; its own gate repeats this search.
    """
    for run in runs:
        if (isinstance(run, dict) and isinstance(run.get("id"), int) and run["id"] > current["id"]
                and run.get("workflow_id") == current["workflow_id"]
                and run.get("event") == current["event"]
                and run.get("head_sha") == current["head_sha"]
                and run.get("conclusion") != "cancelled"):
            return run
    return None


def read_github(path: str) -> dict:
    # Thanks to GitHub, "REST API endpoints for workflow runs": a run reports its
    # workflow, event and head commit; listings filter by head_sha and event.
    # https://docs.github.com/en/rest/actions/workflow-runs
    api = os.environ.get("GITHUB_API_URL", "https://api.github.com")
    request = urllib.request.Request(
        f"{api}/repos/{os.environ['GITHUB_REPOSITORY']}/{path}",
        headers={"Authorization": f"Bearer {os.environ['GITHUB_TOKEN']}",
                 "Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28"},
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        payload = response.read(MAX_RUN_METADATA_BYTES + 1)
    if len(payload) > MAX_RUN_METADATA_BYTES:
        raise ValueError("GitHub run metadata exceeded the bounded limit.")
    result = json.loads(payload)
    if not isinstance(result, dict):
        raise ValueError("GitHub run metadata is not an object.")
    return result


def find_superseding_run() -> dict | None:
    """Look up the replacement of a cancelled pull-request run; any failure finds none."""
    if os.environ.get("GITHUB_EVENT_NAME") != "pull_request" or os.environ.get("LUDUS_CI_BATCH_SHA"):
        return None
    try:
        current = read_github(f"actions/runs/{int(os.environ['GITHUB_RUN_ID'])}")
        if (not isinstance(current.get("id"), int) or not isinstance(current.get("workflow_id"), int)
                or re.fullmatch(r"[0-9a-f]{40}", str(current.get("head_sha"))) is None
                or current.get("event") != "pull_request"):
            return None
        listing = read_github(f"actions/workflows/{current['workflow_id']}/runs"
                              f"?head_sha={current['head_sha']}&event=pull_request&per_page=100")
        runs = listing.get("workflow_runs")
        return superseding_run(current, runs) if isinstance(runs, list) else None
    except (KeyError, TypeError, ValueError, OSError, urllib.error.URLError):
        return None


def run_gate(results: dict) -> int:
    failures = gate_failures(results)
    if not failures:
        print("Required CI gate passed for the classified change scope.")
        return 0
    if has_cancelled_dependency(results):
        # A concurrency-cancelled duplicate must not leave a failing required
        # check beside the run that replaced it. Without a replacement, the
        # cancelled run validated nothing and the gate still fails.
        replacement = find_superseding_run()
        if replacement is not None:
            print(f"::notice::Run cancelled before validation completed; superseded by run "
                  f"{replacement['id']}, whose required gate owns this commit's result.")
            return 0
        failures.append("Validation was cancelled and no newer run of this workflow covers this commit; "
                        "re-run this workflow.")
    print("\n".join(failures), file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("classify", "gate"))
    args = parser.parse_args()
    if args.command == "gate":
        return run_gate(json.loads(os.environ["RESULTS"]))
    event_name = os.environ.get("GITHUB_EVENT_NAME", "")
    try:
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
        required, reason = classify(event_name, event, Path.cwd())
        tier = validation_tier(event_name, event, is_batch=bool(os.environ.get("LUDUS_CI_BATCH_SHA")))
    except (OSError, ValueError, KeyError):
        required, reason = True, "Event metadata unavailable; full validation required."
        tier = FULL_TIER
    value = "true" if required else "false"
    with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
        output.write(f"native_required={value}\n")
        output.write(f"validation_tier={tier}\n")
    message = f"Native validation required: {value}. Validation tier: {tier}. {reason}"
    print(message)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as summary:
            summary.write(message + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
