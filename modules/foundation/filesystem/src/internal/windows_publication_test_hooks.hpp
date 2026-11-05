#pragma once

namespace ludus::foundation::filesystem::detail
{
// Test-only process interruption boundary. The production archive has no hook.
void InterruptWindowsPublication(bool isPublished) noexcept;
} // namespace ludus::foundation::filesystem::detail
