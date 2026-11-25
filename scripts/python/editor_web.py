"""Pinned Qt source build, existing editor compilation, and Pages packaging."""
from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

import host_luslc
import managed_venv
from web_package import EDITOR_MAXIMUM_MEMORY_PAGES, SDK_NOTICES, PORT_NOTICES, validate_wasm

ROOT = Path(__file__).resolve().parents[2]
PAYLOAD = ("index.html", "editor.css", "editor.js", "ludus-icon.png", "ludus_editor.js", "ludus_editor.wasm", "qtloader.js")
PLAYERS = ("cornell-box", "live-edit-game", "scripted-game")
# One notice per pinned syntax grammar (ADR 0026), copied from third_party/<name>/LICENSE.
TREE_SITTER_GRAMMAR_NOTICES = tuple(f"tree-sitter-{language}.txt" for language in
                                    ("c", "cpp", "slang", "luau", "json", "cmake", "markdown"))
PLAYER_PAYLOAD = tuple(f"players/{sample}/{name}" for sample in PLAYERS
                       for name in ("index.html", "player.js", "game.js", "game.wasm"))
QT_SOURCE_CHANGES_NOTICE = "Qt-source-changes.diff"
# Ludus's changes to the pinned Qt source, applied to the extracted archive before Qt is
# built. Each replaces one exact, unique excerpt; the shipped notice carries their diff.
QT_SOURCE_CHANGES = (
    {
        # Backport of qtbase commit 3a217d2592, "wasm: Make suspendresumecontrol event handling
        # reentrant", by Even Oscar Andersen (The Qt Company), first released in Qt 6.11.0. Thanks
        # to its author and reviewer. Qt 6.10.3 sends a snapshot count of the native event queue, but
        # QEventDispatcherWasm::onWakeup() runs a nested processEvents() that can send the rest of
        # that snapshot first. The outer loop then shifts undefined from the empty queue, and the
        # JavaScript TypeError unwinds Qt's event loop: the editor stops processing events. Events
        # queue behind each host-folder Asyncify suspension (browser/host_folder.js), so a
        # project opened from disk makes such batches common. The 6.10.3 `return count;` that
        # follows is kept and now returns the number of events sent, as Qt 6.11 does.
        # https://code.qt.io/cgit/qt/qtbase.git/commit/?id=3a217d2592
        "path": "src/corelib/platform/wasm/qwasmsuspendresumecontrol.cpp",
        "original": "    int count = pendingEvents[\"length\"].as<int>();\n"
                    "    if (count == 0)\n"
                    "        return false;\n"
                    "    while (count-- > 0) {\n"
                    "        // Grab one event (handler and arg), and call it\n"
                    "        emscripten::val event = pendingEvents.call<val>(\"shift\");\n"
                    "        auto it = m_eventHandlers.find(event[\"index\"].as<int>());\n"
                    "        Q_ASSERT(it != m_eventHandlers.end());\n"
                    "        it->second(event[\"arg\"]);\n"
                    "    }\n",
        "replacement": "    if (pendingEvents[\"length\"].as<int>() == 0)\n"
                       "        return 0;\n"
                       "\n"
                       "    int count = 0;\n"
                       "    while (pendingEvents[\"length\"].as<int>() > 0) { // Make sure it is reentrant\n"
                       "        // Grab one event (handler and arg), and call it\n"
                       "        emscripten::val event = pendingEvents.call<val>(\"shift\");\n"
                       "        auto it = m_eventHandlers.find(event[\"index\"].as<int>());\n"
                       "        Q_ASSERT(it != m_eventHandlers.end());\n"
                       "        it->second(event[\"arg\"]);\n"
                       "        ++count;\n"
                       "    }\n",
    },
    {
        # Ludus change. A browser event that waited in the queue above while the instance was
        # suspended is sent after its dispatch ended, when DOM Event.composedPath() returns an
        # empty array. Qt 6.10.3 then reads className from undefined, and that JavaScript
        # TypeError also unwinds Qt's event loop. Such an event is no longer a pointer-capture
        # target, so it reports false, as Qt 6.11's comparison of composedPath()[0] with the
        # window element does (qtbase 01d48cd89c).
        # https://dom.spec.whatwg.org/#dom-event-composedpath
        "path": "src/plugins/platforms/wasm/qwasmevent.cpp",
        "original": "    emscripten::val path = webEvent.call<emscripten::val>(\"composedPath\");\n"
                    "    QString topElementClassName = QString::fromEcmaString(path[0][\"className\"]);\n",
        "replacement": "    emscripten::val path = webEvent.call<emscripten::val>(\"composedPath\");\n"
                       "    // Ludus: an event sent after its dispatch ended has an empty path.\n"
                       "    if (path[\"length\"].as<int>() == 0)\n"
                       "        return false;\n"
                       "    QString topElementClassName = QString::fromEcmaString(path[0][\"className\"]);\n",
    },
)


