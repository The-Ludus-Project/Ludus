#include "internal/windows_publication_test_hooks.hpp"

#include <ludus/foundation/filesystem/persistence.hpp>

#include <string_view>

#include <windows.h>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;

namespace
{
bool shouldInterrupt = false;
bool shouldInterruptAfterPublication = false;
constexpr DWORD INTERRUPTED = 78;
constexpr uint8 NEW_BYTES[] = {'n', 'e', 'w', '!'};
} // namespace

void ludus::foundation::filesystem::detail::InterruptWindowsPublication(bool isPublished) noexcept
{
    if (shouldInterrupt == true && isPublished == shouldInterruptAfterPublication)
    {
        // Bypass application unwinding and cleanup. The OS closes process handles.
        (void)TerminateProcess(GetCurrentProcess(), INTERRUPTED);
    }
}

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 2)
    {
        return 1;
    }
    const std::string_view mode(arguments[1]);
    shouldInterrupt = mode != "retry";
    shouldInterruptAfterPublication = mode == "after";
    WriteDirectory writer;
    if (writer.Open(".").Succeeded() == false)
    {
        return 2;
    }
    const PublicationResult result =
        writer.Publish("output.bin", NEW_BYTES, { .Sync = SyncPolicy::File, .Expected = {} });
    constexpr int PUBLICATION_FAILED = 3;
    return result.Outcome.Succeeded() == true && result.Published == true ? 0 : PUBLICATION_FAILED;
}
