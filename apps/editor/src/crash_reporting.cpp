#include "internal/crash_reporting.h"

#include "internal/crash_capture.h"
#include "ludus_editor_crash_config.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>

#include <QByteArray>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QUrlQuery>

namespace ludus::editor
{
namespace
{
constexpr foundation::usize EXECUTABLE_PATH_CAPACITY = 4096;
constexpr foundation::int32 DIALOG_WIDTH_PIXELS = 600;
constexpr foundation::int32 MAX_DSN_CHARACTERS = 2048;
const QString SETTINGS_ORGANIZATION = QStringLiteral("Ludus");
const QString SETTINGS_APPLICATION = QStringLiteral("Editor");
const QString DSN_KEY = QStringLiteral("crashReporting/dsn");
const QString CONSENT_KEY = QStringLiteral("crashReporting/automaticSubmission");
const QString SENTRY_PROTOCOL_VERSION = QStringLiteral("7");
QString captureProblem;
QString activeDsn;

QString GetCrashDatabasePath()
{
    const QString destination =
        activeDsn.isEmpty() == true
            ? QStringLiteral("unconfigured")
            : QString::fromLatin1(QCryptographicHash::hash(activeDsn.toUtf8(), QCryptographicHash::Sha256).toHex());
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/Ludus/Editor/CrashReports/") + destination;
}

QString ReadDsn(const QSettings& settings)
{
    return settings.value(DSN_KEY, QString::fromUtf8(crash_config::DEFAULT_DSN)).toString().trimmed();
}
} // namespace

// Thanks to Sentry, "example-minidump-upload", upload.py:
// https://github.com/getsentry/example-minidump-upload . Adapt only DSN routing and the
// JSON 'sentry' form field; the external Crashpad handler owns HTTPS and offline retries.
QString MakeSentryMinidumpUrl(const QString& dsn)
{
    const QUrl input(dsn, QUrl::StrictMode);
    const bool isSchemeAllowed = input.scheme() == QStringLiteral("https");
    static const QRegularExpression KEY_PATTERN(QStringLiteral("^[A-Za-z0-9]+$"));
    static const QRegularExpression PROJECT_PATTERN(QStringLiteral("^[0-9]+$"));
    const QString path = input.path();
    const foundation::int32 separator = static_cast<foundation::int32>(path.lastIndexOf(QLatin1Char('/')));
    const QString project = path.mid(separator + 1);
    if (dsn.size() > MAX_DSN_CHARACTERS || input.isValid() == false || isSchemeAllowed == false ||
        input.host().isEmpty() == true || input.password().isEmpty() == false || input.hasQuery() == true ||
        input.hasFragment() == true || KEY_PATTERN.match(input.userName()).hasMatch() == false ||
        PROJECT_PATTERN.match(project).hasMatch() == false || separator < 0)
    {
        return {};
    }
    QUrl endpoint(input);
    endpoint.setUserInfo({});
    endpoint.setPath(path.left(separator) + QStringLiteral("/api/") + project + QStringLiteral("/minidump/"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("sentry_key"), input.userName());
    query.addQueryItem(QStringLiteral("sentry_version"), SENTRY_PROTOCOL_VERSION);
    endpoint.setQuery(query);
    return endpoint.toString(QUrl::FullyEncoded);
}

QString GetEditorLogDirectory()
{
    return GetCrashDatabasePath() + QStringLiteral("/logs");
}

void InitializeEditorCrashReporting() noexcept
{
    QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
    activeDsn = ReadDsn(settings);
    const QString endpoint = MakeSentryMinidumpUrl(activeDsn);
    const bool canUpload = endpoint.isEmpty() == false && settings.value(CONSENT_KEY, false).toBool() == true;
    const QString databasePath = GetCrashDatabasePath();
    if (QDir().mkpath(databasePath) == false)
    {
        captureProblem = QStringLiteral("Cannot create the local crash-report directory.");
        return;
    }
    if (QFile::setPermissions(databasePath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner) == false)
    {
        captureProblem = QStringLiteral("Cannot restrict access to the local crash-report directory.");
        return;
    }
    foundation::StaticArray<char, EXECUTABLE_PATH_CAPACITY> executablePath{};
    if (ReadCrashExecutablePath(executablePath.GetData(), EXECUTABLE_PATH_CAPACITY) == false)
    {
        captureProblem = QStringLiteral("Cannot locate this editor's crash handler.");
        return;
    }
    const QString handler =
        QFileInfo(QString::fromUtf8(executablePath.GetData())).absolutePath() + QStringLiteral("/ludus_crash_handler");
    if (QFileInfo(handler).isExecutable() == false)
    {
        captureProblem = QStringLiteral("The bundled ludus_crash_handler is missing or not executable.");
        return;
    }
    const QJsonObject tags{
        {QStringLiteral("component"), QStringLiteral("editor")},
        {QStringLiteral("revision"), QString::fromUtf8(crash_config::REVISION)},
        {QStringLiteral("source_modified"),
         crash_config::IS_SOURCE_MODIFIED == true ? QStringLiteral("true") : QStringLiteral("false")},
        {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("build_flavor"), QString::fromUtf8(crash_config::BUILD_FLAVOR)},
    };
    const QJsonObject event{
        {QStringLiteral("release"), QString::fromUtf8(crash_config::RELEASE)},
        {QStringLiteral("environment"), QStringLiteral("lab")},
        {QStringLiteral("tags"), tags},
    };
    const QByteArray eventJson = QJsonDocument(event).toJson(QJsonDocument::Compact);
    if (StartCrashCapture(handler.toUtf8().constData(),
                          databasePath.toUtf8().constData(),
                          endpoint.toUtf8().constData(),
                          eventJson.constData(),
                          canUpload) == false)
    {
        captureProblem = QStringLiteral("The external crash handler could not start. The editor can still be used.");
    }
}

void ShowEditorCrashReporting(QWidget* parentOrNull)
{
    QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
    QDialog dialog(parentOrNull);
    dialog.setObjectName(QStringLiteral("crashReportingDialog"));
    dialog.setWindowTitle(QStringLiteral("Crash Reporting"));
    dialog.setMinimumWidth(DIALOG_WIDTH_PIXELS);
    QFormLayout* form = new QFormLayout(&dialog);
    QLabel* explanation = new QLabel(
        QStringLiteral(
            "Crash reports are saved on this Mac. If you enable submission, the separate crash handler sends "
            "pending and future reports to the Ludus team's Sentry project, including after an offline session. "
            "Capture can include child processes launched from this editor. Reports include thread stacks, "
            "some process memory, loaded modules, OS details and build identity. "
            "Memory may contain paths or research data. Project files, screenshots and session logs are not attached."),
        &dialog);
    explanation->setWordWrap(true);
    form->addRow(explanation);
    QLineEdit* dsn = new QLineEdit(ReadDsn(settings), &dialog);
    dsn->setObjectName(QStringLiteral("crashReportingDsn"));
    dsn->setMaxLength(MAX_DSN_CHARACTERS);
    dsn->setPlaceholderText(QStringLiteral("https://public-key@host/project-id"));
    form->addRow(QStringLiteral("Sentry project DSN"), dsn);
    QCheckBox* consent = new QCheckBox(QStringLiteral("Automatically send crash reports"), &dialog);
    consent->setObjectName(QStringLiteral("crashReportingConsent"));
    consent->setChecked(settings.value(CONSENT_KEY, false).toBool());
    form->addRow(consent);
    foundation::usize pending = 0;
    foundation::usize uploaded = 0;
    const QByteArray database = GetCrashDatabasePath().toUtf8();
    const bool hasCounts = ReadCrashReportCounts(database.constData(), pending, uploaded);
    QLabel* status = new QLabel(&dialog);
    status->setWordWrap(true);
    status->setObjectName(QStringLiteral("crashReportingStatus"));
    status->setText(captureProblem.isEmpty() == false ? captureProblem
                    : hasCounts == true ? QStringLiteral("Local capture active. Pending: %1; uploaded: %2.")
                                              .arg(static_cast<foundation::uint64>(pending))
                                              .arg(static_cast<foundation::uint64>(uploaded))
                                        : QStringLiteral("The local report database is unavailable."));
    form->addRow(status);
    QPushButton* openFolder = new QPushButton(QStringLiteral("Open local reports and logs"), &dialog);
    form->addRow(openFolder);
    QObject::connect(openFolder, &QPushButton::clicked, &dialog, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(GetCrashDatabasePath()));
    });
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        const QString selectedDsn = dsn->text().trimmed();
        if ((selectedDsn.isEmpty() == false && MakeSentryMinidumpUrl(selectedDsn).isEmpty() == true) ||
            (consent->isChecked() == true && selectedDsn.isEmpty() == true))
        {
            status->setText(
                QStringLiteral("Enter a valid HTTPS project DSN, or disable submission and clear the DSN."));
            return;
        }
        // A running handler's destination is immutable. Never enable the old endpoint after editing the DSN.
        const bool hasEndpointChanged = selectedDsn != activeDsn;
        const bool canUpload = hasEndpointChanged == false && consent->isChecked() == true;
        if (SetCrashUploadsEnabled(database.constData(), canUpload) == false)
        {
            status->setText(QStringLiteral("Cannot update crash-report permissions. Preferences were not saved."));
            return;
        }
        settings.setValue(DSN_KEY, selectedDsn);
        settings.setValue(CONSENT_KEY, consent->isChecked());
        settings.sync();
        if (settings.status() != QSettings::NoError)
        {
            SetCrashUploadsEnabled(database.constData(), false);
            status->setText(QStringLiteral("Cannot save preferences. Submission has been disabled for this session."));
            return;
        }
        if (hasEndpointChanged == true)
        {
            QMessageBox::information(
                &dialog,
                QStringLiteral("Restart required"),
                QStringLiteral(
                    "Restart the editor to use the new reporting destination. Submission is disabled until then."));
        }
        dialog.accept();
    });
    dialog.exec();
}

void PresentEditorCrashReportingNotice(QWidget* parentOrNull)
{
    QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
    if (captureProblem.isEmpty() == false)
    {
        QMessageBox::warning(parentOrNull, QStringLiteral("Crash capture unavailable"), captureProblem);
        return;
    }
    const QString noticeKey =
        QStringLiteral("crashReporting/notice/") +
        QString::fromLatin1(QCryptographicHash::hash(activeDsn.toUtf8(), QCryptographicHash::Sha256).toHex());
    if (MakeSentryMinidumpUrl(activeDsn).isEmpty() == false && settings.value(noticeKey, false).toBool() == false)
    {
        ShowEditorCrashReporting(parentOrNull);
        settings.setValue(noticeKey, true);
    }
}
} // namespace ludus::editor
