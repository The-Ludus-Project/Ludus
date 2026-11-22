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
    "macos-compile", "macos-reference", "macos-sanitizers", "macos-analysis", "formatting", "debug-runtime", "development",
    "sanitizers", "filesystem-fuzz", "threading-races", "analysis", "editor",
    "assertion-policy", "pch", "build-budget", "scripting-qualification",
)
FULL_TIER = "full"
INTEGRATION_TIER = "integration"
NIGHTLY_TIER = "nightly"
NIGHTLY_JOBS = ("macos-analysis",)
# Batches run main's ci.yml with this candidate script, and main wires a new job only after
# the change adding it is promoted. A job listed here may be absent from the results; its
# presence is still checked as usual. Remove an entry once main's ci.yml defines the job
# (test_ci_scope.PendingPromotionJobTests reports entries main already has).
PENDING_PROMOTION_JOBS: tuple[str, ...] = ()
TIERS = frozenset({FULL_TIER, INTEGRATION_TIER, NIGHTLY_TIER})
# Integration PRs check sources, compile the Editor configuration and run static
# analysis, so a build or analysis break cannot reach develop and fail every
# later batch. Tests, sanitizers and the other compiled variants run once per batch.
PULL_REQUEST_JOBS = ("formatting", "analysis", "editor")
INTEGRATION_JOBS = tuple(name for name in NATIVE_JOBS if name not in PULL_REQUEST_JOBS)
# Changed Objective-C++ is absent from the Linux compile database the analysis job reads,
# so an integration PR that changes a .mm source also runs this one macOS job; every
# other PR skips it (docs/development/continuous-integration.md#platform-only-sources).
OBJCXX_JOB = "macos-objcxx-analysis"
OBJCXX_SUFFIX = ".mm"
EDITOR_ROOT = "apps/editor/"
# Bounded GitHub run metadata read by a cancelled pull-request gate.
MAX_RUN_METADATA_BYTES = 2 * 1024 * 1024
GATE_JOB = "Ubuntu Clang"
# A Batch-Fix evidence dispatch runs one native job; "all" (or empty) runs every job.
ALL_EVIDENCE_JOBS = "all"


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


# Camera admission, view construction, capture, and the fixtures that exercise them.
CAMERA_PATHS = frozenset({
    "modules/graphics/renderer/include/ludus/graphics/renderer/render_graph.hpp",
    "modules/graphics/renderer/include/ludus/graphics/renderer/renderer.hpp",
    "modules/graphics/renderer/src/render_graph.cpp",
    "modules/graphics/renderer/src/render_graph_capture.cpp",
    "modules/graphics/renderer/src/renderer.cpp",
    "modules/graphics/renderer/src/views.cpp",
    "modules/graphics/renderer/tests/render_graph_tests.cpp",
    "apps/editor/tests/render_graph_preview_tests.cpp",
    "scripts/python/camera_pilot.py",
    "scripts/python/test_camera_pilot.py",
})
CAMERA_EDITOR_STEMS = frozenset({
    "render_graph_preview", "render_preview", "render_scene", "viewport_raster_capture",
})


def camera_pilot_matches(paths: list[str]) -> bool:
    for path in paths:
        if path in CAMERA_PATHS:
            return True
        if path.startswith("apps/editor/src/") and PurePosixPath(path).stem in CAMERA_EDITOR_STEMS:
            return True
    return False


def objcxx_changes(paths: list[str], root: Path) -> tuple[bool, bool]:
    """Whether the checkout holds a changed .mm source, and whether one is an Editor source.

    Deleted sources leave nothing to analyze, so only paths present in the checkout count.
    """
    present = [path for path in paths if PurePosixPath(path).suffix == OBJCXX_SUFFIX and (root / path).is_file()]
    return bool(present), any(path.startswith(EDITOR_ROOT) for path in present)


def classify_objcxx(event_name: str, event: dict, root: Path) -> tuple[bool, bool]:
    """Route the macOS Objective-C++ job; an unavailable comparison runs it rather than skip it."""
    if event_name != "pull_request":
        return False, False
    try:
        return objcxx_changes(changed_paths(event_name, event, root), root)
    except (KeyError, TypeError, ValueError, OSError, subprocess.SubprocessError):
        return True, True


def classify(event_name: str, event: dict, root: Path) -> tuple[bool, str, bool]:
    try:
        paths = changed_paths(event_name, event, root)
    except (KeyError, TypeError, ValueError, OSError, subprocess.SubprocessError):
        return True, "Comparison unavailable or event requires full validation.", event_name == "pull_request"
    if documentation_only(paths):
        return False, f"All {len(paths)} changed paths are documentation or documentation tooling.", False
    reason = "Engine, tooling, policy, unknown paths, or an empty diff require full validation."
    return True, reason, camera_pilot_matches(paths)


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


