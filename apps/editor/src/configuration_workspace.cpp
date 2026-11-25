#include "internal/configuration_workspace.h"
#include "internal/editor_files.h"
#include "internal/item_selection.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/config/json.hpp>
#include <ludus/runtime/configuration/host_options.hpp>
#include <ludus/runtime/configuration/schema_catalog.hpp>

#include <QAbstractItemView>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

#include <new>

// Thanks to Wessam Bahnassi, "Game Tuning Infrastructure", Game Engine Gems 2,
// ch.16, pp.263-277: inspectors share runtime metadata, constraints and feedback.
// This offline workspace shows next-launch effects; no live host RPC is implied.
namespace ludus::editor
{
using namespace foundation::config;
using foundation::uint8;
namespace
{
QString Text(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}
QString Display(const Value& value)
{
    if (value.Kind == Type::String || value.Kind == Type::Enum)
    {
        return Text(value.Text.GetView());
    }
    uint8 bytes[512]{};
    foundation::parsing::JsonWriter writer(bytes);
    EncodeValue(writer, value);
    std::span<const uint8> output;
    if (writer.Finish(output) != foundation::parsing::ParseStatus::Ok)
    {
        return {};
    }
    auto result = QString::fromUtf8(reinterpret_cast<const char*>(output.data()), static_cast<qsizetype>(output.size()))
                      .trimmed();
    if (value.Kind == Type::Int64 || value.Kind == Type::Uint64)
    {
        result = result.mid(1, result.size() - 2);
    }
    return result;
}
bool Read(const QString& path, QByteArray& output)
{
    QFile file(path);
    if (QFileInfo(path).isSymLink() || file.open(QIODevice::ReadOnly) == false ||
        file.size() > static_cast<qint64>(MAX_BUNDLE_BYTES))
    {
        return false;
    }
    output = file.read(static_cast<qint64>(MAX_BUNDLE_BYTES) + 1);
    return file.error() == QFileDevice::NoError && output.size() <= static_cast<qsizetype>(MAX_BUNDLE_BYTES);
}
// The catalog schema a bundle names, or null when it is malformed or names none.
const runtime::configuration::SchemaEntry* FindBundleSchemaOrNull(const QByteArray& bytes)
{
    foundation::parsing::JsonDocument document;
    foundation::parsing::ParseError error;
    std::string_view identity;
    if (document.Read({bytes.constData(), static_cast<foundation::usize>(bytes.size())}, error) !=
            foundation::parsing::ParseStatus::Ok ||
        document.Root().Get("schema").String(identity) == false)
    {
        return nullptr;
    }
    return runtime::configuration::FindSchemaOrNull(identity);
}
} // namespace
ConfigurationWorkspace::ConfigurationWorkspace(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("configurationWorkspace"));
    auto* layout = new QVBoxLayout(this);
    auto* description = new QLabel(
        QStringLiteral("Offline configuration preview — edits change saved files, never a running game. Select the "
                       "schema, load a cooked project bundle, edit preferences, then pass the files to the game."),
        this);
    description->setWordWrap(true);
    layout->addWidget(description);
    mSchemaSelectorOrNull = new QComboBox(this);
    mSchemaSelectorOrNull->setObjectName(QStringLiteral("configurationSchema"));
    mSchemaSelectorOrNull->setAccessibleName(QStringLiteral("Configuration schema"));
    for (const runtime::configuration::SchemaEntry& schema : runtime::configuration::GetSchemaCatalog())
    {
        mSchemaSelectorOrNull->addItem(QStringLiteral("%1 (%2)").arg(Text(schema.Title), Text(schema.Identity)),
                                       Text(schema.Identity));
    }
    layout->addWidget(mSchemaSelectorOrNull);
    connect(mSchemaSelectorOrNull, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QByteArray identity = mSchemaSelectorOrNull->itemData(index).toString().toUtf8();
        if (SelectSchema({identity.constData(), static_cast<foundation::usize>(identity.size())}) == false)
        {
            // Cancelled: keep the selector on the schema that still owns the draft.
            const QSignalBlocker blocker(mSchemaSelectorOrNull);
            mSchemaSelectorOrNull->setCurrentIndex(mSchemaSelectorOrNull->findData(Text(GetSchemaIdentity())));
        }
    });
    Table_ = new QTableWidget(0, 6, this);
    Table_->setObjectName(QStringLiteral("configurationSettings"));
    Table_->setHorizontalHeaderLabels({QStringLiteral("Setting"),
                                       QStringLiteral("Type"),
                                       QStringLiteral("Default"),
                                       QStringLiteral("Requested"),
                                       QStringLiteral("Source"),
                                       QStringLiteral("Apply")});
    Table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    ConfigureItemSelection(*Table_);
    Table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(Table_);
    Value_ = new QLineEdit(this);
    Value_->setObjectName(QStringLiteral("configurationValue"));
    Value_->setAccessibleName(QStringLiteral("Configuration value"));
    layout->addWidget(Value_);
    auto button = [&](const QString& label, const char* name, auto action) {
        auto* control = new QPushButton(label, this);
        control->setObjectName(QString::fromLatin1(name));
        layout->addWidget(control);
        connect(control, &QPushButton::clicked, this, action);
    };
    button(QStringLiteral("Apply preference"), "configurationApply", [this]() {
        if (Context_ == nullptr || Table_->currentRow() < 0)
        {
            return;
        }
        (void)Edit(Context_->Schema()[static_cast<foundation::usize>(Table_->currentRow())].Name, Value_->text());
    });
    button(QStringLiteral("Reset to inherited value"), "configurationReset", [this]() {
        if (Context_ == nullptr || Table_->currentRow() < 0)
        {
            return;
        }
        (void)Edit(Context_->Schema()[static_cast<foundation::usize>(Table_->currentRow())].Name, {}, true);
    });
    button(QStringLiteral("Load project bundle…"), "configurationLoadProject", [this]() {
        OpenEditorDocument(this,
                           {
                               .Title = QStringLiteral("Load cooked project configuration"),
                               .Directory = {},
                               .Filter = QStringLiteral("JSON (*.json)"),
                           },
                           [this](const QString& path) { (void)Load(path, Layer::Project); });
    });
    button(QStringLiteral("Load preferences…"), "configurationLoadPreferences", [this]() {
        OpenEditorDocument(this,
                           {
                               .Title = QStringLiteral("Load preferences"),
                               .Directory = {},
                               .Filter = QStringLiteral("JSON (*.json)"),
                           },
                           [this](const QString& path) {
                               if (ConfirmDiscard())
                               {
                                   (void)Load(path, Layer::Preference);
                               }
                           });
    });
    button(QStringLiteral("Save sparse preferences…"), "configurationSave", [this]() { Save(true); });
    Status_ = new QLabel(this);
    Status_->setObjectName(QStringLiteral("configurationStatus"));
    Status_->setWordWrap(true);
    layout->addWidget(Status_);
    connect(Table_, &QTableWidget::itemSelectionChanged, this, [this]() {
        if (Context_ == nullptr || Table_->currentRow() < 0)
        {
            return;
        }
        Binding binding;
        Explanation explanation;
        if (Context_->Bind(Context_->Schema()[static_cast<foundation::usize>(Table_->currentRow())].Name, binding) ==
                Status::Ok &&
            Context_->Explain(binding, explanation) == Status::Ok)
        {
            Value_->setText(Display(explanation.Requested));
        }
    });
    const runtime::configuration::SchemaEntry* hostOrNull =
        runtime::configuration::FindSchemaOrNull(runtime::configuration::HOST_SCHEMA);
    if (hostOrNull == nullptr || resetContext(*hostOrNull) == false)
    {
        setEnabled(false);
        return;
    }
    (void)Report(Status::Ok);
}

