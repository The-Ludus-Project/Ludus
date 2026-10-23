"""Tests for the build slots that share one machine between concurrent builds."""
from __future__ import annotations

import multiprocessing
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build_slots
import engine

SMALL_LAPTOP = build_slots.Machine(cores=10, memory_gibibytes=16)
LARGE_WORKSTATION = build_slots.Machine(cores=32, memory_gibibytes=128)
MEMORY_BOUND_SERVER = build_slots.Machine(cores=64, memory_gibibytes=32)
UNKNOWN_MEMORY = build_slots.Machine(cores=8, memory_gibibytes=0)
PRESET = "linux-clang-development"


def hold_in_child(directory: str, ready, release) -> None:
    with build_slots.hold_slot(Path(directory), build_slots.AGENT_ROLE, 1, "child build", report=lambda _: None):
        ready.set()
        release.wait()


class CheckoutTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def make_primary(self) -> Path:
        (self.root / ".git").mkdir()
        return self.root

    def make_worktree(self) -> Path:
        (self.root / ".git").write_text("gitdir: /repository/.git/worktrees/feature\n", encoding="utf-8")
        return self.root


class PolicyTests(CheckoutTests):
    def test_primary_checkout_builds_interactively(self):
        policy = build_slots.policy_for(self.make_primary(), {}, SMALL_LAPTOP)
        self.assertEqual(policy, build_slots.BuildPolicy(role=build_slots.INTERACTIVE_ROLE, slots=1, jobs=None,
                                                         priority=build_slots.NORMAL_PRIORITY))

    def test_linked_worktree_builds_as_an_agent(self):
        policy = build_slots.policy_for(self.make_worktree(), {}, SMALL_LAPTOP)
        self.assertEqual(policy.role, build_slots.AGENT_ROLE)
        self.assertEqual(policy.slots, 1)
        self.assertEqual(policy.jobs, 8)
        self.assertEqual(policy.priority, build_slots.LOW_PRIORITY)

    def test_defaults_follow_cores_and_memory(self):
        self.assertEqual(build_slots.default_agent_slots(LARGE_WORKSTATION), 4)
        self.assertEqual(build_slots.default_agent_jobs(LARGE_WORKSTATION), 32)
        self.assertEqual(build_slots.default_agent_slots(MEMORY_BOUND_SERVER), 2)
        self.assertEqual(build_slots.default_agent_jobs(MEMORY_BOUND_SERVER), 16)
        self.assertEqual(build_slots.default_agent_slots(UNKNOWN_MEMORY), 1)
        self.assertEqual(build_slots.default_agent_jobs(UNKNOWN_MEMORY), 8)
        self.assertEqual(build_slots.default_agent_slots(build_slots.Machine(cores=2, memory_gibibytes=1)), 1)
        self.assertEqual(build_slots.default_agent_jobs(build_slots.Machine(cores=2, memory_gibibytes=1)), 1)

    def test_environment_overrides(self):
        environment = {build_slots.ROLE_ENVIRONMENT: "Interactive", build_slots.SLOTS_ENVIRONMENT: "0",
                       build_slots.JOBS_ENVIRONMENT: "3", build_slots.PRIORITY_ENVIRONMENT: "background"}
        policy = build_slots.policy_for(self.make_worktree(), environment, SMALL_LAPTOP)
        self.assertEqual(policy, build_slots.BuildPolicy(role=build_slots.INTERACTIVE_ROLE, slots=0, jobs=3,
                                                         priority=build_slots.BACKGROUND_PRIORITY))
        policy = build_slots.policy_for(self.make_worktree(), {build_slots.SLOTS_ENVIRONMENT: "2"}, SMALL_LAPTOP)
        self.assertEqual(policy.slots, 2)

    def test_agent_role_from_the_environment_in_a_primary_checkout(self):
        policy = build_slots.policy_for(self.make_primary(), {build_slots.ROLE_ENVIRONMENT: "agent"}, SMALL_LAPTOP)
        self.assertEqual(policy.role, build_slots.AGENT_ROLE)
        self.assertEqual(policy.priority, build_slots.LOW_PRIORITY)

    def test_nested_build_does_not_wait_or_lower_priority_again(self):
        environment = {build_slots.HELD_ENVIRONMENT: "1"}
        policy = build_slots.policy_for(self.make_worktree(), environment, SMALL_LAPTOP)
        self.assertEqual(policy.slots, 0)
        self.assertEqual(policy.priority, build_slots.NORMAL_PRIORITY)

    def test_invalid_settings_are_reported(self):
        root = self.make_worktree()
        for name, value in ((build_slots.SLOTS_ENVIRONMENT, "-1"), (build_slots.SLOTS_ENVIRONMENT, "many"),
                            (build_slots.JOBS_ENVIRONMENT, "0"), (build_slots.ROLE_ENVIRONMENT, "robot"),
                            (build_slots.PRIORITY_ENVIRONMENT, "urgent")):
            with self.subTest(name=name, value=value), self.assertRaises(build_slots.BuildSlotError):
                build_slots.policy_for(root, {name: value}, SMALL_LAPTOP)


