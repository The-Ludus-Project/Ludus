#!/usr/bin/env python3
"""Capture a develop batch and promote its exact validated commit without rewriting history."""
from __future__ import annotations

import argparse
import base64
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request


ATTEMPT_BRANCH = "ci/last-batch-attempt"
VALIDATION_JOBS = ("native", "sdk", "wiki", "wiki-macos", "wiki-macos-intel",
                   "browser", "threading", "threading-windows", "windows")
# Batch job names are "<caller id> / <reusable job name>".
SUITE_JOBS = (*VALIDATION_JOBS, "recovery")
# A failed suite job rejects the candidate. Once one has failed, the jobs still
# running get this long to report their own results before the run is cancelled,
# so a red batch frees the queue for the fix instead of finishing its longest jobs.
# Measured batches report deterministic failures within 3-13 minutes, and every
# job except the sanitizers and macOS native backends ends within about 45.
STOP_CONCLUSIONS = frozenset({"failure", "timed_out", "startup_failure"})
# Aggregate gates fail whenever a job they read did not succeed, including a job
# that never acquired a runner. Every real failure they report is a failed job of
# its own, so they neither stop the batch nor block a runner retry.
AGGREGATE_JOBS = frozenset({"native / Ubuntu Clang"})
STOP_GRACE_SECONDS = 30 * 60
WATCH_INTERVAL_SECONDS = 60
# Below the watcher's 360-minute job limit, so it reports instead of timing out.
WATCH_LIMIT_SECONDS = 330 * 60
# Starts only after every suite has completed, whatever their results.
COMPLETION_JOB = "Prune superseded caches"
# Runs only when the watcher stopped the batch; develop health reads its presence.
STOP_JOB = "Cancel failed batch"
# The capture job; a newer tick without suite jobs captured nothing.
CAPTURE_JOB = "Capture develop candidate"
BATCH_WORKFLOW = "batch-ci.yml"
MAX_JOB_PAGES = 5
# GitHub cancels a job that no hosted runner accepts and annotates it with this.
# Retry such jobs a bounded number of times: an outage must not hold the single
# promotion slot indefinitely. The delay gives hosted capacity time to recover.
ACQUISITION_FAILURE = "was not acquired by Runner"
MAX_ACQUISITION_RETRIES = 2
RETRY_DELAY_SECONDS = 5 * 60
# Below the retry job's 360-minute limit, so it reports instead of timing out.
RETRY_LIMIT_SECONDS = 330 * 60
# Repository-owned cache key families. Each key ends in input hashes and, for
# compiler caches, the commit that saved it; older entries of a family on the
# batch's ref can never be restored again once a newer one exists.
PRUNED_CACHE_PREFIXES = ("ccache-", "conan-", "editor-pages-", "editor-tools-", "web-tools-", "icu-probe-", "pip-")
# The batch prunes main; the develop compiler-cache workflow prunes develop.
PRUNED_CACHE_REFS = ("refs/heads/main", "refs/heads/develop")
CACHE_KEY_HASH = re.compile(r"-[0-9a-f]{40}(?:[0-9a-f]{24})?$")
MAX_CACHE_PAGES = 20
RECOVERY_PATHS = (
    ".github/workflows/filesystem-recovery.yml",
    "modules/foundation/filesystem/src/persistence_posix.cpp",
    "modules/foundation/filesystem/tests/recovery_probe.cpp",
)


class BatchError(RuntimeError):
    """A failed prerequisite must leave the stable branch unchanged."""


def validate_sha(value: str) -> str:
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{40}", value) is None or value == "0" * 40:
        raise BatchError("Missing or invalid commit SHA.")
    return value


