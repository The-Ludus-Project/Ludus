"""Check effective compiler policy, rather than the order claimed by CMake comments."""

import json
from pathlib import Path
import shlex
import subprocess
import sys


# When LUDUS_ENABLE_PCH is ON, CMake injects precompiled-header flags into every
# compile command. Replaying such a command for a policy/self-sufficiency probe
# must NOT touch the PCH: it would force-include the precompiled foundational
# header (defining assert_config.hpp macros -> breaks the override-rejection
# probe) or feed the binary .pch back as a source (CPCH parse errors / UTF-8
# decode failures). A PCH is a build accelerator only (ADR 0007), so these probes
# strip every PCH-related flag and observe identical results with or without it.
#
# The flavours CMake/Clang emit that must be removed, with their argument(s):
#   -include <cmake_pch.hxx>                     (force-include the PCH header)
#   -include-pch <path.pch|.gch>                 (consume a PCH directly)
#   -Xclang -emit-pch                            (generate a PCH; the PCH entry)
#   -Xclang -include-pch -Xclang <path.pch>      (consume via cc1)
#   -Xclang -include -Xclang <cmake_pch.hxx>     (force-include via cc1, Clang's editor PCH)
#   -Xclang <path.pch|.hxx>                      (cc1 PCH path argument)
#   -x c++-header                                (compile the .hxx as a header)
_PCH_FLAGS_WITH_ARG = ("-include", "-include-pch", "-include-pch-out")
_PCH_XCLANG_DIRECTIVES = ("-emit-pch", "-include-pch", "-include", "-building-pch-with-obj", "-fno-pch-timestamp")


def _looks_like_pch_path(token: str) -> bool:
    return token.endswith((".pch", ".gch")) or "cmake_pch" in token


def compiler_options(entry):
    arguments = entry.get("arguments") or shlex.split(entry["command"])
    result = []
    skip = False
    drop_next = False
    index = 0
    while index < len(arguments):
        arg = arguments[index]
        if skip:
            skip = False
        elif drop_next:
            # The path argument that followed a stripped force-include/PCH flag.
            drop_next = False
        elif arg in ("-o", "-c"):
            skip = True
        elif arg in _PCH_FLAGS_WITH_ARG:
            # Drop the flag and its following path argument (the PCH/header).
            drop_next = True
        elif arg == "-Xclang" and index + 1 < len(arguments) and (
            arguments[index + 1] in _PCH_XCLANG_DIRECTIVES or _looks_like_pch_path(arguments[index + 1])
        ):
            # Drop this "-Xclang <pch-directive-or-path>" pair. If the directive
            # is -include-pch or -include, the following "-Xclang <path>" pair is
            # dropped by the same rule on its own iteration.
            index += 2
            continue
        elif arg == "-x" and index + 1 < len(arguments) and arguments[index + 1] == "c++-header":
            # The PCH build compiles the generated .hxx as a header; the probe
            # supplies its own "-x c++" instead.
            index += 2
            continue
        # Profiling controls output artifacts, not compiler semantics. Clang
        # rejects -ftime-trace with -fsyntax-only under warnings-as-errors.
        # -fpch-* / -Winvalid-pch tune PCH handling and are meaningless (and can
        # warn) once the PCH flags are gone.
        elif not arg.startswith(("@", "-ftime-trace", "-fpch-", "-Winvalid-pch")):
            result.append(arg)
        index += 1
    return result


def is_pch_entry(entry) -> bool:
    """A compile_commands entry that builds a precompiled header, not a real TU.

    When LUDUS_ENABLE_PCH=ON, CMake adds an entry whose source is the generated
    cmake_pch.hxx (compiled to a .pch/.gch). Policy probes must skip it: it is
    not engine source, and it must never be re-fed as a source file.
    """
    source = entry.get("file", "")
    return source.endswith((".hxx", ".pch", ".gch")) or "cmake_pch" in source


def main():
    entries = json.loads(Path(sys.argv[1]).read_text())
    checked = 0
    for entry in entries:
        # Catch2 executables follow the *_tests target convention. Native
        # assertion child executables intentionally do not opt into exceptions.
        command = entry.get("command") or " ".join(entry["arguments"])
        if "CMakeFiles/ludus_" not in command:
            continue
        # Skip the precompiled-header build entry (LUDUS_ENABLE_PCH=ON).
        if is_pch_entry(entry):
            continue
        # The C++ exception policy applies to C++ translation units. The pinned
        # private miniaudio dependency (audio spec design section 2) is compiled
        # as C with its own language flags; re-forcing "-x c++" on a -std=gnu11
        # command is invalid and the no-exceptions contract does not apply to a
        # third-party C unit. Skip C sources.
        if entry.get("file", "").endswith((".c",)):
            continue
        expected = "_tests.dir/" in command
        result = subprocess.run(
            compiler_options(entry) + ["-x", "objective-c++" if entry["file"].endswith(".mm") else "c++", "-dM", "-E", "-"],
            input="", text=True, capture_output=True, cwd=entry["directory"], check=True,
        )
        actual = "#define __cpp_exceptions " in result.stdout
        if actual != expected:
            raise RuntimeError(f"Wrong exception policy: {entry['file']}: {actual}, expected {expected}")
        checked += 1
    if not checked:
        raise RuntimeError("No Ludus compilation commands checked")
    print(f"Verified effective exception policy for {checked} compilation commands")


if __name__ == "__main__":
    main()
