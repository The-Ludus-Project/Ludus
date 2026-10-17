#include "fault_allocator.h"

#include <cstdlib>
#include <new>

namespace
{
ludus::foundation::usize gAttempts = 0;
ludus::foundation::usize gFailAt = 0;
bool gCounting = false;
} // namespace

namespace ludus::s6
{
void FailAllocationsFrom(foundation::usize attempt) noexcept
{
    gAttempts = 0;
    gFailAt = attempt;
    gCounting = true;
}
void StopAllocationFailure() noexcept
{
    gCounting = false;
}
foundation::usize AllocationAttempts() noexcept
{
    return gAttempts;
}
} // namespace ludus::s6

// The production FoundationMemory allocator uses precisely this fallible
// aligned form. Inject failures below the actual provider/VM, with no VM mock
// or acceptance hook in an installed public header. The executable is serial.
// Thanks to LLVM's InconsistentDeclarationParameterNameCheck (23.1.3): it
// compares these replacements with libc++'s reserved parameter names. Names are
// not part of the replacement ABI; retain readable names and annotate only the
// two replacement definitions, keeping every other check enabled.
// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang-tools-extra/clang-tidy/readability/InconsistentDeclarationParameterNameCheck.cpp
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void* operator new(ludus::foundation::usize bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    if (gCounting && (++gAttempts >= gFailAt && gFailAt != 0))
    {
        return nullptr;
    }
    const auto align = static_cast<ludus::foundation::usize>(alignment);
    if (bytes == 0)
    {
        bytes = 1;
    }
    if (bytes > ~ludus::foundation::usize{0} - (align - 1))
    {
        return nullptr;
    }
    return std::aligned_alloc(align, (bytes + align - 1) & ~(align - 1));
}
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* block, std::align_val_t) noexcept
{
    std::free(block);
}
