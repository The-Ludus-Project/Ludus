#include "internal/content_import.h"

#include <ludus/audio/audio_source.h>
#include <ludus/audio/content/definitions.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <span>
#include <string_view>

namespace ludus::editor
{
namespace
{
using namespace foundation;
using CS = content::Status;
std::string_view View(const QByteArray& bytes)
{
    return {bytes.constData(), static_cast<usize>(bytes.size())};
}
QString Text(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}
} // namespace

// Reuses Content's bounded codecs, dependency validation and compare-and-swap
// saves. Immutable candidates adapt the publication contract in
// docs/architecture/content-resources.md, "Saving and replacing content".
ContentImportResult ImportAudioSource(const ContentImportSource& source,
                                      ContentImportGate& gate,
                                      const content::Digest* expectedCatalog,
                                      ContentImportHooks hooks)
{
    const auto& root = source.Root;
    const auto& file = source.File;
    const auto& id = source.Id;
    const auto rootBytes = root.toUtf8(), idBytes = id.toUtf8();
    const auto rootView = View(rootBytes), idView = View(idBytes);
    const auto cancelled = [&]() {
        if (hooks.Cancelled != nullptr && hooks.Cancelled(hooks.Context))
        {
            (void)gate.Cancel();
        }
        return gate.Cancelled();
    };
    const auto failed = [](CS status, const QString& message) { return ContentImportResult{status, message, {}}; };
    if (cancelled())
    {
        return failed(CS::Cancelled, QStringLiteral("Import cancelled before publication."));
    }
    if (content::ValidId(idView) == false)
    {
        return failed(CS::Invalid, QStringLiteral("Use a stable lowercase resource ID, with digits, slash or hyphen."));
    }
    const auto suffix = QFileInfo(file).suffix().toLower();
    if (suffix != QStringLiteral("wav") && suffix != QStringLiteral("flac"))
    {
        return failed(CS::Unsupported, QStringLiteral("Choose a WAV or FLAC source."));
    }
    QFile input(file);
    if (input.open(QIODevice::ReadOnly) == false || input.size() <= 0 ||
        static_cast<uint64>(input.size()) > audio::BROWSER_ENCODED_CAP_BYTES)
    {
        return failed(CS::IoError, QStringLiteral("Cannot read source, or it exceeds the encoded audio limit."));
    }
    const auto readCap = static_cast<int64>(audio::BROWSER_ENCODED_CAP_BYTES) + 1;
    const auto data = input.read(readCap);
    if (static_cast<uint64>(data.size()) > audio::BROWSER_ENCODED_CAP_BYTES)
    {
        return failed(CS::Limit, QStringLiteral("Source exceeds the encoded audio limit."));
    }
    if (data.size() != input.size())
    {
        return failed(CS::Conflict, QStringLiteral("Source changed during reading; retry the import."));
    }
    const std::span<const uint8> encoded(reinterpret_cast<const uint8*>(data.constData()),
                                         static_cast<usize>(data.size()));
    audio::SourceInfo info;
    const auto format = suffix == QStringLiteral("flac") ? audio::SourceFormat::Flac : audio::SourceFormat::Wav;
    if (audio::InspectSource(encoded, format, info) != audio::Status::Ok)
    {
        return failed(CS::Invalid, QStringLiteral("Source decoding failed; the last valid artifact is retained."));
    }
    content::Bytes oldCatalog;
    auto status = content::ReadFile(rootView, "catalog.json", content::MAX_DOCUMENT_BYTES, oldCatalog);
    const bool fresh = status == CS::NotFound;
    content::Catalog catalog;
    content::Diagnostic diagnostic;
    if (fresh == false && (status != CS::Ok || catalog.Read(oldCatalog.String(), diagnostic) != CS::Ok))
    {
        return failed(status == CS::Ok ? CS::Invalid : status,
                      QStringLiteral("Catalog is unreadable or invalid; it has not been replaced."));
    }
    const auto catalogDigest = content::Hash(oldCatalog.Data());
    if (expectedCatalog != nullptr && (fresh || catalogDigest != *expectedCatalog))
    {
        return failed(CS::Conflict, QStringLiteral("Catalog changed since browsing; refresh before importing."));
    }
    const auto* existing = catalog.Find(idView);
    if (existing != nullptr && existing->Type != content::Kind::AudioSource)
    {
        return failed(CS::Conflict, QStringLiteral("This ID belongs to a different resource kind."));
    }
    QHash<QString, content::Digest> dependencies;
    for (const auto& dependent : catalog.Entries())
    {
        if (cancelled())
        {
            return failed(CS::Cancelled, QStringLiteral("Import cancelled during validation."));
        }
        if (dependent.Type != content::Kind::Sound && dependent.Type != content::Kind::Music)
        {
            continue;
        }
        content::Bytes document;
        status = content::ReadFile(rootView, dependent.Path.View(), content::MAX_DOCUMENT_BYTES, document);
        if (status != CS::Ok)
        {
            return failed(status, QStringLiteral("Cannot validate dependency: ") + Text(dependent.Id.View()));
        }
        dependencies.insert(Text(dependent.Path.View()), content::Hash(document.Data()));
        audio::content::Loop loop;
        bool references = false;
        audio::content::Sound sound;
        if (dependent.Type == content::Kind::Music)
        {
            audio::content::Music music;
            if (audio::content::ReadMusic(document.String(), music, diagnostic) != CS::Ok ||
                music.Id.View() != dependent.Id.View())
            {
                return failed(CS::Invalid, QStringLiteral("Invalid music definition: ") + Text(dependent.Id.View()));
            }
            references = music.Source.View() == idView;
            loop = music.Region;
        }
        else
        {
            if (audio::content::ReadSound(document.String(), sound, diagnostic) != CS::Ok ||
                sound.Id.View() != dependent.Id.View())
            {
                return failed(CS::Invalid, QStringLiteral("Invalid sound definition: ") + Text(dependent.Id.View()));
            }
            loop = sound.Region;
            for (usize index = 0; index < sound.VariationCount; ++index)
            {
                references |= sound.Variations[index].View() == idView;
            }
            if (references && loop.Enabled)
            {
                for (usize index = 0; index < sound.VariationCount; ++index)
                {
                    if (cancelled())
                    {
                        return failed(CS::Cancelled, QStringLiteral("Import cancelled during dependency validation."));
                    }
                    if (sound.Variations[index].View() == idView)
                    {
                        continue;
                    }
                    const auto* other = catalog.Find(sound.Variations[index].View());
                    content::Bytes otherBytes;
                    audio::SourceInfo otherInfo;
                    if (other == nullptr ||
                        content::ReadFile(rootView, other->Path.View(), audio::BROWSER_ENCODED_CAP_BYTES, otherBytes) !=
                            CS::Ok ||
                        audio::InspectSource(otherBytes.Data(),
                                             other->Path.View().ends_with(".flac") ? audio::SourceFormat::Flac
                                                                                   : audio::SourceFormat::Wav,
                                             otherInfo) != audio::Status::Ok ||
                        otherInfo.SampleRate != info.SampleRate)
                    {
                        return failed(CS::Invalid, QStringLiteral("Loop variations need matching sample rates."));
                    }
                    const auto path = Text(other->Path.View());
                    const auto digest = content::Hash(otherBytes.Data());
                    if (dependencies.contains(path) && dependencies.value(path) != digest)
                    {
                        return failed(CS::Conflict, QStringLiteral("Dependency changed during validation; retry."));
                    }
                    dependencies.insert(path, digest);
                }
            }
        }
        if (references && loop.Enabled && loop.End > info.Frames)
        {
            return failed(CS::Invalid,
                          QStringLiteral("Reimport would invalidate the saved loop in ") + Text(dependent.Id.View()));
        }
    }
    // Version/profile participates in the artifact key; no importer settings
    // are currently configurable. Bytes are copied without transcoding.
    content::Hasher key;
    constexpr std::string_view PROFILE = "ludus-audio-copy-v1/native/";
    (void)key.Add({reinterpret_cast<const uint8*>(PROFILE.data()), PROFILE.size()});
    const auto formatBytes = suffix.toLatin1();
    (void)key.Add({reinterpret_cast<const uint8*>(formatBytes.constData()), static_cast<usize>(formatBytes.size())});
    (void)key.Add(encoded);
    const auto digest = key.Finish();
    const auto hex = QByteArray(reinterpret_cast<const char*>(digest.Data), 32).toHex();
    const auto relative = QStringLiteral("sources/") + QString::fromLatin1(hex) + QLatin1Char('.') + suffix;
    const auto relativeBytes = relative.toUtf8();
    content::Resource resource;
    (void)resource.Id.Set(idView);
    (void)resource.Path.Set(View(relativeBytes));
    resource.Type = content::Kind::AudioSource;
    content::Catalog nextCatalog;
    for (const auto& entry : catalog.Entries())
    {
        if (entry.Id.View() != idView)
        {
            status = nextCatalog.Put(entry);
            if (status != CS::Ok)
            {
                return failed(status, QStringLiteral("Cannot prepare candidate catalog."));
            }
        }
    }
    content::Bytes next;
    status = nextCatalog.Put(resource);
    if (status == CS::Ok)
    {
        status = nextCatalog.Write(next);
    }
    if (status != CS::Ok)
    {
        return failed(status, QStringLiteral("Cannot prepare candidate catalog within content limits."));
    }
    if (hooks.BeforePublish != nullptr)
    {
        hooks.BeforePublish(hooks.Context);
    }
    // Revalidate every consulted dependency and the source snapshot before the
    // publication boundary. SaveFile still supplies the final catalog CAS.
    QFile currentInput(file);
    if (currentInput.open(QIODevice::ReadOnly) == false || currentInput.size() != data.size() ||
        currentInput.read(readCap) != data)
    {
        return failed(CS::Conflict, QStringLiteral("Source changed during validation; retry."));
    }
    for (auto dependency = dependencies.constBegin(); dependency != dependencies.constEnd(); ++dependency)
    {
        if (cancelled())
        {
            return failed(CS::Cancelled, QStringLiteral("Import cancelled during revision checks."));
        }
        content::Bytes current;
        const auto path = dependency.key().toUtf8();
        if (content::ReadFile(rootView, View(path), audio::BROWSER_ENCODED_CAP_BYTES, current) != CS::Ok ||
            content::Hash(current.Data()) != dependency.value())
        {
            return failed(CS::Conflict, QStringLiteral("Dependency changed during validation; refresh and retry."));
        }
    }
    if (cancelled() || gate.Publish() == false)
    {
        return failed(CS::Cancelled, QStringLiteral("Import cancelled before publication."));
    }
    if (QDir().mkpath(root + QStringLiteral("/sources")) == false)
    {
        return failed(CS::IoError, QStringLiteral("Cannot create the content source directory."));
    }
    status = content::SaveFile(rootView, resource.Path.View(), encoded, nullptr);
    if (status == CS::Conflict)
    {
        content::Bytes candidate;
        if (content::ReadFile(rootView, resource.Path.View(), audio::BROWSER_ENCODED_CAP_BYTES, candidate) == CS::Ok &&
            content::Hash(candidate.Data()) == content::Hash(encoded))
        {
            status = CS::Ok;
        }
    }
    if (status != CS::Ok)
    {
        return failed(status, QStringLiteral("Candidate publication failed; the old catalog is retained."));
    }
    status = content::SaveFile(rootView, "catalog.json", next.Data(), fresh ? nullptr : &catalogDigest);
    if (status != CS::Ok)
    {
        return {status,
                QStringLiteral("Catalog publication failed; the old version is retained. Unreferenced candidate: ") +
                    relative,
                relative};
    }
    return {CS::Ok, QStringLiteral("Imported source; stable ID retained: ") + id, relative};
}
} // namespace ludus::editor
