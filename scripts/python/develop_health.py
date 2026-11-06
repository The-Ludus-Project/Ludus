#!/usr/bin/env python3
"""Admit only proven fixes into develop while its latest validated batch is red.

The gate reads completed batch runs instead of state written by the coordinator,
so it takes effect on develop pull requests without waiting for a promotion.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import sys
import urllib.error
import urllib.request

import batch_ci
import ci_scope


BATCH_WORKFLOW = "batch-ci.yml"
PULL_REQUEST_WORKFLOW = "ci.yml"
GATE_JOB = "Develop health"
# Batch caller job id -> the reusable workflow a fix PR dispatches for evidence.
# Batch job names are "<caller id> / <reusable job name>".
BATCH_WORKFLOWS = {
    "native": "ci.yml",
    "sdk": "project-sdk.yml",
    "wiki": "wiki.yml",
    "browser": "webgpu-probe.yml",
    "threading": "threading-platforms.yml",
    "windows": "windows-direct3d12.yml",
    "recovery": "filesystem-recovery.yml",
}
# A documentation-only change can still break these suites.
DOCUMENTATION_SUITES = frozenset({"wiki"})
FAILED_CONCLUSIONS = frozenset({"failure", "timed_out", "cancelled", "startup_failure"})
DECLARATION = re.compile(r"^[ \t]*Batch-Fix:[ \t]*(\S[^\r\n]*?)[ \t]*\r?$", re.MULTILINE)
MAX_DECLARATIONS = 32
MAX_METADATA_BYTES = 2 * 1024 * 1024
# Scheduler ticks every 30 minutes; 120 completed runs cover about 60 hours of idle ticks.
MAX_SCANNED_BATCH_RUNS = 120
BATCH_RUNS_PAGE_SIZE = 40
MAX_JOB_PAGES = 5
MAX_EVIDENCE_RUNS = 20
MAX_PULL_REQUEST_PAGES = 3


class HealthError(RuntimeError):
    """Health metadata is unavailable; the gate fails closed and asks for a re-run."""


class GitHub:
    def __init__(self, repository: str, token: str, api: str = "https://api.github.com"):
        if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) is None or not token:
            raise HealthError("A GitHub repository and token are required.")
        self.repository = repository
        self.token = token
        self.api = api

    def request(self, path: str, method: str = "GET"):
        # Thanks to GitHub, "REST API endpoints for workflow runs" and "workflow
        # jobs": runs filter by branch, status and head_sha; a completed run's
        # single job can be re-run after it finishes.
        # https://docs.github.com/en/rest/actions/workflow-runs
        # https://docs.github.com/en/rest/actions/workflow-jobs
        request = urllib.request.Request(
            f"{self.api}/repos/{self.repository}/{path}", method=method,
            headers={"Authorization": f"Bearer {self.token}", "Accept": "application/vnd.github+json",
                     "X-GitHub-Api-Version": "2022-11-28"},
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = response.read(MAX_METADATA_BYTES + 1)
        except urllib.error.HTTPError as error:
            raise HealthError(f"GitHub {method} {path} failed with HTTP {error.code}.") from None
        except (urllib.error.URLError, OSError) as error:
            raise HealthError(f"GitHub {method} {path} failed: {error}.") from None
        if len(payload) > MAX_METADATA_BYTES:
            raise HealthError("GitHub response exceeded the bounded metadata limit.")
        if not payload.strip():
            return {}
        result = json.loads(payload)
        if not isinstance(result, (dict, list)):
            raise HealthError(f"GitHub {path} returned unexpected metadata.")
        return result

    def get(self, path: str):
        return self.request(path)

    def post(self, path: str):
        return self.request(path, "POST")


@dataclass(frozen=True)
class Health:
    """The latest validated batch; run_id None means no validated batch was found."""

    run_id: int | None
    run_url: str
    failed_jobs: tuple[str, ...]

    @property
    def is_red(self) -> bool:
        return len(self.failed_jobs) > 0


UNKNOWN_HEALTH = Health(None, "", ())


def split_batch_job(name: object) -> tuple[str, str] | None:
    """Split "native / ASan and UBSan" into its suite and job name."""
    if not isinstance(name, str) or " / " not in name:
        return None
    suite, job = name.split(" / ", 1)
    if suite not in BATCH_WORKFLOWS or not job:
        return None
    return suite, job


def list_jobs(client, run_id: int) -> list[dict]:
    jobs: list[dict] = []
    for page in range(1, MAX_JOB_PAGES + 1):
        listing = client.get(f"actions/runs/{int(run_id)}/jobs?filter=latest&per_page=100&page={page}")
        batch = listing.get("jobs") if isinstance(listing, dict) else None
        if not isinstance(batch, list):
            raise HealthError(f"Run {run_id} returned no job listing.")
        jobs.extend(job for job in batch if isinstance(job, dict))
        total = listing.get("total_count")
        if not batch or not isinstance(total, int) or len(jobs) >= total:
            return jobs
    raise HealthError(f"Run {run_id} has more jobs than the bounded listing reads.")


def classify_batch(run: dict, jobs: list[dict]) -> Health | None:
    """Return the health a completed batch run establishes, or None if it validated nothing.

    A tick that captured nothing skips every suite; a cancelled run, or one whose
    capture failed, proves nothing about the code. Promotion and publication are
    not code validation: a candidate whose suites all passed is healthy even
    when main moved before its promotion.
    """
    if run.get("conclusion") == "cancelled" or not isinstance(run.get("id"), int):
        return None
    suites = [job for job in jobs if split_batch_job(job.get("name")) is not None]
    ran = [job for job in suites if job.get("conclusion") not in {None, "skipped"}]
    if not ran:
        return None
    failed = sorted({job["name"] for job in ran if job.get("conclusion") in FAILED_CONCLUSIONS})
    return Health(run["id"], str(run.get("html_url", "")), tuple(failed))


def validated_batches(client):
    """Yield validated main batch runs, newest first, within the scan bound."""
    scanned = 0
    page = 1
    while scanned < MAX_SCANNED_BATCH_RUNS:
        listing = client.get(f"actions/workflows/{BATCH_WORKFLOW}/runs?branch=main&status=completed"
                             f"&per_page={BATCH_RUNS_PAGE_SIZE}&page={page}")
        runs = listing.get("workflow_runs") if isinstance(listing, dict) else None
        if not isinstance(runs, list):
            raise HealthError("The batch run listing is unavailable.")
        if not runs:
            return
        for run in sorted((run for run in runs if isinstance(run, dict) and isinstance(run.get("id"), int)),
                          key=lambda run: run["id"], reverse=True):
            scanned += 1
            health = classify_batch(run, list_jobs(client, run["id"]))
            if health is not None:
                yield health
            if scanned >= MAX_SCANNED_BATCH_RUNS:
                return
        page += 1


def read_health(client) -> Health:
    return next(iter(validated_batches(client)), UNKNOWN_HEALTH)


def parse_declarations(body: object) -> list[str]:
    """Return the unique "Batch-Fix: <batch job>" declarations in a PR body."""
    if not isinstance(body, str):
        return []
    declared: list[str] = []
    for match in DECLARATION.finditer(body):
        name = match.group(1)
        if name not in declared:
            declared.append(name)
    return declared


def dispatch_command(suite: str, branch: str) -> str:
    workflow = BATCH_WORKFLOWS[suite]
    tier = " -f validation_tier=full" if workflow == PULL_REQUEST_WORKFLOW else ""
    return f"gh workflow run {workflow} --ref {branch}{tier}"


def has_evidence(client, suite: str, job_name: str, head_sha: str) -> bool:
    """Whether the owning workflow's job succeeded on exactly this head commit."""
    workflow = BATCH_WORKFLOWS[suite]
    listing = client.get(f"actions/workflows/{workflow}/runs?head_sha={head_sha}&per_page={MAX_EVIDENCE_RUNS}")
    runs = listing.get("workflow_runs") if isinstance(listing, dict) else None
    if not isinstance(runs, list):
        raise HealthError(f"The {workflow} run listing is unavailable.")
    for run in sorted((run for run in runs if isinstance(run, dict) and isinstance(run.get("id"), int)
                       and run.get("head_sha") == head_sha), key=lambda run: run["id"], reverse=True):
        for job in list_jobs(client, run["id"]):
            if job.get("name") == job_name and job.get("conclusion") == "success":
                return True
    return False


