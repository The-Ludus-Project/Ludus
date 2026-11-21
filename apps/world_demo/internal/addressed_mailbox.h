#pragma once
#include "cancellation.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <limits>
#include <type_traits>

namespace ludus::world_demo
{
/// Stable producer ordinals of addressed messages. Their order breaks ties between messages admitted at the same
/// completed tick, so delivery never depends on which producer happened to admit first.
enum class MessageProducer : uint8
{
    /// Session, tools and other callers admitting between ticks.
    Session,
    /// Gameplay systems admitting during a tick; their messages are delivered at a later tick.
    Gameplay,
};
/// Number of MessageProducer values; one admission sequence counter each.
inline constexpr usize MESSAGE_PRODUCER_COUNT = 2;

/// Total delivery order of an addressed message: admission tick, then producer ordinal, then the producer's own
/// admission sequence. The key is independent of storage slots and of how frames partition ticks.
struct MessageKey final
{
    /// The owner's completed tick when the message was admitted.
    uint64 AdmissionTick = 0;
    /// Producer that admitted the message.
    MessageProducer Producer = MessageProducer::Session;
    /// One-based admission sequence within that producer; never reused.
    uint64 Sequence = 0;
};

/// Whether `first` is delivered before `second`; a strict total order over keys from one mailbox.
[[nodiscard]] constexpr bool IsMessageKeyBefore(const MessageKey& first, const MessageKey& second) noexcept
{
    if (first.AdmissionTick != second.AdmissionTick)
    {
        return first.AdmissionTick < second.AdmissionTick;
    }
    if (first.Producer != second.Producer)
    {
        return static_cast<uint8>(first.Producer) < static_cast<uint8>(second.Producer);
    }
    return first.Sequence < second.Sequence;
}

/// Bounded mailbox of typed messages addressed to entities
/// (docs/architecture/gameplay-foundations.md#calls-commands-events-and-asynchronous-results).
///
/// The protocol: producers Admit owned payloads with a checked target and an optional cancellation scope; the owner
/// Captures the pending batch at its declared capture point, which sorts it by MessageKey; it delivers the captured
/// batch in that order and Releases it once the delivery is published. Messages admitted while a batch is captured
/// stay pending for the next capture, so delivery never drains recursively. Admission fails with CapacityExceeded
/// once pending and captured messages fill Capacity; nothing is ever dropped after admission.
///
/// The mailbox stores values only: it validates neither targets nor scopes, which the owner checks at admission and
/// again at delivery. Every operation is bounded and allocates nothing. Not thread-safe.
/// @tparam Payload Trivially copyable message payload; one mailbox per payload type keeps each domain typed.
/// @tparam Capacity Most pending plus captured messages.
template <typename Payload, usize Capacity>
class AddressedMailbox final
{
public:
    static_assert(Capacity > 0);
    static_assert(std::is_trivially_copyable_v<Payload>);

    /// One admitted message.
    struct Message final
    {
        /// One-based token returned at admission; unique within this mailbox.
        uint64 Token = 0;
        /// Addressed entity, checked by the owner again at delivery.
        EntityId Target{};
        /// Cancellation scope recorded at admission; revalidated at delivery.
        CancellationToken Scope{};
        /// Delivery order.
        MessageKey Key{};
        /// Owned payload.
        Payload Value{};
    };

    /// Admit a message for the next capture.
    /// @param target Addressed entity; the owner has validated it.
    /// @param scope Cancellation scope, or the default token.
    /// @param producer Admitting producer.
    /// @param admissionTick Owner's completed tick at admission.
    /// @param value Payload copied into the mailbox.
    /// @param outToken Receives the message token; unchanged on failure.
    /// @return CapacityExceeded when pending and captured messages fill Capacity; InvalidConfiguration for an
    /// unknown producer; IdentityExhausted when the token or the producer's sequence would wrap. Failure admits
    /// nothing and changes no counter.
    [[nodiscard]] Status Admit(EntityId target,
                               CancellationToken scope,
                               MessageProducer producer,
                               uint64 admissionTick,
                               const Payload& value,
                               uint64& outToken) noexcept
    {
        const usize producerIndex = static_cast<usize>(producer);
        if (producerIndex >= MESSAGE_PRODUCER_COUNT)
        {
            return Status::InvalidConfiguration;
        }
        if (GetOccupancy() == Capacity)
        {
            return Status::CapacityExceeded;
        }
        if (mNextToken == std::numeric_limits<uint64>::max() ||
            mNextSequence[producerIndex] == std::numeric_limits<uint64>::max())
        {
            return Status::IdentityExhausted;
        }
        const uint64 token = mNextToken++;
        mPending[mPendingCount++] =
        {
            .Token = token,
            .Target = target,
            .Scope = scope,
            .Key = { .AdmissionTick = admissionTick, .Producer = producer, .Sequence = mNextSequence[producerIndex]++ },
            .Value = value,
        };
        outToken = token;
        return Status::Success;
    }
    /// Capture every pending message as the delivery batch, sorted by MessageKey.
    /// @return InvalidPhase while a previously captured batch is unreleased; nothing moves.
    [[nodiscard]] Status Capture() noexcept
    {
        if (mCapturedCount != 0)
        {
            return Status::InvalidPhase;
        }
        for (usize index = 0; index < mPendingCount; ++index)
        {
            mCaptured[index] = mPending[index];
        }
        mCapturedCount = mPendingCount;
        mPendingCount = 0;
        foundation::Sort(mCaptured.GetData(), mCapturedCount, [](const Message& first, const Message& second) {
            return IsMessageKeyBefore(first.Key, second.Key);
        });
        return Status::Success;
    }
    /// Release the captured batch after its delivery was published.
    void ReleaseCaptured() noexcept
    {
        mCapturedCount = 0;
    }
    /// Drop every pending and captured message; tokens and sequences are never reused.
    void Clear() noexcept
    {
        mPendingCount = 0;
        mCapturedCount = 0;
    }
    /// The captured batch in delivery order; valid until the next Capture, ReleaseCaptured or Clear.
    [[nodiscard]] foundation::BorrowedView<const Message> GetCaptured() const noexcept
    {
        return {mCaptured.GetData(), mCapturedCount};
    }
    /// Messages awaiting the next capture, in admission order; valid until the next mutation.
    [[nodiscard]] foundation::BorrowedView<const Message> GetPending() const noexcept
    {
        return {mPending.GetData(), mPendingCount};
    }
    /// Pending plus captured messages; admission requires this to be below Capacity.
    [[nodiscard]] LUDUS_INLINE usize GetOccupancy() const noexcept
    {
        return mPendingCount + mCapturedCount;
    }

private:
    foundation::StaticArray<Message, Capacity> mPending{};
    foundation::StaticArray<Message, Capacity> mCaptured{};
    foundation::StaticArray<uint64, MESSAGE_PRODUCER_COUNT> mNextSequence{{1, 1}};
    usize mPendingCount = 0;
    usize mCapturedCount = 0;
    uint64 mNextToken = 1;
};
} // namespace ludus::world_demo
