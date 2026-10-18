#include "internal/project_setup_dialog.h"

#include "internal/engine_pin.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace ludus::editor
{
namespace
{
QString SavedWebSdk(const QString& projectDirectory)
{
    // Display saved metadata only. The shared tooling backend validates it
    // before repair; opening this dialog never runs project code or tools.
    QFile file(QDir(projectDirectory).filePath(QStringLiteral(".ludus/setup.json")));
    if (file.open(QIODevice::ReadOnly) == false || file.size() > foundation::int64{64} * 1024)
    {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("web_sdk")).toString();
}

struct DirectoryFieldOptions
{
    QString Name;
    QString Hint;
};

QLineEdit* AddDirectoryField(QVBoxLayout* layout, QWidget* parent, const DirectoryFieldOptions& options)
{
    auto* row = new QWidget(parent);
    auto* horizontal = new QHBoxLayout(row);
    horizontal->setContentsMargins(0, 0, 0, 0);
    auto* edit = new QLineEdit(row);
    edit->setObjectName(options.Name);
    edit->setPlaceholderText(options.Hint);
    edit->setAccessibleName(options.Hint);
    auto* browse = new QPushButton(QStringLiteral("Browse…"), row);
    horizontal->addWidget(edit);
    horizontal->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, row, [row, edit]() {
        auto* picker = new QFileDialog(row, QStringLiteral("Choose an installed Ludus engine"), edit->text());
        picker->setAttribute(Qt::WA_DeleteOnClose);
        picker->setFileMode(QFileDialog::Directory);
        picker->setOption(QFileDialog::ShowDirsOnly);
        QObject::connect(picker, &QFileDialog::fileSelected, edit, &QLineEdit::setText);
        picker->open();
    });
    layout->addWidget(row);
    return edit;
}
} // namespace