class CommandTests(unittest.TestCase):
    def which(self, name):
        return f"/usr/bin/{name}"

    def test_priority_prefix_per_platform(self):
        self.assertEqual(build_slots.priority_prefix(build_slots.NORMAL_PRIORITY, "Darwin", self.which), [])
        self.assertEqual(build_slots.priority_prefix(build_slots.LOW_PRIORITY, "Darwin", self.which),
                         ["/usr/bin/taskpolicy", "-c", "utility"])
        self.assertEqual(build_slots.priority_prefix(build_slots.BACKGROUND_PRIORITY, "Darwin", self.which),
                         ["/usr/bin/taskpolicy", "-b"])
        self.assertEqual(build_slots.priority_prefix(build_slots.LOW_PRIORITY, "Linux", self.which),
                         ["/usr/bin/ionice", "-c", "2", "-n", "7", "/usr/bin/nice", "-n", "10"])
        self.assertEqual(build_slots.priority_prefix(build_slots.BACKGROUND_PRIORITY, "Linux", self.which),
                         ["/usr/bin/ionice", "-c", "3", "/usr/bin/nice", "-n", "19"])

    def test_priority_prefix_without_tools(self):
        self.assertEqual(build_slots.priority_prefix(build_slots.LOW_PRIORITY, "Darwin", lambda _: None), [])
        self.assertEqual(build_slots.priority_prefix(build_slots.LOW_PRIORITY, "Windows", self.which),
                         ["/usr/bin/nice", "-n", "10"])

    def test_parallel_arguments_respect_the_callers_choice(self):
        policy = build_slots.BuildPolicy(role=build_slots.AGENT_ROLE, slots=1, jobs=6,
                                         priority=build_slots.LOW_PRIORITY)
        self.assertEqual(build_slots.parallel_arguments(policy, ["--target", "ludus_editor"], {}), ["--parallel", "6"])
        for chosen in (["-j", "2"], ["-j2"], ["--parallel", "2"], ["--parallel=2"]):
            self.assertEqual(build_slots.parallel_arguments(policy, chosen, {}), [])
        self.assertEqual(build_slots.parallel_arguments(policy, [], {build_slots.PARALLEL_LEVEL_ENVIRONMENT: "4"}), [])
        interactive = build_slots.BuildPolicy(role=build_slots.INTERACTIVE_ROLE, slots=1, jobs=None,
                                              priority=build_slots.NORMAL_PRIORITY)
        self.assertEqual(build_slots.parallel_arguments(interactive, [], {}), [])

    def test_slot_directory_is_per_user(self):
        self.assertEqual(build_slots.slot_directory({"HOME": "/Users/a"}, "Darwin"),
                         Path("/Users/a/Library/Caches/Ludus/build-slots"))
        self.assertEqual(build_slots.slot_directory({"HOME": "/home/a"}, "Linux"), Path("/home/a/.cache/ludus/build-slots"))
        self.assertEqual(build_slots.slot_directory({"HOME": "/home/a", "XDG_CACHE_HOME": "/cache"}, "Linux"),
                         Path("/cache/ludus/build-slots"))
        self.assertEqual(build_slots.slot_directory({build_slots.DIRECTORY_ENVIRONMENT: "/slots"}, "Linux"),
                         Path("/slots"))


