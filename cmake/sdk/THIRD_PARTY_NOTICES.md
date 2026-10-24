# Ludus SDK — third-party redistributable notices

The Ludus runtime SDK links the following third-party components into the static
engine libraries. When you redistribute an application built against this SDK,
the applicable notices below travel with the redistributable link closure
(requirement P02). This file is installed to
`share/Ludus/licenses/THIRD_PARTY_NOTICES.md` inside every SDK prefix; the
authoritative, machine-readable inventory (names, versions, kinds) is the
`dependencies` array in `share/Ludus/LudusSdkManifest.json`.

## Bundled static dependencies (shipped inside the SDK)

| Component | Version | License | Linked by |
| --- | --- | --- | --- |
| yyjson | 0.10.0 | MIT | `Ludus::Content` (PRIVATE JSON parser) |
| miniaudio | 0.11.23 | MIT-0 | `Ludus::Audio` (PRIVATE decoder/device backend) |
| volk | 1.4.357.0 | MIT | `Ludus::GraphicsRhi` (Vulkan loader meta-loader) |
| FreeType | 2.14.3 | FTL **or** GPLv2 (dual) — see FreeType FTL | `Ludus::Text` (PRIVATE, part of the final link closure) |
| HarfBuzz | 14.5.1 | "Old MIT" / modern MIT-style | `Ludus::Text` (PRIVATE, part of the final link closure) |

> FreeType and HarfBuzz are **private** link dependencies of the static
> `Ludus::Text` library. `PRIVATE` on a static library does not remove them from
> the *final consumer's* static link line, so their object code and license
> obligations are part of any application that links `Ludus::Text`.

## Reimplemented algorithms

FoundationMath's scalar Philox4x32-10 implementation and known-answer fixtures
refer to Random123 at revision `9545ff6413f258be2f04c1d319d99aaef7521150`
(`include/Random123/philox.h`, `tests/kat_vectors`). No Random123 library is linked.
Its BSD-3-Clause notice is retained here for source and SDK redistribution:

Copyright 2010-2011, D. E. Shaw Research. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions, and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions, and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
* Neither the name of D. E. Shaw Research nor the names of its contributors may
  be used to endorse or promote products derived from this software without
  specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## System dependencies (NOT bundled — must exist on the target)

| Component | Obligation |
| --- | --- |
| C / C++ runtime (`libc`, `libstdc++` matching `cxx_runtime_abi`) | System runtime; must match the manifest `cxx_runtime_abi`. |
| POSIX threads (`pthread`, `Threads::Threads`) | System; pulled in by the assertion/concurrency paths. |
| Vulkan loader (`libvulkan.so.1`) + GPU/driver | Runtime requirement for RHI; not redistributed by the SDK. |
| Wayland client (`wayland-client`) | Only when the Wayland Platform backend is enabled. |

## Shader-toolchain prerequisite (build time, not redistributed)

`ludus_compile_shader` requires the pinned Slang compiler and `spirv-val`
(`config/shader_toolchain.json`). These are host build-time tools; they are not
part of a shipped game.

Each bundled component's full upstream license text is placed next to this file
in `share/Ludus/licenses/` by the release-packaging step. Where a build has not
yet produced those files (e.g. a developer `cmake --install` without the release
packager), this notices file plus the manifest inventory record the obligation.

## xxHash 0.8.3

Yann Collet and contributors, BSD-2-Clause. https://github.com/Cyan4973/xxHash/tree/v0.8.3
The complete license is bundled as `xxhash-LICENSE`. Used privately by FoundationHash.

## Optional Luau behavior provider

Only SDKs with the `Behavior` component bundle Luau at reviewed commit
`1eca9fda3e4753a1592000f6cfdf659aaa778b7d` with Ludus's loader-owner and
assertion-helper linkage patches
(`config/luau_toolchain.json`). Copyright Roblox Corporation and contributors;
Luau and its Lua portions are MIT licensed. Both complete notices are installed
under `share/Ludus/licenses/Luau/` and copied by the standard game release helper.
The compiler/analyzer under `share/Ludus/behavior/bin/` are paired host build tools;
they are not player runtime dependencies. https://github.com/luau-lang/luau

## Vulkan Memory Allocator 3.3.0

AMD GPUOpen and contributors, MIT. https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tree/v3.3.0
The complete license is bundled as `VMA-LICENSE`. Used privately by the Vulkan
portable resource implementation; no allocator API is exported.

## Open Image Denoise 2.5.1 (macOS Apple silicon SDKs built with the denoiser)

`./init.sh` builds macOS Apple silicon SDKs with Intel Open Image Denoise unless
`--no-denoiser` is given; the manifest `dependencies` array lists it when present.
Such an SDK bundles the official RenderKit 2.5.1 release's shared runtime under
`lib/` (`libOpenImageDenoise`, its core and Metal/CPU device modules, and the
oneTBB library the CPU device uses) with its CMake package under
`lib/cmake/OpenImageDenoise-2.5.1/`. `Ludus::GraphicsRenderer` links it privately,
so every executable that links the renderer loads it at run time.
Copyright Intel Corporation; Apache-2.0. oneTBB is Apache-2.0. Upstream's
`LICENSE.txt` and third-party program notices are installed under
`share/Ludus/licenses/OpenImageDenoise/` and copied by the standard game release
helper; `ludus_install_denoiser_runtime` installs the runtime into a macOS app.
https://github.com/RenderKit/oidn/releases/tag/v2.5.1
