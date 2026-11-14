"""Exercise the Kanban Actionable/Unlocks derivation from issue dependencies."""
from __future__ import annotations

import unittest

import kanban_dependencies as kanban


def issue(number, status="Backlog", priority="P2", is_open=True, blocked_by=(), blocking=(),
          actionable=None, unlocks=None):
    return kanban.Issue(
        item_id=f"item{number}", issue_id=f"issue{number}", number=number, title=f"Issue {number}",
        is_open=is_open, status=status, priority=priority, actionable=actionable, unlocks=unlocks,
        blocked_by=[(f"issue{other}", is_other_open) for other, is_other_open in blocked_by],
        blocking=[(f"issue{other}", is_other_open) for other, is_other_open in blocking],
    )


def chain():
    """1 -> 2 -> 3, and 1 -> 3 directly; 1 is In Validation."""
    return [
        issue(1, status="In Validation", blocking=[(2, True), (3, True)]),
        issue(2, blocked_by=[(1, True)], blocking=[(3, True)]),
        issue(3, blocked_by=[(1, True), (2, True)]),
    ]


class FakeGitHub:
    def __init__(self, pages):
        self.pages = list(pages)
        self.mutations = []

    def post(self, query, variables):
        if query.startswith("mutation"):
            self.mutations.append(query)
            return {}
        return self.pages.pop(0)


def project_page(nodes, has_next=False, fields=None):
    if fields is None:
        fields = [
            {"id": "status", "name": "Status", "dataType": "SINGLE_SELECT", "options": []},
            {"id": "act", "name": "Actionable", "dataType": "SINGLE_SELECT",
             "options": [{"id": f"o{index}", "name": name} for index, name in enumerate(kanban.ACTIONABLE_OPTIONS)]},
            {"id": "unl", "name": "Unlocks", "dataType": "NUMBER"},
        ]
    return {"organization": {"projectV2": {
        "id": "project", "fields": {"nodes": fields},
        "items": {"pageInfo": {"hasNextPage": has_next, "endCursor": "next"}, "nodes": nodes},
    }}}


def item_node(number, state="OPEN", blocked_by=(), blocking=(), status="Backlog", total=None):
    def connection(edges):
        nodes = [{"id": f"issue{other}", "state": other_state} for other, other_state in edges]
        return {"totalCount": len(nodes) if total is None else total, "nodes": nodes}
    return {
        "id": f"item{number}", "status": {"name": status}, "priority": {"name": "P2"},
        "actionable": None, "unlocks": None,
        "content": {"id": f"issue{number}", "number": number, "title": "t", "state": state,
                    "blockedBy": connection(blocked_by), "blocking": connection(blocking)},
    }


