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
    foundation::BorrowedView<const char> Text; ///< UTF-8 Slang or LUSL source, without embedded NUL.
};
/// @brief One selected render-graph configuration value, visible to a LUSL source as an object-like
/// macro (docs/modules/tooling/shader-language.md). Slang sources do not read them.
///
/// The LUSL preprocessor owns the admission rules: unique identifier names and the bounds of
/// `shader_language::MAX_CONFIGURATION_VALUES` and `shader_language::MAX_CONFIGURATION_TEXT_BYTES`.
struct ConfigurationValue final
{
    foundation::BorrowedView<const char> Name;  ///< Macro name, an identifier.
    foundation::BorrowedView<const char> Value; ///< Replacement text, lexed as LUSL tokens; may be empty.
};
/// How a LUSL source reports style findings (naming and bare `bool` operands).
enum class LanguageStyle : foundation::uint8
{
    Strict, ///< Style findings are errors; the engine repository and the default for game projects.
    Warn,   ///< Style findings are warnings.
    Off,    ///< Style findings are not reported.
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
    /// @brief Configuration values for a `.lusl` Source; ignored for Slang sources.
    ///
    /// A Source ending in `.lusl` is lowered to Slang first, with the same library `luslc` uses; the
    /// generated module takes the path `Source + ".slang"` in the compiled snapshot, replacing any
    /// file there. The fields below apply only to such a Source.
    foundation::BorrowedView<const ConfigurationValue> Configuration;
    LanguageStyle Style = LanguageStyle::Strict; ///< Style setting of a `.lusl` Source.
    /// Development bounds checks for a `.lusl` Source; release builds leave this false.
    bool ShouldCheckBounds = false;
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
    /// One 0/1 byte per request configuration value on Ready, set when a `.lusl` Source read it;
    /// empty for Slang sources. Build identity records the values read.
    foundation::Array<foundation::uint8> ConfigurationReadMask;
    /// The Slang module generated from a `.lusl` Source on Ready, for read-only display and build
    /// identity; empty for Slang sources.
    foundation::Array<char> GeneratedSource;
};
/// Most declarations and members one ReflectDeclarations call reports.
inline constexpr foundation::usize MAX_REFLECTED_DECLARATIONS = 1024;
/// Most parameters and fields one ReflectDeclarations call reports.
inline constexpr foundation::usize MAX_REFLECTED_MEMBERS = 8192;

/// What a reflected module-level declaration is.
enum class DeclarationKind : foundation::uint8
{
    Function, ///< A non-generic function; one record per overload.
    Struct,   ///< A struct; its members are its instance fields.
    Constant, ///< A `static const` variable.
};

/// Shape of a reflected type.
enum class ReflectedShape : foundation::uint8
{
    Void,        ///< `void`, a return type only.
    Scalar,      ///< A scalar of ReflectedType::Scalar.
    Vector,      ///< ReflectedType::Rows components.
    Matrix,      ///< ReflectedType::Rows by ReflectedType::Columns.
    Struct,      ///< The struct ReflectedType::Name names.
    Unsupported, ///< Resources, generics and other types a LUSL declaration cannot spell.
};

/// Element type of a reflected scalar, vector or matrix.
enum class ReflectedScalar : foundation::uint8
{
    Bool,    ///< `bool`.
    Int16,   ///< `int16_t`; LUSL `int16`.
    Uint16,  ///< `uint16_t`; LUSL `uint16`.
    Int32,   ///< `int`; LUSL `int32`.
    Uint32,  ///< `uint`; LUSL `uint32`.
    Int64,   ///< `int64_t`; LUSL `int64`.
    Uint64,  ///< `uint64_t`; LUSL `uint64`.
    Float16, ///< `half`; LUSL `float16`.
    Float32, ///< `float`; LUSL `float32`.
    Float64, ///< `double`; LUSL `float64`.
};

/// Direction of a reflected parameter.
enum class ReflectedDirection : foundation::uint8
{
    In,    ///< Read only; Slang's default.
    Out,   ///< Written only.
    Inout, ///< Read and written.
};

