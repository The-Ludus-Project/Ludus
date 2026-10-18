#include <ludus/foundation/base/config.h>

#include <ludus/diagnostics/session.hpp>

#include <ludus/foundation/base/assert_config.hpp>
#include <ludus/foundation/base/diagnostic_output.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace fb = ludus::foundation;
namespace fbd = ludus::foundation::diagnostics;

namespace ludus::diagnostics
{
namespace
{
// Descriptors passed by the external diagnostic helper. The helper owns the
// collector ends; the engine only duplicates and reads what it is given.
constexpr const char* REPORT_FD_ENV = "LUDUS_DIAGNOSTIC_REPORT_FD";
constexpr const char* CONTROL_FD_ENV = "LUDUS_DIAGNOSTIC_CONTROL_FD";
// Local Debug opt-out. "0" forces report-only even on an interactive terminal.
constexpr const char* INTERACTIVE_ENV = "LUDUS_DIAGNOSTIC_INTERACTIVE";

// SDK-owned capability: only a non-CI Debug build may request presentation.
constexpr bool BUILD_ELIGIBLE = LUDUS_ASSERT_DIALOGS_AVAILABLE == 1;

// Parse a small nonnegative descriptor number without allocation. Returns -1 for
// absent/empty/malformed values so a bad env never selects a fd.
int ReadDescriptor(const char* name) noexcept
{
    const char* value = ::getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return -1;
    }
    int result = 0;
    for (const char* cursor = value; *cursor != '\0'; ++cursor)
    {
        if (*cursor < '0' || *cursor > '9' || result > 100000000)
        {
            return -1;
        }
        result = result * 10 + (*cursor - '0');
    }
    return result;
}

#if defined(LUDUS_PLATFORM_LINUX)
// Nonblocking connect to a Unix display socket without retaining it. Success
// proves a live endpoint is reachable, unlike a mere PATH/env lookup.
bool CanConnectUnix(const char* path) noexcept
{
    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const fb::usize limit = sizeof(address.sun_path) - 1;
    fb::usize length = 0;
    while (path[length] != '\0' && length < limit)
    {
        address.sun_path[length] = path[length];
        ++length;
    }
    if (path[length] != '\0')
    {
        return false; // Path too long for the address; treat as unavailable.
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        return false;
    }
    const int result = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    const bool reachable = result == 0 || errno == EINPROGRESS || errno == EISCONN;
    (void)::close(fd);
    return reachable;
}

// A found dialog executable is NOT proof of presentation: validate a live
// display connection (Wayland/X11 socket), not merely env-var presence.
bool HasGraphicalDisplay() noexcept
{
    const int saved_errno = errno;
    bool available = false;

    if (const char* wayland = ::getenv("WAYLAND_DISPLAY"); wayland != nullptr && wayland[0] != '\0')
    {
        if (wayland[0] == '/')
        {
            available = CanConnectUnix(wayland);
        }
        else if (const char* runtime = ::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && runtime[0] != '\0')
        {
            char path[256];
            const fb::usize runtime_length = ::strnlen(runtime, sizeof(path));
            const fb::usize name_length = ::strnlen(wayland, sizeof(path));
            if (runtime_length + 1 + name_length < sizeof(path))
            {
                fb::usize offset = 0;
                ::memcpy(path, runtime, runtime_length);
                offset += runtime_length;
                path[offset++] = '/';
                ::memcpy(path + offset, wayland, name_length);
                offset += name_length;
                path[offset] = '\0';
                available = CanConnectUnix(path);
            }
        }
    }

    if (available == false)
    {
        if (const char* display = ::getenv("DISPLAY"); display != nullptr && display[0] == ':')
        {
            char path[64];
            fb::usize offset = 0;
            const char prefix[] = "/tmp/.X11-unix/X";
            ::memcpy(path, prefix, sizeof(prefix) - 1);
            offset += sizeof(prefix) - 1;
            for (fb::usize i = 1; display[i] != '\0' && display[i] != '.' && offset < sizeof(path) - 1; ++i)
            {
                path[offset++] = display[i];
            }
            path[offset] = '\0';
            available = CanConnectUnix(path);
        }
    }

    errno = saved_errno;
    return available;
}

#else
bool HasGraphicalDisplay() noexcept
{
    // Cocoa presentation is deferred; never mistake DISPLAY for a native UI.
    return false;
}
#endif

bool HasInteractiveTerminal() noexcept
{
    return ::isatty(STDIN_FILENO) == 1 && ::isatty(STDOUT_FILENO) == 1;
}

// Visible notice for a failed interactive setup. Uses the ordinary blocking
// emergency writer (Logging's channel), never the nonblocking assertion
// transport: this is a startup diagnostic, not a failure report.
void ReportVisible(const char* message) noexcept
{
    fb::usize length = 0;
    while (message[length] != '\0')
    {
        ++length;
    }
    (void)fbd::WriteEmergencyBytes(message, length);
}
} // namespace

SessionResult InitializeDiagnosticSession(Interactivity requested) noexcept
{
    const int saved_errno = errno;
    SessionResult result{};

    // 1) Report transport: reuse Base's nonblocking datagram socket.
    const int report_fd = ReadDescriptor(REPORT_FD_ENV);
    if (report_fd >= 0)
    {
        result.ReportTransportReady = fbd::ConfigureEmergencySocket(report_fd);
    }

    // 2) CI detection (shared Base primitive; also the later failure-path veto).
    result.DetectedCi = fbd::IsContinuousIntegration();

    bool opt_out = requested == Interactivity::ReportOnly;
    if (const char* value = ::getenv(INTERACTIVE_ENV); value != nullptr && value[0] == '0')
    {
        opt_out = true; // Local headless Debug opts out explicitly.
    }

    const bool interactive_intended = BUILD_ELIGIBLE && result.DetectedCi == false && opt_out == false;

    if (interactive_intended)
    {
        // 3) Configure the control endpoint only when interactive is intended.
        const int control_fd = ReadDescriptor(CONTROL_FD_ENV);
        if (control_fd >= 0)
        {
            result.ControlEndpointReady = fbd::ConfigureControlEndpoint(control_fd) == fbd::ControlState::Ready;
        }

        // 4) Validate real presentation capability: a usable terminal or a live
        //    display, AND a completed control handshake.
        const bool presentable = (HasInteractiveTerminal() || HasGraphicalDisplay()) && result.ControlEndpointReady;
        if (presentable)
        {
            result.Mode = SessionMode::Interactive;
        }
        else
        {
            result.Mode = SessionMode::ReportOnly;
            result.InteractiveSetupFailed = true;
            ReportVisible("[LUDUS diagnostics] interactive assertion setup unavailable "
                          "(no usable terminal/display or control endpoint); using report-only mode\n");
        }
    }
    else
    {
        result.Mode = SessionMode::ReportOnly;
    }

    errno = saved_errno;
    return result;
}
} // namespace ludus::diagnostics