ProjectSetupDialog::ProjectSetupDialog(const QString& projectDirectory,
                                       QWidget* parentOrNull,
                                       const ProjectSetupContext& context)
    : QDialog(parentOrNull), mExpectedFlavor(context.ExpectedFlavor)
{
    setObjectName(QStringLiteral("projectSetupDialog"));
    setWindowTitle(QStringLiteral("Repair Project Setup"));
    setMinimumWidth(540);
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(
        QStringLiteral("Your game needs an installed Ludus engine to build. Repair updates this computer's build "
                       "settings, builds the game, and runs its tests."),
        this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    layout->addWidget(new QLabel(QStringLiteral("Engine for desktop builds"), this));
    Engine_ = new QComboBox(this);
    Engine_->setObjectName(QStringLiteral("setupEngineChoice"));
    Engine_->setAccessibleName(QStringLiteral("Engine for desktop builds"));
    Engine_->addItem(QStringLiteral("Automatic (recommended)"));
    Engine_->addItem(QStringLiteral("Build and install the engine used by this editor"));
    Engine_->addItem(QStringLiteral("Choose a different installed engine…"));
    layout->addWidget(Engine_);
    EngineHelp_ = new QLabel(this);
    EngineHelp_->setObjectName(QStringLiteral("setupEngineHelp"));
    EngineHelp_->setWordWrap(true);
    layout->addWidget(EngineHelp_);
    NativeFields_ = new QWidget(this);
    auto* nativeLayout = new QVBoxLayout(NativeFields_);
    nativeLayout->setContentsMargins(0, 0, 0, 0);
    // Offer this editor's own installs first, so choosing an engine never needs a typed path.
    const QStringList detected = DiscoverEngineInstalls(context.ToolingRoot, context.ExpectedFlavor);
    mDetectedEnginesOrNull = new QComboBox(NativeFields_);
    mDetectedEnginesOrNull->setObjectName(QStringLiteral("setupDetectedEngines"));
    mDetectedEnginesOrNull->setAccessibleName(QStringLiteral("Engines installed by this editor"));
    mDetectedEnginesOrNull->addItem(QStringLiteral("Engines installed by this editor…"));
    for (const QString& install : detected)
    {
        mDetectedEnginesOrNull->addItem(QDir::toNativeSeparators(install), install);
    }
    mDetectedEnginesOrNull->setVisible(detected.isEmpty() == false);
    nativeLayout->addWidget(mDetectedEnginesOrNull);
    Sdk_ = AddDirectoryField(nativeLayout,
                             NativeFields_,
                             {
                                 .Name = QStringLiteral("setupSdk"),
                                 .Hint = QStringLiteral("Folder containing the installed desktop engine"),
                             });
    mSdkCheckOrNull = new QLabel(NativeFields_);
    mSdkCheckOrNull->setObjectName(QStringLiteral("setupSdkCheck"));
    mSdkCheckOrNull->setWordWrap(true);
    nativeLayout->addWidget(mSdkCheckOrNull);
    layout->addWidget(NativeFields_);
    connect(mDetectedEnginesOrNull, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString install = mDetectedEnginesOrNull->itemData(index).toString();
        if (install.isEmpty() == false)
        {
            Sdk_->setText(install);
        }
    });

    Browser_ = new QCheckBox(QStringLiteral("Also set up browser builds"), this);
    Browser_->setObjectName(QStringLiteral("setupBrowser"));
    layout->addWidget(Browser_);
    BrowserFields_ = new QWidget(this);
    auto* browserLayout = new QVBoxLayout(BrowserFields_);
    browserLayout->setContentsMargins(0, 0, 0, 0);
    auto* browserHelp = new QLabel(
        QStringLiteral("A game that runs in a web browser needs a separate Ludus engine built for the web. "
                       "Choose an installed browser engine below. Leave browser builds off for desktop-only repair."),
        BrowserFields_);
    browserHelp->setWordWrap(true);
    browserLayout->addWidget(browserHelp);
    WebSdk_ = AddDirectoryField(browserLayout,
                                BrowserFields_,
                                {
                                    .Name = QStringLiteral("setupWebSdk"),
                                    .Hint = QStringLiteral("Folder containing the installed browser engine"),
                                });
    const QString savedWeb = SavedWebSdk(projectDirectory);
    WebSdk_->setText(savedWeb);
    Browser_->setChecked(savedWeb.isEmpty() == false);
    layout->addWidget(BrowserFields_);

    Buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    Buttons_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Repair Project"));
    layout->addWidget(Buttons_);
    connect(Buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(Buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(Engine_, &QComboBox::currentIndexChanged, this, [this]() { Refresh(); });
    connect(Browser_, &QCheckBox::toggled, this, [this]() { Refresh(); });
    connect(Sdk_, &QLineEdit::textChanged, this, [this]() { Refresh(); });
    connect(WebSdk_, &QLineEdit::textChanged, this, [this]() { Refresh(); });
    Refresh();
}

ProjectSetupOptions ProjectSetupDialog::Options() const noexcept
{
    const auto choice = static_cast<EngineChoice>(Engine_->currentIndex());
    return
    {
        .Sdk = choice == EngineChoice::Installed ? Sdk_->text().trimmed() : QString(),
        .WebSdk = Browser_->isChecked() ? WebSdk_->text().trimmed() : QString(),
        .PrepareEngine = choice == EngineChoice::Build,
        .DisableWeb = Browser_->isChecked() == false,
    };
}

void ProjectSetupDialog::Refresh() noexcept
{
    const auto choice = static_cast<EngineChoice>(Engine_->currentIndex());
    NativeFields_->setVisible(choice == EngineChoice::Installed);
    BrowserFields_->setVisible(Browser_->isChecked());
    switch (choice)
    {
        case EngineChoice::Current:
            EngineHelp_->setText(
                QStringLiteral("Reuses the selected engine, or prepares this editor's engine for a new project. "
                               "An engine this editor installed is rebuilt when it is older than the editor. "
                               "No folders need to be entered. First-time preparation may download tools. "
                               "Incompatible selections need an explicit replacement."));
            break;
        case EngineChoice::Build:
            EngineHelp_->setText(QStringLiteral("Builds and installs a desktop engine from this editor's Ludus "
                                                "checkout. This can download tools and dependencies and take a few "
                                                "minutes. It must match the engine version your project requires."));
            break;
        case EngineChoice::Installed:
            EngineHelp_->setText(QStringLiteral("Choose a Ludus engine installation that matches your project's "
                                                "engine version and build profile. An SDK is an installed engine's "
                                                "headers and libraries; select its installation folder, not a "
                                                "build folder."));
            break;
    }
    // Validate a chosen folder before repair records it in .ludus/local.json.
    const QString sdk = Sdk_->text().trimmed();
    const EngineInstallCheck sdkCheck =
        sdk.isEmpty() ? EngineInstallCheck{} : CheckEngineInstallFolder(sdk, mExpectedFlavor);
    mSdkCheckOrNull->setText(sdkCheck.Message);
    mSdkCheckOrNull->setVisible(sdk.isEmpty() == false);
    const bool isNativeReady = choice != EngineChoice::Installed || sdkCheck.IsInstall == true;
    const bool isWebReady = Browser_->isChecked() == false || WebSdk_->text().trimmed().isEmpty() == false;
    Buttons_->button(QDialogButtonBox::Ok)->setEnabled(isNativeReady && isWebReady);
}
} // namespace ludus::editor