def contains_commit(client, ancestor: str, head: str) -> bool:
    if ancestor == head:
        return True
    comparison = client.get(f"compare/{batch_ci.validate_sha(ancestor)}...{batch_ci.validate_sha(head)}"
                            "?per_page=1&page=2")
    return (isinstance(comparison, dict) and comparison.get("status") == "ahead"
            and comparison.get("merge_base_commit", {}).get("sha") == ancestor)


def attempted_candidate(client) -> str:
    reference = client.get(f"git/ref/heads/{batch_ci.ATTEMPT_BRANCH}")
    sha = reference.get("object", {}).get("sha") if isinstance(reference, dict) else None
    return batch_ci.validate_sha(sha)


def evaluate(client, health: Health, *, body: object, head_sha: str, branch: str,
             is_documentation_only: bool, waiver: str = "") -> tuple[bool, list[str]]:
    """Decide whether a develop pull request may enter, with the reasons to report."""
    if health.run_id is None:
        return True, [f"No validated batch in the last {MAX_SCANNED_BATCH_RUNS} completed batch runs; "
                      "develop is treated as open."]
    if not health.is_red:
        return True, [f"The latest validated batch passed: {health.run_url}"]
    failed = "\n".join(f"- {name}" for name in health.failed_jobs)
    header = f"develop is red. The latest validated batch failed: {health.run_url}\n{failed}"
    if waiver.strip() == str(health.run_id):
        return True, [header, f"A maintainer waived batch {health.run_id} (DEVELOP_HEALTH_WAIVER); "
                              "develop is open until the next validated batch."]
    declared = parse_declarations(body)
    if not declared:
        suites = {split_batch_job(name)[0] for name in health.failed_jobs}
        if is_documentation_only and suites.isdisjoint(DOCUMENTATION_SUITES):
            return True, [header, "This documentation-only change cannot affect the failing suites."]
        return False, [header, "Only fixes may enter develop until a batch passes. To submit a fix, add a "
                               "`Batch-Fix: <failed batch job>` line to the PR description for each job it "
                               "fixes, merge current develop, dispatch the owning workflow on this branch, "
                               f"and re-run the {GATE_JOB} check."]
    if len(declared) > MAX_DECLARATIONS:
        return False, [header, f"Declare at most {MAX_DECLARATIONS} Batch-Fix jobs."]
    unknown = [name for name in declared if name not in health.failed_jobs]
    if unknown:
        return False, [header, "These Batch-Fix declarations are not failed jobs of that batch:\n"
                       + "\n".join(f"- {name}" for name in unknown)]
    candidate = attempted_candidate(client)
    if not contains_commit(client, candidate, head_sha):
        return False, [header, f"Merge develop first: evidence must include the last attempted batch "
                               f"candidate {candidate}, which this head {head_sha} does not contain."]
    missing = []
    for name in declared:
        suite, job = split_batch_job(name)
        if not has_evidence(client, suite, job, head_sha):
            missing.append(f"- {name}: no successful `{job}` job on {head_sha}; run "
                           f"`{dispatch_command(suite, branch)}`")
    if missing:
        return False, [header, "Prove each declared fix on this exact head commit, then re-run the "
                       f"{GATE_JOB} check:\n" + "\n".join(missing)]
    remaining = [name for name in health.failed_jobs if name not in declared]
    lines = [header, "Fix proven on this head commit for:\n" + "\n".join(f"- {name}" for name in declared)]
    if remaining:
        lines.append("Still red after this fix:\n" + "\n".join(f"- {name}" for name in remaining))
    return True, lines