class GitHub:
    def __init__(self, repository: str, token: str):
        if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) is None or not token:
            raise BatchError("A GitHub repository and token are required.")
        self.repository = repository
        self.token = token

    def request(self, path: str, method: str = "GET", data: dict | None = None):
        # Thanks to GitHub, "REST API endpoints for Git references": only an
        # explicit 404 means an absent marker; authentication/API failures fail closed.
        # https://docs.github.com/en/rest/git/refs
        request = urllib.request.Request(
            f"https://api.github.com/repos/{self.repository}/{path}", method=method,
            data=json.dumps(data).encode() if data is not None else None,
            headers={"Authorization": f"Bearer {self.token}", "Accept": "application/vnd.github+json",
                     "X-GitHub-Api-Version": "2022-11-28", "Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = response.read(2 * 1024 * 1024 + 1)
        except urllib.error.HTTPError as error:
            if error.code == 404 and method == "GET" and path == f"git/ref/heads/{ATTEMPT_BRANCH}":
                return None
            raise BatchError(f"GitHub {method} {path} failed with HTTP {error.code}.") from None
        if len(payload) > 2 * 1024 * 1024:
            raise BatchError("GitHub response exceeded the bounded metadata limit.")
        # DELETE answers 204 No Content; any other empty body is malformed and fails closed.
        if method == "DELETE" and not payload:
            return None
        return json.loads(payload)

    def head(self, branch: str) -> str | None:
        result = self.request(f"git/ref/heads/{branch}")
        if result is None:
            return None
        if result.get("object", {}).get("type") != "commit":
            raise BatchError(f"{branch} does not reference a commit.")
        return validate_sha(result["object"]["sha"])

    def is_ancestor(self, base: str, head: str) -> bool:
        if base == head:
            return True
        # Thanks to GitHub, "REST API endpoints for commits", "Compare two
        # commits": changed files (including patches) appear only on page one.
        # Request page two with one commit to keep ancestry metadata small even
        # for a large batch. Status and merge base describe the whole comparison,
        # including when this page has no commits. Recovery uses the Git diff.
        # https://docs.github.com/en/rest/commits/commits#compare-two-commits
        comparison = self.request(f"compare/{validate_sha(base)}...{validate_sha(head)}?per_page=1&page=2")
        return comparison.get("status") == "ahead" and comparison.get("merge_base_commit", {}).get("sha") == base


def git(root: Path, arguments: list[str], token: str = "") -> str:
    environment = {**os.environ, "GIT_TERMINAL_PROMPT": "0"}
    if token:
        # Keep credentials out of command lines, repository configuration and logs.
        authorization = base64.b64encode(f"x-access-token:{token}".encode()).decode()
        environment.update(GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="http.https://github.com/.extraheader",
                           GIT_CONFIG_VALUE_0=f"AUTHORIZATION: basic {authorization}")
    result = subprocess.run(["git", *arguments], cwd=root, env=environment, capture_output=True,
                            text=True, timeout=120)
    if result.returncode:
        raise BatchError(f"Git {arguments[0]} failed: {result.stderr.strip()}")
    return result.stdout


def snapshot(client: GitHub, root: Path, expected_main: str, *, retry: bool = False) -> dict:
    base = client.head("main")
    candidate = client.head("develop")
    if base != validate_sha(expected_main):
        return {"should_run": "false", "reason": "main advanced before this scheduler started; next tick will retry."}
    if base == candidate:
        return {"should_run": "false", "reason": "develop has no unpromoted changes."}
    if not client.is_ancestor(base, candidate):
        raise BatchError("develop must descend from main; reconcile the branches before validating a batch.")
    previous = client.head(ATTEMPT_BRANCH)
    if previous == candidate and not retry:
        return {"should_run": "false", "reason": f"develop {candidate} was already attempted; fix develop or dispatch with retry."}
    remote = f"https://github.com/{client.repository}.git"
    git(root, ["fetch", "--no-tags", remote, base, candidate], client.token)
    paths = git(root, ["diff", "--name-only", "--no-renames", "-z", f"{base}..{candidate}", "--"]).split("\0")
    recovery = any(path in RECOVERY_PATHS or path.startswith("tests/filesystem/recovery") for path in paths)
    if previous is None:
        client.request("git/refs", "POST", {"ref": f"refs/heads/{ATTEMPT_BRANCH}", "sha": candidate})
    elif previous != candidate:
        client.request(f"git/refs/heads/{ATTEMPT_BRANCH}", "PATCH", {"sha": candidate, "force": False})
    return {"should_run": "true", "base_sha": base, "candidate_sha": candidate,
            "recovery_required": "true" if recovery else "false",
            "reason": f"Validate frozen develop commit {candidate} above main {base}."}


def describe_snapshot(outputs: dict) -> tuple[str, str]:
    """Return the run annotation and job summary for a capture decision.

    A skipped capture validates nothing; the workflow's unvalidated job then cancels
    the run so that it never reads as a passing batch or a promotion.
    """
    reason = outputs["reason"]
    if outputs.get("should_run") == "true":
        return "", f"### Validating develop candidate\n\n{reason}\n"
    # Thanks to GitHub, "Workflow commands for GitHub Actions", setting a notice
    # message and escaping its data:
    # https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-commands
    message = f"{reason} Validation and promotion were skipped; main is unchanged."
    escaped = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    summary = (f"### No promotion this run\n\n{reason}\n\n"
               "Validation and promotion were skipped, so `main` is unchanged. "
               "Nothing was validated, so this run is cancelled rather than reported as passing.\n")
    return f"::notice title=No promotion this run::{escaped}", summary


def validate_results(results: dict, base: str, candidate: str) -> None:
    if set(results) != {"snapshot", "recovery", *VALIDATION_JOBS}:
        raise BatchError("Batch validation job inventory is incomplete or unexpected.")
    capture = results["snapshot"]
    outputs = capture.get("outputs", {})
    if (capture.get("result") != "success" or outputs.get("should_run") != "true"
            or outputs.get("base_sha") != base or outputs.get("candidate_sha") != candidate
            or outputs.get("recovery_required") not in {"true", "false"}):
        raise BatchError("The frozen candidate does not match the successful snapshot.")
    for name in VALIDATION_JOBS:
        if results[name].get("result") != "success":
            raise BatchError(f"Required batch validation did not succeed: {name}.")
    expected = "success" if outputs["recovery_required"] == "true" else "skipped"
    if results["recovery"].get("result") != expected:
        raise BatchError("Filesystem recovery did not report its required result.")


def push_fast_forward(root: Path, remote: str, base: str, candidate: str, token: str = "") -> None:
    validate_sha(base)
    validate_sha(candidate)
    git(root, ["fetch", "--no-tags", remote, base, candidate], token)
    git(root, ["merge-base", "--is-ancestor", base, candidate])
    # Thanks to the Git project, "git-push", --force-with-lease=<ref>:<expect>:
    # an explicit lease rejects a concurrent main update. The ancestry check
    # above additionally forbids a non-fast-forward, even with the lease.
    # https://git-scm.com/docs/git-push
    git(root, ["push", f"--force-with-lease=refs/heads/main:{base}", remote,
               f"{candidate}:refs/heads/main"], token)


def promote(client: GitHub, root: Path, base: str, candidate: str, results: dict) -> None:
    validate_sha(base)
    validate_sha(candidate)
    validate_results(results, base, candidate)
    if client.head("main") != base:
        raise BatchError("main changed during validation; the candidate must be validated again.")
    if not client.is_ancestor(base, candidate) or not client.is_ancestor(candidate, client.head("develop")):
        raise BatchError("The tested candidate must descend from main and remain in develop's history.")
    push_fast_forward(root, f"https://github.com/{client.repository}.git", base, candidate, client.token)


def cache_family(key: str) -> str | None:
    """Return the key without its trailing input/commit hashes, or None if not repository-owned."""
    if not key.startswith(PRUNED_CACHE_PREFIXES):
        return None
    family = key
    while (match := CACHE_KEY_HASH.search(family)) is not None:
        family = family[:match.start()]
    # An empty hashFiles() result leaves a trailing separator.
    return family.rstrip("-") or None


def list_caches(client: GitHub, ref: str) -> list[dict]:
    # Thanks to GitHub, "REST API endpoints for GitHub Actions cache", List
    # GitHub Actions caches for a repository and Delete a cache by ID:
    # https://docs.github.com/en/rest/actions/cache#list-github-actions-caches-for-a-repository
    caches: list[dict] = []
    for page in range(1, MAX_CACHE_PAGES + 1):
        listing = client.request(f"actions/caches?ref={ref}&per_page=100&page={page}")
        entries = listing.get("actions_caches") if isinstance(listing, dict) else None
        if not isinstance(entries, list):
            raise BatchError("GitHub returned no cache listing.")
        caches.extend(entry for entry in entries if isinstance(entry, dict))
        total = listing.get("total_count")
        if not entries or not isinstance(total, int) or len(caches) >= total:
            return caches
    raise BatchError("The repository has more caches than the bounded listing reads.")


def select_superseded_caches(caches: list[dict], ref: str) -> list[dict]:
    """Keep the newest entry of each repository-owned family on ref; return the rest."""
    newest: dict[str, dict] = {}
    superseded: list[dict] = []
    for entry in caches:
        key = entry.get("key")
        if entry.get("ref") != ref or not isinstance(key, str) or not isinstance(entry.get("id"), int):
            continue
        family = cache_family(key)
        if family is None:
            continue
        order = (str(entry.get("created_at", "")), entry["id"])
        kept = newest.get(family)
        if kept is None:
            newest[family] = entry
        elif order > (str(kept.get("created_at", "")), kept["id"]):
            superseded.append(kept)
            newest[family] = entry
        else:
            superseded.append(entry)
    return sorted(superseded, key=lambda entry: entry["id"])


def prune_caches(client: GitHub, ref: str) -> tuple[list[dict], list[str]]:
    """Delete superseded caches on ref. Return deleted entries and per-entry failures.

    The repository cache limit evicts least-recently-used entries once it is full.
    Each batch saves a new commit-keyed compiler cache for every variant, so without
    pruning the stale copies evict the large dependency and sanitizer caches.
    """
    if re.fullmatch(r"refs/heads/[A-Za-z0-9._/-]+", ref) is None:
        raise BatchError("Cache pruning needs an explicit branch ref.")
    deleted: list[dict] = []
    failures: list[str] = []
    for entry in select_superseded_caches(list_caches(client, ref), ref):
        try:
            client.request(f"actions/caches/{entry['id']}", "DELETE")
        except BatchError as error:
            # Another run may already have removed it; keep pruning the rest.
            failures.append(f"{entry['key']}: {error}")
        else:
            deleted.append(entry)
    return deleted, failures


def describe_pruning(deleted: list[dict], failures: list[str]) -> str:
    freed = sum(entry.get("size_in_bytes", 0) for entry in deleted if isinstance(entry.get("size_in_bytes"), int))
    lines = [f"### Cache pruning\n\nDeleted {len(deleted)} superseded caches ({freed / 2**20:.0f} MiB).\n"]
    lines += [f"- `{entry['key']}`" for entry in deleted]
    if failures:
        lines.append(f"\n{len(failures)} deletions failed:\n")
        lines += [f"- {failure}" for failure in failures]
    return "\n".join(lines) + "\n"


def is_suite_job(name: object) -> bool:
    """Whether name is a validation job such as "native / ASan and UBSan"."""
    if not isinstance(name, str) or " / " not in name:
        return False
    suite, job = name.split(" / ", 1)
    return suite in SUITE_JOBS and bool(job)


def parse_time(value: object) -> datetime | None:
    if not isinstance(value, str):
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return parsed if parsed.tzinfo is not None else None


def failed_suite_jobs(jobs: list[dict]) -> list[dict]:
    return [job for job in jobs if is_suite_job(job.get("name")) and job.get("conclusion") in STOP_CONCLUSIONS]


def stopping_suite_jobs(jobs: list[dict]) -> list[dict]:
    """Failed suite jobs that reject the candidate by themselves, excluding aggregate gates."""
    return [job for job in failed_suite_jobs(jobs) if job.get("name") not in AGGREGATE_JOBS]


def watch_decision(jobs: list[dict], now: datetime, grace_seconds: float = STOP_GRACE_SECONDS) -> str:
    """Return "done", "stop" or "wait" for one listing of the batch's jobs.

    "done": every suite has completed, so there is nothing left to stop.
    "stop": a suite job failed at least grace_seconds ago; the jobs still running
    have had their chance to report and the run should be cancelled.
    "wait": no suite job has failed yet, or the grace period is still running.
    """
    if any(job.get("name") == COMPLETION_JOB for job in jobs):
        return "done"
    failed = stopping_suite_jobs(jobs)
    if not failed:
        return "wait"
    # A failed job without a completion time counts from now: never stop early.
    first = min(parse_time(job.get("completed_at")) or now for job in failed)
    return "stop" if (now - first).total_seconds() >= grace_seconds else "wait"


def list_run_jobs(client: GitHub, run_id: int, *, attempt: int | None = None) -> list[dict]:
    # Thanks to GitHub, "REST API endpoints for workflow jobs", List jobs for a
    # workflow run: filter=latest reports each job's most recent attempt.
    # https://docs.github.com/en/rest/actions/workflow-jobs#list-jobs-for-a-workflow-run
    run_path = f"actions/runs/{int(run_id)}"
    endpoint = (f"{run_path}/jobs?filter=latest&" if attempt is None
                else f"{run_path}/attempts/{int(attempt)}/jobs?")
    jobs: list[dict] = []
    for page in range(1, MAX_JOB_PAGES + 1):
        listing = client.request(f"{endpoint}per_page=100&page={page}")
        entries = listing.get("jobs") if isinstance(listing, dict) else None
        if not isinstance(entries, list):
            raise BatchError(f"Run {run_id} returned no job listing.")
        jobs.extend(entry for entry in entries if isinstance(entry, dict))
        total = listing.get("total_count")
        if not entries or not isinstance(total, int) or len(jobs) >= total:
            return jobs
    raise BatchError(f"Run {run_id} has more jobs than the bounded listing reads.")


def watch(client: GitHub, run_id: int, *, clock=None, sleep=time.sleep,
          grace_seconds: float = STOP_GRACE_SECONDS, interval_seconds: float = WATCH_INTERVAL_SECONDS,
          limit_seconds: float = WATCH_LIMIT_SECONDS, max_errors: int = 5) -> tuple[bool, list[str]]:
    """Poll the batch run; return whether to stop it and the failed suite jobs seen.

    Watching fails open: if the listing stays unavailable or the limit passes,
    the batch is left to finish normally and its suites still decide promotion.
    """
    clock = clock or (lambda: datetime.now(timezone.utc))
    started = clock()
    errors = 0
    failed: list[str] = []
    observed_failures: set[int] = set()
    while (clock() - started).total_seconds() < limit_seconds:
        try:
            jobs = list_run_jobs(client, run_id)
        except BatchError as error:
            errors += 1
            print(f"Batch CI: job listing unavailable ({errors}/{max_errors}): {error}", file=sys.stderr)
            if errors >= max_errors:
                return False, failed
        else:
            errors = 0
            failures = failed_suite_jobs(jobs)
            failed = sorted({job["name"] for job in failures})
            observed_at = clock()
            decision = watch_decision(jobs, observed_at, grace_seconds)
            for job in failures:
                job_id = job.get("id")
                if isinstance(job_id, int) and job_id not in observed_failures:
                    observed_failures.add(job_id)
                    print(f"Batch report observed failure {job_id} at {observed_at.isoformat()}", flush=True)
            if decision == "stop":
                for job in jobs:
                    job_id = job.get("id")
                    if (isinstance(job_id, int) and is_suite_job(job.get("name"))
                            and job.get("status") in {"queued", "waiting", "pending", "requested"}):
                        print(f"Batch report observed queued job {job_id}", flush=True)
            if decision != "wait":
                return decision == "stop", failed
        sleep(interval_seconds)
    return False, failed


def describe_watch(should_stop: bool, failed: list[str]) -> str:
    if should_stop:
        lines = [f"### Batch stopped early\n\nThese suite jobs failed, so `main` cannot advance to this candidate. "
                 f"Jobs still running {STOP_GRACE_SECONDS // 60} minutes after the first failure are cancelled "
                 "so the next candidate can start. Develop health treats this run as red.\n"]
    elif failed:
        lines = ["### Batch finished with failures\n\nEvery job reported before the stop deadline.\n"]
    else:
        return "### No suite failed while watching\n"
    return "\n".join([*lines, *(f"- {name}" for name in failed)]) + "\n"


def attempt_identity(job: dict) -> tuple[str, str] | None:
    """Return the name and completion time that identify a job across run attempts.

    A re-run attempt copies each job it does not run under a new id, keeping the
    name and completion time but none of the original's annotations (observed in
    batch run 45203619841, where an unacquired job's copy had no annotations).
    """
    name = job.get("name")
    completed_at = job.get("completed_at")
    if not isinstance(name, str) or not isinstance(completed_at, str):
        return None
    return name, completed_at


def is_unacquired(client: GitHub, job: dict, known: frozenset[tuple[str, str]] = frozenset()) -> bool:
    """Whether GitHub cancelled job because no hosted runner accepted it.

    Only a cancelled job that never had a runner is checked, and only GitHub's own
    acquisition annotation qualifies: a cancellation by a person, by the stop job
    or by a newer run carries no such annotation and is never retried. known holds
    the attempt identities of jobs already found unacquired in an earlier attempt,
    so a copy that a later attempt did not run keeps that classification.
    """
    if job.get("conclusion") != "cancelled" or job.get("runner_name") or not isinstance(job.get("id"), int):
        return False
    if attempt_identity(job) in known:
        return True
    # Thanks to GitHub, "REST API endpoints for check runs", List check run
    # annotations: a workflow job's id is its check run id.
    # https://docs.github.com/en/rest/checks/runs#list-check-run-annotations
    annotations = client.request(f"check-runs/{job['id']}/annotations?per_page=50")
    if not isinstance(annotations, list):
        raise BatchError(f"Job {job['id']} returned no annotation listing.")
    return any(isinstance(entry, dict) and ACQUISITION_FAILURE in str(entry.get("message", ""))
               for entry in annotations)


def retry_candidates(client: GitHub, jobs: list[dict],
                     known: frozenset[tuple[str, str]] = frozenset()) -> tuple[list[dict], str]:
    """Return the jobs to retry, or none and the reason the run is not retried.

    known holds the attempt identities of jobs found unacquired in earlier attempts.
    """
    if any(job.get("name") == STOP_JOB and job.get("conclusion") not in {None, "skipped"} for job in jobs):
        return [], "The batch was stopped after a suite failed; the candidate is already rejected."
    failed = sorted(job.get("name", "?") for job in jobs
                    if job.get("conclusion") in STOP_CONCLUSIONS and job.get("name") not in AGGREGATE_JOBS)
    if failed:
        return [], "Jobs failed for reasons other than runner capacity: " + ", ".join(failed) + "."
    unacquired = [job for job in jobs if is_unacquired(client, job, known)]
    if not unacquired:
        return [], "No job was cancelled for lack of a runner."
    return sorted(unacquired, key=lambda job: job["id"]), ""


def newer_batch_state(client: GitHub, run_id: int) -> str:
    """Return "none", "started" or "pending" for batch runs created after run_id.

    A newer tick that captured nothing runs only its capture job and cancels
    itself. A newer run with suite jobs validates a later develop commit, which
    contains this candidate, so retrying this run would only delay it.
    """
    listing = client.request(f"actions/workflows/{BATCH_WORKFLOW}/runs?branch=main&per_page=20")
    runs = listing.get("workflow_runs") if isinstance(listing, dict) else None
    if not isinstance(runs, list):
        raise BatchError("The batch run listing is unavailable.")
    state = "none"
    for run in runs:
        if not isinstance(run, dict) or not isinstance(run.get("id"), int) or run["id"] <= run_id:
            continue
        jobs = list_run_jobs(client, run["id"])
        if any(is_suite_job(job.get("name")) for job in jobs):
            return "started"
        capture = [job for job in jobs if job.get("name") == CAPTURE_JOB]
        if run.get("status") != "completed" and (not capture or capture[0].get("status") != "completed"):
            state = "pending"
    return state


def wait_for_run(client: GitHub, run_id: int, *, clock, sleep, deadline: datetime,
                 interval_seconds: float, after_attempt: int = 0) -> dict | None:
    """Poll until the run has an attempt above after_attempt and that attempt completed."""
    while clock() < deadline:
        run = client.request(f"actions/runs/{int(run_id)}")
        if not isinstance(run, dict):
            raise BatchError(f"Run {run_id} returned no metadata.")
        attempt = run.get("run_attempt")
        if isinstance(attempt, int) and attempt > after_attempt and run.get("status") == "completed":
            return run
        sleep(interval_seconds)
    return None


def retry_unacquired(client: GitHub, run_id: int, *, clock=None, sleep=time.sleep,
                     retries: int = MAX_ACQUISITION_RETRIES, delay_seconds: float = RETRY_DELAY_SECONDS,
                     interval_seconds: float = WATCH_INTERVAL_SECONDS,
                     limit_seconds: float = RETRY_LIMIT_SECONDS) -> tuple[bool, list[str]]:
    """Re-run batch jobs GitHub cancelled before a hosted runner accepted them.

    GitHub re-runs jobs only in a completed run, so a retried job starts after
    every other job has finished and asks for one runner alone: the serialized
    admission the batch used before its macOS jobs ran in parallel. Each round
    re-runs one job and its dependents, at most retries rounds in all. Return
    whether the run still needs a person (capacity exhausted or time ran out)
    and the lines that report what happened.
    """
    clock = clock or (lambda: datetime.now(timezone.utc))
    deadline = clock() + timedelta(seconds=limit_seconds)
    lines: list[str] = []
    attempt = 0
    known: frozenset[tuple[str, str]] = frozenset()
    for round_number in range(retries + 1):
        run = wait_for_run(client, run_id, clock=clock, sleep=sleep, deadline=deadline,
                           interval_seconds=interval_seconds, after_attempt=attempt)
        if run is None:
            return True, [*lines, "Stopped waiting for the batch run within the retry time limit."]
        attempt = run["run_attempt"]
        candidates, reason = retry_candidates(client, list_run_jobs(client, run_id), known)
        if not candidates:
            return False, [*lines, reason]
        known = known | {identity for job in candidates if (identity := attempt_identity(job)) is not None}
        names = ", ".join(job.get("name", "?") for job in candidates)
        if round_number == retries:
            return True, [*lines, f"GitHub could not supply a runner after {retries} retries: {names}. "
                                  "This is runner capacity, not a code failure; dispatch the batch with "
                                  "retry once capacity returns."]
        while (state := newer_batch_state(client, run_id)) == "pending":
            if clock() >= deadline:
                return True, [*lines, "A newer tick did not finish capturing within the retry time limit."]
            sleep(interval_seconds)
        if state == "started":
            return False, [*lines, "A newer batch already validates a later develop commit; not retrying."]
        sleep(delay_seconds)
        job = candidates[0]
        client.request(f"actions/jobs/{job['id']}/rerun", "POST", {})
        lines.append(f"Retry {round_number + 1} of {retries}: re-running {job.get('name', '?')} "
                     "and the jobs that depend on it.")
    return True, lines


def describe_retry(needs_attention: bool, lines: list[str]) -> str:
    title = "### Runner retry needs attention" if needs_attention else "### Runner retry"
    return "\n".join([title, "", *(f"- {line}" for line in lines)]) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("snapshot", "promote", "prune-caches", "watch", "retry-runners"))
    parser.add_argument("--retry", action="store_true")
    parser.add_argument("--base", default="")
    parser.add_argument("--candidate", default="")
    arguments = parser.parse_args()
    try:
        # The develop compiler-cache workflow prunes its own scope; every other
        # command controls the batch and runs only from trusted main.
        allowed = PRUNED_CACHE_REFS if arguments.command == "prune-caches" else ("refs/heads/main",)
        if os.environ.get("GITHUB_REF") not in allowed:
            raise BatchError("Batch control may run only from main." if arguments.command != "prune-caches"
                             else "Cache pruning may run only from main or develop.")
        client = GitHub(os.environ["GITHUB_REPOSITORY"], os.environ["GH_TOKEN"])
        if arguments.command == "snapshot":
            outputs = snapshot(client, Path.cwd(), os.environ["GITHUB_SHA"], retry=arguments.retry)
            with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as stream:
                for name, value in outputs.items():
                    if name != "reason":
                        stream.write(f"{name}={value}\n")
            annotation, summary = describe_snapshot(outputs)
            print(annotation or outputs["reason"])
        elif arguments.command == "watch":
            should_stop, failed = watch(client, int(os.environ["GITHUB_RUN_ID"]))
            with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as stream:
                stream.write(f"should_stop={'true' if should_stop else 'false'}\n")
            summary = describe_watch(should_stop, failed)
            print(summary, end="")
        elif arguments.command == "retry-runners":
            needs_attention, lines = retry_unacquired(client, int(os.environ["BATCH_RUN_ID"]))
            summary = describe_retry(needs_attention, lines)
            print(summary, end="")
            if needs_attention:
                escaped = " ".join(lines[-1:]).replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
                print(f"::error title=Batch runner retry::{escaped}")
                if os.environ.get("GITHUB_STEP_SUMMARY"):
                    with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as stream:
                        stream.write(summary)
                return 1
        elif arguments.command == "prune-caches":
            deleted, failures = prune_caches(client, os.environ["GITHUB_REF"])
            summary = describe_pruning(deleted, failures)
            print(summary, end="")
            for failure in failures:
                escaped = failure.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
                print(f"::warning title=Cache not deleted::{escaped}")
        else:
            promote(client, Path.cwd(), arguments.base, arguments.candidate, json.loads(os.environ["BATCH_RESULTS"]))
            summary = f"Promoted exactly {arguments.candidate} to main.\n"
            print(summary, end="")
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as stream:
                stream.write(summary)
        return 0
    except (BatchError, KeyError, TypeError, ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"Batch CI: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