def run(argv, *, env=None, cwd=ROOT):
    print("+ " + " ".join(map(str, argv)), flush=True)
    subprocess.run(list(map(str, argv)), cwd=cwd, env=env, check=True)


def download(url: str, path: Path, expected: str):
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        path.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(url, path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise ValueError("Source archive checksum mismatch; extraction refused: " + path.name)


def extract(archive: Path, destination: Path):
    # Python's data filter rejects escaping symlinks, absolute paths and devices.
    # Thanks to Python Software Foundation, "tarfile — Read and write tar archive
    # files", Extraction filters: source archives remain inside ignored output.
    # https://docs.python.org/3/library/tarfile.html#extraction-filters
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as source:
        source.extractall(destination, filter="data")


def get_qt_source_changes_digest() -> str:
    """Identity of QT_SOURCE_CHANGES for the Qt build stamp."""
    return hashlib.sha256(json.dumps(QT_SOURCE_CHANGES, sort_keys=True).encode()).hexdigest()


def apply_qt_source_changes(source: Path) -> None:
    """Apply QT_SOURCE_CHANGES to an extracted Qt source; an applied change is left as it is."""
    for change in QT_SOURCE_CHANGES:
        path = source / change["path"]
        text = path.read_bytes()
        original = change["original"].encode()
        replacement = change["replacement"].encode()
        if text.count(replacement) == 1 and original not in text:
            continue
        if text.count(original) != 1:
            raise ValueError(f"Qt source {path} matches neither the pinned Qt nor Ludus's change. Delete "
                             f"{source} and rerun scripts/build-web-editor --bootstrap to restore the pinned source.")
        path.write_bytes(text.replace(original, replacement))


def describe_qt_source_changes(source: Path) -> str:
    """Unified diff from the pinned Qt archive to the changed source that is built and shipped."""
    lines = []
    for change in QT_SOURCE_CHANGES:
        changed = (source / change["path"]).read_text(encoding="utf-8")
        if changed.count(change["replacement"]) != 1:
            raise ValueError("Qt source does not contain Ludus's change: " + change["path"])
        pinned = changed.replace(change["replacement"], change["original"])
        lines.extend(difflib.unified_diff(pinned.splitlines(keepends=True), changed.splitlines(keepends=True),
                                          "a/" + change["path"], "b/" + change["path"]))
    return "".join(lines)


def prepare_sdk(lock, tools: Path):
    sdk = ROOT / "out/host-tools/emsdk"
    if not (sdk / "emsdk").is_file():
        if sdk.exists():
            raise ValueError("Incomplete SDK directory: " + str(sdk))
        archive = tools / "emsdk.tar.gz"
        download(lock["emsdk_archive"], archive, lock["emsdk_archive_sha256"])
        extracted = tools / "emsdk-source"
        extract(archive, extracted)
        shutil.move(str(extracted / ("emsdk-" + lock["emscripten"])), sdk)
    run([sdk / "emsdk", "install", lock["emscripten"]])
    run([sdk / "emsdk", "activate", lock["emscripten"]])


def sdk_environment(sdk: Path, lock: dict) -> dict:
    result = subprocess.run(["bash", "-c", 'source "$1" >/dev/null 2>&1 && env -0', "--", sdk / "emsdk_env.sh"],
                            check=True, capture_output=True)
    env = dict(os.fsdecode(item).split("=", 1) for item in result.stdout.split(b"\0") if item)
    version = subprocess.check_output([sdk / "upstream/emscripten/em++", "--version"], env=env, text=True)
    if not re.search(rf"\b{re.escape(lock['emscripten'])}\b", version):
        raise ValueError("The editor requires Ludus's pinned Emscripten compiler.")
    port = (sdk / "upstream/emscripten/tools/ports/emdawnwebgpu.py").read_text()
    if f"_VERSION = '{lock['emdawnwebgpu']}'" not in port or f"SHA512 = '{lock['emdawnwebgpu_sha512']}'" not in port:
        raise ValueError("Browser SDK WebGPU pin differs from config/web_toolchain.json.")
    return env


def qt_build(cmake, ninja, source, build, prefix, host, env):
    # Thanks to Qt Group, "Qt for WebAssembly", Building Qt from Source: source
    # builds permit the engine's pinned SDK, while binary Qt requires its SDK ABI.
    # Static, exception-free, single-threaded Qt plus Asyncify supports the current
    # workspace's modal dialogs without requiring cross-origin isolation headers.
    # https://doc.qt.io/qt-6.10/wasm.html
    argv = [cmake, "-S", source, "-B", build, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}",
            f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'out/host-tools/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake'}",
            f"-DQT_HOST_PATH={host}", f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_BUILD_TYPE=MinSizeRel",
            "-DBUILD_SHARED_LIBS=OFF", "-DQT_BUILD_TESTS=OFF", "-DQT_BUILD_EXAMPLES=OFF",
            "-DFEATURE_thread=OFF", "-DFEATURE_exceptions=OFF", "-DFEATURE_sql=OFF", "-DFEATURE_network=ON",
            "-DFEATURE_dbus=OFF", "-DFEATURE_concurrent=OFF", "-DFEATURE_testlib=OFF", "-DFEATURE_printsupport=OFF",
            "-DQT_QMAKE_DEVICE_OPTIONS=QT_EMSCRIPTEN_ASYNCIFY=1"]
    run(argv, env=env)
    run([cmake, "--build", build, "--parallel", os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL", "2")], env=env)
    run([cmake, "--install", build], env=env)


def validate_package(site: Path):
    manifest = json.loads((site / "build-info.json").read_text())
    for name in (*PAYLOAD, *PLAYER_PAYLOAD):
        path = site / name
        if not path.is_file() or path.is_symlink():
            raise ValueError("Missing or symlinked editor asset: " + name)
        if hashlib.sha256(path.read_bytes()).hexdigest() != manifest["sha256"].get(name):
            raise ValueError("Editor asset checksum mismatch: " + name)
    validate_wasm((site / "ludus_editor.wasm").read_bytes(), EDITOR_MAXIMUM_MEMORY_PAGES)
    for sample in PLAYERS:
        validate_wasm((site / "players" / sample / "game.wasm").read_bytes())
        if "game.wasm" not in (site / "players" / sample / "game.js").read_text():
            raise ValueError("Sample JavaScript does not name its wasm module: " + sample)
    if "ludus_editor.wasm" not in (site / "ludus_editor.js").read_text():
        raise ValueError("Editor JavaScript does not name the staged wasm module.")
    if manifest.get("single_threaded") is not True:
        raise ValueError("Pages editor must not require shared memory.")
    for name in ("Ludus.txt", "Qt-LGPL-3.0.txt", QT_SOURCE_CHANGES_NOTICE, "Emscripten.txt", "miniaudio.txt", "yyjson.txt", "tree-sitter.txt", *TREE_SITTER_GRAMMAR_NOTICES, "Luau.txt", "Luau-lua.txt", "emdawnwebgpu.txt", "webgpu-native.txt"):
        if not (site / "licenses" / name).is_file():
            raise ValueError("Missing editor license: " + name)
    print("Editor package: verified assets, hashes, single-threaded wasm memory and notices.")


def package(build: Path, source: Path, sdk: Path, qt_lock: dict, web_lock: dict):
    site = ROOT / "out/editor-pages"
    site.mkdir(parents=True, exist_ok=True)
    for name in ("index.html", "editor.css", "editor.js"):
        source_file = ROOT / "docs/wiki/editor/index.html" if name == "index.html" else ROOT / "apps/editor/browser" / name
        if name == "index.html":
            (site / name).write_text(source_file.read_text().replace("../assets/ludus-icon.png", "ludus-icon.png"))
        else:
            shutil.copyfile(source_file, site / name)
    shutil.copyfile(ROOT / "docs/wiki/assets/ludus-icon.png", site / "ludus-icon.png")
    for name in ("ludus_editor.js", "ludus_editor.wasm", "qtloader.js"):
        shutil.copyfile(build / "apps/editor" / name, site / name)
    for sample in PLAYERS:
        destination = site / "players" / sample
        destination.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "apps/web_samples/player.html", destination / "index.html")
        shutil.copyfile(ROOT / "apps/web_samples/player.js", destination / "player.js")
        for name in ("game.js", "game.wasm"):
            shutil.copyfile(build / "apps/web_samples" / sample / name, destination / name)
    licenses = site / "licenses"
    licenses.mkdir(exist_ok=True)
    shutil.copyfile(ROOT / "LICENSE", licenses / "Ludus.txt")
    shutil.copyfile(source / "LICENSES/LGPL-3.0-only.txt", licenses / "Qt-LGPL-3.0.txt")
    for path in (source / "LICENSES").glob("*.txt"):
        shutil.copyfile(path, licenses / ("Qt-" + path.name))
    (licenses / QT_SOURCE_CHANGES_NOTICE).write_text(describe_qt_source_changes(source), encoding="utf-8")
    third_party = source / "src/3rdparty"
    notices = set()
    for path in third_party.rglob("*"):
        if path.is_file() and path.name.upper().startswith(("LICENSE", "COPYING", "COPYRIGHT")):
            notices.add(path)
        if path.name == "qt_attribution.json":
            notices.add(path)
            metadata = json.loads(path.read_text(), strict=False)
            for entry in metadata if isinstance(metadata, list) else [metadata]:
                for name in entry.get("LicenseFile", "").split():
                    notice = path.parent / name
                    if not notice.is_file() or not notice.resolve().is_relative_to(source.resolve()):
                        raise ValueError("Missing or escaping Qt third-party notice: " + str(notice))
                    notices.add(notice)
    for path in notices:
        destination = licenses / "qt-third-party" / path.relative_to(third_party)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)
    shutil.copyfile(ROOT / "third_party/miniaudio/LICENSE-MIT-0.txt", licenses / "miniaudio.txt")
    shutil.copyfile(ROOT / "third_party/yyjson/LICENSE", licenses / "yyjson.txt")
    # The editor's syntax runtime; the build verifies the downloaded LICENSE equals this copy.
    shutil.copyfile(ROOT / "third_party/tree-sitter/LICENSE", licenses / "tree-sitter.txt")
    # The syntax service's grammars, checked the same way against third_party/<grammar>/LICENSE.
    for name in TREE_SITTER_GRAMMAR_NOTICES:
        shutil.copyfile(ROOT / "third_party" / name.removesuffix(".txt") / "LICENSE", licenses / name)
    for relative, name in SDK_NOTICES.items():
        shutil.copyfile(sdk / "upstream/emscripten" / relative, licenses / name)
    for relative, name in (("LICENSE.txt", "Luau.txt"), ("lua_LICENSE.txt", "Luau-lua.txt")):
        shutil.copyfile(ROOT / "out/luau-probe/source" / relative, licenses / name)
    port = sdk / "upstream/emscripten/cache/ports/emdawnwebgpu/emdawnwebgpu_pkg"
    for relative, name in PORT_NOTICES.items():
        shutil.copyfile(port / relative, licenses / name)
    header = (port / "webgpu/include/webgpu/webgpu.h").read_bytes()
    notice, separator, _ = header.partition(b"#ifndef WEBGPU_H_")
    if not separator or b"BSD 3-Clause License" not in notice:
        raise ValueError("Pinned WebGPU native notice is missing")
    (licenses / "webgpu-native.txt").write_bytes(notice)
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    notice = f"""Ludus Editor browser preview
Ludus sources and rebuild instructions: https://github.com/The-Ludus-Project/Ludus/tree/{commit}
Qt source base: {qt_lock['qtbase_archive']}
Ludus changes to that Qt source: licenses/{QT_SOURCE_CHANGES_NOTICE}
Qt is statically linked under LGPL-3.0; license texts and third-party notices
are included in licenses/. Rebuild scripts compile Qt from source; users may
modify or replace Qt and rebuild this open-source application.
Tool and payload identity: build-info.json.
"""
    (site / "NOTICE.txt").write_text(notice)
    manifest = {"commit": commit, "qt": qt_lock, "browser_toolchain": web_lock, "single_threaded": True,
                "sha256": {name: hashlib.sha256((site / name).read_bytes()).hexdigest() for name in (*PAYLOAD, *PLAYER_PAYLOAD)}}
    (site / "build-info.json").write_text(json.dumps(manifest, indent=2) + "\n")
    validate_package(site)
    return site


