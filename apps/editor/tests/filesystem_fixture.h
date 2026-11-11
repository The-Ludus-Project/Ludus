#pragma once

#include <QString>

/// Creates a real filesystem symlink, including directory links on Windows.
/// QFile::link creates a .lnk shortcut on Windows and cannot test path confinement.
[[nodiscard]] bool CreateTestSymbolicLink(const QString& target, const QString& link);