/// A span of DeclarationResult::Text.
struct ReflectedText final
{
    foundation::uint32 Offset = 0; ///< First byte.
    foundation::uint32 Size = 0;   ///< Byte count.
};

/// @brief A type as Slang reflection reports it.
struct ReflectedType final
{
    ReflectedShape Shape = ReflectedShape::Unsupported; ///< Category.
    ReflectedScalar Scalar = ReflectedScalar::Float32;  ///< Element of a Scalar, Vector or Matrix.
    foundation::uint8 Rows = 1;                         ///< Components of a Vector; rows of a Matrix.
    foundation::uint8 Columns = 1;                      ///< Columns of a Matrix.
    /// Element count of a one-dimensional array of this type; zero when the type is not an array.
    foundation::uint32 ArrayCount = 0;
    /// Struct name of a Struct; Slang's spelling of an Unsupported type.
    ReflectedText Name;
};

/// @brief A parameter of a reflected function, or a field of a reflected struct.
struct ReflectedMember final
{
    ReflectedText Name;                                    ///< Parameter or field name.
    ReflectedType Type;                                    ///< Its type.
    ReflectedDirection Direction = ReflectedDirection::In; ///< A parameter's direction; In for fields.
};

/// @brief One module-level declaration, in Slang's declaration order.
struct ReflectedDeclaration final
{
    DeclarationKind Kind = DeclarationKind::Function; ///< Category.
    ReflectedText Name;                               ///< Declared name.
    ReflectedType Type;                               ///< Return type of a Function, type of a Constant.
    foundation::uint32 FirstMember = 0;               ///< First of its members in DeclarationResult::Members.
    foundation::uint32 MemberCount = 0;               ///< Parameters of a Function, fields of a Struct.
};

/// @brief Output of Compiler::ReflectDeclarations.
struct DeclarationResult final
{
    Status Outcome = Status::InvalidRequest; ///< Final outcome.
    /// Every function, struct and `static const` the module declares at module level, including
    /// the declarations of the files it includes; generic declarations are skipped. Empty unless Ready.
    foundation::Array<ReflectedDeclaration> Declarations;
    /// Parameters and fields; see ReflectedDeclaration::FirstMember.
    foundation::Array<ReflectedMember> Members;
    /// UTF-8 names that ReflectedText spans index.
    foundation::Array<char> Text;
    foundation::Array<foundation::uint8> Diagnostics; ///< Bounded UTF-8 Slang diagnostics.
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
    /// A `.lusl` Source is first lowered to Slang (see Request::Configuration); LUSL diagnostics use
    /// Slang's `path(line,column): error LUSL0504: message` form, and the generated module's
    /// `#line` directives make Slang's own diagnostics name LUSL lines.
    /// @param request Input views remain alive only for this call; source bytes are copied.
    /// @param outResult Replaced with complete output/status/diagnostics. Failures expose no artifacts.
    /// @return The same status stored in outResult. May allocate; never call on a rendering thread.
    [[nodiscard]] Status Compile(const Request& request, Result& outResult) noexcept;
    /// @brief Load request.Source as a Slang module and report its module-level declarations,
    /// for generating LUSL `extern slang` declarations from Slang reflection.
    ///
    /// Reads only the captured snapshot, like Compile. request.Entry, OutputTarget and EntryStage
    /// are not used, but must be valid. A `.lusl` Source is an InvalidRequest.
    /// @param request The snapshot and the Slang source to reflect.
    /// @param outResult Replaced; declarations are reported only on Ready. CompileFailed when Slang
    /// rejects the module, InvalidRequest beyond MAX_REFLECTED_DECLARATIONS or MAX_REFLECTED_MEMBERS.
    /// @return The same status stored in outResult. May allocate; never call on a rendering thread.
    [[nodiscard]] Status ReflectDeclarations(const Request& request, DeclarationResult& outResult) noexcept;

private:
    /// Compile a validated request whose Source is Slang.
    [[nodiscard]] Status compileSlang(const Request& request, Result& outResult) noexcept;

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
