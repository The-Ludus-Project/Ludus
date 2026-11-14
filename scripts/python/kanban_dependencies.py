#!/usr/bin/env python3
"""Derive Ludus Kanban's Actionable and Unlocks fields from issue dependencies.

Priority says how much an outcome matters; GitHub's **Blocked by** relationship
says what must finish first. This tool reads both from the project and writes
two derived fields, so the board can show which cards can start now and how
much work each one opens:

- **Actionable**: ``Actionable`` when an open issue has no open blocker,
  ``Waiting on validation`` when every open blocker is on the board in
  **In Validation** (merged, awaiting batch evidence), otherwise ``Blocked``.
- **Unlocks**: the number of distinct open issues that transitively wait on
  this one.

Closed issues have both fields cleared. Pull requests and draft items are left
alone. Only values that differ are written. See
docs/wiki/contribute/index.md#prioritize-roadmap-work.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request


DEFAULT_OWNER = "The-Ludus-Project"
DEFAULT_PROJECT = 1
GRAPHQL_URL = "https://api.github.com/graphql"
ACTIONABLE_FIELD = "Actionable"
UNLOCKS_FIELD = "Unlocks"
STATUS_FIELD = "Status"
PRIORITY_FIELD = "Priority"
IN_VALIDATION = "In Validation"
ACTIONABLE = "Actionable"
WAITING_ON_VALIDATION = "Waiting on validation"
BLOCKED = "Blocked"
ACTIONABLE_OPTIONS = (ACTIONABLE, WAITING_ON_VALIDATION, BLOCKED)
# Cards eligible for the "next up" summary.
SCHEDULABLE_STATUSES = frozenset({"Backlog", "Ready"})
# Smaller pages keep the relationship-heavy query under GitHub's gateway timeout.
ITEMS_PAGE_SIZE = 50
MAX_ITEM_PAGES = 50
# GitHub returns at most this many relationship nodes per issue in one request;
# a larger list is reported instead of silently undercounting.
MAX_RELATIONSHIPS = 100
MUTATIONS_PER_REQUEST = 20
REQUEST_TIMEOUT_SECONDS = 60
# Bounded retries for transient gateway failures (HTTP 502/503/504, timeouts).
MAX_ATTEMPTS = 4
RETRY_BASE_SECONDS = 2
RETRYABLE_HTTP_STATUSES = frozenset({502, 503, 504})
SUMMARY_ROWS = 15
PRIORITY_RANK = {"P0": 0, "P1": 1, "P2": 2, "P3": 3}
UNRANKED_PRIORITY = len(PRIORITY_RANK)
TOKEN_VARIABLES = ("LUDUS_PROJECT_TOKEN", "GH_TOKEN", "GITHUB_TOKEN")


class SyncError(RuntimeError):
    """Project metadata is unavailable or inconsistent; nothing further is written."""


@dataclass
class Issue:
    """One open or closed issue card with its dependency edges (node ids)."""

    item_id: str
    issue_id: str
    number: int
    title: str
    is_open: bool
    status: str | None
    priority: str | None
    actionable: str | None
    unlocks: float | None
    blocked_by: list[tuple[str, bool]] = field(default_factory=list)
    blocking: list[tuple[str, bool]] = field(default_factory=list)


@dataclass(frozen=True)
class Derived:
    """Desired field values; ``None`` clears the field."""

    actionable: str | None
    unlocks: int | None


@dataclass(frozen=True)
class Change:
    item_id: str
    number: int
    field_name: str
    value: str | int | None


def classify(issue: Issue, issues_by_id: dict[str, Issue]) -> str:
    """Return the Actionable option for an open issue."""
    open_blockers = [identifier for identifier, is_open in issue.blocked_by if is_open]
    if len(open_blockers) == 0:
        return ACTIONABLE
    for identifier in open_blockers:
        blocker = issues_by_id.get(identifier)
        if blocker is None or blocker.status != IN_VALIDATION:
            return BLOCKED
    return WAITING_ON_VALIDATION


def count_unlocks(issue: Issue, issues_by_id: dict[str, Issue]) -> int:
    """Count distinct open issues that transitively wait on ``issue``.

    Off-board dependents count once but are not traversed further, because
    their own edges are not part of the project read.
    """
    reached: set[str] = set()
    pending = [issue]
    while len(pending) != 0:
        current = pending.pop()
        for identifier, is_open in current.blocking:
            if is_open == False or identifier in reached or identifier == issue.issue_id:
                continue
            reached.add(identifier)
            dependent = issues_by_id.get(identifier)
            if dependent is not None:
                pending.append(dependent)
    return len(reached)


def derive(issues: list[Issue]) -> dict[str, Derived]:
    """Compute desired values keyed by project item id."""
    issues_by_id = {issue.issue_id: issue for issue in issues}
    result: dict[str, Derived] = {}
    for issue in issues:
        if issue.is_open == False:
            result[issue.item_id] = Derived(None, None)
            continue
        result[issue.item_id] = Derived(classify(issue, issues_by_id), count_unlocks(issue, issues_by_id))
    return result


def plan_changes(issues: list[Issue], derived: dict[str, Derived]) -> list[Change]:
    """List only the field writes whose value differs from the board."""
    changes: list[Change] = []
    for issue in issues:
        wanted = derived[issue.item_id]
        if issue.actionable != wanted.actionable:
            changes.append(Change(issue.item_id, issue.number, ACTIONABLE_FIELD, wanted.actionable))
        current_unlocks = None if issue.unlocks is None else int(issue.unlocks)
        if current_unlocks != wanted.unlocks:
            changes.append(Change(issue.item_id, issue.number, UNLOCKS_FIELD, wanted.unlocks))
    return changes


def rank_next(issues: list[Issue], derived: dict[str, Derived]) -> list[Issue]:
    """Order startable Backlog/Ready cards by Priority, then Unlocks, then age."""
    candidates = [
        issue for issue in issues
        if issue.is_open == True and issue.status in SCHEDULABLE_STATUSES
        and derived[issue.item_id].actionable in (ACTIONABLE, WAITING_ON_VALIDATION)
    ]
    return sorted(candidates, key=lambda issue: (
        PRIORITY_RANK.get(issue.priority or "", UNRANKED_PRIORITY),
        -(derived[issue.item_id].unlocks or 0),
        issue.number,
    ))


ITEMS_QUERY = """
query($owner: String!, $number: Int!, $after: String) {
  organization(login: $owner) {
    projectV2(number: $number) {
      id
      fields(first: 50) {
        nodes {
          ... on ProjectV2FieldCommon { id name dataType }
          ... on ProjectV2SingleSelectField { options { id name } }
        }
      }
      items(first: %d, after: $after) {
        pageInfo { hasNextPage endCursor }
        nodes {
          id
          status: fieldValueByName(name: "%s") { ... on ProjectV2ItemFieldSingleSelectValue { name } }
          priority: fieldValueByName(name: "%s") { ... on ProjectV2ItemFieldSingleSelectValue { name } }
          actionable: fieldValueByName(name: "%s") { ... on ProjectV2ItemFieldSingleSelectValue { name } }
          unlocks: fieldValueByName(name: "%s") { ... on ProjectV2ItemFieldNumberValue { number } }
          content {
            ... on Issue {
              id number title state
              blockedBy(first: %d) { totalCount nodes { id state } }
              blocking(first: %d) { totalCount nodes { id state } }
            }
          }
        }
      }
    }
  }
}
""" % (ITEMS_PAGE_SIZE, STATUS_FIELD, PRIORITY_FIELD, ACTIONABLE_FIELD, UNLOCKS_FIELD,
       MAX_RELATIONSHIPS, MAX_RELATIONSHIPS)


class GitHub:
    """Minimal GraphQL client; ``post`` is replaced in tests."""

    def __init__(self, token: str):
        if not token:
            raise SyncError("A GitHub token is required.")
        self.token = token

    def post(self, query: str, variables: dict) -> dict:
        body = json.dumps({"query": query, "variables": variables}).encode()
        request = urllib.request.Request(GRAPHQL_URL, data=body, method="POST", headers={
            "Authorization": f"Bearer {self.token}",
            "Content-Type": "application/json",
            "User-Agent": "ludus-kanban-dependencies",
        })
        for attempt in range(1, MAX_ATTEMPTS + 1):
            try:
                with urllib.request.urlopen(request, timeout=REQUEST_TIMEOUT_SECONDS) as response:
                    payload = json.loads(response.read())
                break
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
                is_transient = isinstance(error, TimeoutError) or (
                    isinstance(error, urllib.error.HTTPError) and error.code in RETRYABLE_HTTP_STATUSES)
                if is_transient == False or attempt == MAX_ATTEMPTS:
                    raise SyncError(f"GitHub GraphQL request failed after {attempt} attempt(s): {error}. "
                                    "Re-run the sync; writes are idempotent.") from error
                time.sleep(RETRY_BASE_SECONDS * attempt)
        if payload.get("errors"):
            raise SyncError(f"GitHub GraphQL returned errors: {json.dumps(payload['errors'])[:2000]}")
        return payload["data"]


@dataclass
class Project:
    project_id: str
    field_ids: dict[str, str]
    option_ids: dict[str, str]
    issues: list[Issue]


def relationship(issue_number: int, name: str, connection: dict) -> list[tuple[str, bool]]:
    nodes = connection["nodes"]
    if connection["totalCount"] > len(nodes):
        raise SyncError(f"Issue #{issue_number} has {connection['totalCount']} {name} relationships; "
                        f"the tool reads at most {MAX_RELATIONSHIPS}. Raise MAX_RELATIONSHIPS "
                        "or paginate that connection before syncing.")
    return [(node["id"], node["state"] == "OPEN") for node in nodes]


def read_project(client: GitHub, owner: str, number: int) -> Project:
    project_id = None
    fields: list[dict] = []
    issues: list[Issue] = []
    after = None
    for _ in range(MAX_ITEM_PAGES):
        data = client.post(ITEMS_QUERY, {"owner": owner, "number": number, "after": after})
        project = (data.get("organization") or {}).get("projectV2")
        if project is None:
            raise SyncError(f"Project {owner}/{number} is not readable with this token.")
        project_id = project["id"]
        fields = [node for node in project["fields"]["nodes"] if node]
        for node in project["items"]["nodes"]:
            content = node.get("content") or {}
            if "number" not in content:
                continue
            value = lambda key, name: (node.get(key) or {}).get(name)
            issues.append(Issue(
                item_id=node["id"], issue_id=content["id"], number=content["number"],
                title=content["title"], is_open=content["state"] == "OPEN",
                status=value("status", "name"), priority=value("priority", "name"),
                actionable=value("actionable", "name"), unlocks=value("unlocks", "number"),
                blocked_by=relationship(content["number"], "blocked-by", content["blockedBy"]),
                blocking=relationship(content["number"], "blocking", content["blocking"]),
            ))
        page = project["items"]["pageInfo"]
        if page["hasNextPage"] == False:
            break
        after = page["endCursor"]
    else:
        raise SyncError(f"Project has more than {MAX_ITEM_PAGES * ITEMS_PAGE_SIZE} items; raise MAX_ITEM_PAGES.")

    field_ids = {node["name"]: node["id"] for node in fields}
    for required in (ACTIONABLE_FIELD, UNLOCKS_FIELD, STATUS_FIELD):
        if required not in field_ids:
            raise SyncError(f"Project field '{required}' is missing. Create it (Actionable: single select with "
                            f"{', '.join(ACTIONABLE_OPTIONS)}; Unlocks: number) and re-run.")
    actionable = next(node for node in fields if node["name"] == ACTIONABLE_FIELD)
    option_ids = {option["name"]: option["id"] for option in actionable.get("options", [])}
    missing = [name for name in ACTIONABLE_OPTIONS if name not in option_ids]
    if missing:
        raise SyncError(f"Field '{ACTIONABLE_FIELD}' lacks options {missing}; add them and re-run.")
    return Project(project_id, field_ids, option_ids, issues)


def mutation_text(alias: str, project: Project, change: Change) -> str:
    target = (f'projectId: "{project.project_id}", itemId: "{change.item_id}", '
              f'fieldId: "{project.field_ids[change.field_name]}"')
    if change.value is None:
        return f"{alias}: clearProjectV2ItemFieldValue(input: {{{target}}}) {{ clientMutationId }}"
    if change.field_name == ACTIONABLE_FIELD:
        value = f'{{singleSelectOptionId: "{project.option_ids[change.value]}"}}'
    else:
        value = f"{{number: {int(change.value)}}}"
    return f"{alias}: updateProjectV2ItemFieldValue(input: {{{target}, value: {value}}}) {{ clientMutationId }}"


def apply_changes(client: GitHub, project: Project, changes: list[Change]) -> None:
    for start in range(0, len(changes), MUTATIONS_PER_REQUEST):
        batch = changes[start:start + MUTATIONS_PER_REQUEST]
        body = "\n".join(mutation_text(f"m{index}", project, change) for index, change in enumerate(batch))
        client.post(f"mutation {{\n{body}\n}}", {})


def describe(change: Change) -> str:
    shown = "(cleared)" if change.value is None else change.value
    return f"#{change.number}: {change.field_name} -> {shown}"


def print_summary(issues: list[Issue], derived: dict[str, Derived]) -> None:
    open_issues = [issue for issue in issues if issue.is_open == True]
    counts = {name: 0 for name in ACTIONABLE_OPTIONS}
    for issue in open_issues:
        counts[derived[issue.item_id].actionable] += 1
    print("Open issues: " + ", ".join(f"{name} {count}" for name, count in counts.items()))
    print(f"Next up (Backlog/Ready, startable, by Priority then Unlocks), top {SUMMARY_ROWS}:")
    for issue in rank_next(issues, derived)[:SUMMARY_ROWS]:
        wanted = derived[issue.item_id]
        note = " [waiting on validation]" if wanted.actionable == WAITING_ON_VALIDATION else ""
        print(f"  #{issue.number} {issue.priority or '--'} unlocks {wanted.unlocks}: {issue.title}{note}")


def resolve_token() -> str:
    for name in TOKEN_VARIABLES:
        value = os.environ.get(name, "")
        if value:
            return value
    try:
        completed = subprocess.run(["gh", "auth", "token"], capture_output=True, text=True, check=False)
    except OSError:
        completed = None
    if completed is not None and completed.returncode == 0 and completed.stdout.strip():
        return completed.stdout.strip()
    raise SyncError("No GitHub token found. Set LUDUS_PROJECT_TOKEN (or GH_TOKEN) to a token with the "
                    "'project' scope, or sign in with 'gh auth login --scopes project'.")


def sync(client: GitHub, owner: str, number: int, is_dry_run: bool) -> list[Change]:
    project = read_project(client, owner, number)
    derived = derive(project.issues)
    changes = plan_changes(project.issues, derived)
    for change in changes:
        print(("would set " if is_dry_run else "set ") + describe(change))
    if is_dry_run == False:
        apply_changes(client, project, changes)
    print(f"{len(changes)} field value(s) {'differ' if is_dry_run else 'written'}.")
    print_summary(project.issues, derived)
    return changes


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["sync"])
    parser.add_argument("--owner", default=DEFAULT_OWNER)
    parser.add_argument("--project", type=int, default=DEFAULT_PROJECT)
    parser.add_argument("--dry-run", action="store_true", help="report differences without writing")
    options = parser.parse_args(arguments)
    try:
        sync(GitHub(resolve_token()), options.owner, options.project, options.dry_run)
    except SyncError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
