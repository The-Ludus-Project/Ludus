#include "internal/project_creation_dialog.h"
#include "design_tokens.h"
#include "internal/theme_fonts.h"
#include "internal/workspace_style.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QVariant>

namespace ludus::editor
{
namespace
{
/// Longest project name the descriptor admits, in UTF-8 bytes (ludus_tools.descriptor MAX_NAME_BYTES).
constexpr qsizetype MAX_PROJECT_NAME_BYTES = 128;

// Return the nearest folder at or above the existing part of `path` that holds a
// `.git` entry, or an empty string. Mirrors ludus_tools.ci_setup.find_git_root.
QString FindGitWorkTree(const QString& path)
{
    QFileInfo current(QDir::cleanPath(path));
    while (current.exists() == false && current.absoluteFilePath() != current.absoluteDir().absolutePath())
    {
        current = QFileInfo(current.absolutePath());
    }
    QDir directory(current.absoluteFilePath());
    while (true)
    {
        if (QFileInfo::exists(directory.filePath(QStringLiteral(".git"))) == true)
        {
            return directory.absolutePath();
        }
        if (directory.cdUp() == false)
        {
            return QString();
        }
    }
}
} // namespace

ProjectCreationDialog::ProjectCreationDialog(QWidget* parent) : QDialog(parent)
{
    // Thanks to David Lightbown, Designing the User Experience of Game
    // Development Tools (CRC Press, 2015), ch. 4, pp. 53-62 and ch. 5,
    // pp. 80-86, 112-115: design around the user's task and disclose specialist choices
    // progressively. Our task is Name -> Location -> Create; engine selection
    // lives in the shared tooling backend. https://www.uxofgametools.com/
    // Review: docs/architecture/editor-design-review.md.
    setObjectName(QStringLiteral("newProjectDialog"));
    setWindowTitle(QStringLiteral("New Ludus Project"));
    setMinimumWidth(540);
    ApplyWorkspaceStyle(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(design::SPACING_LARGE,
                               design::SPACING_LARGE,
                               design::SPACING_LARGE,
                               design::SPACING_LARGE);
    layout->setSpacing(design::SPACING_MEDIUM);
    auto* heading = new QLabel(QStringLiteral("Create a project"), this);
    heading->setFont(MakeTypeFont(heading->font(), TypeRole::Title));
    layout->addWidget(heading);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    Name_ = new QLineEdit(QStringLiteral("New Game"), this);
    Name_->setObjectName(QStringLiteral("newProjectName"));
    Name_->setMaxLength(128);
    form->addRow(QStringLiteral("&Name"), Name_);
    auto* locationRow = new QWidget(this);
    auto* locationLayout = new QHBoxLayout(locationRow);
    locationLayout->setContentsMargins(0, 0, 0, 0);
#if defined(Q_OS_WASM)
    // The browser session filesystem has one projects folder; the download names the project.
    const QString defaultLocation = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                        .filePath(QStringLiteral("projects"));
#else
    auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documents.isEmpty())
    {
        documents = QDir::homePath();
    }
    const QString defaultLocation = QDir(documents).filePath(QStringLiteral("Ludus Projects"));
#endif
    Location_ = new QLineEdit(defaultLocation, locationRow);
    Location_->setObjectName(QStringLiteral("newProjectLocation"));
    auto* browse = new QPushButton(QStringLiteral("Browse..."), locationRow);
    browse->setObjectName(QStringLiteral("browseProjectLocation"));
    locationLayout->addWidget(Location_, 1);
    locationLayout->addWidget(browse);
    form->addRow(QStringLiteral("&Location"), locationRow);
    layout->addLayout(form);
    Destination_ = new QLabel(this);
    Destination_->setObjectName(QStringLiteral("newProjectDestination"));
    Destination_->setWordWrap(true);
    Destination_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(Destination_);
    Validation_ = new QLabel(this);
    Validation_->setObjectName(QStringLiteral("newProjectValidation"));
    Validation_->setWordWrap(true);
    layout->addWidget(Validation_);
    auto* note = new QLabel(
        QStringLiteral("Ludus selects the engine automatically and checks that your project builds and tests. "
                       "First-time setup may download tools and dependencies. Progress and cancellation are "
                       "available in the workspace."),
        this);
    note->setWordWrap(true);
    layout->addWidget(note);
    mGitHubCiOrNull = new QCheckBox(QStringLiteral("Add a GitHub Actions build and test workflow"), this);
    mGitHubCiOrNull->setObjectName(QStringLiteral("newProjectGitHubCi"));
    layout->addWidget(mGitHubCiOrNull);
    mGitHubCiNoteOrNull = new QLabel(this);
    mGitHubCiNoteOrNull->setObjectName(QStringLiteral("newProjectGitHubCiNote"));
    mGitHubCiNoteOrNull->setWordWrap(true);
    layout->addWidget(mGitHubCiNoteOrNull);
    auto* advanced = new QPushButton(QStringLiteral("Advanced engine selection"), this);
    advanced->setCheckable(true);
    advanced->setObjectName(QStringLiteral("newProjectAdvanced"));
    auto* advancedPanel = new QWidget(this);
    auto* advancedForm = new QFormLayout(advancedPanel);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    Sdk_ = new QLineEdit(advancedPanel);
    Sdk_->setObjectName(QStringLiteral("newProjectSdk"));
    Sdk_->setPlaceholderText(QStringLiteral("Automatic (editor engine or LUDUS_SDK_PREFIX)"));
    auto* sdkRow = new QWidget(advancedPanel);
    auto* sdkLayout = new QHBoxLayout(sdkRow);
    sdkLayout->setContentsMargins(0, 0, 0, 0);
    auto* sdkBrowse = new QPushButton(QStringLiteral("Browse..."), sdkRow);
    sdkLayout->addWidget(Sdk_, 1);
    sdkLayout->addWidget(sdkBrowse);
    advancedForm->addRow(QStringLiteral("Development SDK"), sdkRow);
    advancedPanel->hide();
    layout->addWidget(advanced);
    layout->addWidget(advancedPanel);
    connect(advanced, &QPushButton::toggled, advancedPanel, &QWidget::setVisible);
#if defined(Q_OS_WASM)
    // Engine selection, CI and the first build need the desktop tooling; the browser creates the files only.
    form->setRowVisible(locationRow, false);
    note->setText(QStringLiteral("Creates the minimal C++ project in this browser session. Use File > Download "
                                 "Project Folder to keep it, then open it in the desktop editor to select an engine, "
                                 "build and run it."));
    mGitHubCiOrNull->setChecked(false);
    mGitHubCiOrNull->setEnabled(false);
    mGitHubCiOrNull->hide();
    mGitHubCiNoteOrNull->hide();
    advanced->hide();
#endif
    const auto choose = [this](QLineEdit* field, const QString& title) {
        // Qt Group, QFileDialog documentation: asynchronous native directory
        // selection; https://doc.qt.io/qt-6/qfiledialog.html. Thanks for the
        // platform picker API; never use a nested exec() for this dialog.
        auto* picker = new QFileDialog(this, title, field->text());
        picker->setAttribute(Qt::WA_DeleteOnClose);
        picker->setFileMode(QFileDialog::Directory);
        picker->setOption(QFileDialog::ShowDirsOnly);
        connect(picker, &QFileDialog::fileSelected, field, &QLineEdit::setText);
        picker->open();
    };
    connect(browse, &QPushButton::clicked, this, [this, choose]() {
        choose(Location_, QStringLiteral("Choose the parent folder for your project"));
    });
    connect(sdkBrowse, &QPushButton::clicked, this, [this, choose]() {
        choose(Sdk_, QStringLiteral("Choose a Development SDK"));
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    Create_ = buttons->button(QDialogButtonBox::Ok);
    Create_->setText(QStringLiteral("Create Project"));
    Create_->setProperty("visualRole", QStringLiteral("primary"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        Update();
        if (Create_->isEnabled())
        {
            accept();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(Name_, &QLineEdit::textChanged, this, [this]() { Update(); });
    connect(Location_, &QLineEdit::textChanged, this, [this]() { Update(); });
    Name_->selectAll();
    Name_->setFocus();
    Update();
}

ProjectCreationOptions ProjectCreationDialog::Options() const
{
    return
    {
        .Destination = QDir(Location_->text().trimmed()).filePath(Name_->text().trimmed()),
        .Name = Name_->text().trimmed(),
        .Sdk = Sdk_->text().trimmed(),
        .ShouldAddGitHubCi = mGitHubCiOrNull->isEnabled() == true && mGitHubCiOrNull->isChecked() == true,
    };
}

void ProjectCreationDialog::Update()
{
    const auto options = Options();
    QString problem;
    bool invalidName =
        options.Name.isEmpty() || options.Name == QStringLiteral(".") || options.Name == QStringLiteral("..");
    for (const char32_t character : options.Name.toUcs4())
    {
        invalidName = invalidName || QChar::isPrint(character) == false || character == U'/' || character == U'\\';
    }
    if (invalidName)
    {
        problem = QStringLiteral("Enter a project name without path separators or control characters.");
    }
    else if (options.Name.toUtf8().size() > MAX_PROJECT_NAME_BYTES)
    {
        problem = QStringLiteral("Shorten the project name to at most %1 UTF-8 bytes; non-English characters "
                                 "use several bytes each.")
                      .arg(MAX_PROJECT_NAME_BYTES);
    }
    else if (QDir::isAbsolutePath(Location_->text().trimmed()) == false)
    {
        problem = QStringLiteral("Choose an absolute parent folder using Browse.");
    }
    else if (QFileInfo(Location_->text().trimmed()).isFile())
    {
        problem = QStringLiteral("The location must be a folder.");
    }
    else if (QFileInfo::exists(options.Destination))
    {
        problem = QStringLiteral("This project folder already exists. Choose a new name or location.");
    }
    // GitHub reads workflows only from a repository root, and a new project
    // folder can never be the root of an existing work tree.
    const QString workTree = QDir::isAbsolutePath(Location_->text().trimmed()) == true
                                 ? FindGitWorkTree(Location_->text().trimmed())
                                 : QString();
#if !defined(Q_OS_WASM)
    mGitHubCiOrNull->setEnabled(workTree.isEmpty());
#endif
    mGitHubCiNoteOrNull->setText(
        workTree.isEmpty() == true
            ? QStringLiteral("Builds and tests every push to main and every pull request once the project is "
                             "in its own GitHub repository and LUDUS_RELEASE_SDK_URL and "
                             "LUDUS_RELEASE_SDK_SHA256 are set.")
            : QStringLiteral("Unavailable: this location is inside the Git repository at %1. GitHub runs "
                             "workflows only from the repository root, so add CI there instead.")
                  .arg(QDir::toNativeSeparators(workTree)));
    Destination_->setText(QStringLiteral("New folder: %1").arg(QDir::toNativeSeparators(options.Destination)));
    Validation_->setText(problem);
    Validation_->setVisible(problem.isEmpty() == false);
    Create_->setEnabled(problem.isEmpty());
}
} // namespace ludus::editor
