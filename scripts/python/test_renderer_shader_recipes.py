"""Renderer recipe ownership and source/installed CMake boundary regressions."""

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / "modules/graphics/renderer/shaders"


class RendererShaderRecipesTests(unittest.TestCase):
    def configure(self, installed=False, names=None):
        recipes = json.loads((SHADERS / "recipes.json").read_text())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            helper = ROOT / "cmake/LudusRendererShaders.cmake"
            if installed:
                helper = root / "sdk/LudusRendererShaders.cmake"
                helper.parent.mkdir()
                shutil.copyfile(ROOT / "cmake/LudusRendererShaders.cmake", helper)
                shutil.copytree(SHADERS, helper.parent / "shaders")
            project = root / "project"
            project.mkdir()
            (project / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.29)\nproject(Recipes NONE)\n"
                "add_library(cooked INTERFACE)\n"
                "function(ludus_require_file path)\n"
                ' if(NOT EXISTS "${path}")\n message(FATAL_ERROR "Missing ${path}")\n endif()\n'
                "endfunction()\n"
                "function(ludus_compile_shader)\n"
                ' cmake_parse_arguments(SH "RASTER" "TARGET;NAME;SOURCE;VERTEX;FRAGMENT;COMPUTE" "INCLUDES;DEPENDS" ${ARGN})\n'
                ' if(NOT EXISTS "${SH_SOURCE}")\n message(FATAL_ERROR "Missing source")\n endif()\n'
                " foreach(path IN LISTS SH_INCLUDES)\n"
                '  if(NOT IS_DIRECTORY "${path}")\n message(FATAL_ERROR "Missing include")\n endif()\n'
                " endforeach()\n"
                ' file(APPEND "${CMAKE_BINARY_DIR}/calls.txt" "${SH_NAME}|${SH_SOURCE}|${SH_VERTEX}|${SH_FRAGMENT}|${SH_COMPUTE}|${SH_DEPENDS}\\n")\n'
                "endfunction()\n"
                f'include("{helper}")\n'
                "ludus_compile_renderer_shaders(TARGET cooked SHADERS "
                + " ".join(names if names is not None else recipes)
                + ")\n"
            )
            result = subprocess.run(
                ["cmake", "-S", str(project), "-B", str(root / "build")],
                text=True,
                capture_output=True,
                check=False,
            )
            calls = root / "build/calls.txt"
            return result, calls.read_text().splitlines() if calls.exists() else []

    def test_source_and_installed_recipes_match(self):
        recipes = json.loads((SHADERS / "recipes.json").read_text())
        for installed in [False, True]:
            with self.subTest(installed=installed):
                result, calls = self.configure(installed)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(len(calls), len(recipes))
                for call in calls:
                    name, source, vertex, fragment, compute, dependency = call.split(
                        "|"
                    )
                    recipe = recipes[name]
                    self.assertEqual(Path(source).name, recipe["source"])
                    self.assertEqual(vertex, recipe.get("vertex", ""))
                    self.assertEqual(fragment, recipe.get("fragment", ""))
                    self.assertEqual(compute, recipe.get("compute", ""))
                    self.assertEqual(Path(dependency).name, "recipes.json")
                    # Diagnostic and material wrappers share production entries.
                    text = (SHADERS / recipe["source"]).read_text()
                    for shared in ["renderer_lit.slang", "renderer_gpu_preview.slang"]:
                        if f'#include "{shared}"' in text:
                            text += (SHADERS / shared).read_text()
                    for entry in [vertex, fragment, compute]:
                        if entry:
                            self.assertIn(entry + "(", text)

    def test_unknown_and_duplicate_recipes_fail_configure(self):
        for names, diagnostic in [
            (["renderer_missing"], "Unknown renderer shader recipe"),
            (["renderer_lit", "renderer_lit"], "Duplicate renderer shader recipe"),
        ]:
            for installed in [False, True]:
                with self.subTest(names=names, installed=installed):
                    result, _ = self.configure(installed, names)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(diagnostic, result.stderr)

    def test_consumers_do_not_duplicate_builtin_recipes(self):
        for name in [
            "apps/editor/CMakeLists.txt",
            "modules/graphics/renderer/CMakeLists.txt",
            "tests/sdk_consumer/renderer/CMakeLists.txt",
            "examples/reference-scene/CMakeLists.txt",
        ]:
            text = (ROOT / name).read_text()
            self.assertNotIn("renderer_vertex_entry", text)
            self.assertNotRegex(text, r"ludus_compile_shader\([^)]*NAME\s+renderer_")
            self.assertIn("ludus_compile_renderer_shaders(", text)


if __name__ == "__main__":
    unittest.main()
