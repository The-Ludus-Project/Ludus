"""Compile target detection with real Clang target triples; no SDK/linker needed.

The Apple fixture supplies only SDK macro values. It does not claim to validate
an Apple SDK or an engine backend. Negative cases must fail for their intended
diagnostic, rather than because a cross-target standard library is missing.
"""

from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import check_target_macros as target_macros  # noqa: E402


def main() -> int:
    compiler, root = sys.argv[1], Path(sys.argv[2]).resolve()
    base = root / "modules/foundation/base/include"
    platform = root / "modules/platform/include"
    failures = []
    count = 0

    with tempfile.TemporaryDirectory(prefix="ludus-target-") as directory:
        fixture = Path(directory)
        (fixture / "TargetConditionals.h").write_text(
            "#pragma once\n"
            "#ifndef TARGET_OS_OSX\n#define TARGET_OS_OSX __is_target_os(macos)\n#endif\n"
            "#ifndef TARGET_OS_IOS\n#define TARGET_OS_IOS __is_target_os(ios)\n#endif\n"
            "#ifndef TARGET_OS_MAC\n#define TARGET_OS_MAC 1\n#endif\n",
            encoding="utf-8",
        )

        def check(name, target, source, flags=(), diagnostic=None):
            nonlocal count
            count += 1
            command = [
                compiler, f"--target={target}", "-std=c++23", "-fno-exceptions",
                "-Wall", "-Wextra", "-Wpedantic", "-Wundef", "-Werror",
                "-nostdinc++", "-fsyntax-only", "-x", "c++", "-",
                f"-I{base}", f"-I{platform}", f"-I{fixture}", *flags,
            ]
            result = subprocess.run(command, input=source, text=True, capture_output=True, timeout=15)
            if diagnostic is None:
                ok = result.returncode == 0
            else:
                ok = result.returncode != 0 and diagnostic in result.stderr
            if not ok:
                failures.append(f"{name}:\n{result.stdout}{result.stderr}")

        cases = [
            ("x86_64-linux-gnu", "LINUX", "X86_64", 64, "LITTLE", True),
            ("aarch64-linux-gnu", "LINUX", "ARM64", 64, "LITTLE", True),
            ("aarch64_be-linux-gnu", "LINUX", "ARM64", 64, "BIG", True),
            ("x86_64-linux-gnux32", "LINUX", "X86_64", 32, "LITTLE", True),
            ("x86_64-pc-windows-msvc", "WINDOWS", "X86_64", 64, "LITTLE", True),
            ("aarch64-pc-windows-msvc", "WINDOWS", "ARM64", 64, "LITTLE", True),
            ("x86_64-w64-windows-gnu", "WINDOWS", "X86_64", 64, "LITTLE", True),
            ("x86_64-apple-macosx14.0", "MACOS", "X86_64", 64, "LITTLE", True),
            ("arm64-apple-macosx14.0", "MACOS", "ARM64", 64, "LITTLE", True),
            ("arm64-apple-ios17.0", "IOS", "ARM64", 64, "LITTLE", False),
            ("x86_64-apple-ios17.0-simulator", "IOS", "X86_64", 64, "LITTLE", False),
            ("arm64-apple-ios17.0-macabi", "IOS", "ARM64", 64, "LITTLE", False),
            ("aarch64-linux-android", "ANDROID", "ARM64", 64, "LITTLE", False),
            ("x86_64-linux-android", "ANDROID", "X86_64", 64, "LITTLE", False),
            ("wasm32-unknown-emscripten", "WEB", "WASM32", 32, "LITTLE", False),
            ("wasm64-unknown-emscripten", "WEB", "WASM64", 64, "LITTLE", False),
        ]
        header = "#include <ludus/foundation/base/compiler.h>\n#include <ludus/platform/config.h>\n"
        for target, os_name, arch, bits, endian, desktop in cases:
            source = header + (
                f"static_assert(LUDUS_TARGET_OS == LUDUS_OS_{os_name});\n"
                f"static_assert(LUDUS_TARGET_ARCH == LUDUS_CPU_{arch});\n"
                f"static_assert(LUDUS_POINTER_BITS == {bits});\n"
                "static_assert(LUDUS_POINTER_BITS == sizeof(void*) * 8);\n"
                f"static_assert(LUDUS_TARGET_ENDIAN == LUDUS_ENDIAN_{endian});\n"
                "static_assert(LUDUS_TARGET_COMPILER == LUDUS_CXX_CLANG);\n"
                "LUDUS_INLINE constexpr int InlineProbe() noexcept { return 7; }\n"
                "static_assert(InlineProbe() == 7);\n"
                "LUDUS_NOINLINE LUDUS_COLD void ColdProbe() noexcept {}\n"
                "void TrapProbe() noexcept { LUDUS_DEBUG_BREAK(); }\n"
                "int BranchProbe(int value) noexcept {\n"
                "if (LUDUS_LIKELY(value > 0)) { return value; }\n"
                "if (LUDUS_UNLIKELY(value < 0)) { return -value; }\n"
                "LUDUS_UNREACHABLE(); }\n"
                "#if !LUDUS_HAS_BUILTIN(__builtin_trap) || !LUDUS_HAS_CPP_ATTRIBUTE(nodiscard)\n"
                '#error "expected Clang feature queries"\n#endif\n'
            )
            for candidate in ("WEB", "WINDOWS", "MACOS", "LINUX", "ANDROID", "IOS", "DESKTOP"):
                present = candidate == os_name or (candidate == "DESKTOP" and desktop)
                source += f"#if {'!' if present else ''}defined(LUDUS_PLATFORM_{candidate})\n"
                source += f'#error "wrong legacy OS flag {candidate}"\n#endif\n'
            for candidate in ("WASM32", "WASM64", "X86_64", "ARM64"):
                source += f"#if {'!' if candidate == arch else ''}defined(LUDUS_ARCH_{candidate})\n"
                source += f'#error "wrong legacy architecture flag {candidate}"\n#endif\n'
            if "msvc" in target:
                source += '#if !defined(LUDUS_COMPILER_MSVC_ABI) || defined(LUDUS_COMPILER_MSVC)\n#error "clang-cl identity"\n#endif\n'
            else:
                source += '#if defined(LUDUS_COMPILER_MSVC_ABI)\n#error "unexpected Microsoft ABI"\n#endif\n'
            check(target, target, source)

        linux = "x86_64-linux-gnu"
        for flavor in ("DEBUG", "DEVELOPMENT", "PROFILE", "RELEASE"):
            check(flavor, linux, header + f"#ifndef LUDUS_IS_{flavor}_BUILD\n#error \"missing flavor\"\n#endif\n",
                  [f"-DLUDUS_BUILD_{flavor}=1"])
        for flags, flavor in (([], "DEVELOPMENT"), (["-DDEBUG=1"], "DEBUG"), (["-D_DEBUG=1"], "DEBUG"),
                              (["-DNDEBUG=1"], "RELEASE"), (["-DNDEBUG=1", "-DLUDUS_BUILD_PROFILE=1"], "PROFILE")):
            check(f"fallback-{flags}", linux,
                  header + f"#ifndef LUDUS_IS_{flavor}_BUILD\n#error \"wrong fallback\"\n#endif\n", flags)

        for target, diagnostic in (
            ("x86_64-unknown-freebsd", "unsupported operating system"),
            ("wasm32-wasip1", "unsupported operating system"),
            ("i686-linux-gnu", "unsupported CPU architecture"),
            ("arm64-apple-tvos17.0", "unsupported Apple operating system"),
            ("arm64-apple-watchos10.0", "unsupported Apple operating system"),
            ("arm64ec-pc-windows-msvc", "ARM64EC is not supported"),
        ):
            check(target, target, header, diagnostic=diagnostic)

        for macro in ("LUDUS_PLATFORM_LINUX", "LUDUS_PLATFORM_WINDOWS", "LUDUS_PLATFORM_DESKTOP",
                      "LUDUS_TARGET_OS", "LUDUS_TARGET_ARCH", "LUDUS_ARCH_ARM64", "LUDUS_POINTER_BITS",
                      "LUDUS_TARGET_ENDIAN", "LUDUS_TARGET_COMPILER", "LUDUS_COMPILER_MSVC_ABI"):
            check(macro, linux, header, [f"-D{macro}=1"], "outputs must not be overridden")

        check("conflicting-flavors", linux, header,
              ["-DLUDUS_BUILD_DEBUG=1", "-DLUDUS_BUILD_RELEASE=1"], "select exactly one build flavor")
        for flavor in ("DEBUG", "DEVELOPMENT", "PROFILE", "RELEASE"):
            check(f"zero-{flavor}", linux, header, [f"-DLUDUS_BUILD_{flavor}=0"], "selected build flavor must equal 1")
        check("backend-conflict", linux, header,
              ["-DLUDUS_PLATFORM_WAYLAND=1", "-DLUDUS_PLATFORM_HEADLESS=1"], "select at most one windowing backend")
        check("wrong-browser", linux, header, ["-DLUDUS_PLATFORM_BROWSER=1"], "browser backend requires Emscripten")
        check("wrong-cocoa", linux, header, ["-DLUDUS_PLATFORM_COCOA=1"], "Cocoa backend requires macOS")
        check("cocoa-conflict", "arm64-apple-macosx14.0", header,
              ["-DLUDUS_PLATFORM_COCOA=1", "-DLUDUS_PLATFORM_HEADLESS=1"], "select at most one windowing backend")
        check("backend-cocoa", "arm64-apple-macosx14.0", header, ["-DLUDUS_PLATFORM_COCOA=1"])
        check("wrong-wayland", "aarch64-linux-android", header, ["-DLUDUS_PLATFORM_WAYLAND=1"],
              "Wayland/X11 backend requires desktop Linux")
        check("wrong-headless", "wasm32-unknown-emscripten", header, ["-DLUDUS_PLATFORM_HEADLESS=1"],
              "native headless backend cannot target Emscripten")
        for backend in ("WAYLAND", "X11", "HEADLESS"):
            check(f"backend-{backend}", linux, header, [f"-DLUDUS_PLATFORM_{backend}=1"])
        check("backend-browser", "wasm32-unknown-emscripten", header, ["-DLUDUS_PLATFORM_BROWSER=1"])
        for backend in ("WAYLAND", "X11", "HEADLESS", "BROWSER", "COCOA"):
            check(f"zero-backend-{backend}", linux, header, [f"-DLUDUS_PLATFORM_{backend}=0"],
                  "selected backend must equal 1")
        check("sdk-target", linux, header, ["-DLUDUS_EXPECTED_TARGET_OS=LUDUS_OS_LINUX"])
        check("sdk-target-web", "wasm32-unknown-emscripten", header, ["-DLUDUS_EXPECTED_TARGET_OS=LUDUS_OS_WEB"])
        check("sdk-target-conflict", linux, header, ["-DLUDUS_EXPECTED_TARGET_OS=LUDUS_OS_WEB"],
              "compiler target disagrees with the configured SDK target")
        check("sanitizer-marker", linux, header, ["-DLUDUS_BUILD_SANITIZED=1"])
        check("zero-sanitizer-marker", linux, header, ["-DLUDUS_BUILD_SANITIZED=0"], "sanitizer marker must equal 1")
        check("unknown-pointer-width", linux, header, ["-U__SIZEOF_POINTER__"], "unknown pointer width")
        check("unsupported-pointer-width", linux, header, ["-U__SIZEOF_POINTER__", "-D__SIZEOF_POINTER__=16"],
              "unsupported pointer width")
        check("unknown-byte-order", linux, header, ["-U__BYTE_ORDER__"], "unknown byte order")
        check("unsupported-byte-order", linux, header, ["-U__BYTE_ORDER__", "-D__BYTE_ORDER__=0"],
              "unsupported byte order")
        check("invalid-web-architecture", linux, header, ["-D__EMSCRIPTEN__=1"],
              "Emscripten requires a WebAssembly architecture")
        check("web-before-linux", "wasm32-unknown-emscripten", header,
              ["-D__linux__=1", "-DLUDUS_EXPECTED_TARGET_OS=LUDUS_OS_WEB"])

        # Frontend dispatch only: these synthetic cases cannot validate MSVC/GCC
        # code generation. Real Clang target/extension compilation is tested above.
        for name, flags, expected in (
            ("gcc-compatible", ["-U__clang__"], "GCC"),
            ("msvc-compatible", ["-U__clang__", "-U__GNUC__", "-D_MSC_VER=1938"], "MSVC"),
        ):
            check(name, linux, header + f"static_assert(LUDUS_TARGET_COMPILER == LUDUS_CXX_{expected});\n", flags)
        check("unknown-compiler", linux, header, ["-U__clang__", "-U__GNUC__"], "unsupported compiler frontend")

    # Target/frontend classification belongs to Band 0, including in new code. The
    # compiler-free scan also runs in the pull-request Source formatting job.
    failures.extend(target_macros.violations(root))

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"Target detection: {count} positive/negative compile cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
