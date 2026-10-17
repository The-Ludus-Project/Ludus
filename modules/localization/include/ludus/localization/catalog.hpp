#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/strings/shared_string.hpp>

#include <span>
#include <string_view>

namespace ludus::localization
{
using foundation::int64;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

inline constexpr usize kMaxCatalogBytes = usize{32} * 1024 * 1024;
inline constexpr uint32 kMaxMessages = 65536;
inline constexpr usize kMaxTextBytes = usize{64} * 1024;
inline constexpr usize kMaxIdentifierBytes = 128;
/// Largest dynamic `ludus-icu-mf1-v1` pattern stored in a package, in UTF-8 bytes.
inline constexpr usize MAX_PATTERN_BYTES = usize{16} * 1024;
/// Most argument declarations one `ludus-icu-mf1-v1` message may have.
inline constexpr usize MAX_ARGUMENTS = 32;
/// Most catalogs on one locale's message-fallback path, including the locale itself.
inline constexpr usize MAX_FALLBACK_PROVIDERS = 8;

/// Outcome of every fallible localization operation; failures preserve outputs.
enum class Status : uint8
{
    Ok,                ///< The operation succeeded and wrote its output.
    Invalid,           ///< Malformed bytes, an invalid handle or a key outside the ID grammar.
    Unsupported,       ///< A package version, record width or profile this reader does not implement.
    Limit,             ///< A size or count exceeds the documented ceiling.
    OutOfMemory,       ///< The allocation domain could not provide the catalog copy.
    NotFound,          ///< The domain or key is absent from this catalog.
    WrongCatalog,      ///< The binding belongs to another prepared catalog.
    TokenExhausted,    ///< The process-local catalog token space is exhausted.
    SchemaMismatch,    ///< The key exists, but its argument schema differs from the descriptor.
    RequiresArguments, ///< The message has arguments; it must be formatted, not resolved statically.
};

/// Bounded failure report of PrepareCatalog.
struct Diagnostic final
{
    Status Result{Status::Ok}; ///< Status of the last prepare; Ok after success.
    usize Offset{};            ///< Byte offset in the cooked input where validation failed.
};

/// Exact authored identity; neither dictionary indices nor hashes are persistent.
struct MessageKey final
{
    std::string_view Domain;
    std::string_view Key;
};

/// Value category of one `ludus-icu-mf1-v1` message argument.
enum class ArgumentType : uint8
{
    Text,   ///< Counted literal UTF-8, never interpreted as a message.
    Count,  ///< `int64` within the declared bounds; plural selector or integer display.
    Number, ///< Finite `float64` measurement within the declared bounds; display only.
    Enum,   ///< One of the declared ASCII tokens, selected with `select`.
};

/// Who produces a Text argument; user text is filtered more strictly by L2 formatting.
enum class TextOrigin : uint8
{
    Game, ///< Authored game content.
    User, ///< Typed by a player; controls and bidi overrides are rejected.
};

/// What an Enum argument does with a token outside its declaration.
enum class UnknownPolicy : uint8
{
    Other,  ///< Select the `other` branch.
    Reject, ///< Fail argument validation.
};

/// @brief One declared argument of a message schema, as a generated descriptor spells it.
///
/// Binding compares the fields meaningful for Type exactly: Name and Type always;
/// Origin and MaxBytes for Text; Minimum and Maximum for Count and Number;
/// Unknown and the ordered Values for Enum. Other fields are ignored. Views
/// borrow storage that must outlive the bind call; generated descriptors use
/// static storage.
struct ArgumentDescriptor final
{
    std::string_view Name;                        ///< Declared name, `[a-z][a-z0-9_]*`, at most 64 bytes.
    ArgumentType Type{ArgumentType::Text};        ///< Value category.
    TextOrigin Origin{TextOrigin::Game};          ///< Text only: producer of the text.
    UnknownPolicy Unknown{UnknownPolicy::Reject}; ///< Enum only: handling of undeclared tokens.
    uint32 MaxBytes{};                            ///< Text only: UTF-8 byte limit, 1-4096.
    int64 Minimum{};                              ///< Count/Number only: inclusive lower bound.
    int64 Maximum{};                              ///< Count/Number only: inclusive upper bound.
    std::span<const std::string_view> Values;     ///< Enum only: declared tokens in authored order.
};

/// @brief Exact key plus the complete expected argument schema of one message.
///
/// Generated per domain by `scripts/localization cook-release --bindings`.
/// An empty Arguments span describes an argument-free (static) message. The
/// constructor (rather than aggregate initialization) keeps a braced key such
/// as `BindMessage({"game/ui", "menu/play"}, binding)` unambiguous.
struct MessageDescriptor final
{
    /// Describes key with its ordered argument schema; arguments must outlive every bind call.
    constexpr MessageDescriptor(MessageKey key, std::span<const ArgumentDescriptor> arguments) noexcept
        : Key(key), Arguments(arguments)
    {
    }

