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
#include <QVBoxLayout>

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
const QString APPROVED_DSN_KEY = QStringLiteral("crashReporting/approvedDsn");
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

QString SaveCrashReportingConsent(QSettings& inoutSettings,
                                  const QString& selectedDsn,
                                  bool hasConsent,
                                  bool shouldSaveDestination)
{
    // A running handler's destination is immutable. Never enable the old endpoint after editing the DSN.
    const QByteArray database = GetCrashDatabasePath().toUtf8();
    if (SetCrashUploadsEnabled(database.constData(), selectedDsn == activeDsn && hasConsent == true) == false)
    {
        return QStringLiteral("Cannot update crash-report permissions. Preferences were not saved.");
    }
    if (shouldSaveDestination == true)
    {
        inoutSettings.setValue(DSN_KEY, selectedDsn);
    }
    inoutSettings.setValue(CONSENT_KEY, hasConsent);
    inoutSettings.setValue(APPROVED_DSN_KEY, hasConsent == true ? selectedDsn : QString());
    inoutSettings.sync();
    if (inoutSettings.status() != QSettings::NoError)
    {
        SetCrashUploadsEnabled(database.constData(), false);
        return QStringLiteral("Cannot save preferences. Submission has been disabled for this session.");
    }
    return {};
}
} // namespace

// Thanks to Sentry Native 0.17.4, sentry_utils.c, sentry__dsn_get_minidump_url:
// https://github.com/getsentry/sentry-native/blob/0.17.4/src/sentry_utils.c . Adapt DSN routing;
// Relay's merge_formdata accepts the optional JSON 'sentry' field:
// https://github.com/getsentry/relay/blob/master/relay-server/src/processing/errors/errors/utils/formdata.rs .
// The external Crashpad handler owns HTTPS and offline retries.
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

bool HasEditorCrashUploadConsent(const QSettings& settings, const QString& dsn)
{
    return dsn.isEmpty() == false && settings.value(CONSENT_KEY, false).toBool() == true &&
           settings.value(APPROVED_DSN_KEY).toString() == dsn;
}

void InitializeEditorCrashReporting() noexcept
{
    const QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
    InitializeEditorCrashReporting(settings);
}

