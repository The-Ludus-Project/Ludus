# Unicode: pinned ICU4C host service

`Ludus::Unicode` (`modules/foundation/unicode`) owns the engine's private ICU4C
78.3 integration (CLDR 48, Unicode 17.0), the owner planned by the
[localization architecture](../../architecture/localization.md#current-capabilities-and-module-boundaries).
Its public header names no ICU type: it exposes only the host lifecycle, the
linguistic-data identity and allocation diagnostics. ICU headers, targets and
include directories never enter an engine public header or the installed SDK
include set. [Localization formatting](../localization.md#formatting-messages-with-arguments)
is its first consumer; L4 paragraph services (bidi, line breaking) will add to it.

## Acquiring ICU

The module is opt-in. `LUDUS_BUILD_UNICODE` defaults to OFF, so ordinary
configure, build, install and SDK consumers neither download nor require ICU,
and Localization keeps its formatter API but reports `BackendUnavailable`.
Enabling it needs the same explicit, checksum-verified bootstrap as the
[L0 probe](../../architecture/localization-l0.md#reproduce):

```sh
./scripts/icu-probe bootstrap      # once: pinned sources and data, sliced with tools/icu-probe/data-filter.json
cmake --preset macos-clang-development -DLUDUS_BUILD_UNICODE=ON
./scripts/icu-probe run --preset macos-clang-development   # builds and tests L0, L1 and L2 together
```

Use the `linux-clang-*` presets on Ubuntu and the `web-emscripten-*` presets for
wasm32. Configure only verifies the bootstrapped tree (`scripts/icu-probe
verify-sources`); a missing or modified tree fails with the bootstrap command
and nothing is downloaded. [`cmake/IcuProfile.cmake`](../../../cmake/IcuProfile.cmake)
compiles every upstream `common/` and `i18n/` unit once, with the Ludus
no-exception, sanitizer and flavor options and the pinned `UCONFIG` profile in
[`config/icu_toolchain.json`](../../../config/icu_toolchain.json); the L0 probe and
the L1 profile checker link the same targets. The 601,168-byte sliced data
package is embedded as a generated build-tree source.

An SDK installed with the option carries `Ludus::Unicode`, its public header,
the two ICU static archives as a raw private link closure under
`lib/Ludus/unicode`, and ICU's license under `share/Ludus/licenses/ICU`.

## Host lifecycle

```cpp
#include <ludus/foundation/unicode/unicode.hpp>

namespace unicode = ludus::foundation::unicode;

// Host startup, before any formatter context exists.
if (unicode::InitializeUnicodeHost(ludus::foundation::GetSystemAllocationDomain()) != unicode::UnicodeStatus::Ok)
{
    // DataMismatch or BackendFailure: ICU was cleaned up; dynamic text is unavailable.
}
// ... prepare formatter contexts, format, destroy every context ...
static_cast<void>(unicode::ShutdownUnicodeHost()); // u_cleanup at host teardown only
```

`InitializeUnicodeHost` follows the L0 ordering exactly: it installs ICU memory
hooks backed by the given `AllocationDomain` before any other ICU call, registers
the embedded package (ICU never opens files), warms the static locale cache,
`UnifiedCache` and number-skeleton tables, and compares the running ICU, CLDR and
Unicode versions with the pinned build. `GetLinguisticDataLabel` then reports the
same spelling cooked packages carry, `icu4c-78.3/cldr-48/unicode-17.0`.

ICU4C 78.3 cannot recover from allocation failure (wrong plural branches with a
success status, sticky errors, crashes; see the
[L0 findings](../../architecture/localization-l0.md#allocation-failure-findings)).
From hook installation onward every denied ICU allocation therefore terminates
through FoundationBase's emergency path (`LUDUS_FATAL`). ICU memory is measured,
not capped: `GetUnicodeAllocationStatistics` reports allocations, frees, live
blocks and bytes and the peak.

The host is a process service owned by the engine host, never by a catalog,
editor project or live-reloaded module. Initialize and shut it down on the host
thread while no ICU user exists. `ShutdownUnicodeHost` calls `u_cleanup`, which
also clears the hooks and data registration; a live formatter context at that
point terminates, because cleanup would invalidate its ICU objects. The host may
be initialized again afterwards. Locales are always created from explicit tags;
the process default locale is never read or set.

## Validation

`ludus_foundation_unicode_tests` checks the pinned label, warm-up allocations,
repeated initialization, explicit locales, user retention and that shutdown
releases every ICU block. `ludus_localization_format_probe` sweeps every ICU
allocation of host initialization on native and wasm32 and requires each denial
to terminate through the emergency path; see
[Localization validation](../localization.md#validation).
