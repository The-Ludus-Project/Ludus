#pragma once

namespace ludus::editor
{
/// Correct Qt 6.12.0 Cocoa table identity ownership in this process.
/// Native row, column and cell elements no longer delete the table identity they
/// borrow or the cell interfaces that the Qt item-view table still caches; the
/// table releases those cells itself on model changes and destruction.
/// Call on the GUI thread after QApplication loads the Cocoa plugin and before
/// creating editor widgets. Repeated calls are harmless; other Qt versions are unchanged.
/// @return False if the affected runtime does not expose the expected native layout.
[[nodiscard]] bool InstallQtAccessibilityOwnershipFix() noexcept;
} // namespace ludus::editor
