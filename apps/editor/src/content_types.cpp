#include "internal/content_types.h"

#include <QLatin1StringView>

namespace ludus::editor
{
ContentType ToContentType(content::Kind kind) noexcept
{
    switch (kind)
    {
        case content::Kind::AudioSource:
            return ContentType::AudioSource;
        case content::Kind::Sound:
            return ContentType::Sound;
        case content::Kind::Music:
            return ContentType::Music;
        case content::Kind::Mesh:
            return ContentType::Mesh;
        case content::Kind::CookedScene:
            return ContentType::CookedScene;
        case content::Kind::CookedGeometry:
            return ContentType::CookedGeometry;
    }
    return ContentType::AudioSource;
}

bool TryParseSceneAssetKind(QStringView kind, ContentType& outType) noexcept
{
    struct KindName final
    {
        QLatin1StringView Name;
        ContentType Type;
    };
    // The artifact kinds scene_assets.cpp writes (see IsArtifactKind there).
    static constexpr KindName KINDS[] = {
        {QLatin1StringView("texture"), ContentType::Texture},
        {QLatin1StringView("material"), ContentType::Material},
        {QLatin1StringView("mesh"), ContentType::Mesh},
        {QLatin1StringView("scene"), ContentType::Scene},
        {QLatin1StringView("lighting"), ContentType::Lighting},
        {QLatin1StringView("license"), ContentType::License},
    };
    for (const KindName& candidate : KINDS)
    {
        if (kind == candidate.Name)
        {
            outType = candidate.Type;
            return true;
        }
    }
    return false;
}

ContentCategory GetContentCategory(ContentType type) noexcept
{
    switch (type)
    {
        case ContentType::Texture:
            return ContentCategory::Textures;
        case ContentType::Material:
            return ContentCategory::Materials;
        case ContentType::Mesh:
        case ContentType::CookedGeometry:
            return ContentCategory::Meshes;
        case ContentType::Scene:
        case ContentType::CookedScene:
        case ContentType::Lighting:
            return ContentCategory::Scenes;
        case ContentType::AudioSource:
        case ContentType::Sound:
        case ContentType::Music:
            return ContentCategory::Audio;
        case ContentType::License:
            return ContentCategory::Other;
    }
    return ContentCategory::Other;
}

QString GetContentTypeName(ContentType type)
{
    switch (type)
    {
        case ContentType::AudioSource:
            return QStringLiteral("Audio source");
        case ContentType::Sound:
            return QStringLiteral("Sound");
        case ContentType::Music:
            return QStringLiteral("Music");
        case ContentType::Mesh:
            return QStringLiteral("Mesh");
        case ContentType::CookedScene:
            return QStringLiteral("Cooked scene");
        case ContentType::CookedGeometry:
            return QStringLiteral("Cooked geometry");
        case ContentType::Texture:
            return QStringLiteral("Texture");
        case ContentType::Material:
            return QStringLiteral("Material");
        case ContentType::Scene:
            return QStringLiteral("Scene");
        case ContentType::Lighting:
            return QStringLiteral("Lighting");
        case ContentType::License:
            return QStringLiteral("Licence");
    }
    return QStringLiteral("Unknown");
}

QString GetContentCategoryName(ContentCategory category)
{
    switch (category)
    {
        case ContentCategory::Textures:
            return QStringLiteral("Textures");
        case ContentCategory::Materials:
            return QStringLiteral("Materials");
        case ContentCategory::Meshes:
            return QStringLiteral("Meshes");
        case ContentCategory::Scenes:
            return QStringLiteral("Scenes");
        case ContentCategory::Audio:
            return QStringLiteral("Audio");
        case ContentCategory::Other:
            return QStringLiteral("Other");
    }
    return QStringLiteral("Other");
}

EditorIcon GetContentTypeIcon(ContentType type) noexcept
{
    switch (type)
    {
        case ContentType::AudioSource:
            return EditorIcon::AudioSource;
        case ContentType::Sound:
            return EditorIcon::Audio;
        case ContentType::Music:
            return EditorIcon::Music;
        case ContentType::Mesh:
            return EditorIcon::Mesh;
        case ContentType::CookedGeometry:
            return EditorIcon::Geometry;
        case ContentType::Texture:
            return EditorIcon::Texture;
        case ContentType::Material:
            return EditorIcon::Material;
        case ContentType::Scene:
        case ContentType::CookedScene:
            return EditorIcon::Scene;
        case ContentType::Lighting:
            return EditorIcon::Lighting;
        case ContentType::License:
            return EditorIcon::License;
    }
    return EditorIcon::UnknownAsset;
}

bool HasContentThumbnail(ContentType type) noexcept
{
    return type == ContentType::Texture || type == ContentType::Material;
}
} // namespace ludus::editor
