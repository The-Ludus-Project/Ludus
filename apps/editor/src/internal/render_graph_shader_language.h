#pragma once

#include <ludus/foundation/base/core.h>

#include <QString>

namespace ludus::editor
{
enum class RenderGraphShaderLanguage : foundation::uint8
{
    Lusl,
    Slang,
};

[[nodiscard]] LUDUS_INLINE QString GetShaderSourceSuffix(RenderGraphShaderLanguage language)
{
    return language == RenderGraphShaderLanguage::Slang ? QStringLiteral(".slang") : QStringLiteral(".lusl");
}

[[nodiscard]] LUDUS_INLINE bool IsLuslShaderSource(const QString& path)
{
    return path.endsWith(GetShaderSourceSuffix(RenderGraphShaderLanguage::Lusl)) ||
           path.endsWith(QStringLiteral(".luslh"));
}

[[nodiscard]] LUDUS_INLINE bool IsRenderGraphShaderSource(const QString& path)
{
    return path.endsWith(GetShaderSourceSuffix(RenderGraphShaderLanguage::Slang)) || IsLuslShaderSource(path) == true;
}
} // namespace ludus::editor
