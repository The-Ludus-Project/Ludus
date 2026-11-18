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

1. Call `graphics::rhi::Initialize` to load Vulkan and create the instance.
   Wayland builds enable both `VK_KHR_surface` and `VK_KHR_wayland_surface`.
2. Create a platform window and obtain `window->GetNativeWindowInfo()`.
   This descriptor contains borrowed, typed native handles and a window-system
   tag. It requires neither Vulkan headers nor Wayland client headers.
3. Pass the descriptor to `graphics::rhi::ConnectWindow`. The RHI creates the
   surface, selects a GPU with graphics and presentation support, and requests
   one queue from each selected family (one total when the family is shared).
   The device must support `VK_KHR_swapchain` and swapchain maintenance1 for
   safe presentation retirement.
4. Call `graphics::rhi::Shutdown` before destroying the window or its display.
   Shutdown destroys the device, surface, and instance, then finalizes Volk.

Lifecycle calls must be serialized. Connecting the same window again succeeds;
connecting a different window while one is connected returns false. A failed
connection releases its temporary surface and can be retried. Headless windows
have no presentation connection; the smoke app skips `ConnectWindow` for them.

For swapchains that share a device, use the
[multiple-surface device API](fullscreen-rendering.md#multiple-surfaces-per-device-r14b).

## Verification

The normal test suite checks connection before initialization. Compiling the RHI
test with its public header before Volk checks native-handle type compatibility;
linking the Wayland build also checks the platform-specific Volk symbols.

On a host with a Wayland compositor and an eligible Vulkan GPU, run the live
lifecycle test (also supported with `linux-clang-asan-ubsan`):

```sh
./scripts/build linux-clang-debug
LUDUS_TEST_WAYLAND=1 out/build/linux-clang-debug/modules/graphics/rhi/ludus_graphics_rhi_tests '[wayland]'
```

The facade case checks repeated connection, invalid handles, shutdown and
reinitialization. The multiple-surface case presents to two windows and injects
`VK_ERROR_SURFACE_LOST_KHR` at acquisition and presentation, in separate
runs. It checks that the lost surface retains its completion counts, the
primary swapchain still presents, and a replacement starts with fresh counters.
Both cases are opt-in. The Linux **Development and SDK** batch runs the
multiple-surface case under headless Weston with Lavapipe and Vulkan validation
layers.
