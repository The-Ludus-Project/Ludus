"""Exercise the shared producer/consumer gate; these are policy tests, not device evidence."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CMAKE = ROOT / 'out/host-tools/venv/bin/cmake'


class Profiles(unittest.TestCase):
    def gate(self, system, architecture, compiler='Clang', version='23.1.3', pointer=8,
             emscripten=False, osx=''):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / 'profile.cmake'
            script.write_text(f'''
set(CMAKE_SYSTEM_NAME "{system}")
set(CMAKE_SYSTEM_PROCESSOR "{architecture}")
set(CMAKE_CXX_COMPILER_ID "{compiler}")
set(CMAKE_CXX_COMPILER_VERSION "{version}")
set(CMAKE_SIZEOF_VOID_P {pointer})
set(EMSCRIPTEN {'ON' if emscripten else 'OFF'})
set(CMAKE_OSX_ARCHITECTURES "{osx}")
include("{ROOT / 'cmake/LudusBehaviorProfile.cmake'}")
ludus_behavior_admit_profile("{ROOT / 'config/behavior_profiles.json'}" profile)
message(STATUS "PROFILE=${{profile}}")
''')
            return subprocess.run([str(CMAKE), '-P', str(script)], capture_output=True, text=True)

    def test_native_qualification_profiles(self):
        for system in ('Linux', 'Darwin'):
            for architecture, normalized in (('x86_64', 'x86_64'), ('AMD64', 'x86_64'),
                                             ('aarch64', 'arm64'), ('arm64', 'arm64')):
                with self.subTest(system=system, architecture=architecture):
                    result = self.gate(system, architecture)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(f'PROFILE={system}-{normalized}', result.stdout)

    def test_pinned_wasm32_qualification_profile(self):
        result = self.gate('Emscripten', 'x86', version='22.0.0', pointer=4, emscripten=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('PROFILE=Emscripten-wasm32', result.stdout)

    def test_unimplemented_engine_targets_fail_closed(self):
        for system, architecture in (('Windows', 'x86_64'), ('Android', 'arm64'), ('iOS', 'arm64')):
            with self.subTest(system=system):
                result = self.gate(system, architecture)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('requires an engine backend', result.stderr)

    def test_unknown_profiles_and_compilers_fail_closed(self):
        cases = [('Linux', 'riscv64', 'Clang', '23.1.3', 8, False),
                 ('FreeBSD', 'x86_64', 'Clang', '23.1.3', 8, False),
                 ('Linux', 'x86_64', 'GNU', '23.1.3', 8, False),
                 ('Darwin', 'arm64', 'AppleClang', '23.1.3', 8, False),
                 ('Linux', 'x86_64', 'Clang', '18.1.3', 8, False),
                 ('Linux', 'x86_64', 'Clang', '24.0.0', 8, False),
                 ('Linux', 'x86_64', 'Clang', '23.1.3', 4, False),
                 ('Emscripten', 'x86', 'Clang', '22.0.0', 8, True),
                 ('Emscripten', 'wasm32', 'Clang', '22.0.0', 4, False),
                 ('Emscripten', 'x86', 'Clang', '18.1.3', 4, True)]
        for case in cases:
            with self.subTest(case=case):
                self.assertNotEqual(self.gate(*case).returncode, 0)

    def test_explicit_macos_architecture_overrides_host(self):
        result = self.gate('Darwin', 'arm64', osx='x86_64')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('PROFILE=Darwin-x86_64', result.stdout)
        result = self.gate('Darwin', 'arm64', osx='x86_64;arm64')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('one declared target architecture', result.stderr)


if __name__ == '__main__':
    unittest.main()
