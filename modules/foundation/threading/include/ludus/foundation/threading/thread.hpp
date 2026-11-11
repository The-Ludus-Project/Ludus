#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/time/time.hpp>

#include <new>
#include <type_traits>

/// @file
/// One joinable native thread plus the calling thread's sleep, yield and identity
/// operations, implemented over FoundationThreading's native backends. Engine and test
/// code uses these instead of `<thread>` and `<chrono>`, whose headers pull `<format>`
/// into every translation unit on libstdc++ (ADR 0003, ADR 0005).
namespace ludus::foundation::threading
{
/// Result of starting or joining a Thread. Every failure leaves the thread object unchanged.
enum class ThreadStatus : uint8
{
    Ok,              ///< The operation succeeded.
    InvalidArgument, ///< The entry function is null.
    InvalidState,    ///< Start while the thread is running, or Join while it is not.
    Unavailable,     ///< The operating system refused to create a thread; browser builds have no threads.
};

/// Nonthrowing thread entry. The context is borrowed until the entry returns.
using ThreadEntry = void (*)(void* contextOrNull) noexcept;

/// Identity of a running thread. Equal values name the same thread while both are alive;
/// a finished thread's value may be reused by a later thread.
struct ThreadId
{
    /// Opaque native identity, never an index into engine storage.
    uint64 Value = 0;
    /// Compare identities read on the same host.
    bool operator==(const ThreadId&) const noexcept = default;
};

/// @brief One joinable native thread with explicit start and join status.
///
/// The object owns the native handle and, for a callable passed by value, an inline copy of
/// that callable; a function pointer's context or a callable passed by reference stays the
/// caller's and must remain valid until Join returns. A started thread must be joined before
/// the object is destroyed, as with `std::thread`, and the object is pinned in memory while
/// the thread runs because the native backend reads the entry from it. Place threads in a
/// StaticArray or a local, never in a container that relocates its elements.
/// Browser builds have no threads: Start reports Unavailable and runs nothing.
class Thread final
{
public:
    /// Bytes reserved for the native handle and entry record; checked against the backend.
    static constexpr usize NATIVE_STORAGE_BYTES = 32;
    /// Bytes of inline storage for a callable passed to Start by value; larger closures are
    /// rejected at compile time so no thread start allocates.
    static constexpr usize INLINE_CALLABLE_BYTES = 64;
    /// Alignment of the inline callable storage.
    static constexpr usize INLINE_CALLABLE_ALIGNMENT = 16;

    /// Construct a thread object that is not running; no native resource is created.
    Thread() noexcept = default;
    /// @pre IsJoinable() == false: a running thread must have been joined.
    ~Thread() noexcept;
    /// Not copyable: the object identifies one native thread.
    Thread(const Thread&) = delete;
    /// Not copy-assignable: the object identifies one native thread.
    Thread& operator=(const Thread&) = delete;
    /// Not movable: the native backend reads the entry record at this object's address.
    Thread(Thread&&) = delete;
    /// Not move-assignable: the native backend reads the entry record at this object's address.
    Thread& operator=(Thread&&) = delete;

    /// @brief Start the thread at entry with the borrowed context.
    /// @param entry Function the new thread runs to completion; null is InvalidArgument.
    /// @param contextOrNull Passed to entry unchanged; the caller keeps it alive until Join returns.
    /// @return Ok once the thread runs; InvalidState while a previous start is unjoined; Unavailable
    ///         when the operating system refuses or the build has no threads. Failure leaves the
    ///         object not running.
    [[nodiscard]] ThreadStatus Start(ThreadEntry entry, void* contextOrNull) noexcept;
    /// @brief Start the thread on a caller-owned callable, invoked once as `inoutCallable()`.
    /// The callable is referenced, not copied: keep it alive and unmoved until Join returns.
    /// Its call must not throw; the engine builds without exceptions.
    template <typename Callable>
    [[nodiscard]] ThreadStatus Start(Callable& inoutCallable) noexcept
    {
        return Start(&invokeCallable<Callable>, static_cast<void*>(&inoutCallable));
    }
    /// @brief Start the thread on a callable moved into the object's inline storage.
    /// The copy lives until Join returns (or until a failed Start returns), so a lambda
    /// written at the call site may capture by reference as long as the referents outlive
    /// Join. Closures larger than INLINE_CALLABLE_BYTES are rejected at compile time.
    template <typename Callable>
        requires(std::is_lvalue_reference_v<Callable> == false)
    [[nodiscard]] ThreadStatus Start(Callable&& callable) noexcept
    {
        using Stored = std::remove_cvref_t<Callable>;
        static_assert(sizeof(Stored) <= INLINE_CALLABLE_BYTES, "closure exceeds Thread::INLINE_CALLABLE_BYTES");
        static_assert(alignof(Stored) <= INLINE_CALLABLE_ALIGNMENT,
                      "closure exceeds Thread::INLINE_CALLABLE_ALIGNMENT");
        static_assert(std::is_nothrow_move_constructible_v<Stored>, "the stored callable must move without throwing");
        if (mIsRunning == true)
        {
            return ThreadStatus::InvalidState;
        }
        Stored* stored = ::new (static_cast<void*>(mCallableStorage)) Stored(static_cast<Callable&&>(callable));
        mDestroyCallableOrNull = &destroyCallable<Stored>;
        const ThreadStatus status = Start(&invokeCallable<Stored>, static_cast<void*>(stored));
        if (status != ThreadStatus::Ok)
        {
            releaseCallable();
        }
        return status;
    }
    /// Whether Start succeeded more recently than Join.
    [[nodiscard]] bool IsJoinable() const noexcept
    {
        return mIsRunning;
    }
    /// @brief Block until the thread's entry returns, then release the native handle.
    /// @return Ok after the thread finished; InvalidState when no thread is running.
    [[nodiscard]] ThreadStatus Join() noexcept;

private:
    template <typename Callable>
    static void invokeCallable(void* contextOrNull) noexcept
    {
        (*static_cast<Callable*>(contextOrNull))();
    }
    template <typename Callable>
    static void destroyCallable(void* storage) noexcept
    {
        static_cast<Callable*>(storage)->~Callable();
    }
    void releaseCallable() noexcept
    {
        if (mDestroyCallableOrNull != nullptr)
        {
            mDestroyCallableOrNull(static_cast<void*>(mCallableStorage));
            mDestroyCallableOrNull = nullptr;
        }
    }

    // Raw arrays keep the native record and the stored closure opaque in this public header:
    // the backend's handle types live in a private header, and FoundationContainers is above
    // this module.
    alignas(alignof(void*)) uint8 mNativeStorage[NATIVE_STORAGE_BYTES] = {};
    alignas(INLINE_CALLABLE_ALIGNMENT) uint8 mCallableStorage[INLINE_CALLABLE_BYTES] = {};
    void (*mDestroyCallableOrNull)(void*) noexcept = nullptr;
    bool mIsRunning = false;
};

/// @brief Block the calling thread for at least interval, at the operating system's granularity.
/// Interrupted waits resume until the interval has elapsed. Browser builds return immediately.
void SleepFor(time::Duration interval) noexcept;

/// Offer the processor to another ready thread; returns immediately when none is ready.
void YieldThread() noexcept;

/// Identity of the calling thread, comparable with values read on other threads of this process.
[[nodiscard]] ThreadId GetCallingThreadId() noexcept;
} // namespace ludus::foundation::threading
