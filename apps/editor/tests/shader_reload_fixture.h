#pragma once

#include "internal/preview_shader_build.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include "ludus_editor_shader_reload_paths.h"

/// @file
/// Copies of the build's renderer shader sources for live shader tests, which must never edit
/// the checkout itself.
namespace ludus::editor::tests
{
/// The build's renderer shader roots.
inline PreviewShaderSources GetCheckoutSources()
{
    PreviewShaderSources sources;
    sources.RecipeDirectory = QString::fromUtf8(shader_reload_paths::RECIPE_DIRECTORY);
    for (const char* directory : shader_reload_paths::INCLUDE_DIRECTORIES)
    {
        sources.IncludeDirectories.append(QString::fromUtf8(directory));
    }
    return sources;
}
/// Copy the shader files of every root under root, so tests edit a copy and never the checkout.
inline PreviewShaderSources CopySources(const PreviewShaderSources& sources, const QString& root)
{
    PreviewShaderSources result;
    QStringList directories{sources.RecipeDirectory};
    directories += sources.IncludeDirectories;
    for (qsizetype i = 0; i < directories.size(); ++i)
    {
        const QString target = QStringLiteral("%1/root%2").arg(root).arg(i);
        REQUIRE(QDir().mkpath(target) == true);
        QDirIterator files(directories[i],
                           {QStringLiteral("*.slang"), QStringLiteral("*.h"), QStringLiteral("*.json")},
                           QDir::Files,
                           QDirIterator::Subdirectories);
        while (files.hasNext() == true)
        {
            const QString path = files.next();
            const QString relative = QDir(directories[i]).relativeFilePath(path);
            if (relative.startsWith(QStringLiteral("out")) == true || relative.startsWith(QLatin1Char('.')) == true)
            {
                continue;
            }
            const QString destination = QDir(target).filePath(relative);
            REQUIRE(QDir().mkpath(QFileInfo(destination).absolutePath()) == true);
            REQUIRE(QFile::copy(path, destination) == true);
        }
        if (i == 0)
        {
            result.RecipeDirectory = target;
        }
        else
        {
            result.IncludeDirectories.append(target);
        }
    }
    return result;
}
inline QByteArray ReadText(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}
inline void WriteText(const QString& path, const QByteArray& text)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
    REQUIRE(file.write(text) == text.size());
}
} // namespace ludus::editor::tests
