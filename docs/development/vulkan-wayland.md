# Vulkan and Wayland

The RHI uses Conan's `volk::volk_headers` target. `src/volk_impl.cpp` defines
`VOLK_IMPLEMENTATION` exactly once, so Volk is compiled with the same private
`VK_USE_PLATFORM_WAYLAND_KHR` definition as the RHI. Do not also link
`volk::volk` or `Vulkan::Vulkan`: the former is a separately compiled loader,
and the latter conflicts with Volk's function-pointer symbols.

`LUDUS_PLATFORM_ENABLE_WAYLAND` controls both the platform backend and the RHI's
Wayland support. The existing platform detection checks for wayland-client,
wayland-protocols, and wayland-scanner. With the option off, the RHI builds
without the Wayland macro or Wayland extension requests.

## Window connection

1. Create a platform window and obtain `window->GetNativeWindowInfo()`.
   This descriptor contains borrowed, typed native handles and a window-system
   tag. It requires neither Vulkan headers nor Wayland client headers.
2. Pass the descriptor to `graphics::rhi::Start` and check the session state
   and error with `graphics::rhi::GetStartup`. Vulkan startup loads the
   Wayland surface extensions, creates the surface, selects a GPU with graphics
   and presentation support, and requests one queue from each selected family
   (one total when the family is shared). The device must support
   `VK_KHR_swapchain` and swapchain maintenance1 for safe presentation retirement.
3. When the session is ready, use `SetFrameTarget` and check the results of
   `BeginFrameStatus` and `EndFrameStatus`. Only end frames that opened successfully.
4. Call `graphics::rhi::Shutdown` before destroying the window or its display.
   Shutdown destroys the device, surface, and instance, then finalizes Volk.

Run lifecycle calls on the main thread. To retry a failed session or switch its
primary window, call `Shutdown` before starting again. A headless window uses
the same startup API with a headless descriptor.

Surface-format discovery sizes temporary storage from the driver's complete
advertised list, including lists larger than 64 entries. That storage counts
against `StorageLimits::HostBytes` and is released after selection. If the list
grows between the count and data queries, discovery retries up to three times;
it never selects from an incomplete prefix. The existing BGRA8 UNORM preference
and compatible RGBA8/BGRA8 sRGB fallbacks are unchanged. A query error, exhausted
host budget or unstable list fails discovery without publishing a selected format.
Retry startup after a transient query failure; for budget pressure, review the
device's host-memory budget and retained resources before retrying.

For swapchains that share a device, use the
[multiple-surface device API](fullscreen-rendering.md#multiple-surfaces-per-device-r14b).

## Drawable size and scale

Supply the initial window size in logical surface units. After each event pump,
read `GetNativeWindowInfo()` and pass its `Width` and `Height` directly to
`SetFrameTarget`. They already include the backing scale. Moving the window to
another output can recreate its swapchain even when the logical size stays the
same. A zero dimension in an xdg configure leaves that logical axis up to the
client. A suspended configure changes visibility and retains the logical size.

With both fractional-scale-v1 and viewporter, the backend rounds each scaled
dimension to the nearest pixel and maps the buffer back to the logical viewport.
Without either extension it uses the compositor’s preferred integer buffer scale,
or the largest integer scale among the outputs the surface currently occupies.
Before an output advertises a scale, or on a surface too old for buffer scaling,
the integer scale is one.

## Verification

The driver-independent surface-format regressions exercise a 72-entry list,
count/list races, shrinking lists, format preferences, query errors and host-budget
rejection:

```sh
out/build/linux-clang-debug/modules/graphics/rhi/ludus_graphics_rhi_tests '[surface-formats]'
```

The normal test suite checks frame status before startup. Compiling the RHI
test with its public header before Volk checks native-handle type compatibility;
linking the Wayland build also checks the platform-specific Volk symbols.

On a host with a Wayland compositor and an eligible Vulkan GPU, run the live
lifecycle test (also supported with `linux-clang-asan-ubsan`):

```sh
./scripts/build linux-clang-debug
LUDUS_TEST_WAYLAND=1 out/build/linux-clang-debug/modules/graphics/rhi/ludus_graphics_rhi_tests '[wayland]'
```

The facade case checks invalid window descriptors, duplicate starts,
idempotent shutdown and restart. The multiple-surface case presents to two windows and injects
`VK_ERROR_SURFACE_LOST_KHR` at acquisition and presentation, in separate
runs. It checks that the lost surface retains its completion counts, the
primary swapchain still presents, and a replacement starts with fresh counters.
Both cases are opt-in. The Linux **Development and SDK** batch runs the
multiple-surface case under headless Weston with Lavapipe and Vulkan validation
layers.

The **project-sdk** workflow’s `wayland_scale` dispatch builds World Demo from
its game sources against the installed Linux SDK. It resizes a floating window
and moves it between 100%, 150% and 200% headless Sway outputs. The check
compares the Platform snapshot with buffers attached by Mesa’s Vulkan driver,
sends Space to the focused window, and checks subsequent presentation. A
separate Weston run exercises the integer-only protocol path at 200%.
