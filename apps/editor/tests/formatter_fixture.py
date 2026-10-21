#!/usr/bin/env python3
"""Bounded-process failure fixtures; no shell or project files are executed."""
import sys
import time

if sys.argv[1] == "--version":
    print("clang-format version " + sys.argv[0].rsplit("-", 1)[-1])
    sys.exit(0)

source = sys.stdin.buffer.read()
mode = sys.argv[1]
if mode in ("delayed", "timeout"):
    time.sleep(0.2 if mode == "delayed" else 30)
if mode == "failure":
    sys.stderr.write("fixture failure")
    sys.exit(3)
if mode == "transport":
    sys.stdout.write("x" * (8 * 1024 * 1024 + 1))
    sys.exit(0)
if mode == "malformed":
    print("<not-replacements/>")
    sys.exit(0)
if mode == "range":
    print('<replacements incomplete_format="false"><replacement offset="99999" length="1"> </replacement></replacements>')
    sys.exit(0)
if mode == "split":
    print('<replacements incomplete_format="false"><replacement offset="1" length="1"> </replacement></replacements>')
    sys.exit(0)
print('<replacements xml:space="preserve" incomplete_format="false"><replacement offset="3" length="0"> </replacement></replacements>')