@unittest.skipUnless(os.name == "posix", "build slots use POSIX file locks")
class SlotTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.slots = Path(self.directory.name) / "slots"

    def test_free_slot_is_taken_without_waiting(self):
        messages = []
        with build_slots.hold_slot(self.slots, build_slots.AGENT_ROLE, 2, "first", report=messages.append) as first:
            with build_slots.hold_slot(self.slots, build_slots.AGENT_ROLE, 2, "second",
                                       report=messages.append) as second:
                self.assertEqual({first, second}, {0, 1})
                self.assertEqual((self.slots / "agent-0.lock").read_text(encoding="utf-8").strip(), "first")
        self.assertEqual(messages, [])
        self.assertEqual((self.slots / "agent-0.lock").read_text(encoding="utf-8"), "")

    def test_pools_are_separate(self):
        with build_slots.hold_slot(self.slots, build_slots.AGENT_ROLE, 1, "agent", report=self.fail):
            with build_slots.hold_slot(self.slots, build_slots.INTERACTIVE_ROLE, 1, "primary",
                                       report=self.fail) as slot:
                self.assertEqual(slot, 0)

    def test_disabled_pool_does_not_lock(self):
        with build_slots.hold_slot(self.slots, build_slots.AGENT_ROLE, 0, "unlimited", report=self.fail) as slot:
            self.assertIsNone(slot)
        self.assertFalse(self.slots.exists())

    def test_full_pool_waits_and_reports_the_holder(self):
        context = multiprocessing.get_context("spawn")
        ready = context.Event()
        release = context.Event()
        child = context.Process(target=hold_in_child, args=(str(self.slots), ready, release))
        child.start()
        self.addCleanup(child.join)
        self.addCleanup(release.set)
        self.assertTrue(ready.wait(timeout=10))
        messages = []
        sleeps = []
        times = iter([0.0, 0.0, 1.0, 61.0, 62.0, 62.0])

        def sleep(seconds):
            sleeps.append(seconds)
            if len(sleeps) == 3:
                release.set()
                child.join(timeout=10)

        with build_slots.hold_slot(self.slots, build_slots.AGENT_ROLE, 1, "waiting build", report=messages.append,
                                   sleep=sleep, clock=lambda: next(times)) as slot:
            self.assertEqual(slot, 0)
        self.assertEqual(len(sleeps), 3)
        self.assertEqual(len(messages), 3)
        self.assertIn("child build", messages[0])
        self.assertIn(build_slots.SLOTS_ENVIRONMENT, messages[0])
        self.assertIn("child build", messages[1])
        self.assertIn("after waiting 1 min 2 s", messages[2])

    def test_machine_share_marks_nested_builds(self):
        environment = {build_slots.DIRECTORY_ENVIRONMENT: str(self.slots)}
        policy = build_slots.BuildPolicy(role=build_slots.AGENT_ROLE, slots=1, jobs=4,
                                         priority=build_slots.LOW_PRIORITY)
        messages = []
        with build_slots.machine_share(Path("/checkout"), PRESET, policy, environment, report=messages.append):
            self.assertEqual(environment[build_slots.HELD_ENVIRONMENT], "1")
            nested = build_slots.policy_for(Path(self.directory.name), environment, SMALL_LAPTOP)
            self.assertEqual(nested.slots, 0)
        self.assertNotIn(build_slots.HELD_ENVIRONMENT, environment)
        self.assertEqual(len(messages), 1)
        self.assertIn("agent build: 4 jobs, low priority", messages[0])


class EngineTests(CheckoutTests):
    def test_scripted_build_runs_with_the_policy(self):
        root = self.make_worktree()
        runs = []
        environment = {build_slots.DIRECTORY_ENVIRONMENT: str(root / "slots")}
        with patch.dict("os.environ", environment), \
                patch.object(build_slots, "detect_machine", return_value=SMALL_LAPTOP), \
                patch.object(build_slots, "priority_prefix", return_value=["/usr/bin/nice", "-n", "10"]), \
                patch.object(engine, "run", side_effect=lambda args, **_: runs.append([str(arg) for arg in args])), \
                patch.object(engine, "cmake", return_value=Path("cmake")), \
                patch("builtins.print"):
            os.environ.pop(build_slots.PARALLEL_LEVEL_ENVIRONMENT, None)
            engine.cmake_build(root, PRESET, ["--target", "ludus_editor"])
        self.assertEqual(runs, [["/usr/bin/nice", "-n", "10", "cmake", "--build", "--preset", PRESET,
                                 "--parallel", "8", "--target", "ludus_editor"]])

    def test_invalid_setting_is_an_engine_error(self):
        with patch.dict("os.environ", {build_slots.JOBS_ENVIRONMENT: "none"}), \
                self.assertRaises(engine.EngineError):
            engine.cmake_build(self.make_primary(), PRESET)


if __name__ == "__main__":
    unittest.main()
