#!/usr/bin/env python3
"""Report a completed batch attempt from Actions metadata and trusted job logs."""
from __future__ import annotations

import argparse
from datetime import datetime
import html
import os
from pathlib import Path
import re
import sys
import urllib.request

from batch_ci import (BatchError, GitHub, SUITE_JOBS, STOP_JOB, failed_suite_jobs,
                      is_suite_job, list_run_jobs, parse_time)


CAPTURE = "Capture develop candidate"
WATCH = "Watch for failed suites"
PROMOTE = "Promote validated candidate"
PUBLISH = "Publish promoted documentation"
NO_VALIDATION = "Mark run as not validated"
CAPTURE_STEP = "Capture exact commit, or report why no batch runs"
PROMOTE_STEP = "Promote only the tested commit with an explicit main lease"
SHA = r"[0-9a-f]{40}"
FROZEN = re.compile(rf"Validate frozen develop commit ({SHA}) above main ({SHA})\.")
NO_CAPTURE_REASON = re.compile(
    rf"main advanced before this scheduler started; .+|develop has no unpromoted changes\."
    rf"|develop {SHA} was already attempted; .+"
)
UNKNOWN = "unknown"


def load_optional(read, *arguments, default=None):
    try:
        return read(*arguments)
    except (BatchError, OSError, ValueError) as error:
        print(f"Batch outcome telemetry unavailable: {error}", file=sys.stderr)
        return default


def read_job_log(client: GitHub, job_id: int) -> str:
    # GitHub redirects to a signed log URL. Auth applies only to the API request.
    # https://docs.github.com/en/rest/actions/workflow-jobs#download-job-logs-for-a-workflow-run
    request = urllib.request.Request(
        f"https://api.github.com/repos/{client.repository}/actions/jobs/{job_id}/logs",
        headers={"X-GitHub-Api-Version": "2022-11-28"},
    )
    request.add_unredirected_header("Authorization", f"Bearer {client.token}")
    limit = 2 * 1024 * 1024
    with urllib.request.urlopen(request, timeout=15) as response:
        payload = response.read(limit + 1)
    if len(payload) > limit:
        raise BatchError(f"Job {job_id} log exceeded the report's read limit.")
    return payload.decode("utf-8-sig", errors="replace")


def find_job(jobs: list[dict], name: str) -> dict:
    return next((job for job in jobs if job.get("name") == name), {})


def find_step(job: dict, name: str) -> dict:
    return next((step for step in (job.get("steps") or []) if step.get("name") == name), {})


def ran(job: dict) -> bool:
    return job.get("conclusion") not in {None, "skipped"} and bool(job.get("steps"))


def log_lines(log: str):
    for line in log.splitlines():
        timestamp, _, message = line.partition(" ")
        when = parse_time(timestamp)
        if when is not None:
            yield when, message


def capture_evidence(log: str) -> tuple[str | None, str | None, str | None]:
    for _, message in log_lines(log):
        frozen = FROZEN.fullmatch(message)
        if frozen:
            return *frozen.groups(), None
        if message.startswith("##[notice]"):
            reason = message.removeprefix("##[notice]").split(" Validation and promotion")[0]
            if NO_CAPTURE_REASON.fullmatch(reason):
                return None, None, reason
    return None, None, None


def markdown(value: object) -> str:
    clean = " ".join(str(value).split())
    return re.sub(r"([\\\[\]|`*_~])", r"\\\1", html.escape(clean))


def show_time(value: datetime | None) -> str:
    return value.isoformat(timespec="seconds") if value is not None else UNKNOWN


def elapsed(start: datetime | None, end: datetime | None) -> str:
    if start is None or end is None or end < start:
        return UNKNOWN
    seconds = int((end - start).total_seconds())
    hours, remainder = divmod(seconds, 3600)
    minutes, seconds = divmod(remainder, 60)
    return f"{hours}h {minutes:02}m {seconds:02}s" if hours else f"{minutes}m {seconds:02}s"


def launch_source(run: dict, attempt: int) -> str:
    if attempt > 1:
        return "rerun"
    if run.get("event") == "schedule":
        return "schedule"
    if run.get("event") == "workflow_dispatch":
        actor = run.get("actor") or {}
        if actor.get("login") == "github-actions[bot]":
            # Both retired automatic dispatchers used this same token. A stale
            # outcome cannot identify which dispatcher submitted the request.
            return "automatic dispatch (protected-develop wake or stale-request handoff unknown)"
        if actor.get("type") == "User":
            return "maintainer dispatch (user account)"
    return UNKNOWN