bool ConfigurationWorkspace::resetContext(const runtime::configuration::SchemaEntry& schema)
{
    Context* contextOrNull = new (std::nothrow) Context;
    Diagnostic error;
    if (contextOrNull == nullptr ||
        contextOrNull->Initialize(schema.Settings, foundation::GetSystemAllocationDomain(), error) != Status::Ok)
    {
        delete contextOrNull;
        (void)Report(Status::OutOfMemory);
        return false;
    }
    (void)contextOrNull->SealStartup();
    delete Context_;
    Context_ = contextOrNull;
    mSchemaOrNull = &schema;
    PreferencePath_.clear();
    PreferenceStamp_.clear();
    Dirty_ = false;
    {
        const QSignalBlocker blocker(mSchemaSelectorOrNull);
        mSchemaSelectorOrNull->setCurrentIndex(mSchemaSelectorOrNull->findData(Text(schema.Identity)));
    }
    Table_->clearSelection();
    Value_->clear();
    Render();
    return true;
}

bool ConfigurationWorkspace::SelectSchema(std::string_view identity)
{
    const runtime::configuration::SchemaEntry* schemaOrNull = runtime::configuration::FindSchemaOrNull(identity);
    if (schemaOrNull == nullptr)
    {
        return Report(Status::InvalidSchema);
    }
    if (schemaOrNull == mSchemaOrNull)
    {
        return true;
    }
    if (ConfirmDiscard() == false || resetContext(*schemaOrNull) == false)
    {
        return false;
    }
    return Report(Status::Ok);
}