void InitializeEditorCrashReporting(const QSettings& settings) noexcept
{
    activeDsn = ReadDsn(settings);
    const QString endpoint = MakeSentryMinidumpUrl(activeDsn);
    const bool canUpload = endpoint.isEmpty() == false && HasEditorCrashUploadConsent(settings, activeDsn) == true;
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

bool ShowEditorCrashReporting(QWidget* parentOrNull)
{
    QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
    const QString savedDsn = ReadDsn(settings);
    bool shouldOfferRestart = false;
    QDialog dialog(parentOrNull);
    dialog.setObjectName(QStringLiteral("crashReportingDialog"));
    dialog.setWindowTitle(QStringLiteral("Crash Reporting"));
    dialog.setMinimumWidth(DIALOG_WIDTH_PIXELS);
    QFormLayout* form = new QFormLayout(&dialog);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    QLabel* explanation = new QLabel(
        QStringLiteral(
            "Crash reports are saved on this Mac. If you opt in, the crash handler sends pending and future reports "
            "through Sentry so the maintainers of the selected project can diagnose crashes, including fatal "
            "assertions. Uploads can resume after an offline session. Reports include thread stacks, some process "
            "memory, loaded modules, OS details and "
            "build identity. Memory may contain paths or research data. Reports can include child processes launched "
            "from the Editor. Project files, screenshots and session logs are not attached. You can turn off uploads "
            "here at any time."),
        &dialog);
    explanation->setWordWrap(true);
    form->addRow(explanation);
    QLineEdit* dsn = new QLineEdit(savedDsn, &dialog);
    dsn->setObjectName(QStringLiteral("crashReportingDsn"));
    dsn->setMaxLength(MAX_DSN_CHARACTERS);
    dsn->setPlaceholderText(QStringLiteral("https://public-key@host/project-id"));
    dsn->setCursorPosition(0);
    form->addRow(QStringLiteral("Sentry project DSN"), dsn);
    QCheckBox* consent = new QCheckBox(QStringLiteral("Automatically send crash reports"), &dialog);
    consent->setObjectName(QStringLiteral("crashReportingConsent"));
    consent->setChecked(HasEditorCrashUploadConsent(settings, savedDsn));
    form->addRow(consent);
    QObject::connect(dsn, &QLineEdit::textChanged, consent, [consent]() { consent->setChecked(false); });
    foundation::usize pending = 0;
    foundation::usize uploaded = 0;
    const QByteArray database = GetCrashDatabasePath().toUtf8();
    const bool hasCounts = ReadCrashReportCounts(database.constData(), pending, uploaded);
    QLabel* status = new QLabel(&dialog);
    status->setWordWrap(true);
    status->setObjectName(QStringLiteral("crashReportingStatus"));
    status->setText(captureProblem.isEmpty() == false ? captureProblem
                    : savedDsn != activeDsn
                        ? QStringLiteral("Restart the Editor to apply the saved destination. Uploads are paused.")
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
        const QString problem = SaveCrashReportingConsent(settings, selectedDsn, consent->isChecked(), true);
        if (problem.isEmpty() == false)
        {
            status->setText(problem);
            return;
        }
        shouldOfferRestart = selectedDsn != activeDsn;
        dialog.accept();
    });
    consent->setFocus();
    dialog.exec();
    if (shouldOfferRestart == false)
    {
        return false;
    }

    QMessageBox restart(QMessageBox::Information,
                        QStringLiteral("Restart Editor?"),
                        QStringLiteral("Restart the Editor to apply the new crash reporting destination. "
                                       "Uploads are paused until then."),
                        QMessageBox::NoButton,
                        parentOrNull);
    QPushButton* restartNow = restart.addButton(QStringLiteral("Restart Now"), QMessageBox::AcceptRole);
    QPushButton* later = restart.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
    restart.setDefaultButton(later);
    restart.setEscapeButton(later);
    restart.exec();
    return restart.clickedButton() == restartNow;
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
    if (MakeSentryMinidumpUrl(activeDsn).isEmpty() == true || settings.value(noticeKey, false).toBool() == true ||
        HasEditorCrashUploadConsent(settings, activeDsn) == true)
    {
        return;
    }

    QDialog dialog(parentOrNull);
    dialog.setObjectName(QStringLiteral("crashReportingNotice"));
    dialog.setWindowTitle(QStringLiteral("Send crash reports?"));
    dialog.setMinimumWidth(DIALOG_WIDTH_PIXELS);
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    const bool hasTeamDestination =
        crash_config::HAS_TEAM_SENTRY_DEFAULT == true && activeDsn == QString::fromUtf8(crash_config::DEFAULT_DSN);
    QLabel* explanation = new QLabel(
        hasTeamDestination == true
            ? QStringLiteral("Help the Ludus team diagnose crashes by sending reports through Sentry.")
            : QStringLiteral("Send crash reports to the configured Sentry project at %1 for its maintainers to review.")
                  .arg(QUrl(activeDsn).host()),
        &dialog);
    explanation->setTextFormat(Qt::PlainText);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    QLabel* details = new QLabel(
        QStringLiteral("Pending and future reports can include stack traces, system and build details, and parts of "
                       "process memory, which may contain paths or sensitive data. This includes fatal assertions "
                       "and crashes in child processes launched by the Editor. You can change your choice under "
                       "Help > Crash Reporting."),
        &dialog);
    details->setTextFormat(Qt::PlainText);
    details->setWordWrap(true);
    layout->addWidget(details);
    QLabel* status = new QLabel(&dialog);
    status->setWordWrap(true);
    status->hide();
    layout->addWidget(status);
    QDialogButtonBox* buttons = new QDialogButtonBox(&dialog);
    QPushButton* keepLocal =
        buttons->addButton(QStringLiteral("Keep Reports on This Mac"), QDialogButtonBox::RejectRole);
    QPushButton* send = buttons->addButton(QStringLiteral("Send Crash Reports"), QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);
    send->setAutoDefault(false);
    keepLocal->setDefault(true);
    keepLocal->setFocus();
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        const QString problem = SaveCrashReportingConsent(settings, activeDsn, true, false);
        if (problem.isEmpty() == false)
        {
            status->setText(problem);
            status->show();
            return;
        }
        dialog.accept();
    });
    dialog.exec();
    settings.setValue(noticeKey, true);
}
} // namespace ludus::editor
