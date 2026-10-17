#pragma once

#include <ludus/content/scene_cook/scene_assets.h>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/platform/entropy.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <cstring>
#include <span>
#include <string_view>

namespace ludus::editor::tests
{
/// Cook replacement mirroring the former Python test fixture: one triangle
/// object, one material and no textures, so it composes with authored worlds.
inline bool CookFixture(std::string_view source,
                        content::scene_cook::CookResult& outResult,
                        content::scene_cook::CookError& outError,
                        const content::scene_cook::Cancellation* cancelOrNull) noexcept
{
    (void)outError;
    (void)cancelOrNull;
    QByteArray bundle("LUSCN01", 8);
    QDataStream stream(&bundle, QIODevice::Append);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << quint32(1) << quint32(1) << quint32(1) << quint32(0) << quint32(1);
    for (const float value : {0.0F, 0.0F, 3.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F})
    {
        stream << value;
    }
    stream << quint32(0xffffffffU) << 0.0F << 0.5F << 1.0F << 1.0F << 1.0F << quint32(3) << quint32(3);
    const float corners[3][2] = {{0.0F, 0.0F}, {1.0F, 0.0F}, {0.0F, 1.0F}};
    for (const float (&corner)[2] : corners)
    {
        stream << corner[0] << corner[1] << 0.0F << 0.0F << 0.0F << 1.0F << 0.0F << 0.0F;
    }
    stream << quint32(0) << quint32(1) << quint32(2) << quint32(0) << quint32(0);
    for (const float value : {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F})
    {
        stream << value;
    }
    QFile input(QDir(QString::fromUtf8(source.data(), static_cast<qsizetype>(source.size())))
                    .filePath(QStringLiteral("source")));
    if (!input.open(QIODevice::ReadOnly))
    {
        return false;
    }
    const QByteArray digest = QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256).toHex();
    const QByteArray manifest =
        QByteArrayLiteral("{\"version\": 1, \"inputs\": {\"source\": \"") + digest + QByteArrayLiteral("\"}}\n");
    const QByteArray license("Synthetic fixture license\n");
    const auto assign = [](foundation::Array<foundation::uint8>& outBytes, const QByteArray& bytes) {
        return outBytes.TryResize(static_cast<foundation::usize>(bytes.size())) &&
               (bytes.isEmpty() || std::memcpy(outBytes.GetData(), bytes.constData(), outBytes.GetSize()) != nullptr);
    };
    return assign(outResult.Bundle, bundle) && assign(outResult.Manifest, manifest) &&
           assign(outResult.License, license);
}

/// Copy a directory tree; used to give a test a source it may delete.
inline bool CopyTree(const QString& from, const QString& to)
{
    if (!QDir().mkpath(to))
    {
        return false;
    }
    QDirIterator files(from, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext())
    {
        const QString file = files.next();
        const QString target = QDir(to).filePath(QDir(from).relativeFilePath(file));
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(file, target))
        {
            return false;
        }
    }
    return true;
}

/// Create a fixture source directory accepted by CookFixture.
inline bool CreateFixtureSource(const QString& source)
{
    QFile file(QDir(source).filePath(QStringLiteral("source")));
    return QDir().mkpath(source) && file.open(QIODevice::WriteOnly) && file.write("fixture") == 7;
}

/// Import a source into a new project level and save it, as the Editor would.
inline bool PrepareImportedLevel(const QString& root, const QString& relative, const QString& source, QString& outError)
{
    namespace cook = ludus::content::scene_cook;
    if (!QDir().mkpath(root))
    {
        outError = QStringLiteral("cannot create project");
        return false;
    }
    const QByteArray project = root.toUtf8();
    const QByteArray path = relative.toUtf8();
    const QByteArray from = source.toUtf8();
    const auto view = [](const QByteArray& bytes) {
        return std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    };
    const auto span = [](const foundation::Array<foundation::uint8>& bytes) {
        return std::span<const foundation::uint8>(bytes.GetData(), bytes.GetSize());
    };
    const foundation::identity::EntropySource entropy = platform::GetSystemEntropySource();
    cook::AssetOptions options;
    options.CookOrNull = &CookFixture;
    options.EntropyOrNull = &entropy;
    cook::AssetError error;
    foundation::Array<foundation::uint8> level;
    foundation::Array<foundation::uint8> imported;
    foundation::Array<foundation::uint8> saved;
    const bool isPrepared = cook::CreateLevel("Room", options, level, error) &&
                            cook::ImportScene(view(project), view(from), span(level), options, imported, error) &&
                            cook::SaveLevel(view(project), view(path), span(imported), nullptr, saved, error);
    if (!isPrepared)
    {
        const std::string_view message = error.Message.View();
        outError = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
    }
    return isPrepared;
}
} // namespace ludus::editor::tests