def gate_failures(results: dict, evidence_job: str = "") -> list[str]:
    """Compare job results with the classified scope.

    A non-empty evidence_job other than "all" names the one native job a scoped
    evidence dispatch runs; every other native job must then be skipped.
    """
    failures = []
    is_scoped = evidence_job not in ("", ALL_EVIDENCE_JOBS)
    if is_scoped and evidence_job not in NATIVE_JOBS:
        return [f"Unknown evidence job {evidence_job!r}; choose one of: {', '.join(NATIVE_JOBS)}."]
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
    objcxx = outputs.get("objcxx_changed")
    # Batches run main's workflow with this candidate script. A workflow from before the
    # Objective-C++ job wires neither its output nor its result; the batch analyzes .mm
    # sources in the macOS native job, so there is nothing more to check.
    has_objcxx_job = OBJCXX_JOB in results
    absent = {name for name in PENDING_PROMOTION_JOBS if name not in results}
    native_jobs = tuple(name for name in NATIVE_JOBS if name not in absent)
    if objcxx is None and has_objcxx_job == False:
        expected_jobs = {"changes", *native_jobs}
    elif objcxx not in {"true", "false"}:
        return ["Change classification did not report a valid objcxx_changed output."]
    else:
        expected_jobs = {"changes", *native_jobs, OBJCXX_JOB}
    if set(results) != expected_jobs:
        failures.append("Required native job inventory is incomplete or unexpected.")
    if objcxx is not None:
        # Batches and full-tier PRs analyze Objective-C++ in the macOS native jobs instead.
        expected = ("success" if required == "true" and tier == INTEGRATION_TIER and objcxx == "true"
                    and not is_scoped else "skipped")
        actual = results.get(OBJCXX_JOB, {}).get("result")
        if actual != expected:
            failures.append(f"{OBJCXX_JOB}: expected {expected}, received {actual}.")
    for name in native_jobs:
        deferred = ((tier == INTEGRATION_TIER and name in INTEGRATION_JOBS)
                    or (tier != NIGHTLY_TIER and name in NIGHTLY_JOBS))
        is_selected = not is_scoped or name == evidence_job
        if is_selected and is_scoped and (deferred or required != "true"):
            failures.append(f"Evidence job {name} does not run in the {tier} tier; dispatch with "
                            "validation_tier=full (nightly for macos-analysis).")
        expected = "success" if required == "true" and not deferred and is_selected else "skipped"
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


def run_gate(results: dict, evidence_job: str = "") -> int:
    failures = gate_failures(results, evidence_job)
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
        return run_gate(json.loads(os.environ["RESULTS"]), os.environ.get("EVIDENCE_JOB", ""))
    event_name = os.environ.get("GITHUB_EVENT_NAME", "")
    try:
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
        required, reason, camera = classify(event_name, event, Path.cwd())
        tier = validation_tier(event_name, event, is_batch=bool(os.environ.get("LUDUS_CI_BATCH_SHA")))
        objcxx, objcxx_editor = classify_objcxx(event_name, event, Path.cwd())
    except (OSError, ValueError, KeyError):
        required, reason, camera = True, "Event metadata unavailable; full validation required.", True
        tier = FULL_TIER
        objcxx, objcxx_editor = False, False
    # Only integration PRs run the macOS Objective-C++ job; the gate expects it skipped elsewhere.
    objcxx = objcxx and required and tier == INTEGRATION_TIER
    objcxx_editor = objcxx_editor and objcxx
    value = "true" if required else "false"
    with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
        output.write(f"camera_pilot={str(camera).lower()}\n")
        output.write(f"native_required={value}\n")
        output.write(f"validation_tier={tier}\n")
        output.write(f"objcxx_changed={str(objcxx).lower()}\n")
        output.write(f"objcxx_editor={str(objcxx_editor).lower()}\n")
    message = f"Native validation required: {value}. Validation tier: {tier}. {reason}"
    if tier == INTEGRATION_TIER:
        message += (" Camera pilot selected." if camera else
                    " Camera pilot outside scope (0 test runner-minutes).")
        message += (" Changed Objective-C++: macOS Objective-C++ analysis selected." if objcxx else
                    " No changed Objective-C++: macOS Objective-C++ analysis skipped (0 macOS runner-minutes).")
    print(message)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as summary:
            summary.write(message + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
