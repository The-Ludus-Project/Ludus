#include "import_diagnostics.h"

namespace ludus::content::scene_cook::importers
{
namespace
{
/// Distinct messages kept per code before further ones fold into the first entry.
constexpr usize MAX_ENTRIES_PER_CODE = 16;
} // namespace

bool DiagnosticLog::Add(DiagnosticSeverity severity,
                        std::string_view code,
                        std::string_view file,
                        uint32 line,
                        std::string_view message) noexcept
{
    if (severity == DiagnosticSeverity::Error)
    {
        mHasErrors = true;
    }
    usize sameCode = 0;
    Diagnostic* firstOfCodeOrNull = nullptr;
    for (Diagnostic& entry : mEntries)
    {
        if (entry.Code.GetView() != code)
        {
            continue;
        }
        if (firstOfCodeOrNull == nullptr)
        {
            firstOfCodeOrNull = &entry;
        }
        ++sameCode;
        if (entry.Message.GetView() == message)
        {
            ++entry.Count;
            return true;
        }
    }
    if (firstOfCodeOrNull != nullptr && sameCode >= MAX_ENTRIES_PER_CODE)
    {
        ++firstOfCodeOrNull->Count;
        return true;
    }
    Diagnostic entry;
    entry.Severity = severity;
    entry.Line = line;
    const bool isStored = entry.Code.TryAssign(code) == foundation::StringStatus::Ok &&
                          entry.File.TryAssign(file) == foundation::StringStatus::Ok &&
                          entry.Message.TryAssign(message) == foundation::StringStatus::Ok &&
                          mEntries.TryAdd(static_cast<Diagnostic&&>(entry)) == true;
    if (isStored == false)
    {
        mHasOverflowed = true;
    }
    return isStored;
}

uint32 DiagnosticLog::CountOccurrences(std::string_view code) const noexcept
{
    uint32 total = 0;
    for (const Diagnostic& entry : mEntries)
    {
        if (entry.Code.GetView() == code)
        {
            total += entry.Count;
        }
    }
    return total;
}
} // namespace ludus::content::scene_cook::importers
