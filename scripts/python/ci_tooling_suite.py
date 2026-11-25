#!/usr/bin/env python3
"""Run the CI initialization, qualification and analysis contract tests."""

import unittest


TEST_MODULES = (
    "test_ci",
    "test_batch_ci",
    "test_batch_outcome",
    "test_develop_health",
    "test_kanban_dependencies",
    "test_quality",
    "test_windows_qualification",
    "test_ray_reference",
    "test_runtime_profile",
    "test_pull_request_tests",
    "test_ctest_headroom",
    "test_ctest_shard",
    "test_ci_checks",
    # The host luslc that browser builds and their CI jobs lower LUSL shaders with.
    "test_host_luslc",
)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromNames(TEST_MODULES)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