def rerun_gate(client, pull: dict) -> str:
    number = pull.get("number")
    head_sha = pull.get("head", {}).get("sha")
    listing = client.get(f"actions/workflows/{PULL_REQUEST_WORKFLOW}/runs?event=pull_request"
                         f"&head_sha={batch_ci.validate_sha(head_sha)}&per_page=20")
    runs = [run for run in listing.get("workflow_runs", []) if isinstance(run, dict)
            and isinstance(run.get("id"), int) and run.get("head_sha") == head_sha]
    if not runs:
        return f"#{number}: no {PULL_REQUEST_WORKFLOW} run for {head_sha}."
    run = max(runs, key=lambda run: run["id"])
    if run.get("status") != "completed":
        return f"#{number}: run {run['id']} is still in progress; re-run {GATE_JOB} after it completes."
    gates = [job for job in list_jobs(client, run["id"]) if job.get("name") == GATE_JOB]
    if not gates or not isinstance(gates[0].get("id"), int):
        return f"#{number}: run {run['id']} has no {GATE_JOB} job."
    client.post(f"actions/jobs/{gates[0]['id']}/rerun")
    return f"#{number}: re-running {GATE_JOB} (run {run['id']})."


def reevaluate(client, trigger_run_id: int | None) -> list[str]:
    """Re-run the gate on open develop PRs after a batch changes develop's health."""
    if trigger_run_id is not None:
        run = client.get(f"actions/runs/{int(trigger_run_id)}")
        if run.get("path") != f".github/workflows/{BATCH_WORKFLOW}" or run.get("head_branch") != "main":
            return [f"Run {trigger_run_id} is not a main batch run; nothing to re-evaluate."]
        current = classify_batch(run, list_jobs(client, int(trigger_run_id)))
        if current is None:
            return [f"Batch {trigger_run_id} validated nothing; develop health is unchanged."]
        previous = next((health for health in validated_batches(client)
                         if health.run_id is not None and health.run_id < trigger_run_id), UNKNOWN_HEALTH)
        if not current.is_red and not previous.is_red:
            return [f"Batch {trigger_run_id} passed and develop was already green; nothing to re-evaluate."]
    lines = []
    for page in range(1, MAX_PULL_REQUEST_PAGES + 1):
        pulls = client.get(f"pulls?state=open&base=develop&per_page=100&page={page}")
        if not isinstance(pulls, list):
            raise HealthError("The open pull request listing is unavailable.")
        for pull in pulls:
            if isinstance(pull, dict):
                try:
                    lines.append(rerun_gate(client, pull))
                except (HealthError, batch_ci.BatchError) as error:
                    lines.append(f"#{pull.get('number')}: {error}")
        if len(pulls) < 100:
            break
    return lines or ["No open develop pull requests."]


