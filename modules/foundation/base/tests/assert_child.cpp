#include <ludus/foundation/base/assert_format.hpp>

#include "assert_transport.hpp"
#include "internal/diagnostic_record.hpp"

#include <atomic>
#include <barrier>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace ludus::foundation;

#if defined(__cpp_exceptions)
#    error "Native assertion child must use production exception policy"
#endif

namespace
{
void Mark(const char* message)
{
    (void)::write(STDOUT_FILENO, message, std::strlen(message));
}

void AtExit()
{
    Mark("ATEXIT-RAN\n");
}

struct Cleanup
{
    ~Cleanup()
    {
        Mark("DESTRUCTOR-RAN\n");
    }
};

int gConditions = 0;
int gMessages = 0;
std::atomic<bool> gEntered{false};
std::atomic<bool> gRelease{false};

bool Condition()
{
    ++gConditions;
    Mark("CONDITION\n");
    return false;
}

const char* Message()
{
    ++gMessages;
    Mark("MESSAGE\n");
    if (gConditions != 1 || gMessages != 1)
    {
        std::_Exit(80);
    }
    return "child-reason";
}

const char* RecursiveFatal()
{
    Mark("OUTER-MESSAGE\n");
    LUDUS_FATAL("inner-must-not-be-evaluated");
}

const char* RecursiveCheck()
{
    if (LUDUS_CHECK(false, (++gMessages, "inner-check-must-be-suppressed")))
    {
        std::_Exit(81);
    }
    return "outer-check";
}

const char* HoldReport()
{
    gEntered.store(true, std::memory_order_release);
    while (gRelease.load(std::memory_order_acquire) == false)
    {
        std::this_thread::yield();
    }
    return "owner-report";
}

int Contention(bool fatal)
{
    std::thread owner([] { (void)LUDUS_CHECK(false, HoldReport()); });
    while (gEntered.load(std::memory_order_acquire) == false)
    {
        std::this_thread::yield();
    }
    if (fatal)
    {
        LUDUS_FATAL((Mark("BAD-SECONDARY-MESSAGE\n"), "secondary"));
    }
    const bool failed = LUDUS_CHECK(false, (++gMessages, "contended-message"));
    gRelease.store(true, std::memory_order_release);
    owner.join();
    const bool again = LUDUS_CHECK(false, "next-owner");
    return failed == false && again == false && gMessages == 0 ? 0 : 82;
}

int Budget(bool fatal)
{
    int messages = 0;
    int conditions = 0;
    for (int i = 0; i < 80; ++i)
    {
        if (LUDUS_CHECK((++conditions, false), (++messages, "budget-report")))
        {
            return 83;
        }
    }
    const bool successful_check = LUDUS_CHECK((++conditions, true));
    if (successful_check == false || conditions != 81 || messages != 63)
    {
        return 84;
    }
    if (fatal)
    {
        LUDUS_FATAL("fatal-bypasses-budget");
    }
    return 0;
}

// This runs before main and requires no logger or runtime initialization.
const bool gBeforeMain = [] {
    ConfigureTestTransport();
    return LUDUS_CHECK(false, "before-main");
}();

struct AfterMain
{
    ~AfterMain()
    {
        if (LUDUS_CHECK(false, "after-main"))
        {
            std::_Exit(85);
        }
    }
} gAfterMain;
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || gBeforeMain)
    {
        return 90;
    }
    const char* mode = argv[1];
    if (std::strcmp(mode, "budget") == 0 || std::strcmp(mode, "budget-fatal") == 0)
    {
        // This executable's before-main Check consumed one report.
        return Budget(std::strcmp(mode, "budget-fatal") == 0);
    }
    if (std::strcmp(mode, "contention") == 0 || std::strcmp(mode, "secondary") == 0)
    {
        return Contention(std::strcmp(mode, "secondary") == 0);
    }
    if (std::strcmp(mode, "recursive-check") == 0)
    {
        const bool result = LUDUS_CHECK(false, RecursiveCheck());
        const bool next = LUDUS_CHECK(false, "after-recursion");
        return result == false && next == false && gMessages == 0 ? 0 : 86;
    }
    if (std::strcmp(mode, "text") == 0)
    {
        char text[1300];
        for (usize i = 0; i < sizeof(text) - 1; ++i)
        {
            text[i] = 'x';
        }
        text[sizeof(text) - 1] = '\0';
        const bool result = LUDUS_CHECK(false, text);
        const bool escaped = LUDUS_CHECK(false, "line\n\t\x1b\\end");
        return result == false && escaped == false ? 0 : 87;
    }
    if (std::strcmp(mode, "metadata") == 0)
    {
#define LUDUS_TEST_FALSE_TOKEN false
        const bool result = LUDUS_CHECK(LUDUS_TEST_FALSE_TOKEN, "literal {} stays literal");
#undef LUDUS_TEST_FALSE_TOKEN
        return result ? 88 : 0;
    }
    if (std::strcmp(mode, "lifetime") == 0)
    {
        return 0;
    }
    if (std::strcmp(mode, "closed-output") == 0)
    {
        int descriptors[2];
        if (::pipe(descriptors) != 0)
        {
            return 93;
        }
        (void)::close(descriptors[0]);
        if (::dup2(descriptors[1], STDERR_FILENO) < 0)
        {
            return 94;
        }
        (void)::close(descriptors[1]);
        return LUDUS_CHECK(false, "closed pipe must not kill Check") ? 95 : 0;
    }
    if (std::strcmp(mode, "primary-secondary") == 0)
    {
        std::thread primary([] { LUDUS_FATAL(HoldReport()); });
        while (gEntered.load(std::memory_order_acquire) == false)
        {
            std::this_thread::yield();
        }
        const auto& packet = diagnostics::internal::gFatalPacket;
        if (packet.MinimalReady.load(std::memory_order_acquire) == false ||
            packet.CompleteReady.load(std::memory_order_acquire) ||
            std::strstr(packet.Minimal, "<unconditional>") == nullptr)
        {
            std::_Exit(69);
        }
        LUDUS_REQUIRE(false, (Mark("BAD-SECONDARY-MESSAGE\n"), "must-not-overwrite-primary"));
    }
    if (std::strcmp(mode, "identity") == 0)
    {
        char long_text[1025];
        for (usize i = 0; i < 1024; ++i)
        {
            long_text[i] = '\n';
        }
        long_text[1024] = '\0';
        const diagnostics::AssertionSite site{long_text, "retained-function", long_text, 987};
        if (diagnostics::detail::BeginCheck(site) == false)
        {
            return 71;
        }
        return diagnostics::detail::FinishCheck("retained-message") ? 72 : 0;
    }
    if (std::strcmp(mode, "barrier") == 0)
    {
        std::barrier start(9);
        std::atomic<int> conditions{0};
        std::atomic<int> failures{0};
        std::thread workers[8];
        for (auto& worker : workers)
        {
            worker = std::thread([&] {
                start.arrive_and_wait();
                std::mutex application_mutex;
                const std::lock_guard lock(application_mutex);
                if (LUDUS_CHECK((conditions.fetch_add(1), false), "synchronized Check") == false)
                {
                    failures.fetch_add(1);
                }
            });
        }
        start.arrive_and_wait();
        for (auto& worker : workers)
        {
            worker.join();
        }
        return conditions == 8 && failures == 8 ? 0 : 73;
    }
    const Cleanup cleanup;
    (void)std::atexit(AtExit);
    if (std::strcmp(mode, "formatted-assert") == 0)
    {
        LUDUS_ASSERT_F(Condition(), "{}", diagnostics::DiagnosticCString(Message()));
        // Reached only when disabled (no evaluation) or when an enabled ASSERT
        // resumed (condition+message ran exactly once). A terminal ASSERT never
        // returns here. The driver decides which outcome to expect.
        Mark("ASSERT-RETURNED\n");
        if (LUDUS_ENABLE_ASSERTS == 0)
        {
            return gConditions == 0 && gMessages == 0 ? 0 : 67;
        }
        return gConditions == 1 && gMessages == 1 ? 0 : 67;
    }
    if (std::strcmp(mode, "formatted-malformed") == 0)
    {
        LUDUS_FATAL_F("bad {", 42);
    }
    if (std::strcmp(mode, "formatted-require") == 0)
    {
        LUDUS_REQUIRE_F(Condition(), "{}", diagnostics::DiagnosticCString(Message()));
    }
    if (std::strcmp(mode, "formatted-fatal") == 0)
    {
        LUDUS_FATAL_F("{}", diagnostics::DiagnosticCString(RecursiveFatal()));
    }
    if (std::strcmp(mode, "formatted-check") == 0)
    {
        return LUDUS_CHECK_F(false, "{}", diagnostics::DiagnosticCString(RecursiveCheck())) || gMessages != 0 ? 76 : 0;
    }
    if (std::strcmp(mode, "assert") == 0)
    {
        LUDUS_ASSERT(Condition(), Message());
        // Reached only when disabled or when an enabled ASSERT resumed. A
        // terminal ASSERT never returns here.
        Mark("ASSERT-RETURNED\n");
        if (LUDUS_ENABLE_ASSERTS == 0)
        {
            return gConditions == 0 && gMessages == 0 ? 0 : 89;
        }
        return gConditions == 1 && gMessages == 1 ? 0 : 89;
    }
    if (std::strcmp(mode, "assert-repeated") == 0)
    {
        // Two independent failing ASSERTs. If both resume, each evaluates its
        // condition/message once and control returns after each; a later failure
        // must be independently reportable (the slot is released each time).
        LUDUS_ASSERT(Condition(), Message());
        const int after_first = gConditions;
        LUDUS_ASSERT(Condition(), "second");
        Mark("ASSERT-RETURNED\n");
        return after_first == 1 && gConditions == 2 ? 0 : 96;
    }
    if (std::strcmp(mode, "require") == 0)
    {
        LUDUS_REQUIRE(Condition(), Message());
    }
    else if (std::strcmp(mode, "fatal") == 0)
    {
        LUDUS_FATAL("unconditional-reason");
    }
    else if (std::strcmp(mode, "recursive-fatal") == 0)
    {
        LUDUS_FATAL(RecursiveFatal());
    }
    else if (std::strcmp(mode, "packet") == 0)
    {
        LUDUS_FATAL(([] {
            const auto& packet = diagnostics::internal::gFatalPacket;
            if (packet.MinimalReady.load(std::memory_order_acquire) == false ||
                packet.CompleteReady.load(std::memory_order_acquire) || packet.MinimalSize == 0)
            {
                std::_Exit(91);
            }
            std::thread reader([] {
                if (packet.MinimalReady.load(std::memory_order_acquire) == false ||
                    packet.Minimal[packet.MinimalSize] != '\0' ||
                    std::strstr(packet.Minimal, "<unconditional>") == nullptr)
                {
                    std::_Exit(74);
                }
            });
            reader.join();
            Mark("MINIMAL-COMMITTED-BEFORE-MESSAGE\n");
            return "packet-reason";
        })());
    }
    Mark("FATAL-RETURNED\n");
    return 92;
}
