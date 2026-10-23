#pragma once

#include <QString>

QT_BEGIN_NAMESPACE
class QWidget;
class QSettings;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Initialize local capture once, before QApplication, using persisted preferences and bundled handler.
/// No environment variables or special launcher are required. Main thread only.
void InitializeEditorCrashReporting() noexcept;
/// Open private crash preferences and report counts; no report is sent without the user's consent.
void ShowEditorCrashReporting(QWidget* parentOrNull);
/// Present consent once per configured destination, or report a startup failure, after the window is shown.
void PresentEditorCrashReportingNotice(QWidget* parentOrNull);
/// Local bounded log directory, available even if crash-handler startup failed.
QString GetEditorLogDirectory();
/// Convert an HTTPS DSN to Sentry's minidump endpoint. Returns empty for invalid/unsafe DSNs.
QString MakeSentryMinidumpUrl(const QString& dsn);
/// Consent is bound to the exact destination the user approved, including across app updates.
bool HasEditorCrashUploadConsent(const QSettings& settings, const QString& dsn);
} // namespace ludus::editor