    MessageKey Key;                                ///< Exact authored identity.
    std::span<const ArgumentDescriptor> Arguments; ///< Ordered argument schema, possibly empty.
};

/// @brief Process-local handle to one message of one prepared catalog.
///
/// Valid only with the catalog (or a copy of it) that produced it; it is not a
/// persistent identity and is never reused across loads.
class MessageBinding final
{
private:
    uint64 mToken{};
    uint32 mIndex{};
    friend class Catalog;
};

/// Which catalog provided a resolved message for the requested locale.
enum class Origin : uint8
{
    Source,         ///< The catalog is the source locale's.
    Translation,    ///< The requested locale's own approved translation.
    SourceFallback, ///< The source locale's message, reached through explicit fallback.
    Fallback,       ///< Another translated locale on the manifest's fallback path.
};

/// Borrowed static text and its provenance; valid while a lease of its catalog is alive.
struct ResolvedText final
{
    std::string_view Text;             ///< Counted logical UTF-8, not NUL-terminated.
    Origin Provenance{Origin::Source}; ///< Which catalog provided Text.
    std::string_view ProviderLocale;   ///< Locale label of that catalog.
};

namespace detail
{
/// Validated offsets of a catalog's owned bytes; all zero for an empty catalog. Private to the reader.
struct CatalogLayout final
{
    uint64 Token{};
    uint32 Version{};
    uint32 Count{};
    uint32 Records{};
    uint32 RecordBytes{};
    uint32 Providers{};
    uint32 ProviderCount{};
    uint32 Arguments{};
    uint32 Values{};
};
} // namespace detail

/// @brief Immutable snapshot AND lifetime lease.
///
/// Copies share one allocation. Separate copies may be read/destroyed on different threads after synchronized
/// publication.
/// The allocation domain must outlive every copy. Borrowed views require a live
/// lease; assignment/destruction of the last lease invalidates them.
///
/// Each successful prepare gets a new token, even for identical schemas/locales.
/// Rebind after replacement; copies/moves retain the token. There is no global
/// current language, implicit fallback, or cross-catalog binding compatibility.
class Catalog final
{
public:
    Catalog() noexcept = default;
    ~Catalog() noexcept = default;
    Catalog(const Catalog&) noexcept = default;
    Catalog& operator=(const Catalog&) noexcept = default;
    Catalog(Catalog&& other) noexcept;
    Catalog& operator=(Catalog&& other) noexcept;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] uint32 GetMessageCount() const noexcept;
    [[nodiscard]] std::string_view GetDomain() const& noexcept;
    [[nodiscard]] std::string_view GetDomain() const&& = delete;
    [[nodiscard]] std::string_view GetLocale() const& noexcept;
    [[nodiscard]] std::string_view GetLocale() const&& = delete;
    [[nodiscard]] std::string_view GetSourceLocale() const& noexcept;
    [[nodiscard]] std::string_view GetSourceLocale() const&& = delete;
    /// @brief Linguistic data that validated and decoded a version 2 package.
    ///
    /// For example `icu4c-78.3/cldr-48/unicode-17.0`; empty for version 1
    /// literal catalogs and invalid catalogs. L2 formatting must run the same data.
    [[nodiscard]] std::string_view GetLinguisticData() const& noexcept;
    /// Deleted: a temporary catalog cannot lend a retained view.
    [[nodiscard]] std::string_view GetLinguisticData() const&& = delete;

    /// @brief Cold exact-key binary search for an argument-free message; outputs are unchanged on any failure.
    ///
    /// Equivalent to binding a descriptor with an empty argument schema: a
    /// message that declares arguments returns SchemaMismatch.
    [[nodiscard]] Status BindMessage(MessageKey key, MessageBinding& output) const noexcept;
    /// @brief Cold exact-key and exact argument-schema bind.
    ///
    /// @param descriptor Key and complete expected schema, usually generated.
    /// @param outBinding Written only on Ok.
    /// @return Ok; Invalid for an invalid catalog, key or descriptor; NotFound
    /// for an absent domain/key; SchemaMismatch when the key exists but any
    /// compared argument field differs. No allocation or hashing.
    [[nodiscard]] Status BindMessage(const MessageDescriptor& descriptor, MessageBinding& outBinding) const noexcept;
    /// @brief O(1), zero allocation. Returns the cooked static UTF-8 text.
    ///
    /// Version 1 text is literal; version 2 text is the ICU-decoded output of an
    /// argument-free pattern (`'{'` is already `{`). A message with arguments
    /// returns RequiresArguments, even when one translation omits them all.
    [[nodiscard]] Status ResolveStatic(MessageBinding binding, ResolvedText& output) const& noexcept;
    [[nodiscard]] Status ResolveStatic(MessageBinding, ResolvedText&) const&& = delete;

private:
    foundation::SharedString mBytes;
    detail::CatalogLayout mLayout;
    [[nodiscard]] uint32 readWord(usize offset) const noexcept;
    [[nodiscard]] std::string_view getSlice(usize offset, usize length) const noexcept;
    [[nodiscard]] std::string_view getSliceAt(usize directoryEntry) const noexcept;
    [[nodiscard]] Status findKey(std::string_view domain, std::string_view key, uint32& outIndex) const noexcept;
    [[nodiscard]] bool isSchemaEqual(uint32 index, std::span<const ArgumentDescriptor> arguments) const noexcept;
    friend Status
    PrepareCatalog(std::span<const uint8>, const foundation::AllocationDomain&, Catalog&, Diagnostic&) noexcept;
};

/// @brief Validates a portable cooked catalog and copies the caller's bytes.
///
/// Accepts package version 1 (`ludus-static-utf8-v1`) and version 2
/// (`ludus-icu-mf1-v1`, static text plus dynamic patterns and their argument
/// schemas); see docs/modules/localization.md for both layouts. Every offset,
/// count, label, key order, UTF-8 range, schema field and provider index is
/// checked before one allocation from domain. Transactional, including an input
/// that borrows the previous output. No I/O and no ICU. Caller must not mutate
/// input concurrently. Failure preserves output; diagnostic reports a byte
/// offset, including malformed cooked files without allocating.
[[nodiscard]] Status PrepareCatalog(std::span<const uint8> bytes,
                                    const foundation::AllocationDomain& domain,
                                    Catalog& output,
                                    Diagnostic& diagnostic) noexcept;
} // namespace ludus::localization
