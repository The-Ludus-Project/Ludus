#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/parsing/json.hpp>

#include <ludus/content/content.h>

#include <initializer_list>
#include <span>
#include <string_view>

namespace ludus::content
{
/// @brief Compatibility facade.
///
/// Typed Content and Audio validation stays in its domain.
/// Borrowed nodes and strings expire on the document's next Read or destruction.
struct JsonValue final
{
    const void* Value = nullptr;
    [[nodiscard]] JsonValue Get(const char* key) const noexcept;
    [[nodiscard]] JsonValue At(usize index) const noexcept;
    [[nodiscard]] usize Count() const noexcept;
    [[nodiscard]] bool Array() const noexcept;
    /// Return whether this borrowed value is a JSON object.
    [[nodiscard]] bool Object() const noexcept;
    [[nodiscard]] bool String(std::string_view& output) const noexcept;
    [[nodiscard]] bool Integer(uint64& output) const noexcept;
    [[nodiscard]] bool Number(float64& output) const noexcept;
    [[nodiscard]] bool Boolean(bool& output) const noexcept;
    [[nodiscard]] bool Fields(std::initializer_list<const char*> required) const noexcept;
};
class JsonDocument final
{
public:
    JsonDocument() noexcept = default;
    /// Construct an empty document whose workspace is allocated from `domain`.
    /// The domain and its callback context must outlive the document.
    explicit JsonDocument(const AllocationDomain& domain) noexcept;
    ~JsonDocument() noexcept;
    JsonDocument(const JsonDocument&) = delete;
    JsonDocument& operator=(const JsonDocument&) = delete;
    [[nodiscard]] Status Read(std::string_view input, Diagnostic& diagnostic) noexcept;
    [[nodiscard]] JsonValue Root() const noexcept;

private:
    foundation::parsing::JsonDocument mDocument;
};
/// @brief Fixed-capacity canonical writer.
///
/// Numeric spelling/escaping use the same pinned C codec. Overflow reports failure and never returns a truncated
/// document.
class JsonWriter final
{
public:
    explicit JsonWriter(std::span<uint8> output) noexcept : mWriter(output) {}
    void Raw(std::string_view value) noexcept;
    void String(std::string_view value) noexcept;
    void Integer(uint64 value) noexcept;
    void Number(float64 value) noexcept;
    void Boolean(bool value) noexcept;
    [[nodiscard]] Status Finish(Bytes& output) noexcept;

private:
    foundation::parsing::JsonWriter mWriter;
};
} // namespace ludus::content
