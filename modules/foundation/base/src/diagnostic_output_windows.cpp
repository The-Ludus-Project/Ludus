#include <ludus/foundation/base/diagnostic_output.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

// Windows emergency output: bounded synchronous writes to the standard error
// handle. The report socket and helper control endpoint are POSIX transports and
// are unavailable here, so assertions are report-only and terminal, as in the
// browser backend. Thanks to Microsoft, "WriteFile function
// (fileapi.h)", Win32 API reference:
// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile
namespace ludus::foundation::diagnostics
{
namespace
{
constinit ControlState gControlState = ControlState::Unconfigured;
constexpr uint32 MAX_WRITE_ATTEMPTS = 4;

bool WriteStandardError(const char* data, usize size) noexcept
{
    const HANDLE output = ::GetStdHandle(STD_ERROR_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    const DWORD savedError = ::GetLastError();
    usize offset = 0;
    for (uint32 attempt = 0; attempt < MAX_WRITE_ATTEMPTS && offset < size; ++attempt)
    {
        DWORD written = 0;
        const DWORD request = static_cast<DWORD>(size - offset);
        if (::WriteFile(output, data + offset, request, &written, nullptr) == 0 || written == 0)
        {
            break;
        }
        offset += static_cast<usize>(written);
    }
    ::SetLastError(savedError);
    return offset == size;
}
bool HasEnvironmentValue(const char* name, char* outFirst) noexcept
{
    char value[2] = {};
    const DWORD length = ::GetEnvironmentVariableA(name, value, static_cast<DWORD>(sizeof(value)));
    if (length == 0)
    {
        return false;
    }
    // A longer value reports its required size without writing; read it again.
    if (length >= sizeof(value))
    {
        char first[64] = {};
        if (::GetEnvironmentVariableA(name, first, static_cast<DWORD>(sizeof(first))) == 0)
        {
            return false;
        }
        *outFirst = first[0];
        return true;
    }
    *outFirst = value[0];
    return true;
}
} // namespace

bool ConfigureEmergencySocket(int) noexcept
{
    return false;
}

DeliveryStatus TryWriteEmergencyBytes(const char*, usize) noexcept
{
    // The assertion transport never falls back to standard error.
    return DeliveryStatus::Unavailable;
}

bool WriteEmergencyBytes(const char* data, usize size) noexcept
{
    if (size == 0)
    {
        return true;
    }
    if (data == nullptr)
    {
        return false;
    }
    return WriteStandardError(data, size);
}

ControlState ConfigureControlEndpoint(int) noexcept
{
    gControlState = ControlState::Failed;
    return gControlState;
}

ControlState ControlEndpointState() noexcept
{
    return gControlState;
}

ControlDecision RequestAssertDecision(uint32, const char*, usize) noexcept
{
    return ControlDecision::Terminate;
}

bool IsContinuousIntegration() noexcept
{
    const DWORD savedError = ::GetLastError();
    // The same CI signals as the POSIX backend; LUDUS_CI=0/1 overrides them.
    static const char* const signals[] =
        {"CI", "CONTINUOUS_INTEGRATION", "GITHUB_ACTIONS", "GITLAB_CI", "BUILDKITE", "JENKINS_URL", "TEAMCITY_VERSION"};
    bool isDetected = false;
    char first = '\0';
    for (const char* signal : signals)
    {
        if (HasEnvironmentValue(signal, &first) == true && first != '\0')
        {
            isDetected = true;
            break;
        }
    }
    if (HasEnvironmentValue("LUDUS_CI", &first) == true && first != '\0')
    {
        isDetected = first != '0';
    }
    ::SetLastError(savedError);
    return isDetected;
}
} // namespace ludus::foundation::diagnostics
