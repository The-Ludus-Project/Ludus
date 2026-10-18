#pragma once

// Bounded dependency resolution for the best-effort front-ends. Parsers never
// touch the file system: they name a dependency (an MTL library, an external
// glTF buffer) relative to the import root and a SourceResolver confines it to
// that root, reads it under an explicit byte budget and reports the outcome.
// See docs/architecture/parsing.md#first-image-and-geometry-profiles.

#include "byte_source.h"

#include <ludus/content/content.h>
#include <ludus/content/scene_cook/scene_cook.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/strings/string.hpp>

#include <span>
#include <string_view>

namespace ludus::content::scene_cook::detail
{
class SequentialFile;
} // namespace ludus::content::scene_cook::detail

namespace ludus::content::scene_cook::importers
{
using foundation::Array;
using foundation::String;
using foundation::core::uint8;
using foundation::core::usize;

/// A dependency inside the import root: root-relative for reports, native (or the
/// in-memory key) for reads.
struct ResolvedSource final
{
    String Relative;
    String Native;
};

/// Source of dependency bytes for a parser; implementations own the confinement policy.
class SourceResolver
{
public:
    SourceResolver() noexcept = default;
    SourceResolver(const SourceResolver&) = delete;
    SourceResolver& operator=(const SourceResolver&) = delete;
    virtual ~SourceResolver() noexcept = default;
    /// Resolve name (a root-relative path with '/' separators) inside the import root.
    /// @return false for an empty, absolute, escaping or otherwise unusable name, or when
    /// file access is disabled; outSource is unspecified then.
    [[nodiscard]] virtual bool Resolve(std::string_view name, ResolvedSource& outSource) noexcept = 0;
    /// Read the whole dependency. @return Ok; NotFound; IoError; Limit when it holds more
    /// than maximumBytes; OutOfMemory. outBytes is replaced only on Ok.
    [[nodiscard]] virtual Status
    Read(const ResolvedSource& source, usize maximumBytes, Array<uint8>& outBytes) noexcept = 0;
};

/// Resolver over the native file system, confined to a resolved root directory.
class NativeSourceResolver final : public SourceResolver
{
public:
    /// @param root realpath of the import root; must outlive the resolver. Empty
    /// disables all dependency access.
    LUDUS_INLINE explicit NativeSourceResolver(std::string_view root) noexcept : mRoot(root) {}
    [[nodiscard]] bool Resolve(std::string_view name, ResolvedSource& outSource) noexcept override;
    [[nodiscard]] Status
    Read(const ResolvedSource& source, usize maximumBytes, Array<uint8>& outBytes) noexcept override;

private:
    std::string_view mRoot;
};

/// One named in-memory dependency for MemorySourceResolver; bytes are borrowed.
struct MemorySourceFile final
{
    std::string_view Name;
    std::span<const uint8> Bytes;
};

/// Resolver over caller-owned memory, for tests and fuzz targets. Names must be
/// plain relative paths without "." or ".." components; files must outlive it.
class MemorySourceResolver final : public SourceResolver
{
public:
    LUDUS_INLINE explicit MemorySourceResolver(std::span<const MemorySourceFile> files) noexcept : mFiles(files) {}
    [[nodiscard]] bool Resolve(std::string_view name, ResolvedSource& outSource) noexcept override;
    [[nodiscard]] Status
    Read(const ResolvedSource& source, usize maximumBytes, Array<uint8>& outBytes) noexcept override;

private:
    std::span<const MemorySourceFile> mFiles;
};

/// ByteSource over an open native file, for streaming entry files.
class FileByteSource final : public ByteSource
{
public:
    LUDUS_INLINE explicit FileByteSource(detail::SequentialFile& inoutFile) noexcept : mFile(inoutFile) {}
    [[nodiscard]] Status Read(std::span<uint8> outBuffer, usize& outCount) noexcept override;

private:
    detail::SequentialFile& mFile;
};

/// True when name is a plain relative path: nonempty, no leading '/', no '\\' or NUL,
/// and no empty, "." or ".." component.
[[nodiscard]] bool IsPlainRelativePath(std::string_view name) noexcept;

/// Directory part of a root-relative path including its trailing '/', or empty.
[[nodiscard]] std::string_view GetDirectoryPrefix(std::string_view relativePath) noexcept;

/// Join a root-relative directory prefix (from GetDirectoryPrefix) and a relative
/// name, folding "." and ".." components lexically. @return false when the result
/// would leave the import root, is empty, or memory is exhausted.
[[nodiscard]] bool JoinRelativePath(std::string_view directoryPrefix, std::string_view name, String& outPath) noexcept;
} // namespace ludus::content::scene_cook::importers