HOST_FOLDER_HARNESS = "apps/editor/tests/browser/host_folder_harness.cpp"
HOST_FOLDER_TEST = "tools/web-browser-tests/host_folder_fs.mjs"


def test_host_folder(sdk: Path, web_lock: dict) -> None:
    """Build the Qt-free host-folder harness with the pinned SDK and run its Node checks."""
    env = sdk_environment(sdk, web_lock)
    output = ROOT / "out/editor-host-folder-test"
    output.mkdir(parents=True, exist_ok=True)
    harness = output / "host_folder_harness.mjs"
    run([sdk / "upstream/emscripten/em++", "-std=c++23", "-O1", "-fno-exceptions", "-Werror",
         f"-I{ROOT / 'modules/foundation/base/include'}", ROOT / HOST_FOLDER_HARNESS, "-o", harness,
         "-sASYNCIFY", "-lembind", f"--js-library={ROOT / 'apps/editor/browser/suspended_input.js'}",
         f"--js-library={ROOT / 'apps/editor/browser/host_folder.js'}",
         "-sMODULARIZE", "-sEXPORT_ES6", "-sENVIRONMENT=node", "-sALLOW_MEMORY_GROWTH", "-sASSERTIONS=1",
         "-sEXPORTED_RUNTIME_METHODS=ccall,HEAPU8", "-sEXPORTED_FUNCTIONS=_malloc,_free"], env=env)
    node = env.get("EMSDK_NODE")
    if not node:
        raise ValueError("The pinned Emscripten SDK did not report its Node.js; run scripts/build-web-editor --bootstrap.")
    run([node, ROOT / HOST_FOLDER_TEST, harness], env=env)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootstrap", action="store_true", help="download pinned SDK, Qt source and Qt host tools")
    parser.add_argument("--rebuild-qt", action="store_true", help="rebuild Qt after local source modifications")
    parser.add_argument("--qt-host", type=Path, help="existing Qt host tools of the pinned Qt version")
    parser.add_argument("--check-package", action="store_true", help="verify an existing staged package without building")
    parser.add_argument("--test-host-folder", action="store_true",
                        help="build and run the host-folder file-system checks (needs the pinned SDK)")
    args = parser.parse_args()
    if args.check_package:
        validate_package(ROOT / "out/editor-pages")
        return 0
    if args.test_host_folder:
        web_lock = json.loads((ROOT / "config/web_toolchain.json").read_text())
        sdk = ROOT / "out/host-tools/emsdk"
        if args.bootstrap:
            prepare_sdk(web_lock, ROOT / "out/editor-tools")
        if not (sdk / "upstream/emscripten/em++").is_file():
            raise ValueError("Missing pinned Emscripten SDK; run scripts/build-web-editor --bootstrap --test-host-folder.")
        test_host_folder(sdk, web_lock)
        return 0
    qt_lock = json.loads((ROOT / "config/editor_web_toolchain.json").read_text())
    web_lock = json.loads((ROOT / "config/web_toolchain.json").read_text())
    tools = ROOT / "out/editor-tools"
    tools.mkdir(parents=True, exist_ok=True)
    native = json.loads((ROOT / "config/tool_versions.json").read_text())["managed"]
    host_tools = ROOT / "out/host-tools/venv"
    if args.bootstrap and (managed_venv.ensure(host_tools) or not (host_tools / "bin/cmake").is_file()):
        run([host_tools / "bin/python", "-m", "pip", "install", f"cmake=={native['cmake']}", f"ninja=={native['ninja']}"])
    cmake, ninja = host_tools / "bin/cmake", host_tools / "bin/ninja"
    sdk = ROOT / "out/host-tools/emsdk"
    source = tools / "src/qtbase"
    host = args.qt_host.resolve() if args.qt_host else tools / "host" / qt_lock["qt"] / ("macos" if platform.system() == "Darwin" else "gcc_64")
    prefix, build = tools / "qt-wasm-static", tools / "qt-static-build"
    if args.bootstrap:
        prepare_sdk(web_lock, tools)
        if not (source / "CMakeLists.txt").is_file():
            archive = tools / "qtbase.tar.xz"
            download(qt_lock["qtbase_archive"], archive, qt_lock["qtbase_sha256"])
            extracted = tools / "qt-source"
            extract(archive, extracted)
            source.parent.mkdir(exist_ok=True)
            shutil.move(str(extracted / ("qtbase-everywhere-src-" + qt_lock["qt"])), source)
        if not host.is_dir():
            installer = tools / "installer"
            managed_venv.ensure(installer)
            run([installer / "bin/python", "-m", "pip", "install", f"aqtinstall=={qt_lock['aqtinstall']}"])
            os_name, arch = ("mac", "clang_64") if platform.system() == "Darwin" else ("linux", "linux_gcc_64")
            archives = ["qtbase"] + (["icu"] if os_name == "linux" else [])
            run([installer / "bin/python", "-m", "aqt", "install-qt", os_name, "desktop", qt_lock["qt"], arch,
                 "--archives", *archives, "-O", tools / "host"], cwd=tools)
    for path in (cmake, ninja, source / "CMakeLists.txt", host / "lib/cmake/Qt6/Qt6Config.cmake"):
        if not path.is_file():
            raise ValueError("Missing editor prerequisite; run scripts/build-web-editor --bootstrap: " + str(path))
    if f'QT_REPO_MODULE_VERSION "{qt_lock["qt"]}"' not in (source / ".cmake.conf").read_text():
        raise ValueError("Qt source version differs from the pinned editor toolchain.")
    apply_qt_source_changes(source)
    env = sdk_environment(sdk, web_lock)
    if args.bootstrap:
        if not (ROOT / "out/host-tools/bin/clang++").is_file():
            from engine import configure_system_tool_shims, load_tool_versions
            configure_system_tool_shims(ROOT, load_tool_versions(ROOT))
        run([sys.executable, ROOT / "scripts/script-provider", "bootstrap"])
        run([sys.executable, ROOT / "scripts/shader-probe", "bootstrap"])
        run([sys.executable, ROOT / "scripts/bootstrap-spirv-cross"])
    identity = {"qt": qt_lock, "web": web_lock, "qt_network": True,
                "qt_source_changes": get_qt_source_changes_digest()}
    stamp = prefix / "ludus-build.json"
    if args.rebuild_qt or not stamp.exists() or json.loads(stamp.read_text()) != identity:
        qt_build(cmake, ninja, source, build, prefix, host, env)
        stamp.write_text(json.dumps(identity, indent=2) + "\n")
    # The browser build has no luslc target; the web samples' LUSL shaders lower with a host build.
    try:
        luslc = host_luslc.ensure(ROOT)
    except host_luslc.HostLuslcError as error:
        raise ValueError(str(error)) from error
    app = ROOT / "out/build/web-editor"
    run([cmake, "--fresh", "-S", ROOT, "-B", app, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}",
         f"-DCMAKE_TOOLCHAIN_FILE={prefix / 'lib/cmake/Qt6/qt.toolchain.cmake'}", f"-DQT_HOST_PATH={host}",
         "-DCMAKE_BUILD_TYPE=MinSizeRel", "-DLUDUS_BUILD_FLAVOR=Release", "-DLUDUS_USE_INIT_OPTIONS=OFF",
         "-DLUDUS_BUILD_TESTS=OFF", "-DLUDUS_BUILD_SMOKE_APP=OFF", "-DLUDUS_BUILD_WEB_PROBES=OFF",
         "-DLUDUS_BUILD_EDITOR=ON", "-DLUDUS_WARNINGS_AS_ERRORS=ON", "-DLUDUS_BUILD_WEB_SAMPLES=ON", "-DLUDUS_BUILD_BEHAVIOR=ON",
         f"-DLUDUS_SLANG_COMPILER={ROOT / 'out/shader-tools/slang/bin/slangc'}", f"-DLUDUS_LUSLC={luslc}",
         f"-DLUDUS_SPIRV_VALIDATOR={ROOT / 'out/shader-tools/spirv-tools/usr/bin/spirv-val'}",
         f"-DLUDUS_SPIRV_CROSS={ROOT / 'out/shader-tools/spirv-cross/bin/spirv-cross'}"], env=env)
    run([cmake, "--build", app, "--target", "ludus_editor", "ludus_web_live_edit", "ludus_web_scripted", "ludus_web_cornell",
         "--parallel", os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL", "2")], env=env)
    print("Staged browser editor: " + str(package(app, source, sdk, qt_lock, web_lock)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print("Browser editor: " + str(error), file=sys.stderr)
        sys.exit(1)