std::string_view ConfigurationWorkspace::GetSchemaIdentity() const noexcept
{
    return mSchemaOrNull == nullptr ? std::string_view{} : mSchemaOrNull->Identity;
}

void ConfigurationWorkspace::Save(bool choosePath)
{
#if defined(Q_OS_WASM)
    (void)choosePath;
    const auto path = QStringLiteral("/browser-preferences.json");
    if (SavePreferences(path))
    {
        (void)DownloadEditorDocument(this, path);
    }
#else
    const QString path = choosePath == false && PreferencePath_.isEmpty() == false
                             ? PreferencePath_
                             : QFileDialog::getSaveFileName(this,
                                                            QStringLiteral("Save preferences"),
                                                            PreferencePath_,
                                                            QStringLiteral("JSON (*.json)"));
    if (path.isEmpty() == false)
    {
        (void)SavePreferences(path);
    }
#endif
}
ConfigurationWorkspace::~ConfigurationWorkspace()
{
    delete Context_;
}
bool ConfigurationWorkspace::Report(Status status, const Diagnostic& error)
{
    const bool success = status == Status::Ok || status == Status::PendingRestart;
    if (success == false && error.ByteOffset != UNKNOWN_BYTE_OFFSET)
    {
        Status_->setText(
            QStringLiteral("Configuration %1 at byte %2").arg(Text(StatusName(status))).arg(error.ByteOffset));
        return false;
    }
    Status_->setText(
        success ? QStringLiteral("Preview of %1 updated. Save preferences explicitly; the game applies them when it "
                                 "next reads the saved file.")
                      .arg(Text(GetSchemaIdentity()))
                : QStringLiteral("Configuration %1: %2 (record %3)")
                      .arg(Text(StatusName(status)), Text(error.Key.GetView()))
                      .arg(error.Record));
    return success;
}
void ConfigurationWorkspace::Render()
{
    if (Context_ == nullptr)
    {
        return;
    }
    Table_->setRowCount(static_cast<int>(Context_->Schema().size()));
    int row = 0;
    for (const auto& descriptor : Context_->Schema())
    {
        Binding binding;
        Explanation explanation;
        (void)Context_->Bind(descriptor.Name, binding);
        (void)Context_->Explain(binding, explanation);
        const QString columns[]{Text(descriptor.Name),
                                Text(TypeName(descriptor.Default.Kind)),
                                Display(descriptor.Default),
                                Display(explanation.Requested),
                                explanation.IsDefault ? QStringLiteral("default")
                                                      : Text(LayerName(explanation.Winner)) + QStringLiteral(": ") +
                                                            Text(explanation.From.Source.GetView()),
                                Text(ApplyName(descriptor.Application))};
        for (int column = 0; column < 6; ++column)
        {
            auto* item = new QTableWidgetItem(columns[column]);
            QString help = Text(descriptor.Help);
            if (column == 4)
            {
                help += QStringLiteral("\nDefault: ") + Display(descriptor.Default);
                for (foundation::usize rank = 0; rank < LAYER_COUNT; ++rank)
                {
                    Value assigned;
                    Origin origin;
                    const auto layer = static_cast<Layer>(rank);
                    if (Context_->ReadLayer(binding, layer, assigned, origin) == Status::Ok)
                    {
                        help += QStringLiteral("\n%1: %2 (%3:%4)")
                                    .arg(Text(LayerName(layer)), Display(assigned), Text(origin.Source.GetView()))
                                    .arg(origin.Line);
                    }
                }
            }
            item->setToolTip(help);
            Table_->setItem(row, column, item);
        }
        ++row;
    }
}
bool ConfigurationWorkspace::Edit(std::string_view name, const QString& text, bool reset)
{
    if (Context_ == nullptr)
    {
        return false;
    }
    Binding binding;
    if (Context_->Bind(name, binding) != Status::Ok)
    {
        return Report(Status::UnknownSetting);
    }
    const Type kind = Context_->Schema()[binding.Index].Default.Kind;
    Assignment edit;
    edit.Name = name;
    edit.Remove = reset;
    (void)edit.From.Source.TryAssign("Editor preferences");
    if (reset == false)
    {
        const auto utf8 = text.toUtf8();
        std::string_view input{utf8.constData(), static_cast<foundation::usize>(utf8.size())};
        uint8 bytes[1024]{};
        std::span<const uint8> encoded;
        foundation::parsing::JsonWriter writer(bytes);
        if (kind == Type::String || kind == Type::Enum || kind == Type::Int64 || kind == Type::Uint64)
        {
            writer.String(input);
        }
        else
        {
            writer.Raw(input);
        }
        if (writer.Finish(encoded) != foundation::parsing::ParseStatus::Ok)
        {
            return Report(Status::InvalidValue);
        }
        foundation::parsing::JsonDocument document;
        foundation::parsing::ParseError parseError;
        if (document.Read({reinterpret_cast<const char*>(encoded.data()), encoded.size()}, parseError) !=
                foundation::parsing::ParseStatus::Ok ||
            DecodeValue(document.Root(), kind, edit.Data) != Status::Ok)
        {
            return Report(Status::InvalidValue);
        }
    }
    Diagnostic error;
    auto status = Context_->Prepare(Layer::Preference, {&edit, 1}, false, Context_->Revision(), error);
    if (status == Status::Ok)
    {
        status = Context_->Commit(Context_->Revision(), error);
    }
    if (Report(status, error) == false)
    {
        return false;
    }
    Dirty_ = true;
#if defined(Q_OS_WASM)
    MarkBrowserEdited();
#endif
    Render();
    return true;
}
bool ConfigurationWorkspace::Load(const QString& path, Layer layer)
{
    if (Context_ == nullptr || (layer != Layer::Project && layer != Layer::Preference))
    {
        return Report(Status::InvalidSource);
    }
    QByteArray bytes;
    if (Read(path, bytes) == false)
    {
        return Report(Status::InvalidBundle);
    }
    // A file for another engine schema selects that schema. Loading preferences already replaces the draft
    // (its caller confirmed that); a project bundle asks before discarding unsaved preferences.
    const runtime::configuration::SchemaEntry* namedOrNull = FindBundleSchemaOrNull(bytes);
    const bool isSwitchCancelled = namedOrNull != nullptr && namedOrNull != mSchemaOrNull &&
                                   (layer == Layer::Preference ? resetContext(*namedOrNull) == false
                                                               : SelectSchema(namedOrNull->Identity) == false);
    if (isSwitchCancelled == true)
    {
        Status_->setText(QStringLiteral("'%1' uses the %2 schema. Save or discard the %3 preferences, then load it "
                                        "again.")
                             .arg(QFileInfo(path).fileName(), Text(namedOrNull->Identity), Text(GetSchemaIdentity())));
        return false;
    }
    Diagnostic error;
    auto status = PrepareJson(*Context_,
                              {bytes.constData(), static_cast<foundation::usize>(bytes.size())},
                              layer,
                              mSchemaOrNull->Identity,
                              Context_->Revision(),
                              foundation::GetSystemAllocationDomain(),
                              error);
    if (status == Status::Ok)
    {
        status = Context_->Commit(Context_->Revision(), error);
    }
    if (Report(status, error) == false)
    {
        return false;
    }
    if (layer == Layer::Preference)
    {
        PreferencePath_ = QFileInfo(path).absoluteFilePath();
        PreferenceStamp_ = foundation::Move(bytes);
        Dirty_ = false;
    }
    Render();
    return true;
}
bool ConfigurationWorkspace::SavePreferences(const QString& path)
{
    if (Context_ == nullptr || QFileInfo(path).isSymLink())
    {
        return Report(Status::InvalidState);
    }
    const auto absolute = QFileInfo(path).absoluteFilePath();
    if (absolute == PreferencePath_)
    {
        QByteArray current;
        if (Read(path, current) == false || current != PreferenceStamp_)
        {
            return Report(Status::Conflict);
        }
    }
    uint8 bytes[MAX_BUNDLE_BYTES]{};
    std::span<const uint8> output;
    const Status status = WriteLayer(*Context_, Layer::Preference, mSchemaOrNull->Identity, bytes, output);
    if (status != Status::Ok)
    {
        return Report(status);
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (file.open(QIODevice::WriteOnly) == false ||
        file.write(reinterpret_cast<const char*>(output.data()), static_cast<qint64>(output.size())) !=
            static_cast<qint64>(output.size()) ||
        file.commit() == false)
    {
        return Report(Status::InvalidState);
    }
    PreferencePath_ = absolute;
    PreferenceStamp_ = QByteArray(reinterpret_cast<const char*>(output.data()), static_cast<qsizetype>(output.size()));
    Dirty_ = false;
    return Report(Status::Ok);
}
bool ConfigurationWorkspace::ConfirmDiscard()
{
    return Dirty_ == false || QMessageBox::question(this,
                                                    QStringLiteral("Unsaved configuration preferences"),
                                                    QStringLiteral("Discard unsaved configuration preferences?"),
                                                    QMessageBox::Discard | QMessageBox::Cancel) == QMessageBox::Discard;
}
} // namespace ludus::editor
