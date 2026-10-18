#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/strings/status.hpp>

#include <atomic>

namespace ludus::foundation::detail
{
// Issues each nonzero token once. Issuing the largest token leaves zero, which
// reports exhaustion instead of wrapping into a reused token. Reduced-width
// instantiation is an internal test seam only.
template <typename Token>
[[nodiscard]] StringStatus IssueTableToken(std::atomic<Token>& inoutNextToken, Token& outToken) noexcept
{
    Token token = inoutNextToken.load(std::memory_order_relaxed);
    while (token != 0)
    {
        if (inoutNextToken.compare_exchange_weak(token, static_cast<Token>(token + 1), std::memory_order_relaxed) ==
            true)
        {
            outToken = token;
            return StringStatus::Ok;
        }
    }
    return StringStatus::TokenExhausted;
}
// Process-lifetime issuer behind CreateTable. Private seam: tests move it next to
// exhaustion and restore it; the installed API cannot reach it.
[[nodiscard]] std::atomic<uint32>& GetTableTokenIssuer() noexcept;
} // namespace ludus::foundation::detail
