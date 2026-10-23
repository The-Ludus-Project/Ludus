#pragma once

namespace ludus::editor
{
/// Correct Qt 6.12.0 Cocoa table placeholder identity ownership in this process.
/// Call on the GUI thread after QApplication loads the Cocoa plugin and before
/// creating editor widgets. Repeated calls are harmless; other Qt versions are unchanged.
/// @return False if the affected runtime does not expose the expected native layout.
[[nodiscard]] bool InstallQtAccessibilityOwnershipFix() noexcept;
} // namespace ludus::editor
