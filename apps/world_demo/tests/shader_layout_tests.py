"""Independent std140 checks of compile_shader.py's GLSL ES uniform layout for fixed vector/matrix arrays."""
import importlib.util
from pathlib import Path
import unittest
root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("shader", root / "cmake/shaders/compile_shader.py")
shader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shader)
class LayoutTests(unittest.TestCase):
    def layout(self, fields):
        return shader.glsl_es_layout("layout(std140) uniform World {" + fields + "};")
    def test_compiler_array_wrapper(self):
        result = shader.glsl_es_layout("struct A { highp vec4 data[192]; }; layout(std140) uniform World { vec4 view; vec4 settings; A geometry; A color; A rotation; } world;")
        self.assertEqual(result['offsets'], dict(view=0, settings=16, geometry=32, color=3104, rotation=6176))
        self.assertEqual(result['size'], 9248)
        for body in ("vec4 data[64]; float extra;", "mat3 data[64];", "A data[2];"):
            with self.subTest(body=body), self.assertRaises(RuntimeError):
                shader.glsl_es_layout("struct A {" + body + "}; layout(std140) uniform World { A geometry; } world;")
    def test_l3_matrix_layout_and_fixed_array_wrappers(self):
        self.assertEqual(self.layout('mat4 transform; vec4 tint;')['offsets'], dict(transform=0, tint=64))
        self.assertEqual(self.layout('mat4 transform; vec4 tint;')['size'], 80)
        result = shader.glsl_es_layout('struct A { highp mat4 data[64]; }; layout(std140) uniform World { layout(row_major) A matrices; vec4 tint; } world;')
        self.assertEqual(result['offsets'], dict(matrices=0, tint=4096))
        self.assertEqual(result['size'],4112)
        self.assertEqual(self.layout('mat4 matrices[256];')['size'],16384)
    def test_array_stride_and_following_scalar(self):
        result = self.layout("float a[2]; vec3 b[2]; float c;")
        self.assertEqual(result['offsets'], dict(a=0, b=32, c=64))
        self.assertEqual(result['size'], 80)
    def test_portable_uniform_limit(self):
        self.assertEqual(self.layout('vec4 surface[1024];')['size'], 16384)
    def test_unsupported_or_overlarge_layouts_fail(self):
        for fields in ("vec4 a[0];", "vec4 a[1025];", "vec4 a[1024]; float b;", "mat3 a;", "mat4 a[257];", "dvec4 a;", "vec4 a[N];", "vec4 a; vec4 a;"):
            with self.subTest(fields=fields), self.assertRaises(RuntimeError):
                self.layout(fields)
if __name__ == '__main__': unittest.main()
