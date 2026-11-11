#pragma once

// The asset types the Content browser lists, from both project catalogs: the authored audio catalog
// (content/catalog.json, see ludus::content::Kind) and the cooked scene asset catalog (assets/catalog.json, whose
// records carry a "kind" string). See docs/development/editor-workspace.md, "S3 content browser".

#include "internal/editor_icons.h"

#include <ludus/foundation/base/types.h>

#include <ludus/content/content.h>

#include <QString>
#include <QStringView>

namespace ludus::editor
{
/// One browsable asset type. The first six mirror content::Kind; the rest are scene asset catalog kinds.
enum class ContentType : foundation::uint8
{
    AudioSource,
    Sound,
    Music,
    Mesh,
    CookedScene,
    CookedGeometry,
    Texture,
    Material,
    Scene,
    Lighting,
    License,
};

/// Number of values in ContentType.
inline constexpr foundation::usize CONTENT_TYPE_COUNT = 11;

/// The groups the type filter chips offer. A row belongs to exactly one category.
enum class ContentCategory : foundation::uint8
{
    Textures,
    Materials,
    Meshes,
    Scenes,
    Audio,
    Other,
};

/// Number of values in ContentCategory.
inline constexpr foundation::usize CONTENT_CATEGORY_COUNT = 6;

/// Bit set of ContentCategory values; zero means every category.
using ContentCategoryMask = foundation::uint32;

/// The mask bit of one category.
[[nodiscard]] constexpr ContentCategoryMask GetContentCategoryBit(ContentCategory category) noexcept
{
    return ContentCategoryMask{1} << static_cast<foundation::uint32>(category);
}

/// The ContentType of an audio catalog entry.
[[nodiscard]] ContentType ToContentType(content::Kind kind) noexcept;

/// Parse a scene asset catalog record "kind" ("texture", "material", "mesh", "scene", "lighting" or "license").
/// Returns false and leaves outType unchanged for any other kind.
[[nodiscard]] bool TryParseSceneAssetKind(QStringView kind, ContentType& outType) noexcept;

/// The chip a type is filtered by.
[[nodiscard]] ContentCategory GetContentCategory(ContentType type) noexcept;

/// The name shown in the Type column and matched by search, such as "Texture".
[[nodiscard]] QString GetContentTypeName(ContentType type);

/// The plural label of a filter chip, such as "Textures".
[[nodiscard]] QString GetContentCategoryName(ContentCategory category);

/// The icon that stands for a type wherever no thumbnail is drawn.
[[nodiscard]] EditorIcon GetContentTypeIcon(ContentType type) noexcept;

/// Whether the browser draws a preview thumbnail for this type (cheap to produce from its cooked bytes).
[[nodiscard]] bool HasContentThumbnail(ContentType type) noexcept;
} // namespace ludus::editor
