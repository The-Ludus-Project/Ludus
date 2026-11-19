#pragma once

// Versioned UUID authoring records, stage 3 of
// docs/architecture/object-handles-uuid.md#implementation-sequence. A migration
// manifest maps the scoped string IDs of version-1 World documents to committed
// UUIDs once; replaying it never mints identities.
#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/identity/uuid.hpp>

#include <span>
#include <string_view>

namespace ludus::content::identity_records
{
/// Value of the manifest's `format` member.
inline constexpr std::string_view MIGRATION_MANIFEST_FORMAT = "ludus.identity-migration";
/// The only manifest `version` this reader and writer support.
inline constexpr uint64 MIGRATION_MANIFEST_VERSION = 1;
/// Largest accepted manifest text, in bytes.
inline constexpr usize MAX_MANIFEST_BYTES = usize{1024} * 1024;
/// Maximum requested heap bytes for one manifest decode, including the new tables.
inline constexpr usize MAX_MANIFEST_DECODE_BYTES = usize{15} * 1024 * 1024;
/// Most documents one manifest may describe.
inline constexpr usize MAX_MANIFEST_DOCUMENTS = 64;
/// Most objects one manifest document may map (the Editor's World entity limit).
inline constexpr usize MAX_DOCUMENT_OBJECTS = 128;
/// Longest scoped legacy ID, in bytes. Matches the Editor's World ID rule.
inline constexpr usize MAX_LEGACY_ID_BYTES = 64;
/// Largest accepted version-1 World document, in bytes (the Editor's World limit).
inline constexpr usize MAX_WORLD_DOCUMENT_BYTES = usize{512} * 1024;
/// Candidate UUIDs tried for one new identity before generation reports IdentityConflict.
inline constexpr usize MAX_UUID_GENERATION_ATTEMPTS = 4;

/// Result of reading, generating, replaying or publishing identity records.
enum class IdentityRecordStatus : uint8
{
    /// The operation succeeded and its output is available.
    Success,
    /// Input is not valid JSON or does not have the required shape.
    Malformed,
    /// The `format`, envelope `version` or another versioned field is not supported.
    UnsupportedVersion,
    /// The input uses a construct this stage does not migrate, such as a World scene extension.
    Unsupported,
    /// A UUID is malformed, nil, not version 4 or not the RFC 9562 variant.
    InvalidIdentity,
    /// The same UUID appears twice in one manifest, including across documents.
    DuplicateIdentity,
    /// A legacy ID or document path is repeated, or a legacy ID breaks the ID rule.
    InvalidLegacyId,
    /// The manifest has no record for the requested document.
    UnknownDocument,
    /// The document's legacy IDs differ from the manifest's: an ID is missing or extra.
    StaleManifest,
    /// An entity reference names no object of the same document.
    UnresolvedReference,
    /// Input or output exceeds a documented limit.
    Limit,
    /// Allocation failed.
    OutOfMemory,
    /// The entropy source is missing or failed; nothing was minted or written.
    EntropyUnavailable,
    /// Every one of MAX_UUID_GENERATION_ATTEMPTS candidates collided with an existing identity.
    IdentityConflict,
    /// The destination file already exists; it was left unchanged.
    AlreadyExists,
    /// Reading or atomically publishing a file failed.
    IoError,
};

/// @brief Bounded diagnostic of a failed identity-record operation.
///
/// `Message` names the document path, legacy ID or member that failed when known.
struct IdentityRecordError final
{
    /// Failure category; Success when nothing failed.
    IdentityRecordStatus Status = IdentityRecordStatus::Success;
    /// Human-readable detail; empty when the status alone explains the failure.
    MessageText Message;
};

/// Return a stable lowercase name for a status, for tools and logs.
[[nodiscard]] std::string_view GetIdentityRecordStatusName(IdentityRecordStatus status) noexcept;

/// @brief Report whether text satisfies the scoped legacy ID rule.
///
/// 1 to MAX_LEGACY_ID_BYTES ASCII letters, digits, `_`, `-`, `.` or `:`. This is the
/// Editor's World ID rule; the world-demo sample's `[a-z0-9-]{1,31}` IDs are a subset.
[[nodiscard]] bool IsLegacyId(std::string_view text) noexcept;

/// One scoped legacy ID and the source-object UUID committed for it.
struct MigrationObject final
{
    /// The version-1 `id`, retained as the object's readable label.
    ResourceId LegacyId;
    /// The durable identity of the object; never nil.
    foundation::identity::SourceObjectId Object;
};

/// One migrated document: its path, its document identity and its objects.
struct MigrationDocument final
{
    /// Path relative to the content root the manifest was generated for; satisfies content::ValidPath().
    ResourcePath Path;
    /// The document's logical asset identity; never nil.
    foundation::identity::AssetId Document;
    /// Index of the document's first object in the manifest's object table.
    usize FirstObject = 0;
    /// Number of objects, sorted by legacy ID in byte order.
    usize ObjectCount = 0;
};

/// @brief Validated, owned migration manifest.
///
/// Documents are sorted by path and objects by legacy ID, both in byte order, so
/// writing a manifest is deterministic. Every UUID is a nonnil version-4 value
/// and is unique across the whole manifest. The manifest is an explicit,
/// committed record: rebuilding it from the same sources never reproduces it,
/// because generation mints new values. Move-only; one owner; no internal
/// synchronization.
class MigrationManifest final
{
public:
    /// Empty manifest that owns no storage.
    MigrationManifest() noexcept = default;
    /// Copying is disabled: a manifest is a unique committed record with one owner.
    MigrationManifest(const MigrationManifest&) = delete;
    /// Copying is disabled: a manifest is a unique committed record with one owner.
    MigrationManifest& operator=(const MigrationManifest&) = delete;
    /// Transfer the tables; the source becomes empty.
    MigrationManifest(MigrationManifest&& other) noexcept = default;
    /// Transfer the tables; the source becomes empty.
    MigrationManifest& operator=(MigrationManifest&& other) noexcept = default;
    /// Release both tables.
    ~MigrationManifest() noexcept = default;

