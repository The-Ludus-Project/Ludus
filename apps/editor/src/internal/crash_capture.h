#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::editor
{
/// Private native adapter. All strings are borrowed for the call; initialization copies them.
/// Call once on the main thread before QApplication. It captures locally even without an endpoint.
/// Failure leaves the editor usable. It installs OS crash capture, not C++ exception handling.
bool StartCrashCapture(const char* handlerPath,
                       const char* databasePath,
                       const char* uploadUrl,
                       const char* eventJson,
                       bool canUpload) noexcept;
/// Change the database's upload permission from healthy main-thread code. An in-flight request may finish.
bool SetCrashUploadsEnabled(const char* databasePath, bool canUpload) noexcept;
/// Read counts without exposing upstream containers. Outputs are preserved on failure.
bool ReadCrashReportCounts(const char* databasePath,
                           foundation::usize& outPending,
                           foundation::usize& outUploaded) noexcept;
/// Resolve the real executable before Qt initialization. Output is a null-terminated UTF-8 path.
/// Returns false without modifying output if it does not fit.
bool ReadCrashExecutablePath(char* outPath, foundation::usize capacity) noexcept;
} // namespace ludus::editor