def describe_stop(jobs: list[dict], run_url: str, stop_at: datetime | None,
                  watch_log: str) -> list[str]:
    if stop_at is None:
        return ["Required jobs at cancellation: unknown (request timestamp unavailable)."]
    states: dict[str, list[str]] = {name: [] for name in ("Had a result", "Running", "Queued", "Unobserved")}
    queued = {int(match[1]) for _, message in log_lines(watch_log)
              if (match := re.fullmatch(r"Batch report observed queued job (\d+)", message))}
    seen = set()
    for job in jobs:
        name = job.get("name")
        if not is_suite_job(name) and name not in SUITE_JOBS:
            continue
        seen.add(name.split(" / ", 1)[0])
        started = parse_time(job.get("started_at"))
        finished = parse_time(job.get("completed_at"))
        if job.get("conclusion") and finished is not None and finished <= stop_at:
            bucket = "Had a result"
            label = f"{name} ({job['conclusion']})"
        elif (job.get("conclusion") != "skipped" and started is not None and finished is not None
              and started < stop_at < finished):
            bucket = "Running"
            label = name
        elif job.get("id") in queued:
            bucket = "Queued"
            label = name
        else:
            bucket = "Unobserved"
            label = name
        job_id = job.get("id")
        states[bucket].append(f"[{markdown(label)}]({run_url}/job/{job_id})"
                              if isinstance(job_id, int) else markdown(label))
    states["Unobserved"].extend(f"{name} suite" for name in SUITE_JOBS if name not in seen)
    lines = ["**Required workflow jobs at the request**",
             "Results and execution use Actions timestamps. Queued uses the last watcher poll. "
             "Unobserved includes suites with no job record and optional jobs whose eligibility is unknown."]
    for state, names in states.items():
        if state == "Had a result" and names:
            lines.extend(["", "<details>", f"<summary>{state} ({len(names)})</summary>", ""])
            lines.extend(f"- {name}" for name in names)
            lines.extend(["", "</details>"])
        else:
            lines.extend(["", f"**{state} ({len(names)})**"])
            lines.extend(f"- {name}" for name in names)
    return lines