    /// All documents, sorted by path. Borrowed until the manifest changes or is destroyed.
    [[nodiscard]] std::span<const MigrationDocument> GetDocuments() const noexcept;
    /// Objects of one document of this manifest, sorted by legacy ID. Borrowed like GetDocuments().
    [[nodiscard]] std::span<const MigrationObject> GetObjects(const MigrationDocument& document) const noexcept;
    /// Return the document with exactly this path, or null when the manifest has none.
    [[nodiscard]] const MigrationDocument* FindDocument(std::string_view path) const noexcept;
    /// Return the object of `document` with exactly this legacy ID, or null when it has none.
    [[nodiscard]] const MigrationObject* FindObject(const MigrationDocument& document,
                                                    std::string_view legacyId) const noexcept;

    /// @brief Replace this manifest with one parsed from canonical or hand-edited JSON.
    ///
    /// Rejects unknown or duplicate members, an unsupported `format`/`version`,
    /// invalid paths or legacy IDs, unsorted or repeated entries, and any UUID that
    /// is not a nonnil version-4 value or that repeats anywhere in the manifest.
    /// @param json Manifest text, at most MAX_MANIFEST_BYTES.
    /// @param outError Receives the failure; reset to Success on success.
    /// @return Success, or the failure; the manifest is unchanged on failure.
    [[nodiscard]] IdentityRecordStatus Read(std::string_view json, IdentityRecordError& outError) noexcept;

    /// @brief Decode and validate a replacement under a requested-heap budget.
    ///
    /// The peak includes the parser pool, both owned candidate tables (including
    /// their inline strings), and the temporary UUID index while each is live.
    /// It excludes input, stack storage, allocator overhead and the previously
    /// accepted manifest. Outputs describe this attempt even on failure.
    /// @param json Borrowed input for the call, at most MAX_MANIFEST_BYTES.
    /// @param outError Receives the failing field/element or parser byte offset when known.
    /// @param outPeakBytes Peak successfully allocated bytes for this attempt.
    /// @param maxDecodeBytes Requested cap, clamped to MAX_MANIFEST_DECODE_BYTES.
    /// @return Success, or the failure with the previously accepted manifest unchanged.
    [[nodiscard]] IdentityRecordStatus Read(std::string_view json,
                                            IdentityRecordError& outError,
                                            usize& outPeakBytes,
                                            usize maxDecodeBytes = MAX_MANIFEST_DECODE_BYTES) noexcept;

    /// @brief Write the canonical JSON form: fixed member order, two-space indentation, trailing newline.
    /// @param outJson Replaced on success; unchanged on failure.
    /// @return Success, Limit or OutOfMemory.
    [[nodiscard]] IdentityRecordStatus Write(Bytes& outJson) const noexcept;

private:
    friend struct ManifestBuilder;

    foundation::Array<MigrationDocument> mDocuments;
    foundation::Array<MigrationObject> mObjects;
};

/// One version-1 World document offered for migration.
struct MigrationSource final
{
    /// Path relative to the content root; recorded in the manifest. Must satisfy content::ValidPath().
    std::string_view Path;
    /// The document's version-1 World JSON. Only the envelope and entity `id` members are read.
    std::string_view WorldJson;
};

/// @brief Mint a migration manifest for a set of version-1 World documents.
///
/// Reads each document's entity IDs, then obtains one document UUID and one
/// source-object UUID per entity from TryGenerateUuidV4(), in path order and then
/// legacy-ID order. Every candidate is checked against the identities already
/// minted for this manifest; a candidate that collides is replaced, at most
/// MAX_UUID_GENERATION_ATTEMPTS times. No file is read or written and the
/// documents are not changed. Call this once per content root and commit the
/// result with PublishMigrationManifest(); never regenerate it for documents a
/// committed manifest already covers.
/// @param sources Documents to migrate; paths must be distinct.
/// @param entropy Production code passes platform::GetSystemEntropySource(); tests inject controlled
/// sources. There is no fallback generator.
/// @param outManifest Replaced on success; unchanged on failure.
/// @param outError Receives the failure; reset to Success on success.
/// @return Success, Malformed, UnsupportedVersion, InvalidLegacyId, Limit, OutOfMemory,
/// EntropyUnavailable or IdentityConflict.
[[nodiscard]] IdentityRecordStatus GenerateMigrationManifest(std::span<const MigrationSource> sources,
                                                             const foundation::identity::EntropySource& entropy,
                                                             MigrationManifest& outManifest,
                                                             IdentityRecordError& outError) noexcept;

/// @brief Atomically create the manifest file; never replace an existing one.
///
/// Writes the canonical form through content::SaveFile() with no expected digest,
/// so publication fails when the destination already exists or appears before
/// the final rename. A failure leaves no partial manifest at `path`.
/// @param root Trusted existing native content root.
/// @param path Manifest path relative to `root`; its parent directory must exist.
/// @param manifest Validated manifest to publish.
/// @param outError Receives the failure; reset to Success on success.
/// @return Success, AlreadyExists, Limit, OutOfMemory or IoError (also for unsupported platforms).
/// @note Synchronous Linux and macOS file I/O; call from tools, never from frame-critical paths.
[[nodiscard]] IdentityRecordStatus PublishMigrationManifest(std::string_view root,
                                                            std::string_view path,
                                                            const MigrationManifest& manifest,
                                                            IdentityRecordError& outError) noexcept;
} // namespace ludus::content::identity_records
