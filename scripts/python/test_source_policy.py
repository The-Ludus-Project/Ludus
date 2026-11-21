"""Regressions for changed-source review rules and literal safety."""

import contextlib
import io
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import engine
from formatting import format_source
from source_policy import added_lines, check, check_formatting, violations

ROOT = Path(__file__).resolve().parents[2]
CLANG_FORMAT = shutil.which(engine.pinned_llvm_tool(ROOT, "clang-format"))


class SourcePolicyTests(unittest.TestCase):
    def diagnose(self, text, suffix=".cpp"):
        return violations(
            "modules/example/source" + suffix, text, set(range(1, text.count("\n") + 2))
        )

    def test_build_commands_cannot_be_nested_in_source_lists(self):
        for outer in ["add_executable", "add_library", "target_sources"]:
            for inner in ["target_sources", "target_link_libraries", "ludus_apply_project_defaults"]:
                with self.subTest(outer=outer, inner=inner):
                    source = f"{outer}(example main.cpp\n  {inner}(example extra.cpp)\n)\n"
                    findings = self.diagnose(source, ".cmake")
                    self.assertEqual(len(findings), 1, findings)
                    self.assertIn(f"close {outer} before invoking {inner}", findings[0][1])
        source = "ADD_EXECUTABLE(example main.cpp\n TARGET_SOURCES(example extra.cpp)\n)\n"
        self.assertEqual(len(self.diagnose(source, ".cmake")), 1)

    def test_cmake_source_list_literals_comments_and_expansions_are_opaque(self):
        source = '''add_executable(example main.cpp
  "target_sources(fake example.cpp)"
  [=[target_sources(fake example.cpp)]=]
  # target_sources(fake example.cpp)
  #[==[
  target_sources(fake example.cpp)
  ]==]
  ${target_sources(fake)}
  $ENV{target_sources(fake)}
  $<IF:$<BOOL:1>,target_sources(fake).cpp,other.cpp>
)
target_sources(example PRIVATE extra.cpp)
'''
        self.assertEqual(self.diagnose(source, ".cmake"), [])

    def test_source_list_nesting_checks_the_changed_enclosing_command(self):
        source = "add_executable(example main.cpp\n target_sources(example extra.cpp)\n)\n\nset(other value)\n"
        path = "modules/example/CMakeLists.txt"
        self.assertEqual(len(violations(path, source, {1})), 1)
        self.assertEqual(len(violations(path, source, {3})), 1)
        self.assertEqual(violations(path, source, {5}), [])

    def test_closed_source_lists_configure_and_retain_all_sources(self):
        cmake = shutil.which("cmake")
        if cmake is None:
            self.skipTest("CMake is required for the live source-registration regression")
        before = '''add_executable(example main.cpp
target_sources(example PRIVATE extra.cpp)
)
'''
        after = '''add_executable(example main.cpp)
target_sources(example PRIVATE extra.cpp)
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
            (root / "extra.cpp").write_text("int GetValue() { return 1; }\n", encoding="utf-8")
            for name, source in [("before", before), ("after", after)]:
                (root / "CMakeLists.txt").write_text(
                    "cmake_minimum_required(VERSION 3.20)\nproject(SourceLists LANGUAGES CXX)\n"
                    + source
                    + 'get_target_property(sources example SOURCES)\n'
                      'file(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "${sources}")\n',
                    encoding="utf-8",
                )
                configured = subprocess.run(
                    [cmake, "-S", str(root), "-B", str(root / name)],
                    capture_output=True, text=True, check=False,
                )
                if name == "before":
                    self.assertNotEqual(configured.returncode, 0)
                    self.assertIn("target_sources", configured.stderr)
                    self.assertTrue(self.diagnose(source, ".cmake"))
                else:
                    self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
                    self.assertEqual((root / name / "sources.txt").read_text().split(";"),
                                     ["main.cpp", "extra.cpp"])
                    self.assertEqual(self.diagnose(source, ".cmake"), [])

    def test_thread_and_clock_headers_use_the_ludus_owners(self):
        for header in ["thread", "chrono"]:
            with self.subTest(header=header):
                findings = self.diagnose(f"#include <{header}>\n  #  include <{header}>\n")
                self.assertEqual(len(findings), 2, findings)
                self.assertIn("<ludus/foundation/threading/thread.hpp>", findings[0][1])
                self.assertIn(f"<{header}>", findings[0][1])
        accepted = '#include <ludus/foundation/threading/thread.hpp>\n// #include <thread>\nconst char* text = "#include <chrono>";\n#include <atomic>\n'
        self.assertEqual(self.diagnose(accepted), [])
        source = "#include <chrono>\n#include <thread>\n"
        everything = set(range(1, 4))
        for owner in [
            "modules/foundation/time/src/clock.cpp",
            "modules/foundation/logging/src/internal/backend.hpp",
            "modules/foundation/base/tests/assert_child.cpp",
            "apps/editor/src/main_window.cpp",
        ]:
            with self.subTest(owner=owner):
                self.assertEqual(violations(owner, source, everything), [])
        self.assertEqual(len(violations("modules/foundation/time/tests/time_tests.cpp", source, everything)), 2)
        # Only changed lines are reported: an untouched legacy include is not a new finding.
        self.assertEqual(violations("modules/example/source.cpp", source, {3}), [])

    def test_admission_capacity_literals_use_their_owner(self):
        paths = ["modules/content/scene_cook/src/scene_cook.cpp",
                 "modules/content/scene_cook/src/scene_assets.cpp",
                 "modules/content/src/files.cpp", "apps/editor/src/render_scene.cpp"]
        for path in paths:
            for expression in ["500000", "1048576", "4194304", "8388608U", "67108864LL",
                               "usize{16} * 1024 * 1024", "usize{1024} * 1024",
                               "32ULL * 1024U * 1024", "usize{8} << 20"]:
                with self.subTest(path=path, expression=expression):
                    self.assertTrue(violations(path, f"if (count > {expression}) {{}}", {1}))
            for expression in ["format::MAX_VERTICES", "MAX_SAVE_BYTES", "MAX_CATALOG_BYTES",
                               "usize{256} * 1024 * 1024", "128 * 1024 * 1024"]:
                # Unnamed products are still magic numbers; they are not duplicated admission policies.
                messages = [message for _, message in violations(path, f"if (count > {expression}) {{}}", {1})]
                self.assertFalse([message for message in messages if "admission-limit" in message], expression)
        self.assertFalse(violations(paths[0], '// 500000\nconst char* text = "1048576";\n#define CAP 4194304\n', {1, 2, 3}))
        self.assertFalse(violations(paths[0], "if (count > 500000) {}\n", {2}))
        for path in ["modules/content/scene_format/include/ludus/content/scene_format/limits.h",
                     "modules/content/tests/native_saves_tests.cpp", "modules/example/source.cpp"]:
            self.assertFalse(violations(path, "constexpr usize MAX_BYTES = usize{32} * 1024 * 1024;", {1}))

    def test_json_integers_are_qint64(self):
        path = "apps/editor/src/viewport_trace.cpp"
        for source in ['QJsonObject event{{QStringLiteral("tid"), static_cast<int64>(trackId)}};',
                       "tracks.append(static_cast<uint64>(trackId));",
                       'event.insert(QStringLiteral("id"), static_cast<foundation::int64>(id));',
                       "tracks.contains(QJsonValue(static_cast<int64>(trackId)));"]:
            with self.subTest(source=source):
                self.assertTrue(violations(path, source, {1}))
        for source in ['QJsonObject event{{QStringLiteral("tid"), static_cast<qint64>(trackId)}};',
                       'const uint64 epoch = static_cast<uint64>(host.value(QStringLiteral("epoch")).toInteger());',
                       "items.append(static_cast<int32>(index));"]:
            with self.subTest(source=source):
                self.assertFalse(violations(path, source, {1}))
        # Outside Qt sources, append is an ordinary function that takes int64.
        native = "examples/reference-scene/src/main.cpp"
        self.assertFalse(violations(native, "if (append(static_cast<uint64>(backend)) == false) {}", {1}))
        self.assertTrue(violations(native, "QJsonArray tracks; tracks.append(static_cast<uint64>(trackId));", {1}))

    def test_nullable_variables_and_move_parameters(self):
        for source in ["Thing* value = nullptr;", "struct A { Thing* mValue{}; };", "Thing* const value = nullptr;",
                       "void Move(Thing&& inoutOther);", "void Move(Thing&& outOther);"]:
            self.assertTrue(self.diagnose(source), source)
        for source in ["Thing* valueOrNull = nullptr;", "void Move(Thing&& other);",
                       "void Read(const Thing& input);", "Thing* value = GetRequired();"]:
            self.assertFalse(self.diagnose(source), source)
        self.assertFalse(self.diagnose('// Thing* value = nullptr;\nconst char* text = "Thing&& inoutOther";'))

    def test_magic_numbers_need_a_named_owner(self):
        for source in ["int count = 42;", "Text<255> name;", "if (size > 0x400) {}",
                       "float scale = 1e-6f;", "return Size{640, 480};", "char first[64] = {};",
                       "x = 3.5;", "void F() { if (x) { y(); } int z = 7; }"]:
            with self.subTest(source=source):
                self.assertTrue(self.diagnose(source), source)
        for source in ["int count = 0;", "x = y * 2 + 1;", "x = -1;", "float h = 0.5f * w;",
                       "x = 1.0 - t;", "constexpr usize MAX_COUNT = 64;",
                       "static const float32 SCALE{0.25f};", "inline constexpr usize B = usize{16} * 1024;",
                       "constexpr uint64 MAX = uint64{1} << 32U;", "enum class Mode { A = 3, B = 7 };",
                       "static_assert(sizeof(Thing) == 16);", "constexpr Table T = { {3, 4}, {5, 6} };",
                       "x = data[3]; // ludus-style: magic-number glTF vec4 alpha lane",
                       '// 42 in a comment\nconst char* text = "1080";\n#define CAP 4096\n',
                       "int vertex2 = value; Thing3D thing;"]:
            with self.subTest(source=source):
                self.assertFalse(self.diagnose(source), source)
        for path in ["modules/example/tests/thing_tests.cpp", "apps/editor/tests/a.cpp",
                     "modules/foundation/math/benchmarks/sum_bench.cpp", "tools/probe/main.cpp"]:
            self.assertFalse(violations(path, "int count = 42;", {1}), path)

    def test_changed_references_booleans_and_abbreviations(self):
        for source in ["void F(Thing& thing);", "bool ready = true;", "void F(bool enabled);",
                       "int numVertices = 0;", "void Copy(const Buffer& src);", "Context* ctxOrNull = nullptr;"]:
            with self.subTest(source=source):
                self.assertTrue(self.diagnose(source), source)
        for source in ["void F(Thing& outThing, Other& inoutOther, const Thing& thing);",
                       "bool isReady = true; bool mHasItems = false;", "void F(bool& outIsDegenerate);",
                       "int main(int argc, char** argv) { return 0; }", "UniquePtr<Thing> owner;",
                       "template <typename T> class SharedPtr; template <typename T> class WeakPtr;",
                       "QString parameterText; Source source; usize count = 0;",
                       "for (Thing& thing : things) {}",
                       "int vkArgs = 0; // ludus-style: abbreviation mirrors the Vulkan field name"]:
            with self.subTest(source=source):
                self.assertFalse(self.diagnose(source), source)

    def test_pointee_assignment_does_not_make_the_pointer_nullable(self):
        self.assertFalse(self.diagnose("void Clear(Thing** required) { *required = nullptr; }"))

    def test_shader_reuses_owned_pi_constant(self):
        self.assertTrue(self.diagnose("float Shade() { return 1 / 3.141592653589793; }", ".slang"))
        self.assertFalse(self.diagnose("float Shade() { return 1 / PI; }", ".slang"))
        self.assertFalse(violations("modules/foundation/math/shaders/math.slang",
                                   "static const float PI = 3.141592653589793;", {1}))

    def test_shader_else_if_is_control_flow(self):
        self.assertFalse(
            self.diagnose("void Shade() { if (x) {} else if (y) {} }", ".slang")
        )

    def test_one_variable_per_declaration(self):
        for suffix in [".cpp", ".slang"]:
            for text in [
                "usize first = 0, second = 0;",
                "uint first, second;",
                "struct Pair { float3 PositionWorld, NormalWorld; };",
                "const uint32 first[] = {1, 2}, second[] = {3, 4};",
                "void Run() { usize first = F(1, 2),\n second = 0; }",
                "for (uint first = 0, second = 1; first < 2; ++first) {}",
                "uint first = 0; uint second = 1, third = 2;",
                "Handle* first = nullptr, second = nullptr;",
                "namespace detail\n{\nuint first, second;\n}",
            ]:
                with self.subTest(text=text, suffix=suffix):
                    self.assertTrue(self.diagnose(text, suffix))

    def test_expression_commas_are_not_declarations(self):
        for text in [
            "uint first = F(1, 2); uint second = 0;",
            "const Pair value = {1, 2};",
            "Pair<int, int> value = Make<Pair<int, int>>(1, 2);",
            "void Run(uint first, uint second) { F(first, second); }",
            "for (uint i = 0; i < 2; ++i, Update()) {}",
            "uint value = [] { return F(1, 2); }();",
            '// uint first, second;\nconst char* text = "uint first, second;";',
            "namespace detail\n{\nvoid F();\n}\ntemplate <typename A, usize B>\nclass C\n{\n};",
            "namespace a::b\n{\n}\ntemplate <typename A, typename B>\nusing Pair = Make<A, B>;",
        ]:
            with self.subTest(text=text):
                self.assertFalse(self.diagnose(text))

    def test_builtin_shader_recipes_have_one_owner(self):
        self.assertTrue(
            self.diagnose(
                'ludus_compile_shader(TARGET app NAME renderer_lit RASTER SOURCE "lit.slang")',
                ".cmake",
            )
        )
        self.assertFalse(
            self.diagnose(
                "ludus_compile_renderer_shaders(TARGET app SHADERS renderer_lit)",
                ".cmake",
            )
        )
        self.assertFalse(
            self.diagnose(
                'ludus_compile_shader(TARGET app NAME custom SOURCE "custom.slang")',
                ".cmake",
            )
        )

    def test_explicit_values_and_unspellable_lambdas(self):
        self.assertFalse(
            self.diagnose(
                "const Status status = Poll();\nauto callback = [&] { return status; };\n"
            )
        )
        self.assertTrue(self.diagnose("const auto status = Poll();\n"))
        self.assertTrue(self.diagnose("for (const auto& item : items) {}\n"))

    def test_direct_initializers_and_structured_bindings(self):
        self.assertTrue(self.diagnose("auto status(Poll());"))
        self.assertTrue(self.diagnose("auto const& status{Poll()};"))
        self.assertTrue(self.diagnose("auto [first, second] = Pair();"))
        self.assertFalse(self.diagnose("auto Function() -> Status { return Poll(); }"))

    def test_reviewable_iterator_exception(self):
        self.assertFalse(
            self.diagnose(
                "auto iterator = items.begin(); // ludus-style: inferred-type: iterator implementation-dependent iterator spelling\n"
            )
        )
        self.assertTrue(
            self.diagnose(
                "auto status = Poll(); // ludus-style: inferred-type: convenient\n"
            )
        )

    def test_comments_literals_and_preprocessor_are_opaque(self):
        self.assertFalse(
            self.diagnose(
                '// auto status = Poll();\nconst char* message = R"(auto status = Poll();)";\n#define AUTO_EXAMPLE auto status = Poll()\n'
            )
        )

    def test_explicit_boolean_comparisons(self):
        for text in (
            "if (!IsReady()) {}",
            "bool hasFailed = !ready;",
            "if (not IsReady()) {}",
        ):
            with self.subTest(text=text):
                self.assertTrue(self.diagnose(text))
        self.assertFalse(self.diagnose("if (IsReady() == false || status != Ready) {}"))
        self.assertFalse(self.diagnose("bool operator!=(const Value& other) const;"))

    def test_negation_ignores_literals_comments_and_preprocessor(self):
        self.assertFalse(
            self.diagnose(
                '// !ready and not ready\nconst char* message = R"(!ready, not ready)";\n#if !defined(TEST)\n#endif\n'
            )
        )

    def test_negation_ratchet_reports_only_touched_code(self):
        before = "if (!legacy) {}\n"
        after = before + "if (!current) {}\n"
        failures = violations("modules/a.cpp", after, added_lines(before, after))
        self.assertEqual([line for line, _ in failures], [2])

    def test_shader_members_functions_and_spaces(self):
        good = "struct Hit { float3 PositionWorld; float DistanceWorld; };\nfloat3 NormalizeOrZero(float3 value) { return value; }\n"
        self.assertFalse(self.diagnose(good, ".slang"))
        self.assertTrue(self.diagnose("struct Hit { float3 position; };", ".slang"))
        self.assertTrue(self.diagnose("struct Hit { float3 Position; };", ".slang"))
        self.assertTrue(
            self.diagnose("float normalize(float value) { return value; }", ".slang")
        )

    def test_shader_types_and_parameters(self):
        self.assertTrue(self.diagnose("struct sample { float Value; };", ".slang"))
        self.assertTrue(
            self.diagnose("float Evaluate(float Value) { return Value; }", ".slang")
        )
        self.assertFalse(
            self.diagnose("float Evaluate(float value) { return value; }", ".slang")
        )

    def test_camel_case_rejects_underscores_after_correct_initial_case(self):
        for text in [
            "struct Sample_type { float Value; };",
            "struct Sample { float Value_count; };",
            "float Evaluate_brdf(float value) { return value; }",
            "float Evaluate(float incident_value) { return incident_value; }",
            "void Evaluate(out float outValue_count) { outValue_count = 0; }",
        ]:
            with self.subTest(text=text):
                self.assertTrue(self.diagnose(text, ".slang"))

    def test_shader_outputs_and_entry_points(self):
        self.assertFalse(
            self.diagnose(
                "void MultiplyWide(uint a, out uint outLo) { outLo = a; }", ".slang"
            )
        )
        self.assertTrue(
            self.diagnose(
                "void MultiplyWide(uint a, out uint lo) { lo = a; }", ".slang"
            )
        )
        self.assertTrue(
            self.diagnose(
                "[numthreads(64,1,1)] void computeMain(uint3 id : SV_DispatchThreadID) {}",
                ".slang",
            )
        )

    def test_shader_enums_are_scoped_pascal_case_with_a_width(self):
        self.assertFalse(self.diagnose("enum SceneQuery : uint { Miss = 0, Surface = 1, Failure = 2, };", ".slang"))
        self.assertTrue(self.diagnose("enum SceneQuery { Miss = 0, Surface = 1 };", ".slang"))
        self.assertTrue(self.diagnose("enum sceneQuery : uint { Miss = 0 };", ".slang"))
        self.assertTrue(self.diagnose("enum SceneQuery : uint { Miss = 0, SURFACE = 1 };", ".slang"))
        self.assertTrue(self.diagnose("enum SceneQuery : uint\n{\n    Miss,\n    query_surface,\n};", ".slang"))

    def test_shader_names_scene_enumerators_not_v1_aliases(self):
        text = "bool IsHit(SceneQuery status) { return status == SCENE_QUERY_SURFACE; }"
        self.assertTrue(self.diagnose(text, ".slang"))
        self.assertTrue(self.diagnose("bool IsMesh(LightInfo light) { return light.Kind == SCENE_LIGHT_MESH; }",
                                      ".slang"))
        self.assertFalse(self.diagnose("bool IsHit(SceneQuery status) { return status == SceneQuery::Surface; }",
                                       ".slang"))
        alias = "static const SceneQuery SCENE_QUERY_SURFACE = SceneQuery::Surface;"
        self.assertFalse(violations("modules/graphics/renderer/shaders/scene_shader.slang", alias, {1}))

    def test_annotations_are_owned_by_rhi(self):
        text = "[[vk::binding(0,0)]] StructuredBuffer<uint4> scene;"
        self.assertTrue(self.diagnose(text, ".slang"))
        self.assertFalse(
            violations("modules/graphics/rhi/shaders/raster_bindings.slang", text, {1})
        )

    def test_unchanged_debt_is_not_a_bypass_for_new_lines(self):
        before = "auto old = Poll();\n"
        after = before + "auto added = Poll();\n"
        self.assertEqual(added_lines(before, after), {2})
        failures = violations("modules/a.cpp", after, added_lines(before, after))
        self.assertEqual([line for line, _ in failures], [2])

    def test_git_ratchet_checks_tracked_and_untracked_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def git(*arguments):
                return subprocess.run(
                    ["git", *arguments],
                    cwd=root,
                    check=True,
                    text=True,
                    capture_output=True,
                ).stdout

            git("init", "-q")
            git("config", "user.name", "Policy test")
            git("config", "user.email", "policy@example.invalid")
            (root / "modules").mkdir()
            source = root / "modules/a.cpp"
            source.write_text("auto old = Poll();\n")
            git("add", ".")
            git("commit", "-qm", "fixture")
            base = git("rev-parse", "HEAD").strip()
            self.assertFalse(check(root, base))
            source.write_text("auto old = Poll();\nauto added = Poll();\n")
            (root / "modules/new.slang").write_text(
                "float helper(float value) { return value; }"
            )
            self.assertEqual(len(check(root, base)), 2)

    def test_pr_head_excludes_unrelated_synthetic_merge_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def git(*arguments):
                return subprocess.run(
                    ["git", *arguments],
                    cwd=root,
                    check=True,
                    text=True,
                    capture_output=True,
                ).stdout.strip()

            git("init", "-q", "-b", "main")
            git("config", "user.name", "Policy test")
            git("config", "user.email", "policy@example.invalid")
            (root / "modules").mkdir()
            source = root / "modules/a.cpp"
            source.write_text("auto legacy = Poll();\n")
            git("add", ".")
            git("commit", "-qm", "base")
            base = git("rev-parse", "HEAD")
            git("checkout", "-qb", "feature")
            source.write_text("auto legacy = Poll();\nStatus current = Poll();\n")
            git("add", ".")
            git("commit", "-qm", "feature")
            head = git("rev-parse", "HEAD")
            git("checkout", "-q", "main")
            (root / "modules/other.cpp").write_text("auto unrelated = Poll();\n")
            git("add", ".")
            git("commit", "-qm", "unrelated main change")
            git("merge", "-q", "--no-edit", "feature")
            self.assertTrue(check(root, base))
            self.assertFalse(check(root, base, head=head))
            # Head-specific checking still rejects actual PR violations.
            self.assertTrue(check(root, head, head=git("rev-parse", "HEAD")))

    def test_moved_debt_is_not_authored_however_the_diff_aligns(self):
        # A batch diff spanning several pull requests can align an untouched line as added
        # when a block moves past it. Only lines the change wrote count (authored_lines.py).
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def git(*arguments):
                return subprocess.run(
                    ["git", *arguments],
                    cwd=root,
                    check=True,
                    text=True,
                    capture_output=True,
                ).stdout.strip()

            git("init", "-q")
            git("config", "user.name", "Policy test")
            git("config", "user.email", "policy@example.invalid")
            (root / "modules").mkdir()
            source = root / "modules/a.cpp"
            legacy = "const auto* imported = internal::NativeDeviceRequest();\n"
            block = "".join(f"Status initialized{i} = InitializeOwnerSlot({i});\n" for i in range(4))
            source.write_text(legacy + block)
            git("add", ".")
            git("commit", "-qm", "base")
            base = git("rev-parse", "HEAD")
            moved = block + legacy
            source.write_text(moved)
            git("commit", "-qam", "move the legacy line below the block")
            head = git("rev-parse", "HEAD")
            # The line diff reports the untouched legacy line as added.
            self.assertIn(5, added_lines(legacy + block, moved))
            self.assertFalse(check(root, base, head=head))
            self.assertFalse(check(root, base))
            # A violation the change writes is still reported, in a commit and in the work tree.
            source.write_text(moved + "auto added = NativeDeviceRequestCount();\n")
            self.assertEqual([failure.split(":")[1] for failure in check(root, base)], ["6"])
            git("commit", "-qam", "add a line")
            self.assertEqual(len(check(root, base, head=git("rev-parse", "HEAD"))), 1)


class PlatformOnlyWarningTests(unittest.TestCase):
    """Warnings that only batch-only MSVC and Emscripten builds report, from batch failures."""

    def diagnose(self, text, path="tests/example/source.cpp", changed=None):
        lines = set(range(1, text.count("\n") + 2)) if changed is None else changed
        return [(line, message) for line, message in violations(path, text, lines)
                if "preprocessor branch" in message or "C4127" in message]

    def test_constants_used_only_under_a_branch_are_declared_there(self):
        # Batch 44900782876: the web build rejected a native-only tick limit.
        source = """namespace
{
constexpr int64 TIME_LIMIT_SECONDS = 20;
}
int Run()
{
#if defined(LUDUS_PLATFORM_WEB)
    return 0;
#else
    return Tick(TIME_LIMIT_SECONDS);
#endif
}
"""
        findings = self.diagnose(source)
        self.assertEqual(len(findings), 1, findings)
        self.assertEqual(findings[0][0], 3)
        self.assertIn("TIME_LIMIT_SECONDS is used only under a preprocessor branch", findings[0][1])
        moved = source.replace("constexpr int64 TIME_LIMIT_SECONDS = 20;\n",
                               "#if !defined(LUDUS_PLATFORM_WEB)\nconstexpr int64 TIME_LIMIT_SECONDS = 20;\n#endif\n")
        self.assertEqual(self.diagnose(moved), [])

    def test_uses_in_every_branch_or_complementary_blocks_are_unconditional(self):
        source = """static void Run(State& state)
{
}
void Start()
{
#if !defined(COOPERATIVE)
    Run(gState);
#endif
}
void Finish()
{
#if defined(COOPERATIVE)
    Run(gState);
#else
    Wait(gState);
#endif
}
"""
        self.assertEqual(self.diagnose(source), [])
        self.assertEqual(len(self.diagnose(source.replace("#if defined(COOPERATIVE)", "#if defined(OTHER)"))), 1)
        everywhere = """constexpr int32 LIMIT = 4;
int32 Read()
{
#if defined(A)
    return LIMIT;
#elif defined(B)
    return LIMIT + 1;
#else
    return LIMIT - 1;
#endif
}
"""
        self.assertEqual(self.diagnose(everywhere), [])

    def test_static_and_anonymous_functions_are_checked_in_main_files_only(self):
        source = """namespace
{
void SetControlVisible(Widget& widget)
{
    widget.Show();
}
}
void Use(Widget& widget)
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    SetControlVisible(widget);
#endif
}
"""
        def functions(path="tests/example/source.cpp", text=source):
            return [line for line, message in self.diagnose(text, path) if "SetControlVisible" in message]

        self.assertEqual(functions(), [3])
        self.assertEqual(functions("tests/example/source.h"), [])
        self.assertEqual(functions(text=source.replace("namespace\n{\n", "namespace detail\n{\n")), [])
        self.assertEqual(functions(text=source.replace("void SetControlVisible", "static void SetControlVisible")
                                   .replace("namespace\n{\n", "namespace detail\n{\n")), [3])

    def test_parameters_are_discarded_in_branches_that_do_not_read_them(self):
        # Batch 41110487152: the browser editor never read isSignature.
        source = """void Assistance::Request(bool isSignature)
{
#if !defined(Q_OS_WASM)
    mIsSignature = isSignature;
#endif
}
"""
        findings = self.diagnose(source, "apps/example/source.h")
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("discard it in the other branches with static_cast<void>(isSignature)", findings[0][1])
        discarded = source.replace("#endif", "#else\n    static_cast<void>(isSignature);\n#endif")
        self.assertEqual(self.diagnose(discarded, "apps/example/source.h"), [])
        unnamed = "void Request(const Options&, [[maybe_unused]] bool isSignature)\n{\n#if defined(A)\n    Use(isSignature);\n#endif\n}\n"
        self.assertEqual(self.diagnose(unnamed), [])

    def test_a_branch_that_stops_the_build_compiles_no_configuration(self):
        source = """bool Initialize(const Info& info) noexcept
{
#if defined(A)
    return a::Initialize(info);
#elif defined(B)
    return b::Initialize(info);
#else
#    error "No platform backend"
#endif
}
"""
        self.assertEqual(self.diagnose(source), [])
        self.assertEqual(len(self.diagnose(source.replace('#else\n#    error "No platform backend"\n', ""))), 1)

    def test_conditional_uses_report_when_the_declaration_or_a_use_changed(self):
        source = """constexpr int32 POLL_MILLISECONDS = 4;
void Wait()
{
#if !defined(Q_OS_WASM)
    Sleep(POLL_MILLISECONDS);
#endif
}
"""
        self.assertEqual(len(self.diagnose(source, changed={1})), 1)
        self.assertEqual(len(self.diagnose(source, changed={5})), 1)
        self.assertEqual(self.diagnose(source, changed={3}), [])

    def test_waivers_and_maybe_unused_accept_a_deliberate_conditional_use(self):
        source = """constexpr int32 LIMIT = 4; // ludus-style: conditional-use only the probe build reads it
[[maybe_unused]] constexpr int32 OTHER = 4;
int32 Read()
{
#if defined(PROBE)
    return LIMIT + OTHER;
#endif
}
"""
        self.assertEqual(self.diagnose(source), [])

    def test_conditions_naming_only_constants_use_if_constexpr(self):
        # Batch 44900782876: MSVC C4127 in world_demo's --startup-report check.
        for condition in ("LUDUS_COMPILED_LOG_LEVEL > LUDUS_LOG_LEVEL_INFO", "LUDUS_ENABLE_ASSERTS == 0",
                          "!(MAXIMUM_ITEMS < 4)", "base::MAXIMUM_ITEMS != 0"):
            with self.subTest(condition=condition):
                findings = self.diagnose(f"void F()\n{{\n    if ({condition})\n    {{\n    }}\n}}\n")
                self.assertEqual(len(findings), 1, findings)
                self.assertIn("use if constexpr", findings[0][1])
        for condition in ("constexpr (LUDUS_ENABLE_ASSERTS == 0)", "(count < MAXIMUM_ITEMS)", "(LUDUS_UNLIKELY(value))",
                          "(Mode::Fast == MODE)", "(true)", "(item.IsReady())"):
            with self.subTest(condition=condition):
                self.assertEqual(self.diagnose(f"void F()\n{{\n    if {condition}\n    {{\n    }}\n}}\n"), [])
        waived = "void F()\n{\n    if (A_FLAG) // ludus-style: constant-condition a runtime-patched flag\n    {\n    }\n}\n"
        self.assertEqual(self.diagnose(waived), [])


@unittest.skipUnless(CLANG_FORMAT, f"requires pinned {engine.pinned_llvm_tool(ROOT, 'clang-format')}")
class ChangedFormattingTests(unittest.TestCase):
    """Local ``./scripts/check --format-changed`` must agree with CI's source policy."""

    def test_local_changed_formatting_matches_ci_for_slang(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def git(*arguments):
                return subprocess.run(
                    ["git", *arguments],
                    cwd=root,
                    check=True,
                    text=True,
                    capture_output=True,
                ).stdout.strip()

            def local_check(base, fix=False):
                output = io.StringIO()
                error = None
                with contextlib.redirect_stdout(output):
                    try:
                        engine.run_format_changed_check(
                            root, base, fix=fix, clang_format=CLANG_FORMAT
                        )
                    except engine.EngineError as caught:
                        error = caught
                return error, output.getvalue().splitlines()

            def ci_formatting(base, head):
                return [
                    failure
                    for failure in check(root, base, CLANG_FORMAT, head=head)
                    if "source formatting differs" in failure
                ]

            git("init", "-q", "-b", "main")
            git("config", "user.name", "Policy test")
            git("config", "user.email", "policy@example.invalid")
            shutil.copy(ROOT / ".clang-format", root / ".clang-format")
            (root / "modules/example").mkdir(parents=True)
            shader = root / "modules/example/shader.slang"
            shader.write_text(
                format_source(
                    "float Scale(float value) { return value; }\n", shader, CLANG_FORMAT
                )
            )
            # Unchanged formatting debt stays outside the changed-file scope.
            debt = root / "modules/example/debt.slang"
            debt_text = "float  Debt( float value ){return value;}\n"
            debt.write_text(debt_text)
            git("add", ".")
            git("commit", "-qm", "base")
            base = git("rev-parse", "HEAD")

            shader.write_text("float Scale( float value ){ return value+value; }\n")
            git("commit", "-qam", "misformatted shader")
            head = git("rev-parse", "HEAD")
            expected = [
                "modules/example/shader.slang: source formatting differs;"
                " use the pinned formatter and formatting.format_source"
            ]
            self.assertEqual(ci_formatting(base, head), expected)
            self.assertEqual(check_formatting(root, base, CLANG_FORMAT), expected)
            error, lines = local_check(base)
            self.assertIsNotNone(error)
            self.assertIn("--format-changed", str(error))
            self.assertEqual(lines, expected)

            error, lines = local_check(base, fix=True)
            self.assertIsNone(error)
            self.assertEqual(lines, ["Formatted modules/example/shader.slang"])
            self.assertEqual(debt.read_text(), debt_text)
            git("commit", "-qam", "format shader")
            head = git("rev-parse", "HEAD")
            self.assertEqual(check(root, base, CLANG_FORMAT, head=head), [])
            error, lines = local_check(base)
            self.assertIsNone(error)
            self.assertEqual(len(lines), 1)

    def test_check_command_routes_format_changed_with_base(self):
        arguments = engine.make_parser().parse_args(
            ["check", "--format-changed", "--base", "origin/main", "--fix"]
        )
        with patch.object(engine, "repo_root", return_value=ROOT), patch.object(
            engine, "run_format_changed_check"
        ) as format_changed, patch.object(engine, "run_format_check") as format_all:
            self.assertEqual(engine.command_check(arguments), 0)
        format_changed.assert_called_once_with(ROOT, "origin/main", fix=True)
        format_all.assert_not_called()


if __name__ == "__main__":
    unittest.main()
