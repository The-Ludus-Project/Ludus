#pragma once

// Test-side loading of the engine's diffraction test scenes (tests/scenes/diffraction) for the edge-diffraction oracle
// tests: read a scene source file through FoundationFilesystem, parse it and build the oracle's scene.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>

#include "edge_diffraction_oracle.h"

#include <span>
#include <string_view>

namespace ludus::graphics::renderer::testing
{
/// Reads a whole file named by an absolute or working-directory-relative path. Returns false, leaving outBytes
/// unchanged, when it cannot be read.
[[nodiscard]] bool TryReadDiffractionFile(std::string_view path,
                                          foundation::Array<foundation::uint8>& outBytes) noexcept;

/// Writes bytes to a file in an existing directory, replacing it atomically. Returns false when it fails.
[[nodiscard]] bool TryWriteDiffractionFile(std::string_view directory,
                                           std::string_view name,
                                           std::span<const foundation::uint8> bytes) noexcept;

/// Reads, parses and builds the diffraction scene of a scene source file. Returns false, leaving outScene unchanged,
/// when the file is unreadable, malformed or not a diffraction scene.
[[nodiscard]] bool TryLoadDiffractionScene(std::string_view path, EdgeDiffractionScene& outScene) noexcept;
} // namespace ludus::graphics::renderer::testing
