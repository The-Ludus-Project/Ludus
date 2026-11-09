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
import time
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
FULL_TIER = "full"
INTEGRATION_TIER = "integration"
NIGHTLY_TIER = "nightly"
NIGHTLY_JOBS = ("macos-analysis",)
TIERS = frozenset({FULL_TIER, INTEGRATION_TIER, NIGHTLY_TIER})
# Integration PRs check sources, compile the Editor configuration and run static
# analysis, so a build or analysis break cannot reach develop and fail every
# later batch. Tests, sanitizers and the other compiled variants run once per batch.
PULL_REQUEST_JOBS = ("formatting", "analysis", "editor")
INTEGRATION_JOBS = tuple(name for name in NATIVE_JOBS if name not in PULL_REQUEST_JOBS)
# Bounded GitHub run metadata read by a cancelled pull-request gate.
MAX_RUN_METADATA_BYTES = 2 * 1024 * 1024
# Pull-request activity that changes neither the head commit nor, for edits, the
# base: an earlier run of this workflow already validates the same change. Edits
# re-run the workflow for Develop health's Batch-Fix declarations; a compile of the
# unchanged commit would only duplicate the earlier run's 30-minute native jobs.
REVALIDATED_ACTIONS = frozenset({"edited", "ready_for_review"})
GATE_JOB = "Ubuntu Clang"
# Develop health reads the PR description, so a description edit re-evaluates it in
# the edit's own run; the earlier run's verdict on the old description is not reused.
DEVELOP_HEALTH_JOB = "Develop health"
VALIDATING_RUN_POLL_SECONDS = 60
# Below the gate job's 360-minute default timeout, so the gate reports its own failure.
VALIDATING_RUN_WAIT_SECONDS = 300 * 60
MAX_CONSECUTIVE_LOOKUP_FAILURES = 5
RUN_ID = re.compile(r"[1-9][0-9]{0,19}")


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
    if event_name != "pull_request":
        raise ValueError("manual or unsupported event requires native validation")
    base = event["pull_request"]["base"]["sha"]
    head = event["pull_request"]["head"]["sha"]
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
    # The three-dot range covers the entire PR since its merge base, not just its last commit.
    result = subprocess.run(
        ["git", "diff", "--name-only", "--no-renames", "-z", f"{base}...{head}", "--"],
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
    """Use cheap checks for develop PRs and fully validate batches and every other PR."""
    if is_batch:
        return FULL_TIER
    if event_name == "workflow_dispatch":
        inputs = event.get("inputs") if isinstance(event, dict) else None
        if isinstance(inputs, dict) and inputs.get("validation_tier") == FULL_TIER:
            return FULL_TIER
        return NIGHTLY_TIER
    if event_name == "schedule":
        return NIGHTLY_TIER
    if event_name != "pull_request":
        return FULL_TIER
    try:
        if event["pull_request"].get("base", {}).get("ref") == "develop":
            return INTEGRATION_TIER
    except (AttributeError, KeyError, TypeError):
        pass
    return FULL_TIER


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
    validated_by = outputs.get("validated_by") or ""
    if validated_by and (RUN_ID.fullmatch(validated_by) is None or required != "true"):
        return ["Change classification did not report a valid validated_by output."]
    if set(results) != {"changes", *NATIVE_JOBS}:
        failures.append("Required native job inventory is incomplete or unexpected.")
    for name in NATIVE_JOBS:
        deferred = ((tier == INTEGRATION_TIER and name in INTEGRATION_JOBS)
                    or (tier != NIGHTLY_TIER and name in NIGHTLY_JOBS))
        expected = "success" if required == "true" and not deferred and not validated_by else "skipped"
        actual = results.get(name, {}).get("result")
        if actual != expected:
            failures.append(f"{name}: expected {expected}, received {actual}.")
    return failures


def validating_run_id(results: dict) -> int | None:
    """Return the earlier run that owns native validation for this run, if any."""
    value = results.get("changes", {}).get("outputs", {}).get("validated_by") or ""
    return int(value) if RUN_ID.fullmatch(value) else None


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


def read_same_commit_runs() -> tuple[dict, list] | None:
    """Read this pull-request run and the runs of its workflow for the same head commit.

    Raises on unavailable metadata; callers treat any failure as finding no run.
    """
    if os.environ.get("GITHUB_EVENT_NAME") != "pull_request" or os.environ.get("LUDUS_CI_BATCH_SHA"):
        return None
    current = read_github(f"actions/runs/{int(os.environ['GITHUB_RUN_ID'])}")
    if (not isinstance(current.get("id"), int) or not isinstance(current.get("workflow_id"), int)
            or re.fullmatch(r"[0-9a-f]{40}", str(current.get("head_sha"))) is None
            or current.get("event") != "pull_request"):
        return None
    listing = read_github(f"actions/workflows/{current['workflow_id']}/runs"
                          f"?head_sha={current['head_sha']}&event=pull_request&per_page=100")
    runs = listing.get("workflow_runs")
    return (current, runs) if isinstance(runs, list) else None


def find_superseding_run() -> dict | None:
    """Look up the replacement of a cancelled pull-request run; any failure finds none."""
    try:
        found = read_same_commit_runs()
        return superseding_run(*found) if found is not None else None
    except (KeyError, TypeError, ValueError, OSError, urllib.error.URLError):
        return None


def revalidates_unchanged_change(event: dict) -> bool:
    """Whether this pull-request activity keeps the head commit and the base branch."""
    action = event.get("action")
    if action not in REVALIDATED_ACTIONS:
        return False
    # Retargeting is an edit with a base change: its diff and merge result differ.
    changes = event.get("changes")
    return action != "edited" or (isinstance(changes, dict) and "base" not in changes)


def validating_run(current: dict, runs: list, number: int, base_ref: str) -> dict | None:
    """Return the newest earlier, uncancelled run of this workflow, PR, base and head commit.

    The selected run may still be in progress; the gate waits for it. It may itself
    be a revalidation that waits for an older run, so the oldest run that compiled
    decides every revalidation's result.
    """
    def same_pull_request(run: dict) -> bool:
        entries = run.get("pull_requests")
        return isinstance(entries, list) and any(
            isinstance(entry, dict) and entry.get("number") == number
            and isinstance(entry.get("base"), dict) and entry["base"].get("ref") == base_ref
            for entry in entries)

    candidates = [run for run in runs
                  if isinstance(run, dict) and isinstance(run.get("id"), int) and run["id"] < current["id"]
                  and run.get("workflow_id") == current["workflow_id"]
                  and run.get("event") == "pull_request"
                  and run.get("head_sha") == current["head_sha"]
                  and run.get("conclusion") != "cancelled"
                  and same_pull_request(run)]
    return max(candidates, key=lambda run: run["id"], default=None)


def find_validating_run(event: dict) -> int | None:
    """Find an earlier run that already validates this unchanged change; any failure finds none."""
    try:
        if not revalidates_unchanged_change(event):
            return None
        pull_request = event["pull_request"]
        number = pull_request["number"]
        base_ref = pull_request["base"]["ref"]
        if not isinstance(number, int) or not isinstance(base_ref, str):
            return None
        found = read_same_commit_runs()
        if found is None:
            return None
        run = validating_run(*found, number, base_ref)
        return run["id"] if run is not None else None
    except (AttributeError, KeyError, TypeError, ValueError, OSError, urllib.error.URLError):
        return None


def validating_run_failures(run_id: int, *, sleep=time.sleep, clock=time.monotonic) -> list[str]:
    """Wait for the run that owns native validation and require its gate to have passed.

    Every job except Develop health, which this run re-evaluates for the edited
    description, must have succeeded or been skipped. Lookup failures and a
    cancelled owner fail closed; re-run this workflow once the owner can report.
    """
    location = f"run {run_id}"
    deadline = clock() + VALIDATING_RUN_WAIT_SECONDS
    lookup_failures = 0
    while True:
        try:
            run = read_github(f"actions/runs/{run_id}")
            lookup_failures = 0
            if run.get("status") == "completed":
                break
        except (KeyError, TypeError, ValueError, OSError, urllib.error.URLError) as error:
            lookup_failures += 1
            if lookup_failures >= MAX_CONSECUTIVE_LOOKUP_FAILURES:
                return [f"Could not read {location}, which owns native validation for this commit ({error}); "
                        "re-run this workflow."]
        if clock() >= deadline:
            return [f"{location}, which owns native validation for this commit, did not complete in time; "
                    "re-run this workflow after it completes."]
        sleep(VALIDATING_RUN_POLL_SECONDS)
    if run.get("conclusion") == "cancelled":
        return [f"{location}, which owned native validation for this commit, was cancelled; "
                "re-run that run or push a new commit."]
    try:
        listing = read_github(f"actions/runs/{run_id}/jobs?filter=latest&per_page=100")
    except (KeyError, TypeError, ValueError, OSError, urllib.error.URLError) as error:
        return [f"Could not read the jobs of {location} ({error}); re-run this workflow."]
    jobs = listing.get("jobs")
    if not isinstance(jobs, list) or listing.get("total_count") != len(jobs) or not jobs:
        return [f"The job listing of {location} is incomplete; re-run this workflow."]
    failures = []
    gates = [job for job in jobs if isinstance(job, dict) and job.get("name") == GATE_JOB]
    if len(gates) != 1 or gates[0].get("conclusion") != "success":
        failures.append(f"{location}: the {GATE_JOB} gate did not pass.")
    for job in jobs:
        name = job.get("name") if isinstance(job, dict) else None
        conclusion = job.get("conclusion") if isinstance(job, dict) else None
        if name != DEVELOP_HEALTH_JOB and conclusion not in {"success", "skipped"}:
            failures.append(f"{location}: {name} concluded {conclusion}.")
    return failures


def run_gate(results: dict) -> int:
    failures = gate_failures(results)
    owner = validating_run_id(results)
    if not failures and owner is not None:
        # An edit or ready-for-review of an unchanged commit reuses the earlier run's
        # native validation and reports exactly its outcome on this run's gate.
        failures = validating_run_failures(owner)
        if not failures:
            print(f"Required CI gate passed: run {owner} validated this unchanged commit and base.")
            return 0
        print("\n".join(failures), file=sys.stderr)
        return 1
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
        # Documentation-only runs compile nothing, so they have nothing to reuse.
        owner = find_validating_run(event) if required and event_name == "pull_request" else None
    except (OSError, ValueError, KeyError):
        required, reason = True, "Event metadata unavailable; full validation required."
        tier = FULL_TIER
        owner = None
    value = "true" if required else "false"
    with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
        output.write(f"native_required={value}\n")
        output.write(f"validation_tier={tier}\n")
        output.write(f"validated_by={owner if owner is not None else ''}\n")
    message = f"Native validation required: {value}. Validation tier: {tier}. {reason}"
    if owner is not None:
        message += (f" Native jobs are skipped: run {owner} already validates this unchanged commit and base,"
                    " and the gate reports its result.")
    print(message)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as summary:
            summary.write(message + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