def render_report(repository: str, run: dict, jobs: list[dict], logs: dict[int, str],
                  committed_at: str | None, artifacts: list[dict],
                  inherited_capture: dict, inherited_promotion: dict) -> str:
    run_id, attempt = run["id"], run["run_attempt"]
    run_url = f"https://github.com/{repository}/actions/runs/{run_id}"
    attempt_url = f"{run_url}/attempts/{attempt}"
    capture = find_job(jobs, CAPTURE) or inherited_capture
    capture_step = find_step(capture, CAPTURE_STEP)
    candidate, base, skip_reason = capture_evidence(logs.get(capture.get("id"), ""))
    suites = [job for job in jobs if is_suite_job(job.get("name"))]
    promotion = find_job(jobs, PROMOTE)
    publish = find_job(jobs, PUBLISH)
    stop = find_job(jobs, STOP_JOB)
    watch = find_job(jobs, WATCH)
    watch_log = logs.get(watch.get("id"), "")
    failures = failed_suite_jobs(suites)
    dated_failures = [(when, job) for job in failures if (when := parse_time(job.get("completed_at"))) is not None]
    # An undated failure could have been first. Keep it visible without ranking it.
    signal_at, first_failure = (min(dated_failures, key=lambda entry: entry[0])
                                if dated_failures and len(dated_failures) == len(failures) else (None, {}))
    cancel_at = next((when for when, message in log_lines(logs.get(stop.get("id"), ""))
                      if message.removeprefix("✓ ") == f"Request to cancel workflow {run_id} submitted."), None)
    first_id = first_failure.get("id")
    observed_at = next((parse_time(message.rsplit(" at ", 1)[-1]) for _, message in log_lines(watch_log)
                        if message.startswith(f"Batch report observed failure {first_id} at ")), None)
    confirmed_at = next((when for when, message in log_lines(watch_log)
                         if message == f"- {first_failure.get('name')}"), None)

    no_batch = bool(skip_reason or ran(find_job(jobs, NO_VALIDATION)))
    promoted = promotion.get("conclusion") == "success"
    cancelled = run.get("conclusion") == "cancelled"
    if promoted:
        outcome, reason = "Promoted", "The trusted promotion job succeeded."
    elif no_batch:
        outcome = "Stale no-op" if (skip_reason or "").startswith("main advanced") else "No validation"
        reason = skip_reason or "The coordinator marked the attempt as unvalidated."
    elif failures:
        outcome = "Rejected"
        reason = ("A required job failed and the coordinator requested an early stop." if cancel_at else
                  "A required job failed. The cancellation request is unknown." if cancelled else
                  "A required job failed.")
    elif inherited_promotion and ran(publish) and not ran(promotion):
        outcome = "Previously promoted"
        reason = f"This attempt retried publication after promotion in attempt {inherited_promotion['run_attempt']}."
    elif cancelled:
        outcome, reason = "Cancelled", "No coordinator rejection or stale decision was established."
    elif ran(promotion):
        outcome, reason = "Promotion incomplete", "The trusted promotion job did not succeed."
    else:
        outcome, reason = "No promotion established", "The available metadata does not establish an outcome."

    captured_at = (parse_time(capture_step.get("completed_at"))
                   if not no_batch and capture_step.get("conclusion") == "success"
                   and (candidate or suites or promoted) else None)
    promotion_step = find_step(promotion, PROMOTE_STEP)
    promoted_at = parse_time(promotion_step.get("completed_at")) if promoted else None
    submitted_at = parse_time(run.get("created_at")) if attempt == 1 else None
    capture_attempt = capture.get("run_attempt")
    publication = publish.get("conclusion") or UNKNOWN
    if publication == UNKNOWN and no_batch:
        publication = "not run"
    lines = [
        "# Develop-to-main batch outcome",
        f"[Coordinator run {run_id}, attempt {attempt}]({attempt_url})",
        "",
        f"- **{outcome}**. {markdown(reason)}",
        f"- GitHub event: {markdown(run.get('event') or UNKNOWN)}. Trigger: {launch_source(run, attempt)}.",
        f"- Capture: {show_time(captured_at)}"
        + (f" (inherited from attempt {capture_attempt})" if capture_attempt and capture_attempt != attempt else ""),
        f"- Candidate: `{candidate}`" if candidate else
        ("- Candidate: none captured" if no_batch else "- Candidate: unknown"),
        f"- Base: `{base}`" if base else "- Base: unknown",
        f"- Promotion step completion: {show_time(promoted_at)}.",
        f"- Documentation publication: {markdown(publication)}.",
        "",
        "| Measure | Elapsed |",
        "| --- | --- |",
        f"| Submission to capture | {elapsed(submitted_at, captured_at)} |",
        f"| Candidate age at capture (committer date) | {elapsed(parse_time(committed_at), captured_at)} |",
        f"| Capture to promotion | {elapsed(captured_at, promoted_at)} |",
        f"| Capture to first required failure | {elapsed(captured_at, signal_at)} |",
        f"| Diagnostic grace (first failure to cancellation request) | {elapsed(signal_at, cancel_at)} |",
    ]

    if failures:
        lines.extend(["", "## Rejection evidence"])
        if first_failure and isinstance(first_id, int):
            lines.append(f"1. First failed required job: [{markdown(first_failure['name'])}]"
                         f"({run_url}/job/{first_id}) ({markdown(first_failure['conclusion'])}). "
                         f"Actions job completion: {show_time(signal_at)}.")
        else:
            lines.append("1. First failed required job and completion: unknown. "
                         + ", ".join(markdown(job.get("name")) for job in failures) + ".")
        observation = f"2. Watcher first observed this failure: {show_time(observed_at)}."
        if observed_at is None and confirmed_at is not None:
            observation += f" The watcher log confirms it had seen the failure by {show_time(confirmed_at)}."
        lines.append(observation)
        lines.append(f"3. GitHub CLI confirmed submission of the cancellation request: {show_time(cancel_at)}.")
        if cancelled or cancel_at is not None:
            lines.extend(["", *describe_stop(jobs, run_url, cancel_at, watch_log)])
        diagnostics = [item for item in artifacts if item.get("expired") is False
                       and isinstance(item.get("id"), int)
                       and re.search(r"diagnostic|evidence|sanitizer", str(item.get("name", "")), re.I)
                       and (artifact_at := parse_time(item.get("created_at"))) is not None
                       and (cancel_at is None or artifact_at <= cancel_at)]
        links = [f"[{markdown(item['name'])}]({run_url}/artifacts/{item['id']})" for item in diagnostics[:3]]
        lines.extend(["", f"Surviving run-level diagnostics: {', '.join(links) or UNKNOWN}. "
                      f"[All run artifacts]({run_url}#artifacts)."])

    if promoted:
        lines.extend(["", "## Validation jobs and promotion wait"])
        started = parse_time(promotion.get("started_at"))
        completed = [(when, job) for job in suites if job.get("conclusion") == "success"
                     and (when := parse_time(job.get("completed_at"))) is not None
                     and (started is None or when <= started)]
        latest = sorted(completed, key=lambda item: item[0], reverse=True)[:3]
        durations = [(when - job_started, when, job) for when, job in completed
                     if (job_started := parse_time(job.get("started_at"))) is not None and job_started <= when]
        longest = [(when, job) for _, when, job in sorted(durations, key=lambda item: item[0], reverse=True)[:2]]
        lines.append("Last three completions and two longest recorded job runtimes:")
        for when, job in [*latest, *(entry for entry in longest if entry not in latest)]:
            lines.append(f"- [{markdown(job['name'])}]({run_url}/job/{job['id']}) finished at {show_time(when)} "
                         f"after running for {elapsed(parse_time(job.get('started_at')), when)}.")
        gate = latest[0][0] if latest and attempt == 1 else None
        lines.append(f"- Last recorded successful validation job to promotion job start: {elapsed(gate, started)}.")
        lines.append(f"- Promotion job start to promotion step completion: {elapsed(started, promoted_at)}.")
    return "\n".join(lines) + "\n"


