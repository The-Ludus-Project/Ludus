#include "internal/crash_capture.h"

// Thanks to the Chromium authors, "Crashpad Overview Design", Overview/Registration/macOS:
// https://github.com/chromium/crashpad/blob/main/doc/overview_design.md . The Mach exception
// handler snapshots externally; this adapter never formats, symbolizes or uploads on a fault.
// The STL objects below are required by Crashpad's API, confined to healthy startup/database
// calls under ADR 0003's private Crashpad boundary. They do not enter engine headers.
#include <base/files/file_path.h>
#include <client/crash_report_database.h>
#include <client/crashpad_client.h>
#include <client/settings.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#import <Foundation/Foundation.h>

namespace ludus::editor
{
namespace
{
crashpad::CrashpadClient crashClient;
bool hasStartedHandler = false;

bool SetDatabasePermissions(crashpad::CrashReportDatabase& inoutDatabase, bool canUpload) noexcept
{
    // Pausing preserves the pending queue. SetUploadsEnabled(false) alone would mark
    // pending reports as skipped, losing automatic delivery after later consent.
    crashpad::Settings* settings = inoutDatabase.GetSettings();
    if (canUpload == false)
    {
        return settings->SetUploadsPaused(true) == true && settings->SetUploadsEnabled(false) == true;
    }
    return settings->SetUploadsEnabled(true) == true && settings->SetUploadsPaused(false) == true;
}
} // namespace

bool StartCrashCapture(const char* handlerPath,
                       const char* databasePath,
                       const char* uploadUrl,
                       const char* eventJson,
                       bool canUpload) noexcept
{
    if (handlerPath == nullptr || databasePath == nullptr || uploadUrl == nullptr || eventJson == nullptr)
    {
        return false;
    }
    const std::unique_ptr<crashpad::CrashReportDatabase> database =
        crashpad::CrashReportDatabase::Initialize(base::FilePath(databasePath));
    if (database == nullptr || SetDatabasePermissions(*database, canUpload) == false)
    {
        return false;
    }
    std::map<std::string, std::string> annotations;
    annotations.emplace("sentry", eventJson);
    // Upstream's hourly throttle permanently skips other reports. Preserve distinct
    // failures and let the service's fixed quota govern admission.
    const std::vector<std::string> arguments{"--no-identify-client-via-url", "--no-rate-limit"};
    hasStartedHandler = crashClient.StartHandler(base::FilePath(handlerPath),
                                                 base::FilePath(databasePath),
                                                 base::FilePath(),
                                                 uploadUrl,
                                                 "",
                                                 annotations,
                                                 arguments,
                                                 true,
                                                 false);
    return hasStartedHandler;
}

bool SetCrashUploadsEnabled(const char* databasePath, bool canUpload) noexcept
{
    if (databasePath == nullptr)
    {
        return false;
    }
    const std::unique_ptr<crashpad::CrashReportDatabase> database =
        crashpad::CrashReportDatabase::Initialize(base::FilePath(databasePath));
    if (database == nullptr || SetDatabasePermissions(*database, canUpload) == false)
    {
        return false;
    }
    if (canUpload == true && hasStartedHandler == true)
    {
        crashClient.RequestRetry();
    }
    return true;
}

bool ReadCrashReportCounts(const char* databasePath,
                           foundation::usize& outPending,
                           foundation::usize& outUploaded) noexcept
{
    if (databasePath == nullptr)
    {
        return false;
    }
    const std::unique_ptr<crashpad::CrashReportDatabase> database =
        crashpad::CrashReportDatabase::Initialize(base::FilePath(databasePath));
    if (database == nullptr)
    {
        return false;
    }
    std::vector<crashpad::CrashReportDatabase::Report> pending;
    std::vector<crashpad::CrashReportDatabase::Report> completed;
    if (database->GetPendingReports(&pending) != crashpad::CrashReportDatabase::kNoError ||
        database->GetCompletedReports(&completed) != crashpad::CrashReportDatabase::kNoError)
    {
        return false;
    }
    foundation::usize uploaded = 0;
    for (const crashpad::CrashReportDatabase::Report& report : completed)
    {
        if (report.uploaded == true)
        {
            ++uploaded;
        }
    }
    outPending = pending.size();
    outUploaded = uploaded;
    return true;
}

bool ReadCrashExecutablePath(char* outPath, foundation::usize capacity) noexcept
{
    if (outPath == nullptr)
    {
        return false;
    }
    @autoreleasepool
    {
        const char* pathOrNull = [[[NSBundle mainBundle] executablePath] UTF8String];
        if (pathOrNull == nullptr)
        {
            return false;
        }
        const foundation::usize length = std::strlen(pathOrNull);
        if (length >= capacity)
        {
            return false;
        }
        std::memcpy(outPath, pathOrNull, length + 1);
        return true;
    }
}
} // namespace ludus::editor
