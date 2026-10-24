#pragma once

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/platform/base/window.h>

// Private Win32 windowing backend (docs/architecture/windows-direct3d12.md).
// Callers include this header before <windows.h>, or #undef CreateWindow after
// it: the Windows SDK defines CreateWindow as a macro.
namespace ludus::platform::win32
{
/// Registers the Ludus window class and opts the process into per-monitor DPI.
/// Idempotent; call on the thread that creates and pumps the windows.
bool Initialize(const WindowManager::InitializeInfo& info) noexcept;
/// Creates and shows a top-level window owned by the calling thread; nullptr on failure.
foundation::UniquePtr<Window> CreateWindow(const Window::CreateInfo& info) noexcept;
} // namespace ludus::platform::win32