def write_summary(lines: list[str]) -> None:
    text = "\n\n".join(lines) + "\n"
    print(text, end="")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as stream:
            stream.write(f"### {GATE_JOB}\n\n{text}")


def run_gate(client) -> int:
    event_name = os.environ.get("GITHUB_EVENT_NAME", "")
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    pull_request = event.get("pull_request") if event_name == "pull_request" else None
    if not isinstance(pull_request, dict) or pull_request.get("base", {}).get("ref") != "develop":
        write_summary(["Not a develop pull request; the develop health gate does not apply."])
        return 0
    head_sha = batch_ci.validate_sha(pull_request.get("head", {}).get("sha"))
    # Read the description live: a re-run keeps its original event payload.
    pull = client.get(f"pulls/{int(pull_request['number'])}")
    required, _ = ci_scope.classify(event_name, event, Path.cwd())
    is_open, lines = evaluate(client, read_health(client), body=pull.get("body"), head_sha=head_sha,
                              branch=str(pull_request.get("head", {}).get("ref", "<branch>")),
                              is_documentation_only=not required,
                              waiver=os.environ.get("DEVELOP_HEALTH_WAIVER", ""))
    write_summary(lines)
    return 0 if is_open else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("gate", "reevaluate"))
    arguments = parser.parse_args()
    try:
        client = GitHub(os.environ["GITHUB_REPOSITORY"], os.environ["GITHUB_TOKEN"],
                        os.environ.get("GITHUB_API_URL", "https://api.github.com"))
        if arguments.command == "gate":
            return run_gate(client)
        trigger = os.environ.get("TRIGGER_RUN_ID", "")
        write_summary(reevaluate(client, int(trigger) if trigger else None))
        return 0
    except (HealthError, batch_ci.BatchError, KeyError, TypeError, ValueError, OSError) as error:
        print(f"Develop health: {error} Re-run this check.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
