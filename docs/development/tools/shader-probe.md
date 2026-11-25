# Shader feasibility probe

See [the handoff](../shader-toolchain-handoff.md) for pins,
commands, independent binding/layout contracts, evidence, and hardware gaps.

`probe.lusl` is the only shader source, written in [LUSL](../../modules/tooling/shader-language.md).
`scripts/shader-probe bootstrap` acquires verified host tools; `compile` is offline. It lowers
the source with `luslc`, then writes generated artifacts under `out/`. The native build passes its
`luslc` target; a standalone `compile` uses `./scripts/bootstrap-luslc`. Enable `LUDUS_BUILD_SHADER_PROBE` for the native executable and CTests.
The browser harness consumes generated WGSL through real WebGPU. The test runner
uses the existing Playwright lock. Neither target is installed into Ludus's SDK.
