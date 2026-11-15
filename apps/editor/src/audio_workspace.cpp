#include "internal/audio_workspace.h"
#include "internal/editor_files.h"
#include "internal/item_selection.h"

#include <ludus/audio/audio_types.h>
#include <ludus/audio/content/definitions.h>

#include <QCheckBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QVariant>
#include <initializer_list>

namespace ludus::editor
{
using namespace foundation;
namespace
{
/// Decimal places the gain field shows; gain is a linear factor in [0, 1].
constexpr int32 GAIN_DECIMALS = 4;
/// Largest attenuation distance the spatial fields accept, in world units.
constexpr float64 MAXIMUM_DISTANCE_INPUT = 1e9;
class Wave final : public QWidget
{
public:
    explicit Wave(QWidget* parent) : QWidget(parent)
    {
        setMinimumHeight(80);
    }
    QVector<float32> Peaks;
    uint64 Frames = 0;
    QLineEdit* Begin = nullptr;
    QLineEdit* End = nullptr;
    bool MovingBegin = true;
    void Move(const QMouseEvent* event)
    {
        if (Frames == 0 || width() == 0)
        {
            return;
        }
        const auto x = event->position().x();
        const auto frame = static_cast<uint64>((x < 0         ? 0
                                                : x > width() ? width()
                                                              : x) /
                                               width() * static_cast<float64>(Frames));
        (MovingBegin ? Begin : End)->setText(QString::number(frame));
        Q_EMIT(MovingBegin ? Begin : End)->textEdited(QString::number(frame));
        update();
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        MovingBegin = event->button() != Qt::RightButton;
        Move(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        Move(event);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(30, 35, 40));
        painter.setPen(QColor(80, 190, 170));
        if (Peaks.isEmpty())
        {
            return;
        }
        const float32 center = static_cast<float32>(height()) / 2;
        for (qsizetype i = 0; i < Peaks.size(); ++i)
        {
            const int x = static_cast<int>(i * width() / Peaks.size());
            const float32 peak = Peaks[i] < 1 ? Peaks[i] : 1;
            painter.drawLine(x, static_cast<int>(center - center * peak), x, static_cast<int>(center + center * peak));
        }
        if (Frames != 0)
        {
            painter.setPen(QColor(240, 180, 80));
            for (const auto* value : {Begin, End})
            {
                const auto x = static_cast<int>(static_cast<float64>(value->text().toULongLong()) /
                                                static_cast<float64>(Frames) * width());
                painter.drawLine(x, 0, x, height());
            }
        }
    }
};
std::string_view View(const QByteArray& bytes)
{
    return {bytes.constData(), static_cast<usize>(bytes.size())};
}
QString Name(std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
} // namespace
AudioWorkspace::AudioWorkspace(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    List_ = new QListWidget(this);
    ConfigureItemSelection(*List_);
    List_->setObjectName(QStringLiteral("audio-resources"));
    layout->addWidget(List_);
    auto* actions = new QHBoxLayout;
    layout->addLayout(actions);
    const auto button = [&](const QString& name, auto action) {
        auto* b = new QPushButton(name, this);
        actions->addWidget(b);
        connect(b, &QPushButton::clicked, this, action);
    };
    button(QStringLiteral("Import audio"), [this]() { Import(); });
    button(QStringLiteral("New sound"), [this]() { Create(false); });
    button(QStringLiteral("New music"), [this]() { Create(true); });
    button(QStringLiteral("Save audio"), [this]() { Save(); });
    button(QStringLiteral("Reload"), [this]() {
        if (ConfirmDiscard())
        {
            Reload();
        }
    });
    auto* form = new QFormLayout;
    layout->addLayout(form);
    mId = new QLineEdit(this);
    mId->setReadOnly(true);
    mId->setObjectName(QStringLiteral("audio-id"));
    mBus = new QLineEdit(this);
    mGroup = new QLineEdit(this);
    mSources = new QLineEdit(this);
    mLoopBegin = new QLineEdit(this);
    mLoopEnd = new QLineEdit(this);
    mPriority = new QSpinBox(this);
    mPriority->setRange(audio::PRIORITY_MIN, audio::PRIORITY_MAX);
    mCooldown = new QSpinBox(this);
    mCooldown->setRange(0, static_cast<int32>(audio::content::MAX_COOLDOWN_MILLISECONDS));
    mGain = new QDoubleSpinBox(this);
    mGain->setObjectName(QStringLiteral("audio-gain"));
    mGain->setRange(0, 1);
    mGain->setDecimals(GAIN_DECIMALS);
    mRate = new QDoubleSpinBox(this);
    mRate->setRange(static_cast<float64>(audio::RATE_MIN), static_cast<float64>(audio::RATE_MAX));
    mMinimumDistance = new QDoubleSpinBox(this);
    mMaximumDistance = new QDoubleSpinBox(this);
    mMinimumDistance->setRange(0, MAXIMUM_DISTANCE_INPUT);
    mMaximumDistance->setRange(0, MAXIMUM_DISTANCE_INPUT);
    mLoop = new QCheckBox(this);
    mSpatial = new QCheckBox(this);
    mSuppress = new QCheckBox(this);
    form->addRow(QStringLiteral("Resource ID"), mId);
    form->addRow(QStringLiteral("Bus"), mBus);
    form->addRow(QStringLiteral("Group"), mGroup);
    form->addRow(QStringLiteral("Source IDs (comma separated)"), mSources);
    form->addRow(QStringLiteral("Priority"), mPriority);
    form->addRow(QStringLiteral("Gain"), mGain);
    form->addRow(QStringLiteral("Rate"), mRate);
    form->addRow(QStringLiteral("Point source"), mSpatial);
    form->addRow(QStringLiteral("Minimum distance"), mMinimumDistance);
    form->addRow(QStringLiteral("Maximum distance"), mMaximumDistance);
    form->addRow(QStringLiteral("Loop"), mLoop);
    form->addRow(QStringLiteral("Loop begin source frame"), mLoopBegin);
    form->addRow(QStringLiteral("Loop end source frame"), mLoopEnd);
    form->addRow(QStringLiteral("Cooldown ms"), mCooldown);
    form->addRow(QStringLiteral("Suppress while active"), mSuppress);
    Wave_ = new Wave(this);
    static_cast<Wave*>(Wave_)->Begin = mLoopBegin;
    static_cast<Wave*>(Wave_)->End = mLoopEnd;
    layout->addWidget(Wave_);
    auto* playback = new QHBoxLayout;
    layout->addLayout(playback);
    auto* play = new QPushButton(QStringLiteral("Play draft"), this);
    auto* stop = new QPushButton(QStringLiteral("Stop"), this);
#if defined(Q_OS_WASM)
    play->setEnabled(false);
    stop->setEnabled(false);
    play->setToolTip(
        QStringLiteral("Audio playback requires a browser audio-device backend. Download to preview on desktop."));
#endif
    playback->addWidget(play);
    playback->addWidget(stop);
    Message_ = new QLabel(QStringLiteral("Open a project to browse its content/catalog.json."), this);
    Message_->setWordWrap(true);
    layout->addWidget(Message_);
    Preview_ = new AudioPreview(this);
    connect(Preview_, &AudioPreview::Message, Message_, &QLabel::setText, Qt::QueuedConnection);
    connect(
        Preview_,
        &AudioPreview::Waveform,
        this,
        [this](const QString& root, const QVector<float32>& peaks, uint64 frames) {
            if (root != Root_)
            {
                return;
            }
            static_cast<Wave*>(Wave_)->Frames = frames;
            static_cast<Wave*>(Wave_)->Peaks = peaks;
            Wave_->update();
        },
        Qt::QueuedConnection);
    auto* meterLabel = new QLabel(QStringLiteral("Output peak: —"), this);
    layout->addWidget(meterLabel);
    connect(Preview_, &AudioPreview::Meter, this, [meterLabel](float32 peak, uint64 starvations) {
        meterLabel->setText(QStringLiteral("Output peak: %1 · stream starvations: %2")
                                .arg(static_cast<float64>(peak), 0, 'f', 3)
                                .arg(starvations));
    });
    connect(Preview_, &AudioPreview::Imported, this, [this](const QString& root) {
        if (root == Root_ && Dirty_ == false)
        {
            Reload();
        }
    });
    connect(List_, &QListWidget::itemActivated, this, [this]() { Open(); });
    connect(play, &QPushButton::clicked, this, [this]() { Play(); });
    connect(stop, &QPushButton::clicked, Preview_, &AudioPreview::Stop);
    for (QLineEdit* edit : {mBus, mGroup, mSources, mLoopBegin, mLoopEnd})
    {
        connect(edit, &QLineEdit::textEdited, this, [this]() { Edit(); });
    }
    for (QDoubleSpinBox* spin : {mGain, mRate, mMinimumDistance, mMaximumDistance})
    {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this]() { Edit(); });
    }
    for (QSpinBox* spin : {mPriority, mCooldown})
    {
        connect(spin, &QSpinBox::valueChanged, this, [this]() { Edit(); });
    }
    for (QCheckBox* check : {mLoop, mSpatial, mSuppress})
    {
        connect(check, &QCheckBox::toggled, this, [this]() { Edit(); });
    }
    Render();
}
void AudioWorkspace::SetRoot(const QString& root)
{
    if (Root_ == root)
    {
        return;
    }
    if (ConfirmDiscard() == false)
    {
        return;
    }
    StopPreview();
    Root_ = root;
    Catalog_ = ludus::content::Catalog{};
    Sound_ = {};
    Music_ = {};
    HasDocument_ = false;
    Dirty_ = false;
    Reload();
    Render();
}
void AudioWorkspace::Reload()
{
    List_->clear();
    if (Root_.isEmpty())
    {
        Message_->setText(QStringLiteral("Open a project to browse audio content."));
        return;
    }
    ludus::content::Bytes bytes;
    ludus::content::Diagnostic diagnostic;
    const auto root = Root_.toUtf8();
    const auto status = ludus::content::ReadFile(View(root), "catalog.json", ludus::content::MAX_DOCUMENT_BYTES, bytes);
    if (status != ludus::content::Status::Ok || Catalog_.Read(bytes.String(), diagnostic) != ludus::content::Status::Ok)
    {
        Message_->setText(
            QStringLiteral("Content catalog missing or invalid. Import creates a new catalog only when none exists."));
        return;
    }
    for (const auto& item : Catalog_.Entries())
    {
        List_->addItem(Name(item.Id.View()));
    }
    Message_->setText(QStringLiteral("Activate a sound or music resource to edit."));
    if (HasDocument_)
    {
        const auto id = IsMusic_ ? Music_.Id.View() : Sound_.Id.View();
        for (int i = 0; i < List_->count(); ++i)
        {
            if (List_->item(i)->text() == Name(id))
            {
                List_->setCurrentRow(i);
                Open();
                break;
            }
        }
    }
}
void AudioWorkspace::RefreshCatalog()
{
    if (Dirty_ == false)
    {
        Reload();
    }
}
bool AudioWorkspace::OpenResource(const QString& id)
{
    if (ConfirmDiscard() == false)
    {
        return false;
    }
    Reload();
    for (int row = 0; row < List_->count(); ++row)
    {
        if (List_->item(row)->text() == id)
        {
            List_->setCurrentRow(row);
            Open();
            return HasDocument_ && mId->text() == id;
        }
    }
    return false;
}
bool AudioWorkspace::ConfirmDiscard()
{
    if (Dirty_ == false)
    {
        return true;
    }
    const auto result = QMessageBox::question(this,
                                              QStringLiteral("Audio changes"),
                                              QStringLiteral("Save audio changes before continuing?"),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (result == QMessageBox::Save)
    {
        Save();
        return Dirty_ == false;
    }
    if (result == QMessageBox::Discard)
    {
        Dirty_ = false;
        Render();
        return true;
    }
    return false;
}
void AudioWorkspace::Open()
{
    if (List_->currentItem() == nullptr || ConfirmDiscard() == false)
    {
        return;
    }
    const auto id = List_->currentItem()->text().toUtf8();
    const auto* entry = Catalog_.Find(View(id));
    if (entry == nullptr || entry->Type == ludus::content::Kind::AudioSource)
    {
        Message_->setText(QStringLiteral("Create a sound or music definition referencing this source."));
        return;
    }
    if (entry->Type != ludus::content::Kind::Sound && entry->Type != ludus::content::Kind::Music)
    {
        Message_->setText(QStringLiteral("This resource is not a sound or music definition."));
        return;
    }
    ludus::content::Bytes bytes;
    ludus::content::Diagnostic diagnostic;
    const auto root = Root_.toUtf8();
    if (ludus::content::ReadFile(View(root), entry->Path.View(), ludus::content::MAX_DOCUMENT_BYTES, bytes) !=
        ludus::content::Status::Ok)
    {
        Message_->setText(QStringLiteral("Cannot read audio definition."));
        return;
    }
    audio::content::Sound sound;
    audio::content::Music music;
    const bool isMusic = entry->Type == ludus::content::Kind::Music;
    const auto status = isMusic ? audio::content::ReadMusic(bytes.String(), music, diagnostic)
                                : audio::content::ReadSound(bytes.String(), sound, diagnostic);
    if (status != ludus::content::Status::Ok || (isMusic ? music.Id.View() : sound.Id.View()) != entry->Id.View())
    {
        Message_->setText(QStringLiteral("Invalid definition: ") + QString::fromUtf8(diagnostic.Field));
        return;
    }
    StopPreview();
    Sound_ = sound;
    Music_ = music;
    IsMusic_ = isMusic;
    Path_ = Name(entry->Path.View());
    Digest_ = ludus::content::Hash(bytes.Data());
    Dirty_ = false;
    HasDocument_ = true;
    Render();
}
void AudioWorkspace::Render()
{
    Rendering_ = true;
    const auto& region = IsMusic_ ? Music_.Region : Sound_.Region;
    mId->setText(Name(IsMusic_ ? Music_.Id.View() : Sound_.Id.View()));
    mBus->setText(Name(IsMusic_ ? Music_.Bus.View() : Sound_.Bus.View()));
    mGroup->setText(Name(Sound_.Group.View()));
    QStringList sources;
    if (IsMusic_)
    {
        sources.append(Name(Music_.Source.View()));
    }
    else
    {
        for (usize i = 0; i < Sound_.VariationCount; ++i)
        {
            sources.append(Name(Sound_.Variations[i].View()));
        }
    }
    mSources->setText(sources.join(QStringLiteral(", ")));
    mPriority->setValue(IsMusic_ ? Music_.Priority : Sound_.Priority);
    mGain->setValue(static_cast<float64>(IsMusic_ ? Music_.Gain : Sound_.Gain));
    mRate->setValue(static_cast<float64>(Sound_.Rate));
    mMinimumDistance->setValue(static_cast<float64>(Sound_.MinDistance));
    mMaximumDistance->setValue(static_cast<float64>(Sound_.MaxDistance));
    mSpatial->setChecked(Sound_.Positional);
    mSuppress->setChecked(Sound_.SuppressWhileActive);
    mCooldown->setValue(static_cast<int>(Sound_.CooldownMs));
    mLoop->setChecked(region.Enabled);
    mLoopBegin->setText(QString::number(region.Begin));
    mLoopEnd->setText(QString::number(region.End));
    for (QWidget* widget : std::initializer_list<QWidget*>{mGroup,
                                                           mRate,
                                                           mMinimumDistance,
                                                           mMaximumDistance,
                                                           mSpatial,
                                                           mSuppress,
                                                           mCooldown})
    {
        widget->setEnabled(IsMusic_ == false && HasDocument_);
    }
    for (QDoubleSpinBox* spin : {mGain, mRate, mMinimumDistance, mMaximumDistance})
    {
        spin->setProperty("displayedValue", spin->value());
    }
    Rendering_ = false;
    Q_EMIT DocumentChanged();
}
void AudioWorkspace::Edit()
{
    if (Rendering_ == false && HasDocument_)
    {
        Dirty_ = true;
        Message_->setText(QStringLiteral("Unsaved audio draft."));
        Q_EMIT DocumentChanged();
    }
}
bool AudioWorkspace::ReadDraft(audio::content::Sound& sound, audio::content::Music& music)
{
    if (HasDocument_ == false)
    {
        return false;
    }
    // Read forms through Play's shared draft validation without starting output.
    const QByteArray bus = mBus->text().toUtf8();
    const QByteArray group = mGroup->text().toUtf8();
    sound = Sound_;
    music = Music_;
    bool beginOk = false, endOk = false;
    audio::content::Loop region{mLoop->isChecked(),
                                mLoopBegin->text().toULongLong(&beginOk),
                                mLoopEnd->text().toULongLong(&endOk)};
    if (beginOk == false || endOk == false)
    {
        Message_->setText(QStringLiteral("Loop frames must be unsigned integers."));
        return false;
    }
    if (bus.size() > 128 || group.size() > 128)
    {
        Message_->setText(QStringLiteral("Routing names exceed 128 bytes."));
        return false;
    }
    const QStringList sources = mSources->text().split(QLatin1Char(','));
    for (const auto& source : sources)
    {
        if (source.trimmed().toUtf8().size() > 128)
        {
            Message_->setText(QStringLiteral("Source ID exceeds 128 bytes."));
            return false;
        }
    }
    if (IsMusic_)
    {
        (void)music.Bus.Set(View(bus));
        music.Gain =
            (mGain->value() == mGain->property("displayedValue").toDouble() ? (IsMusic_ ? Music_.Gain : Sound_.Gain)
                                                                            : static_cast<float32>(mGain->value()));
        music.Priority = static_cast<uint8>(mPriority->value());
        music.Region = region;
        (void)music.Source.Set(View(mSources->text().trimmed().toUtf8()));
    }
    else
    {
        if (sources.size() > 16)
        {
            Message_->setText(QStringLiteral("At most 16 variations."));
            return false;
        }
        (void)sound.Bus.Set(View(bus));
        (void)sound.Group.Set(View(group));
        sound.Gain =
            (mGain->value() == mGain->property("displayedValue").toDouble() ? (IsMusic_ ? Music_.Gain : Sound_.Gain)
                                                                            : static_cast<float32>(mGain->value()));
        sound.Rate =
            (mRate->value() == mRate->property("displayedValue").toDouble() ? Sound_.Rate
                                                                            : static_cast<float32>(mRate->value()));
        sound.Priority = static_cast<uint8>(mPriority->value());
        sound.Region = region;
        sound.Positional = mSpatial->isChecked();
        sound.MinDistance = (mMinimumDistance->value() == mMinimumDistance->property("displayedValue").toDouble()
                                 ? Sound_.MinDistance
                                 : static_cast<float32>(mMinimumDistance->value()));
        sound.MaxDistance = (mMaximumDistance->value() == mMaximumDistance->property("displayedValue").toDouble()
                                 ? Sound_.MaxDistance
                                 : static_cast<float32>(mMaximumDistance->value()));
        sound.CooldownMs = static_cast<uint32>(mCooldown->value());
        sound.SuppressWhileActive = mSuppress->isChecked();
        sound.VariationCount = static_cast<usize>(sources.size());
        for (qsizetype i = 0; i < sources.size(); ++i)
        {
            (void)sound.Variations[i].Set(View(sources[i].trimmed().toUtf8()));
        }
    }
    ludus::content::Bytes bytes;
    auto status = IsMusic_ ? audio::content::WriteMusic(music, bytes) : audio::content::WriteSound(sound, bytes);
    if (status != ludus::content::Status::Ok)
    {
        Message_->setText(QStringLiteral("Draft failed validation."));
        return false;
    }
    const std::string_view buses[] = {"master", "sfx", "music"}, groups[] = {"default", "impacts"};
    const audio::content::Routing routing{buses, groups};
    ludus::content::Diagnostic diagnostic;
    uint32 musicBus = 0;
    audio::app::EventDescriptor descriptor;
    status = IsMusic_ ? audio::content::ValidateMusic(music, Catalog_, routing, musicBus, diagnostic)
                      : audio::content::Resolve(sound, Catalog_, routing, descriptor, diagnostic);
    if (status != ludus::content::Status::Ok)
    {
        Message_->setText(QStringLiteral("Invalid dependency or preview routing: ") +
                          QString::fromUtf8(diagnostic.Field));
        return false;
    }
    return true;
}
void AudioWorkspace::Save()
{
    audio::content::Sound sound;
    audio::content::Music music;
    if (ReadDraft(sound, music) == false)
    {
        return;
    }
    ludus::content::Bytes bytes;
    auto status = IsMusic_ ? audio::content::WriteMusic(music, bytes) : audio::content::WriteSound(sound, bytes);
    if (status != ludus::content::Status::Ok)
    {
        return;
    }
    const auto root = Root_.toUtf8(), path = Path_.toUtf8();
    status = ludus::content::SaveFile(View(root), View(path), bytes.Data(), &Digest_);
    if (status != ludus::content::Status::Ok)
    {
        Message_->setText(QStringLiteral("Save failed or file changed externally; draft retained."));
        return;
    }
    Sound_ = sound;
    Music_ = music;
    Digest_ = ludus::content::Hash(bytes.Data());
    Dirty_ = false;
    Message_->setText(QStringLiteral("Audio saved."));
#if defined(Q_OS_WASM)
    MarkBrowserEdited();
#endif
    Q_EMIT DocumentChanged();
}
void AudioWorkspace::Play()
{
    audio::content::Sound sound;
    audio::content::Music music;
    if (ReadDraft(sound, music) == false)
    {
        return;
    }
    if (IsMusic_)
    {
        Preview_->Play(Root_, music);
    }
    else
    {
        Preview_->Play(Root_, sound);
    }
}
void AudioWorkspace::StopPreview()
{
    Preview_->Stop();
}
void AudioWorkspace::ShutdownPreview()
{
    Preview_->Shutdown();
}
bool AudioWorkspace::PreviewFinished() const noexcept
{
    return Preview_->Finished();
}

void ludus::editor::AudioWorkspace::Import()
{
    if (Root_.isEmpty() == false)
    {
        Q_EMIT ImportRequested();
    }
}
void ludus::editor::AudioWorkspace::Create(bool music)
{
    if (Root_.isEmpty() || ConfirmDiscard() == false)
    {
        return;
    }
    bool ok = false;
    const auto id = QInputDialog::getText(this,
                                          QStringLiteral("Audio ID"),
                                          QStringLiteral("New logical resource ID"),
                                          QLineEdit::Normal,
                                          music ? QStringLiteral("music/theme") : QStringLiteral("sound/impact"),
                                          &ok)
                        .toUtf8();
    if (ok == false)
    {
        return;
    }
    if (ludus::content::ValidId(View(id)) == false || Catalog_.Find(View(id)) != nullptr)
    {
        Message_->setText(QStringLiteral("Invalid or duplicate ID."));
        return;
    }
    ludus::content::ResourceId source;
    if (List_->currentItem() != nullptr)
    {
        const auto selected = List_->currentItem()->text().toUtf8();
        const auto* item = Catalog_.Find(View(selected));
        if (item != nullptr && item->Type == ludus::content::Kind::AudioSource)
        {
            source = item->Id;
        }
    }
    if (source.View().empty())
    {
        Message_->setText(QStringLiteral("Select an imported source first."));
        return;
    }
    audio::content::Sound sound;
    audio::content::Music definition;
    (void)sound.Id.Set(View(id));
    (void)definition.Id.Set(View(id));
    (void)sound.Bus.Set("sfx");
    (void)sound.Group.Set("default");
    sound.Variations[0] = source;
    sound.VariationCount = 1;
    (void)definition.Bus.Set("music");
    definition.Source = source;
    ludus::content::Bytes bytes;
    auto status = music ? audio::content::WriteMusic(definition, bytes) : audio::content::WriteSound(sound, bytes);
    const auto path = QStringLiteral("definitions/") + QString::fromUtf8(id) + QStringLiteral(".json");
    const auto pathBytes = path.toUtf8(), rootBytes = Root_.toUtf8();
    ludus::content::Resource resource;
    (void)resource.Id.Set(View(id));
    (void)resource.Path.Set(View(pathBytes));
    resource.Type = music ? ludus::content::Kind::Music : ludus::content::Kind::Sound;
    ludus::content::Bytes oldCatalog;
    ludus::content::Catalog candidate;
    ludus::content::Diagnostic diagnostic;
    if (status != ludus::content::Status::Ok ||
        ludus::content::ReadFile(View(rootBytes), "catalog.json", ludus::content::MAX_DOCUMENT_BYTES, oldCatalog) !=
            ludus::content::Status::Ok ||
        candidate.Read(oldCatalog.String(), diagnostic) != ludus::content::Status::Ok ||
        candidate.Put(resource) != ludus::content::Status::Ok)
    {
        Message_->setText(QStringLiteral("Cannot create definition."));
        return;
    }
    ludus::content::Bytes catalogBytes;
    if (candidate.Write(catalogBytes) != ludus::content::Status::Ok ||
        QDir().mkpath(QFileInfo(Root_ + QLatin1Char('/') + path).path()) == false ||
        ludus::content::SaveFile(View(rootBytes), View(pathBytes), bytes.Data(), nullptr) != ludus::content::Status::Ok)
    {
        Message_->setText(QStringLiteral("Cannot save new definition."));
        return;
    }
    const auto digest = ludus::content::Hash(oldCatalog.Data());
    if (ludus::content::SaveFile(View(rootBytes), "catalog.json", catalogBytes.Data(), &digest) !=
        ludus::content::Status::Ok)
    {
        Message_->setText(QStringLiteral("Catalog changed; definition left unregistered."));
        return;
    }
    Catalog_ = ludus::foundation::Move(candidate);
    Sound_ = sound;
    Music_ = definition;
    IsMusic_ = music;
    HasDocument_ = true;
    Dirty_ = false;
    Path_ = path;
    Digest_ = ludus::content::Hash(bytes.Data());
#if defined(Q_OS_WASM)
    MarkBrowserEdited();
#endif
    Reload();
    Render();
}

} // namespace ludus::editor

void ludus::editor::AudioWorkspace::ResetPreview()
{
    (void)Preview_->Reset();
}

void ludus::editor::AudioWorkspace::PromptCreateDefinition(bool isMusic)
{
    Create(isMusic);
}
