#include <ludus/foundation/parsing/json.hpp>

#include <span>
#include <string_view>

using namespace ludus::foundation;
using namespace ludus::foundation::parsing;

int main()
{
    JsonDocument document;
    ParseError error;
    uint64 value = 0;
    if (document.Read(R"({"v":[1,2],"text":"\ud83c\udfae"})", error) != ParseStatus::Ok ||
        document.Root().Get("v").At(1).Integer(value) == false || value != 2)
    {
        return 1;
    }
    JsonLimits limits;
    limits.MaxDepth = 0;
    if (document.Read("[[[[", error, limits) != ParseStatus::LimitExceeded || document.Root().Valid() ||
        document.Read(R"({"a":1,"\u0061":2})", error) != ParseStatus::DuplicateKey || document.Root().Valid())
    {
        return 2;
    }
    uint8 storage[8]{};
    JsonWriter writer(storage);
    writer.Integer(42);
    std::span<const uint8> bytes;
    if (writer.Finish(bytes) != ParseStatus::Ok ||
        document.Read({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, error) != ParseStatus::Ok ||
        document.Root().Integer(value) == false || value != 42)
    {
        return 3;
    }
    return 0;
}
