# Text/font rendering — verified dependency and asset manifest (F0)

Status: these are real, downloaded-and-hashed artifacts, not fabricated values.
Each SHA-256 below was computed from the archive/file actually fetched during
implementation. Never change a hash without re-verifying against the upstream
release.

## Source pins

| Dependency | Version | Archive URL | SHA-256 | License |
| --- | --- | --- | --- | --- |
| FreeType | 2.14.3 | https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz | `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f` | FTL (BSD-style) or GPLv2; used under FTL. `docs/FTL.TXT` in the archive. |
| HarfBuzz | 14.5.1 | https://github.com/harfbuzz/harfbuzz/releases/download/14.5.1/harfbuzz-14.5.1.tar.xz?download=1 | `7e2fa4e8c7c98e8d8140671f5772542afaaa6acccfbd746506886b6d85f7f8d6` | "Old MIT" (`COPYING`). |

Verified from the archives themselves:
`freetype.h` declares `FREETYPE_MAJOR 2 / MINOR 14 / PATCH 3`; HarfBuzz
`meson.build` declares `version: '14.5.1'` and `hb-version.h` defines
`HB_VERSION_STRING "14.5.1"`.

glslang 15.4.0 is named in the design as the host SPIR-V compiler for the two
Vulkan GLSL shaders. It is bootstrapped/used only in F3 (the Vulkan pipeline),
which is pending in this environment (no GPU); no glslang hash is recorded yet
because it is not yet wired into a build. It must be recorded with the same
verify-before-use discipline when F3 is implemented.

## Native dependency resolution (Conan 2)

* FreeType 2.14.3 is available on ConanCenter and is consumed through the normal
  Conan bootstrap with the design option set:
  `shared=False`, `with_png=False`, `with_zlib=False`, `with_brotli=False`,
  `with_bzip2=False`. Its own HarfBuzz-assisted auto-hinter is not enabled
  (FreeType is built first, HarfBuzz links it).
* HarfBuzz 14.5.1 is **not** published on ConanCenter (newest recipe there is
  12.3.0 at time of writing). Per the design, a small local recipe builds the
  exact pinned source: `third_party/harfbuzz/conanfile.py`. It uses HarfBuzz's
  own CMake build with `HB_HAVE_FREETYPE=ON`, built-in Unicode functions, and
  `HB_HAVE_GLIB/ICU/GRAPHITE2/CAIRO/GOBJECT/INTROSPECTION=OFF`,
  `HB_BUILD_UTILS/SUBSET/RASTER/VECTOR/GPU=OFF`. The recipe is exported into the
  project Conan cache by `scripts/python/engine.py` (`export_local_recipes`)
  before the lockfile is resolved.
* `conan.lock` pins:
  `harfbuzz/14.5.1#e3623b9b9c8b255282b298619d005330` and
  `freetype/2.14.3#4ee27b7918b546a96d7e6898e3a02b34`.
* The 4.0.23 Emscripten SDK port pins HarfBuzz 3.2.0 / FreeType 2.13.3, so the
  bare `USE_HARFBUZZ`/`USE_FREETYPE` web ports are **not** used; the locked
  sources must be built separately for wasm (F0 web path — pending here, no
  Emscripten toolchain in this environment).

Post-build verification of the HarfBuzz package: `hb-ft.h` is present and the
archive exports `hb_ft_font_create_referenced` / `hb_ft_font_set_load_flags`
(FreeType integration ON); `hb_subset_input_create` is absent (subsetter OFF).

## Fixture fonts

All fixtures are statically licensed under the SIL Open Font License 1.1 (OFL
text checked in beside the fonts). No system-font or runtime-network assumption
is made; tests load the packaged bytes directly.

| File | Origin | SHA-256 | License |
| --- | --- | --- | --- |
| `modules/text/tests/fixtures/NotoSans-Regular.ttf` | notofonts.github.io NotoSans hinted TTF (unmodified) | `478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823` | OFL 1.1 (`OFL-NotoSans.txt`) |
| `modules/text/tests/fixtures/NotoSansArabic-Regular.ttf` | notofonts.github.io NotoSansArabic hinted TTF (unmodified) | `bdff3e5659d67e67def05b33f749683b9376ae819d65d3dd62ac4640b3aaef48` | OFL 1.1 (`OFL-NotoSansArabic.txt`) |
| `modules/text/tests/fixtures/NotoSansKR-Subset-Regular.ttf` | Static subset of Google Fonts `NotoSansKR[wght].ttf` (instanced to wght=400, subset to the fixture syllables, GSUB/GPOS/GDEF retained) via `make_hangul_fixture.py` | `6ed5abe8fdd4727df1ba8fec4145a04cb8d4b14d48dac38c764cd3524939e4a9` | OFL 1.1 (`OFL-NotoSansKR.txt`) |

The Hangul fixture is subset because upstream Noto Sans KR is only distributed
as a large variable font; the design permits a reproducible subset that retains
the shaping tables. Regenerate byte-reproducibly with fonttools 4.66.1:

```
out/host-tools/venv/bin/python modules/text/tests/fixtures/make_hangul_fixture.py \
    <NotoSansKR[wght].ttf> modules/text/tests/fixtures/NotoSansKR-Subset-Regular.ttf
```

(The large variable source is not committed; the small static output is.)
