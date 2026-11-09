#pragma once

#include <ludus/tooling/shader_compiler/compiler.hpp>

#include <ludus/foundation/base/types.h>

#include <string_view>

// Native process and transport services of ludus-shader-worker. main.cpp owns the
// command line and the length-prefixed framing; each platform file owns its endpoint.
namespace ludus::tools::shader_worker
{
/// Exit status for a malformed command line or endpoint.
inline constexpr foundation::int32 EXIT_USAGE = 2;
/// Exit status for a native setup or transport failure.
inline constexpr foundation::int32 EXIT_FAILURE_STATUS = 1;
/// Milliseconds of transport inactivity after which a connection is abandoned.
inline constexpr foundation::int32 IO_TIMEOUT_MILLISECONDS = 30000;
/// Endpoint connections kept ready while every lane compiles.
inline constexpr foundation::int32 CONNECTION_BACKLOG = 16;
/// Most connections a POSIX endpoint compiles at once, each on its own thread and Slang session.
/// The endpoint uses half the online CPUs up to this bound; Windows and standard streams use one.
inline constexpr foundation::uint32 MAX_CONNECTION_LANES = 4;
/// One bidirectional connection or standard-stream pair.
class Stream
{
public:
    /// Move exactly size bytes, waiting at most the platform I/O timeout between progress.
    /// @return false on timeout, end of stream or native failure.
    [[nodiscard]] virtual bool Transfer(foundation::uint8* bytes, foundation::usize size, bool isWrite) noexcept = 0;

protected:
    Stream() noexcept = default;
    ~Stream() noexcept = default;
    Stream(const Stream&) = default;
    Stream& operator=(const Stream&) = default;
};
/// Read one framed request, compile it and write one framed reply. Defined in main.cpp.
/// @param connectionLanes Requests the serving endpoint compiles at once, reported to the client.
[[nodiscard]] bool ProcessConnection(tooling::shader_compiler::Compiler& inoutCompiler,
                                     Stream& inoutStream,
                                     foundation::uint32 connectionLanes) noexcept;
/// Write the --version line to standard output.
[[nodiscard]] bool WriteVersion(std::string_view text) noexcept;
/// Stop at once on a termination request, even inside a Slang call.
[[nodiscard]] bool InstallStopHandlers() noexcept;
/// Stop when the process that started this worker exits, including a crash or forced kill.
[[nodiscard]] bool WatchParent() noexcept;
/// Serve framed requests over standard input/output until end of input.
[[nodiscard]] foundation::int32 ServeStandardStreams(tooling::shader_compiler::Compiler& inoutCompiler) noexcept;
/// Create the private endpoint, refusing an occupied one, and serve connections. POSIX endpoints
/// compile up to MAX_CONNECTION_LANES connections at once; inoutCompiler serves the first lane and
/// further lanes initialize their own compiler on first use. Windows serves one at a time.
[[nodiscard]] foundation::int32 ServeEndpoint(tooling::shader_compiler::Compiler& inoutCompiler,
                                              std::string_view endpoint) noexcept;
} // namespace ludus::tools::shader_worker