class DerivationTest(unittest.TestCase):
    def test_classifies_unblocked_waiting_and_blocked(self):
        derived = kanban.derive(chain())
        self.assertEqual(derived["item1"].actionable, kanban.ACTIONABLE)
        self.assertEqual(derived["item2"].actionable, kanban.WAITING_ON_VALIDATION)
        # 3 also waits on 2, which is not in validation.
        self.assertEqual(derived["item3"].actionable, kanban.BLOCKED)

    def test_closed_blockers_do_not_block(self):
        issues = [issue(1, blocked_by=[(9, False)])]
        self.assertEqual(kanban.derive(issues)["item1"].actionable, kanban.ACTIONABLE)

    def test_off_board_open_blocker_blocks(self):
        issues = [issue(1, blocked_by=[(9, True)])]
        self.assertEqual(kanban.derive(issues)["item1"].actionable, kanban.BLOCKED)

    def test_unlocks_counts_distinct_transitive_open_dependents(self):
        derived = kanban.derive(chain())
        self.assertEqual(derived["item1"].unlocks, 2)
        self.assertEqual(derived["item2"].unlocks, 1)
        self.assertEqual(derived["item3"].unlocks, 0)

    def test_unlocks_ignores_closed_dependents_and_counts_off_board_once(self):
        issues = [issue(1, blocking=[(2, False), (8, True), (8, True)])]
        self.assertEqual(kanban.derive(issues)["item1"].unlocks, 1)

    def test_unlocks_terminates_on_cycles(self):
        issues = [issue(1, blocking=[(2, True)], blocked_by=[(2, True)]),
                  issue(2, blocking=[(1, True)], blocked_by=[(1, True)])]
        derived = kanban.derive(issues)
        self.assertEqual(derived["item1"].unlocks, 1)
        self.assertEqual(derived["item1"].actionable, kanban.BLOCKED)

    def test_closed_issues_clear_both_fields(self):
        issues = [issue(1, is_open=False, actionable=kanban.ACTIONABLE, unlocks=3)]
        changes = kanban.plan_changes(issues, kanban.derive(issues))
        self.assertEqual({(change.field_name, change.value) for change in changes},
                         {(kanban.ACTIONABLE_FIELD, None), (kanban.UNLOCKS_FIELD, None)})

    def test_unchanged_values_are_not_rewritten(self):
        issues = [issue(1, actionable=kanban.ACTIONABLE, unlocks=0.0)]
        self.assertEqual(kanban.plan_changes(issues, kanban.derive(issues)), [])

    def test_next_up_orders_by_priority_then_unlocks(self):
        issues = [
            issue(10, priority="P2", blocking=[(11, True), (12, True)]),
            issue(11, priority="P2", blocked_by=[(10, True)]),
            issue(12, priority="P2", blocked_by=[(10, True)]),
            issue(20, priority="P1"),
            issue(30, priority=None, blocking=[(11, True)]),
            issue(40, status="In Progress", priority="P0"),
        ]
        ranked = kanban.rank_next(issues, kanban.derive(issues))
        self.assertEqual([entry.number for entry in ranked], [20, 10, 30])


class ProjectReadTest(unittest.TestCase):
    def test_reads_pages_and_skips_non_issue_items(self):
        client = FakeGitHub([
            project_page([item_node(1, blocking=[(2, "OPEN")]), {"id": "draft", "content": {}}], has_next=True),
            project_page([item_node(2, blocked_by=[(1, "OPEN")])]),
        ])
        project = kanban.read_project(client, "owner", 1)
        self.assertEqual([entry.number for entry in project.issues], [1, 2])
        self.assertEqual(project.option_ids[kanban.BLOCKED], "o2")

    def test_rejects_truncated_relationships(self):
        client = FakeGitHub([project_page([item_node(1, blocked_by=[(2, "OPEN")], total=500)])])
        with self.assertRaisesRegex(kanban.SyncError, "MAX_RELATIONSHIPS"):
            kanban.read_project(client, "owner", 1)

    def test_missing_field_names_the_repair(self):
        fields = [{"id": "status", "name": "Status", "dataType": "SINGLE_SELECT", "options": []}]
        client = FakeGitHub([project_page([], fields=fields)])
        with self.assertRaisesRegex(kanban.SyncError, "Create it"):
            kanban.read_project(client, "owner", 1)

    def test_sync_writes_batched_mutations_and_dry_run_writes_none(self):
        nodes = [item_node(number) for number in range(kanban.MUTATIONS_PER_REQUEST + 1)]
        dry = FakeGitHub([project_page(nodes)])
        changes = kanban.sync(dry, "owner", 1, is_dry_run=True)
        self.assertEqual(len(changes), 2 * len(nodes))
        self.assertEqual(dry.mutations, [])

        live = FakeGitHub([project_page(nodes)])
        kanban.sync(live, "owner", 1, is_dry_run=False)
        self.assertEqual(len(live.mutations), 3)
        self.assertIn('singleSelectOptionId: "o0"', live.mutations[0])
        self.assertIn("number: 0", live.mutations[0])


if __name__ == "__main__":
    unittest.main()
