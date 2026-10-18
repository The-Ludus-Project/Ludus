#include <ludus/foundation/parsing/json.hpp>
#include <ludus/foundation/parsing/parsing.hpp>

#include <span>
#include <string_view>

using namespace ludus::foundation;
using namespace ludus::foundation::parsing;

bool ExerciseInstalledParsing() noexcept
{
    const uint8 record[] = {0x78, 0x56, 0x34, 0x12};
    ByteCursor cursor(record);
    uint32 word = 0;
    ParseError error;
    JsonDocument document;
    if (cursor.ReadUint32LittleEndian(word) == false || word != 0x12345678 || cursor.Remaining() != 0 ||
        ValidateUtf8("\xf0\x9f\x8e\xae", error) != ParseStatus::Ok ||
        document.Read(R"({"name":"engine","values":[1,2]})", error) != ParseStatus::Ok)
    {
        return false;
    }
    std::string_view name;
    uint64 value = 0;
    if (document.Root().Fields({"name", "values"}) == false || document.Root().Get("name").String(name) == false ||
        name != "engine" || document.Root().Get("values").At(1).Integer(value) == false || value != 2)
    {
        return false;
    }
    uint8 storage[32]{};
    JsonWriter writer(storage);
    writer.Integer(value);
    std::span<const uint8> bytes;
    if (writer.Finish(bytes) != ParseStatus::Ok || bytes.size() != 2 || bytes[0] != '2' || bytes[1] != '\n' ||
        document.Read(R"({"a":1,"a":2})", error) != ParseStatus::DuplicateKey || document.Root().Valid())
    {
        return false;
    }
    return document.Read("null", error) == ParseStatus::Ok && document.Root().Null();
}
