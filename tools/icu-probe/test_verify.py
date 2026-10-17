"""Classify sanitizer output from allocation-failure sweep points without running ICU."""
import contextlib
import io
from pathlib import Path
import runpy
import subprocess
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
VERIFY = runpy.run_path(str(ROOT / "tools/icu-probe/verify.py"))
CLASSIFY = VERIFY["classify_sanitizer"]
CLASSIFY_CHECK = VERIFY["classify_sanitizer_check"]

LOCATION = "/work/out/icu-probe/source/common/locid.cpp:1234:56: "
UBSAN_SUMMARY = "SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /work/locid.cpp:1234:56 in \n"
LEAK = ("\n=================================================================\n"
        "==4242==ERROR: LeakSanitizer: detected memory leaks\n\n"
        "Direct leak of 64 byte(s) in 1 object(s) allocated from:\n"
        "SUMMARY: AddressSanitizer: 64 byte(s) leaked in 1 allocation(s).\n")


def ubsan(message: str) -> str:
    return "L0 SWEEP-START\n" + LOCATION + "runtime error: " + message + "\n" + UBSAN_SUMMARY


def asan(kind: str) -> str:
    return ("=================================================================\n"
            f"==4242==ERROR: AddressSanitizer: {kind} on address 0x602000000010 at pc 0x55d0 bp 0x7ffc sp 0x7ff0\n"
            f"SUMMARY: AddressSanitizer: {kind} /work/locid.cpp:12 in icu_78::Locale::init\n")


class SanitizerCheckTests(unittest.TestCase):
    def test_ubsan_checks_drop_operand_types_values_and_addresses(self):
        cases = {
            "reference binding to null pointer of type 'const icu_78::Locale'":
                "reference binding to null pointer",
            "member access within null pointer of type 'icu_78::UnicodeString'":
                "member access within null pointer",
            "member call on null pointer of type 'icu_78::Hashtable'": "member call on null pointer",
            "load of null pointer of type 'const char'": "load of null pointer",
            "null pointer passed as argument 1, which is declared to never be null":
                "null pointer passed as argument N",
            "applying non-zero offset 8 to null pointer": "applying non-zero offset N to null pointer",
            "signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'":
                "signed integer overflow",
            "load of misaligned address 0x602000000011 for type 'int', which requires 4 byte alignment":
                "load of misaligned address N",
            "index 5 out of bounds for type 'int[4]'": "index N out of bounds",
            "member access within address 0x602000000010 which does not point to an object of type 'Base'":
                "member access within address N",
            "call to function icu_78::cleanup(void) through pointer to incorrect function type 'void (*)(int)'":
                "call through pointer to incorrect function type",
        }
        for message, check in cases.items():
            with self.subTest(message=message):
                output = ubsan(message)
                self.assertEqual(CLASSIFY(output), "undefined-behavior")
                self.assertEqual(CLASSIFY_CHECK(output), check)

    def test_only_the_first_runtime_error_names_the_check(self):
        output = ubsan("load of null pointer of type 'const char'") + ubsan("member call on null pointer of type 'X'")
        self.assertEqual(CLASSIFY_CHECK(output), "load of null pointer")

    def test_a_printed_ubsan_check_name_is_preferred(self):
        output = (LOCATION + "runtime error: reference binding to null pointer of type 'const icu_78::Locale'\n"
                  "SUMMARY: UndefinedBehaviorSanitizer: null-pointer-use /work/locid.cpp:1234:56 in \n")
        self.assertEqual(CLASSIFY_CHECK(output), "null-pointer-use")

    def test_unparsed_ubsan_reports_fall_back_to_undefined_behavior(self):
        for output in (LOCATION + "runtime error: \n", LOCATION + "runtime error: 'quoted'\n"):
            with self.subTest(output=output):
                self.assertEqual(CLASSIFY(output), "undefined-behavior")
                self.assertEqual(CLASSIFY_CHECK(output), "undefined-behavior")

    def test_asan_kind_is_the_check_and_deadly_signals_stay_crashes(self):
        for kind in ("heap-use-after-free", "heap-buffer-overflow", "double-free"):
            with self.subTest(kind=kind):
                self.assertEqual(CLASSIFY(asan(kind)), kind)
                self.assertEqual(CLASSIFY_CHECK(asan(kind)), kind)
        for signal in ("SEGV", "stack-overflow"):
            with self.subTest(signal=signal):
                self.assertIsNone(CLASSIFY(asan(signal)))
                self.assertIsNone(CLASSIFY_CHECK(asan(signal)))
        self.assertIsNone(CLASSIFY_CHECK("L0 SWEEP denied=1 correct=true\n" + LEAK))


class RunPointTests(unittest.TestCase):
    def run_point(self, output: str, returncode: int) -> dict:
        completed = subprocess.CompletedProcess(["probe"], returncode, stdout=output)
        with patch("subprocess.run", return_value=completed):
            return VERIFY["run_point"](["probe"], 0, 7)

    def test_sanitizer_report_points_keep_the_kind_and_record_the_check(self):
        result = self.run_point(ubsan("reference binding to null pointer of type 'const icu_78::Locale'"), 1)
        self.assertEqual(result, {"fail_at": 7, "outcome": "sanitizer-report", "exit": 1,
                                  "sanitizer": "undefined-behavior",
                                  "sanitizer-check": "reference binding to null pointer"})
        self.assertEqual(self.run_point(asan("heap-use-after-free"), 1)["sanitizer-check"], "heap-use-after-free")

    def test_leak_without_a_result_is_a_leak_check_and_crashes_have_no_check(self):
        self.assertEqual(self.run_point(LEAK, 23)["sanitizer-check"], "leak")
        crash = self.run_point(asan("SEGV"), 1)
        self.assertEqual(crash["outcome"], "crash")
        self.assertNotIn("sanitizer-check", crash)


class SweepSummaryTests(unittest.TestCase):
    def test_summary_keeps_report_kinds_beside_their_checks(self):
        control = {"fail_at": 4, "outcome": "control", "correct": "true", "leaked-blocks": "0",
                   "sanitizer-leak": False}
        points = {1: {"fail_at": 1, "outcome": "sanitizer-report", "exit": 1, "sanitizer": "undefined-behavior",
                      "sanitizer-check": "reference binding to null pointer"},
                  2: {"fail_at": 2, "outcome": "sanitizer-report", "exit": 1, "sanitizer": "heap-use-after-free",
                      "sanitizer-check": "heap-use-after-free"},
                  3: {"fail_at": 3, "outcome": "crash", "exit": -11, "sanitizer": None}}
        sweeps = VERIFY["SWEEPS"]
        namespace = VERIFY["sweep"].__globals__
        with patch.dict(namespace, run_point=lambda command, index, fail_at: points.get(fail_at, control)), \
                contextlib.redirect_stdout(io.StringIO()):
            report = VERIFY["sweep"](["probe"], {name: 3 for name in sweeps})
        summary = report[sweeps[0]]
        self.assertEqual(summary["sanitizer-report"], 2)
        self.assertEqual(summary["sanitizer_report_points"], {"1": "undefined-behavior", "2": "heap-use-after-free"})
        self.assertEqual(summary["sanitizer_report_checks"],
                         {"1": "reference binding to null pointer", "2": "heap-use-after-free"})
        self.assertEqual(summary["crash_points"], [3])


if __name__ == "__main__":
    unittest.main()
