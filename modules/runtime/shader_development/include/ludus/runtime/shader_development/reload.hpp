#pragma once

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>

/// @file
/// Optional development-game consumption of headless shader-worker publications.
namespace ludus::runtime::shader_development
{
/// Runtime package delivery phase, separate from Renderer pipeline readiness.
enum class ReloadState : foundation::uint8
{
    Idle,            ///< No unread publication; the active Renderer generation continues drawing.
    Reading,         ///< An immutable file revision is being read on a dedicated filesystem worker.
    Staged,          ///< A complete package was passed to Renderer; Poll/Update there controls activation.
    Failed,          ///< File, decode or staging failure; active rendering is unchanged.
    RestartRequired, ///< CPU-visible interface mismatch; rebuild/restart the game before applying.
};
/// Failure of the latest delivery attempt; independent of active rendering.
enum class ReloadFailure : foundation::uint8
{
    None,          ///< No delivery failure.
    Unreadable,    ///< File lookup, read or revision validation failed.
    TooLarge,      ///< Empty or over-budget runtime package.
    OutOfMemory,   ///< Owned read/package storage could not be allocated.
    Malformed,     ///< Runtime-package decoding failed.
    StageRejected, ///< Renderer refused the complete candidate; inspect LastStage.
};
/// Delivery report; never describes the last presented image or GPU completion.
struct ReloadReport final
{
    ReloadFailure Failure = ReloadFailure::None; ///< Latest delivery failure.
    ReloadState State = ReloadState::Idle;       ///< Current delivery phase.
    foundation::uint64 Request = 0;              ///< Latest file-read request; stale reads are discarded.
    graphics::rhi::RasterStatus LastStage = graphics::rhi::RasterStatus::Ready; ///< Last Renderer staging outcome.
};
/// Watches one final `.lurgrt` publication and stages it without restarting gameplay.
/// The headless `ludus render-graph watch` producer owns source compilation/cooking;
/// this class owns only bounded file observation, asynchronous reads and delivery.
/// All calls use the Renderer owner thread between frames, outside render critical sections.
/// Metadata observation/open may access disk; payload reads run on a dedicated I/O worker.
/// Destruction drains that worker and belongs to orderly shutdown, never the frame path.
class ReloadSession final
{
public:
    ReloadSession() noexcept;                     ///< Construct without threads, filesystem or graphics work.
    ~ReloadSession() noexcept;                    ///< Drain any outstanding read before releasing its buffer.
    ReloadSession(const ReloadSession&) = delete; ///< Ownership is unique.
    ReloadSession& operator=(const ReloadSession&) = delete; ///< Ownership is unique.
    /// Bind a trusted output directory, relative package path and initialized Renderer activation.
    /// @param root Native directory pathname; copied/pinned during the call.
    /// @param path Relative `.lurgrt` file; may be absent until the first successful build.
    /// @param inoutActivation Outlives this session. Existing active generation is preserved.
    /// @return Ready after watcher/I/O setup; other outcomes leave the session uninitialized.
    [[nodiscard]] graphics::rhi::RasterStatus
    Initialize(foundation::BorrowedView<const char> root,
               foundation::BorrowedView<const char> path,
               graphics::renderer::RenderGraphActivation& inoutActivation) noexcept;
    /// Request a re-read even when metadata did not change. A prior read is cancelled/drained
    /// and its result discarded. Does not cancel an already accepted Renderer candidate.
    void RequestReload() noexcept;
    /// Observe one path and collect/stage complete bytes. nowNanoseconds is a nondecreasing
    /// host monotonic clock value. Call Renderer Poll/Update afterward, then draw its active graph.
    /// Compiler errors leave the final package unchanged and therefore cause no new delivery.
    [[nodiscard]] ReloadReport Service(foundation::uint64 nowNanoseconds) noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
};
} // namespace ludus::runtime::shader_development