def collect_report(client: GitHub, run_id: int, attempt: int) -> str:
    run_path = f"actions/runs/{run_id}"
    run = client.request(f"{run_path}/attempts/{attempt}")
    if (run.get("id") != run_id or run.get("run_attempt") != attempt
            or run.get("status") != "completed" or run.get("head_branch") != "main"
            or run.get("path") != ".github/workflows/batch-ci.yml"
            or (run.get("head_repository") or {}).get("full_name") != client.repository):
        raise BatchError("Expected a completed main coordinator attempt in this repository.")

    def read_jobs(number):
        return list_run_jobs(client, run_id, attempt=number)

    jobs = load_optional(read_jobs, attempt, default=[])
    # Keep inherited jobs from any API response out of this attempt's outcome.
    jobs = [job for job in jobs if job.get("run_attempt", attempt) == attempt]
    # A partial rerun can reuse a successful capture. Never read older stop
    # evidence as if it happened on this attempt, or inherit into a zero-job run.
    prior_capture, prior_promotion = {}, {}
    if not find_job(jobs, CAPTURE) and any(ran(job) for job in jobs
                                          if is_suite_job(job.get("name")) or job.get("name") in {PROMOTE, PUBLISH}):
        for earlier in range(attempt - 1, max(0, attempt - 6), -1):
            earlier_jobs = [job for job in load_optional(read_jobs, earlier, default=[])
                            if job.get("run_attempt", earlier) == earlier]
            promotion = find_job(earlier_jobs, PROMOTE)
            if not prior_promotion and promotion.get("conclusion") == "success":
                prior_promotion = promotion
            capture = find_job(earlier_jobs, CAPTURE)
            if capture:
                if capture.get("conclusion") == "success":
                    prior_capture = capture
                break
    trusted_jobs = [find_job(jobs, name) for name in (CAPTURE, WATCH, STOP_JOB)] + [prior_capture]
    logs = {job["id"]: load_optional(read_job_log, client, job["id"], default="")
            for job in trusted_jobs if ran(job) and isinstance(job.get("id"), int)}
    capture = find_job(jobs, CAPTURE) or prior_capture
    candidate, _, _ = capture_evidence(logs.get(capture.get("id"), ""))
    commit = load_optional(client.request, f"commits/{candidate}", default={}) if candidate else {}
    committed_at = (commit.get("commit", {}).get("committer") or {}).get("date")
    artifacts = []
    if failed_suite_jobs(jobs):
        for page in range(1, 6):
            listing = load_optional(client.request, f"{run_path}/artifacts?per_page=100&page={page}", default={})
            entries = listing.get("artifacts") or []
            artifacts.extend(entries)
            if len(entries) < 100:
                break
    return render_report(client.repository, run, jobs, logs, committed_at, artifacts, prior_capture, prior_promotion)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=int)
    parser.add_argument("attempt", type=int)
    arguments = parser.parse_args()
    try:
        if arguments.run < 1 or arguments.attempt < 1:
            raise BatchError("Run and attempt must be positive.")
        client = GitHub(os.environ["GITHUB_REPOSITORY"], os.environ["GH_TOKEN"])
        print(collect_report(client, arguments.run, arguments.attempt), end="")
        return 0
    except (BatchError, KeyError, TypeError, ValueError, OSError) as error:
        print(f"Batch outcome report unavailable: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
