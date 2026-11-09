# Local Conan recipe for the HarfBuzz pin selected by the text/font rendering
# design (.kiro/specs/text-font-rendering/design.md section 2).
#
# Why a local recipe: ConanCenter does not publish harfbuzz/14.5.1 (its newest
# recipe at the time of writing is 12.3.0). The design forbids silently falling
# back to an older library, so this recipe builds the exact pinned source with
# the design's option set:
#   * static library, OpenType + FreeType integration, built-in Unicode funcs;
#   * no GLib, ICU, Graphite2, Cairo, GObject/introspection, utilities, tests,
#     subsetter, or the experimental raster/vector/GPU/wasm backends.
#
# Source archive URL + SHA-256 are the verified pin recorded in
# docs/architecture/text-font-rendering-evidence/dependency-manifest.md.
# Never change the hash without re-verifying against the upstream release.

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import get, copy
import os


class HarfBuzzPinnedRecipe(ConanFile):
    name = "harfbuzz"
    version = "14.5.1"
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"
    # HarfBuzz is C++; the engine links it with exceptions disabled. We build the
    # library itself with its own flags, and only expose the C API to engine code.
    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    # The release asset API can serve the same pinned archive if the download page returns 504.
    # Both endpoints are checked against the digest in the dependency manifest.
    _source_urls = (
        "https://api.github.com/repos/harfbuzz/harfbuzz/releases/assets/602061249",
        "https://github.com/harfbuzz/harfbuzz/releases/download/14.5.1/harfbuzz-14.5.1.tar.xz?download=1",
    )
    _source_sha256 = "7e2fa4e8c7c98e8d8140671f5772542afaaa6acccfbd746506886b6d85f7f8d6"

    def requirements(self):
        # FreeType first; HarfBuzz links it (avoids the circular hb-assisted
        # auto-hinter path the design intentionally forgoes).
        self.requires("freetype/2.14.3")

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def source(self):
        get(self, self._source_urls, filename=f"harfbuzz-{self.version}.tar.xz",
            sha256=self._source_sha256, headers={"Accept": "application/octet-stream"}, strip_root=True)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        # Design option set via HarfBuzz's own CMake build knobs.
        tc.variables["HB_HAVE_FREETYPE"] = True
        tc.variables["HB_HAVE_GLIB"] = False
        tc.variables["HB_HAVE_ICU"] = False
        tc.variables["HB_HAVE_GRAPHITE2"] = False
        tc.variables["HB_HAVE_CAIRO"] = False
        tc.variables["HB_HAVE_GOBJECT"] = False
        tc.variables["HB_HAVE_INTROSPECTION"] = False
        tc.variables["HB_BUILD_UTILS"] = False
        tc.variables["HB_BUILD_SUBSET"] = False
        tc.variables["HB_BUILD_RASTER"] = False
        tc.variables["HB_BUILD_VECTOR"] = False
        tc.variables["HB_BUILD_GPU"] = False
        tc.variables["HB_BUILD_GPU_DEMO"] = "OFF"
        tc.variables["BUILD_SHARED_LIBS"] = False
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "COPYING", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.libs = ["harfbuzz"]
        self.cpp_info.includedirs = [os.path.join("include", "harfbuzz")]
        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs.extend(["m", "pthread"])
        self.cpp_info.requires = ["freetype::freetype"]
