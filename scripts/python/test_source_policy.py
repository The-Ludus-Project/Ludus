"""Regressions for changed-source review rules and literal safety."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from source_policy import added_lines, check, violations


class SourcePolicyTests(unittest.TestCase):
    def diagnose(self, text, suffix=".cpp"):
        return violations(
            "modules/example/source" + suffix, text, set(range(1, text.count("\n") + 2))
        )

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


if __name__ == "__main__":
    unittest.main()
