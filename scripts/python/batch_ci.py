#!/usr/bin/env python3
"""Capture a develop batch and promote its exact validated commit without rewriting history."""
from __future__ import annotations

import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.error
import urllib.request


ATTEMPT_BRANCH = "ci/last-batch-attempt"
VALIDATION_JOBS = ("native", "sdk", "wiki", "browser", "threading", "windows")
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

    A skipped capture leaves the whole run green, so it must not read as a promotion.
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
               "This run is green only because there was nothing new to validate.\n")
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("snapshot", "promote"))
    parser.add_argument("--retry", action="store_true")
    parser.add_argument("--base", default="")
    parser.add_argument("--candidate", default="")
    arguments = parser.parse_args()
    try:
        if os.environ.get("GITHUB_REF") != "refs/heads/main":
            raise BatchError("Batch control may run only from main.")
        client = GitHub(os.environ["GITHUB_REPOSITORY"], os.environ["GH_TOKEN"])
        if arguments.command == "snapshot":
            outputs = snapshot(client, Path.cwd(), os.environ["GITHUB_SHA"], retry=arguments.retry)
            with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as stream:
                for name, value in outputs.items():
                    if name != "reason":
                        stream.write(f"{name}={value}\n")
            annotation, summary = describe_snapshot(outputs)
            print(annotation or outputs["reason"])
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
