#include <ludus/foundation/base/types.h>

#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/strings/shared_string.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/foundation/strings/string.hpp>
#include <ludus/foundation/strings/string_table.hpp>
#include <ludus/foundation/strings/utf16.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include <string_view>

namespace
{
// Edits, checked slices, display repair and the native UTF-16 boundary.
[[nodiscard]] bool ExerciseInstalledText() noexcept
{
    using namespace ludus::foundation;
    String edited;
    String repaired;
    Utf8View text;
    Utf8View slice;
    usize error = 0;
    usize count = 0;
    bool isTruncated = false;
    StaticString<3> shortened;
    char16_t units[4]{};
    char bytes[8]{};
    if (edited.TryAssign("caf\xC3\xA9 latte, a longer drink") != StringStatus::Ok ||
        edited.TryErase(5, edited.GetSize() - 5) != StringStatus::Ok || edited.TryShrinkToFit() != StringStatus::Ok ||
        ValidateUtf8(edited.GetView(), text, error) != StringStatus::Ok ||
        TrySliceUtf8(text, 4, 1, slice) != StringStatus::SplitsCodePoint ||
        TrySliceUtf8(text, 3, 2, slice) != StringStatus::Ok)
    {
        return false;
    }
    shortened.AssignTruncatedUtf8(text, isTruncated);
    if (shortened.GetView() != "caf" || isTruncated == false ||
        TryRepairUtf8("x\xFF", edited.GetSize(), repaired, count) != StringStatus::Ok || count != 1 ||
        repaired.GetView() != "x\xEF\xBF\xBD" ||
        ConvertUtf8ToUtf16(slice.GetView(), units, count, error) != StringStatus::Ok || count != 1 ||
        units[0] != u'\u00E9')
    {
        return false;
    }
    return ConvertUtf16ToUtf8({units, count}, bytes, count, error) == StringStatus::Ok &&
           std::string_view(bytes, count) == slice.GetView() && GetByteView(u8"\u00E9") == slice.GetView();
}
} // namespace

// Both the relocated SDK consumer and wasm/Node probe execute the public API.
// The web probe also checks 32-bit sizes and private-vendor link closure.
bool ExerciseInstalledStrings() noexcept
{
    if (ExerciseInstalledText() == false)
    {
        return false;
    }
    using namespace ludus::foundation;
    StaticString<8> bounded;
    String unique;
    SharedString shared;
    Utf8View utf8;
    usize error = 0;
    if (bounded.TryAssign("player") != StringStatus::Ok ||
        unique.TryAssign("a string longer than the inline capacity") != StringStatus::Ok ||
        unique.TryAppend(unique.GetView()) != StringStatus::Ok ||
        CreateShared(unique.GetView(), GetSystemAllocationDomain(), shared) != StringStatus::Ok ||
        ValidateUtf8(shared.GetView(), utf8, error) != StringStatus::Ok)
    {
        return false;
    }
    const SharedString copy = shared;
    if (copy.GetData() != shared.GetData() || copy.GetView() != unique.GetView() ||
        TableHash64({}) != 0x2d06800538d394c2ULL ||
        StableFingerprint128({}) != Fingerprint128{0x6001c324468d497fULL, 0x99aa06d3014798d8ULL})
    {
        return false;
    }
    StringTable table;
    NameId id;
    NameId duplicate;
    std::string_view resolved;
    StringTableSnapshot snapshot;
    if (CreateTable({}, GetSystemAllocationDomain(), table) != StringStatus::Ok ||
        table.TryReserve(1, bounded.GetSize()) != StringStatus::Ok ||
        table.TryIntern(bounded.GetView(), id) != StringStatus::Ok ||
        table.TryIntern("player", duplicate) != StringStatus::Ok || id != duplicate ||
        table.TryCaptureSnapshot(snapshot) != StringStatus::Ok || snapshot.EntryCount != 2 ||
        snapshot.DuplicateAdmissions != 1 || snapshot.SpellingBytes != bounded.GetSize())
    {
        return false;
    }
    FrozenStringTable frozen;
    StringTableSnapshot frozenSnapshot;
    return table.TryFreeze(frozen) == StringStatus::Ok && frozen.TryResolve(id, resolved) == StringStatus::Ok &&
           resolved == "player" && frozen.TryFind("player", duplicate) == StringStatus::Ok && duplicate == id &&
           frozen.TryResolveIndex(id.Index, resolved) == StringStatus::Ok && resolved == "player" &&
           frozen.TryCaptureSnapshot(frozenSnapshot) == StringStatus::Ok && frozenSnapshot == snapshot;
}
