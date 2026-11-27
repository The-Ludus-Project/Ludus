// Raises the default thread stack size of Linux test processes that run reference-compute kernels, before lavapipe
// starts its workers. ludus_add_software_vulkan_thread_stack (modules/graphics/renderer/CMakeLists.txt) adds it.
//
// CI runs these tests on lavapipe with GALLIVM_PERF=nopt (docs/development/continuous-integration.md). That
// profile compiles shaders with LLVM's CodeGenOpt::None, which gives each spilled value its own stack slot, so
// a compute shader's frame grows with the size of the kernel. Mesa runs compute shaders on llvmpipe's thread
// pool, created with default thread attributes, and gallivm emits no stack probes. The reference kernel's
// frame outgrew glibc's 8 MiB default: it ran past the guard page into a neighbouring stack (a SIGSEGV in
// shader code and "stack smashing detected" in another thread). Engine shader artifacts and driver defaults
// are unchanged; only the test process's default for threads created without an explicit size is raised.
//
// Thanks to the Mesa authors: Gallivm's init_gallivm_engine selects CodeGenOpt::None for GALLIVM_PERF_NO_OPT
// (src/gallium/auxiliary/gallivm/lp_bld_init.c), lp_build_nir_soa allocates shader scratch with
// lp_build_array_alloca (src/gallium/auxiliary/gallivm/lp_bld_nir_soa.c) and lp_cs_tpool_create starts its
// workers with u_thread_create (src/gallium/drivers/llvmpipe/lp_cs_tpool.c), Mesa 25.0.7,
// https://gitlab.freedesktop.org/mesa/mesa. Thanks to the Linux man-pages project,
// pthread_setattr_default_np(3), https://man7.org/linux/man-pages/man3/pthread_setattr_default_np.3.html.

#include <ludus/foundation/base/types.h>

#include <pthread.h>

namespace
{

using ludus::foundation::usize;

/// Default stack for threads that do not choose one: eight times glibc's 8 MiB default. Stacks reserve
/// address space and commit pages only as they are touched.
constexpr usize SOFTWARE_VULKAN_THREAD_STACK_BYTES = usize(64) * 1024 * 1024; // ludus-style: magic-number 64 MiB

/// Raises the process default thread stack size to at least SOFTWARE_VULKAN_THREAD_STACK_BYTES. A failure keeps
/// the previous default, so the tests run as they would without this file.
void RaiseDefaultThreadStack() noexcept
{
    pthread_attr_t attributes;
    if (pthread_getattr_default_np(&attributes) != 0)
    {
        return;
    }
    usize stackBytes = 0;
    const usize raisedBytes = SOFTWARE_VULKAN_THREAD_STACK_BYTES;
    if (pthread_attr_getstacksize(&attributes, &stackBytes) == 0 && stackBytes < raisedBytes)
    {
        if (pthread_attr_setstacksize(&attributes, raisedBytes) == 0) // ludus-style: abbreviation POSIX naming
        {
            static_cast<void>(pthread_setattr_default_np(&attributes));
        }
    }
    static_cast<void>(pthread_attr_destroy(&attributes));
}

/// Runs RaiseDefaultThreadStack during static initialization, before Catch2 runs a case that starts a device.
struct DefaultThreadStackRaiser final
{
    DefaultThreadStackRaiser() noexcept
    {
        RaiseDefaultThreadStack();
    }
};

const DefaultThreadStackRaiser DEFAULT_THREAD_STACK_RAISER;

} // namespace
