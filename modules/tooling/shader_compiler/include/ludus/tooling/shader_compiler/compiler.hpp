#pragma once

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

/// @file
/// Headless Slang compilation and an optional asynchronous native worker client.
namespace ludus::tooling::shader_compiler
{
/// Maximum number of source files in one immutable compilation snapshot.
inline constexpr foundation::usize MAX_SOURCE_FILES = 64;
/// Maximum combined UTF-8 source bytes per snapshot, excluding terminators.
inline constexpr foundation::usize MAX_SOURCE_BYTES = foundation::usize{4} * 1024 * 1024;
/// Maximum size of one emitted shader artifact or reflection document.
inline constexpr foundation::usize MAX_ARTIFACT_BYTES = foundation::usize{2} * 1024 * 1024;
/// Maximum framed JSON request/response bytes; includes escaped sources and hex artifacts.
inline constexpr foundation::usize MAX_MESSAGE_BYTES = foundation::usize{16} * 1024 * 1024;
/// Wire schema version; incompatible workers are rejected.
inline constexpr foundation::uint32 PROTOCOL_VERSION = 1;
/// Maximum retained compiler diagnostic bytes, including warnings.
inline constexpr foundation::usize MAX_DIAGNOSTIC_BYTES = foundation::usize{64} * 1024;
/// Maximum entry-point identifier bytes, excluding its terminator.
inline constexpr foundation::usize MAX_ENTRY_BYTES = 128;
/// Supported portable targets; device-specific machine code belongs to the graphics driver.
enum class Target : foundation::uint8
{
    Spirv,         ///< Vulkan SPIR-V 1.3.
    Wgsl,          ///< WebGPU WGSL.
    Metal,         ///< Metal shading language source.
    SpirvRayQuery, ///< Vulkan SPIR-V 1.5 compute with spvRayQueryKHR; rejects raster stages.
    /// Direct3D 12 DXIL (shader model 6.0) compiled and signed by the pinned DXC, with
    /// `LUDUS_DXIL` defined to 1 as for offline builds. Only Windows compilers produce it;
    /// other hosts return Unavailable. An unsigned container is never published.
    Dxil,
};
/// Shader entry-point stage.
enum class Stage : foundation::uint8
{
    Vertex,   ///< Raster vertex stage.
    Fragment, ///< Raster fragment stage.
    Compute,  ///< Compute stage.
};
/// Compilation/client outcome. Failure never publishes partial shader artifacts.
enum class Status : foundation::uint8
{
    Ready,           ///< Complete output is available.
    Pending,         ///< Client I/O is in progress; poll again between frames.
    InvalidRequest,  ///< Bad version, path, stage, target or bounds.
    CompileFailed,   ///< Slang rejected the shader; diagnostics describe the failure.
    Unavailable,     ///< Worker, platform or pinned compiler is unavailable.
    OutOfMemory,     ///< Ludus-owned output storage could not be allocated.
    TransportFailed, ///< Disconnected peer, malformed reply or native I/O failure.
    InvalidState,    ///< Operation is incompatible with the current client state.
};
/// Complete source bytes, borrowed only during Compile/Submit. No filesystem fallback occurs.
struct SourceFile final
{
    foundation::BorrowedView<const char> Path; ///< Unique relative UTF-8 path, without NUL/dot/parent components.
    foundation::BorrowedView<const char> Text; ///< UTF-8 Slang source, without embedded NUL.
};
/// An immutable compilation snapshot and explicit entry selection.
struct Request final
{
    foundation::uint64 Identity = 0;                  ///< Caller-owned revision/request identity, echoed unchanged.
    foundation::BorrowedView<const SourceFile> Files; ///< Entire transitive source closure, including Source.
    foundation::BorrowedView<const char> Source;      ///< Path of the root file in Files.
    foundation::BorrowedView<const char> Entry;       ///< Entry-point identifier.
    Target OutputTarget = Target::Spirv;              ///< Requested portable artifact.
    Stage EntryStage = Stage::Vertex;                 ///< Stage used to check an unannotated entry.
};
/// Owned compilation output; caller storage remains independent of compiler/client lifetime.
struct Result final
{
    foundation::uint64 Identity = 0;         ///< Echoed request identity; stale results must be rejected by the caller.
    Status Outcome = Status::InvalidRequest; ///< Final compilation outcome.
    foundation::Array<foundation::uint8> Artifact; ///< Complete target bytes on Ready; empty on failure.
    foundation::Array<foundation::uint8>
        Reflection; ///< Slang target JSON plus nativeEntryPointResourceUsage (name to generated-code usage).
    foundation::Array<foundation::uint8>
        SourceReadMask; ///< One 0/1 byte per request file on Ready, recording actual compiler reads.
    foundation::Array<foundation::uint8> Diagnostics; ///< Bounded UTF-8 diagnostic bytes, including warnings.
};
/// Optional native compiler library. Does not initialize an engine, editor, device or window.
/// Single-thread-affine: all operations and destruction use its creating thread.
/// One global Slang session survives requests. Only the latest identical source snapshot
/// and ray-query profile reuse a module/session; changes replace it before loading sources.
class Compiler final
{
public:
    Compiler() noexcept;                           ///< Construct without initializing Slang.
    ~Compiler() noexcept;                          ///< Release session objects before the global compiler.
    Compiler(const Compiler&) = delete;            ///< Compiler ownership is unique.
    Compiler& operator=(const Compiler&) = delete; ///< Compiler ownership is unique.
    /// Initialize the pinned Slang API. Repeated initialization returns InvalidState.
    /// Returns Unavailable when the pinned version or its SPIR-V optimizer library cannot be
    /// loaded; Slang would otherwise emit unoptimized SPIR-V that differs from slangc's output.
    /// On Windows it also returns Unavailable unless the pinned `dxcompiler.dll` loads from the
    /// executable's directory, where `dxil.dll` must sit to sign DXIL output.
    [[nodiscard]] Status Initialize() noexcept;
    /// Synchronously compile captured sources, with no disk reads through the Slang filesystem.
    /// @param request Input views remain alive only for this call; source bytes are copied.
    /// @param outResult Replaced with complete output/status/diagnostics. Failures expose no artifacts.
    /// @return The same status stored in outResult. May allocate; never call on a rendering thread.
    [[nodiscard]] Status Compile(const Request& request, Result& outResult) noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
};
/// Headless native IPC client: a private Unix socket on Linux/macOS and a local named pipe
/// on Windows. No Slang library is linked into this target; other platforms are Unavailable.
/// Not thread-safe; Submit copies its snapshot, Poll never waits for compiler completion.
/// Callers keep drawing their active generation until a Ready result is validated and staged.
class Client final
{
public:
    Client() noexcept;                         ///< Construct an idle client.
    ~Client() noexcept;                        ///< Cancel pending I/O and release owned buffers.
    Client(const Client&) = delete;            ///< Transport ownership is unique.
    Client& operator=(const Client&) = delete; ///< Transport ownership is unique.
    /// Connect to an existing worker's endpoint and enqueue one copied request.
    /// Connection never waits: a missing or fully occupied worker returns Unavailable.
    /// @param endpoint UTF-8 endpoint without NUL: a socket pathname shorter than the platform
    /// socket limit on Linux/macOS, or a local pipe name starting with `\\.\pipe\` on Windows.
    /// @param request Complete source snapshot and identity; copied before Submit returns.
    /// @return Pending on admission; InvalidState when another request is active.
    [[nodiscard]] Status Submit(foundation::BorrowedView<const char> endpoint, const Request& request) noexcept;
    /// Advance nonblocking I/O. Pending preserves outResult; completion replaces it and returns idle.
    [[nodiscard]] Status Poll(Result& outResult) noexcept;
    /// Discard pending work locally. The worker may finish it, but its result cannot publish here.
    /// On Windows this waits only for the cancelled native I/O to release the client's buffers.
    void Cancel() noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
};
} // namespace ludus::tooling::shader_compiler
