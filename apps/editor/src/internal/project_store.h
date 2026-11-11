#pragma once

// ProjectStore: bounded version-1 descriptor parsing, canonical serialization,
// cooperative-locked atomic save and optimistic conflict detection.
//
// Private editor header (not installed). Owns file I/O only; it holds no widget
// state and starts no processes. See design.md sections 4 and 5.

#include "internal/project_descriptor.h"

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QString>

#include <functional>

namespace ludus::editor
{

// Result of a save attempt. On success the saved bytes/digest were committed and
// Digest holds the sha256 of the committed bytes. On failure the previous saved
// file is preserved and the caller keeps its dirty draft (design section 4).
struct SaveOutcome
{
    ResultCode Code = ResultCode::InvalidProject;
    QString Digest; // 64 lowercase hex of committed bytes, valid only when Ok
    QString Message;

    [[nodiscard]] bool Ok() const noexcept
    {
        return Code == ResultCode::Ok;
    }
};

// Result of reading a descriptor from disk. Carries the raw bytes' digest so the
// caller can later detect external modification (optimistic conflict).
struct LoadOutcome
{
    ParseOutcome Parse;
    QString Digest; // sha256 of the exact bytes parsed
    QByteArray Bytes;

    [[nodiscard]] bool Ok() const noexcept
    {
        return Parse.Ok();
    }
};

// Lowercase-hex sha256 of a byte buffer. Shared by load/save and the protocol
// `expected_sha256` so the UI and tooling agree on the descriptor identity.
[[nodiscard]] QString Sha256Hex(const QByteArray& bytes);

// Serialize a descriptor to the canonical UTF-8 JSON form: only the documented
// fields, stable order, two-space indent, trailing newline. Deterministic so a
// save/reopen round-trip is stable and digests are comparable (design 4).
[[nodiscard]] QByteArray SerializeDescriptor(const ProjectDescriptor& descriptor);

/// Supported version-2 native presets in tooling registry order. Static lifetime.
[[nodiscard]] const QStringList& GetNativeProjectPresets();

// Parse and validate bounded version-1 and version-2 descriptor bytes. Rejects
// malformed JSON, invalid UTF-8, unknown fields and invalid values. Does no I/O.
[[nodiscard]] ParseOutcome ParseDescriptor(const QByteArray& bytes);

class ProjectStore
{
public:
    ProjectStore() = default;

    // Narrow fault-injection seams used only by tests to force partial writes
    // and commit failures without touching real disk behavior. Default is the
    // real QSaveFile path. These are never wired from production code.
    struct FaultHooks
    {
        // Return >=0 to truncate the write to that many bytes (short write);
        // return <0 to write all bytes normally.
        std::function<foundation::isize(const QByteArray&)> TruncateWriteTo;
        // Return true to force the commit (rename) step to fail.
        std::function<bool()> FailCommit;
    };

    void SetFaultHooks(FaultHooks hooks)
    {
        Hooks_ = std::move(hooks);
    }

    // Read a descriptor file (bounded) and parse it. Does not change any
    // caller-held workspace on failure; callers decide what to keep.
    [[nodiscard]] LoadOutcome Load(const QString& descriptorPath) const;

    // Validate the draft, acquire a short cooperative QLockFile beside the
    // settings in an ignored `.ludus/` directory, verify the on-disk bytes still
    // match expectedDigest (Conflict otherwise), then write with QSaveFile
    // (direct-write fallback disabled) and commit. Updates nothing on failure.
    //
    // expectedDigest is the digest recorded at Open/last Save; pass an empty
    // string only for an initial write to a nonexistent path.
    [[nodiscard]] SaveOutcome
    Save(const QString& descriptorPath, const ProjectDescriptor& draft, const QString& expectedDigest) const;

private:
    FaultHooks Hooks_;
};

} // namespace ludus::editor
