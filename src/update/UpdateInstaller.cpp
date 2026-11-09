#include "UpdateInstaller.h"
#include "UpdateArchive.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QUuid>
#include <set>

namespace {
using namespace UpdateInternal;
fs::path nativePath(const QString& value) {
#ifdef Q_OS_WIN
    return fs::path(value.toStdWString());
#else
    return fs::u8path(value.toUtf8().constData());
#endif
}
QString qtPath(const fs::path& value) {
#ifdef Q_OS_WIN
    return QString::fromStdWString(value.wstring());
#else
    return QString::fromUtf8(value.u8string().c_str());
#endif
}
QString digestFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > qint64(MaxFileBytes)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}
bool readManifest(const fs::path& path, Platform platform, QJsonObject& entries, QString& error) {
    QFile file(qtPath(path));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
        error = QStringLiteral("Cannot read bounded package manifest: %1").arg(file.fileName()); return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject() || document.object().size() > int(MaxEntries)) {
        error = QStringLiteral("Invalid update package manifest."); return false;
    }
    entries = document.object();
    static const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));
    std::set<QString> seen;
    for (auto entry = entries.begin(); entry != entries.end(); ++entry) {
        const auto name = entry.key().toUtf8().toStdString();
        if (name == "kega-package.json" || !packagePath(name, platform) || !entry.value().isString() ||
            !sha256.match(entry.value().toString()).hasMatch() || !seen.insert(entry.key().toCaseFolded()).second) {
            error = QStringLiteral("Manifest contains an unsafe path or checksum."); return false;
        }
    }
    return true;
}
std::vector<std::string> restartArguments() {
    // Restart configuration is intentional; positional games and test switches
    // would reopen a running session without its transient state.
    std::vector<std::string> result;
    const auto arguments = QCoreApplication::arguments();
    for (int i = 1; i < arguments.size(); ++i) {
        QString option = arguments[i], value;
        for (const auto& key : {QStringLiteral("--data"), QStringLiteral("--core")}) {
            if (option == key && i + 1 < arguments.size()) value = arguments[++i];
            else if (option.startsWith(key + QLatin1Char('='))) value = option.mid(key.size() + 1);
            else continue;
            result.push_back(key.toStdString());
            result.push_back(QFileInfo(value).absoluteFilePath().toUtf8().toStdString());
            break;
        }
        if (option == QStringLiteral("--fullscreen")) result.push_back("--fullscreen");
    }
    return result;
}
}

bool UpdateInstaller::prepareAndLaunch(const QString& archivePath, QString& error) {
    using namespace UpdateInternal;
    error.clear();
#ifdef Q_OS_WIN
    constexpr auto platform = Platform::Windows;
    const QString helperName = QStringLiteral("KegaRefusionUpdater.exe");
#else
    constexpr auto platform = Platform::Linux;
    const QString helperName = QStringLiteral("KegaRefusionUpdater");
#endif
    Plan plan;
    bool handedOff = false;
    qint64 helperPid = 0;
    auto cleanup = [&] {
        if (!handedOff && !plan.work.empty()) {
            if (helperPid > 0) {
                // The detached Windows image can still be mapped on this error
                // path; the installed helper waits before removing its copy.
                QProcess::startDetached(
                    qtPath(plan.root / (plan.platform == Platform::Windows ? "KegaRefusionUpdater.exe" : "KegaRefusionUpdater")),
                    {QStringLiteral("--cleanup"), qtPath(plan.work), QString::number(helperPid)}, qtPath(plan.root));
            } else {
                std::error_code ignored; fs::remove_all(plan.work, ignored);
            }
        }
    };
    try {
        const QString rootName = QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath();
        if (rootName.isEmpty()) { error = QStringLiteral("Cannot resolve the application directory."); return false; }
        plan.root = nativePath(rootName);
        plan.platform = platform;
        plan.parentPid = std::uint64_t(QCoreApplication::applicationPid());
        plan.restartArguments = restartArguments();
        std::string detail;
        DirectoryLock lock;
        if (!lock.acquire(plan.root, detail)) { error = QString::fromUtf8(detail.c_str()); return false; }
        if (anotherInstance(plan.root, platform, plan.parentPid)) {
            error = QStringLiteral("Close the other Kega Refusion instance before installing an update."); return false;
        }
        const auto helper = plan.root / nativePath(helperName);
        if (!safeTreePath(plan.root, nativePath(helperName), detail) || !fs::is_regular_file(helper)) {
            error = QStringLiteral("The portable update helper is missing or unsafe. Install this release manually."); return false;
        }
        // Unfinished recovery data is never silently discarded by a later update.
        for (const auto& item : fs::directory_iterator(plan.root)) {
            if (item.path().filename().u8string().compare(0, 13, ".kega-update-") != 0 || !item.is_directory()) continue;
            const auto journal = item.path() / "journal.txt";
            if (!fs::is_regular_file(journal)) continue;
            QFile file(qtPath(journal));
            QByteArray journalState;
            if (file.open(QIODevice::ReadOnly) && file.size() <= 4 * 1024 * 1024) journalState = file.readAll();
            if (!journalState.endsWith("complete\n") && !journalState.endsWith("rolled-back\n")) {
                error = QStringLiteral("A previous update was interrupted. Recovery files are preserved in %1.").arg(qtPath(item.path()));
                return false;
            }
        }
        const QString identity = QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
        plan.work = plan.root / nativePath(QStringLiteral(".kega-update-") + identity);
        if (!fs::create_directory(plan.work)) { error = QStringLiteral("Cannot create private update staging directory."); return false; }
        fs::permissions(plan.work, fs::perms::owner_all, fs::perm_options::replace);
        fs::create_directory(plan.work / "package");
        if (!extractPackage(nativePath(archivePath), plan.work / "package", platform, plan.files, detail) ||
            !preflight(plan.root, plan.work / "package", plan.files, platform, detail)) {
            error = QString::fromUtf8(detail.c_str()); cleanup(); return false;
        }
        QJsonObject nextManifest;
        const auto newManifestPath = plan.work / "package/kega-package.json";
        if (!fs::is_regular_file(newManifestPath) ||
            !fs::is_regular_file(plan.work / "package" / nativePath(helperName))) {
            error = QStringLiteral("The update is missing its package manifest or portable helper.");
            cleanup(); return false;
        }
        if (platform == Platform::Linux) {
            for (const auto& executable : {"KegaRefusion", "run.sh", "KegaRefusionUpdater"}) {
                if ((fs::status(plan.work / "package" / executable).permissions() & fs::perms::owner_exec) == fs::perms::none) {
                    error = QStringLiteral("The Linux update contains a non-executable application, launcher or helper.");
                    cleanup(); return false;
                }
            }
        }
        {
            if (!readManifest(newManifestPath, platform, nextManifest, error) || nextManifest.size() + 1 != int(plan.files.size())) {
                if (error.isEmpty()) error = QStringLiteral("Package manifest does not describe every shipped file.");
                cleanup(); return false;
            }
            for (const auto& name : plan.files) {
                if (name == "kega-package.json") continue;
                const auto entry = nextManifest.find(QString::fromUtf8(name.c_str()));
                if (entry == nextManifest.end() || digestFile(qtPath(plan.work / "package" / fs::u8path(name))) != entry.value().toString()) {
                    error = QStringLiteral("Package checksum mismatch: %1").arg(QString::fromUtf8(name.c_str())); cleanup(); return false;
                }
            }
        }
        QJsonObject previous;
        const auto oldManifest = plan.root / "kega-package.json";
        if (fs::is_regular_file(oldManifest)) {
            if (!safeTreePath(plan.root, "kega-package.json", detail) || !readManifest(oldManifest, platform, previous, error)) {
                if (error.isEmpty()) error = QString::fromUtf8(detail.c_str());
                cleanup(); return false;
            }
            // A missing new manifest is supported for older genuine archives, but
            // cannot establish ownership for safe retirement.
            if (!nextManifest.isEmpty()) for (auto entry = previous.begin(); entry != previous.end(); ++entry) {
                const auto name = entry.key().toUtf8().toStdString();
                if (nextManifest.contains(entry.key()) ||
                    !safeTreePath(plan.root, fs::u8path(name), detail)) continue;
                const auto installed = plan.root / fs::u8path(name);
                if (digestFile(qtPath(installed)) != entry.value().toString()) continue;
                const auto snapshot = plan.work / "retired-reference" / fs::u8path(name);
                fs::create_directories(snapshot.parent_path());
                fs::copy_file(installed, snapshot);
                if (digestFile(qtPath(snapshot)) == entry.value().toString()) plan.retire.push_back(name);
                else fs::remove(snapshot);
            }
        }
        fs::create_directory(plan.work / "helper");
        const auto isolatedHelper = plan.work / "helper" / nativePath(helperName);
        fs::copy_file(helper, isolatedHelper);
        if (platform == Platform::Linux) fs::permissions(isolatedHelper, fs::perms::owner_all, fs::perm_options::replace);
        if (!writePlan(plan, detail)) { error = QString::fromUtf8(detail.c_str()); cleanup(); return false; }
        lock.release();
        QProcess helperProcess;
        helperProcess.setProgram(qtPath(isolatedHelper));
        helperProcess.setArguments({QStringLiteral("--apply"), qtPath(plan.work)});
        helperProcess.setWorkingDirectory(rootName);
        if (!helperProcess.startDetached(&helperPid)) {
            error = QStringLiteral("Cannot launch the portable update helper: %1").arg(helperProcess.errorString());
            cleanup(); return false;
        }
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 10000) {
            if (fs::is_regular_file(plan.work / "ready")) { handedOff = true; return true; }
            QFile failed(qtPath(plan.work / "prepare-error.txt"));
            if (failed.open(QIODevice::ReadOnly)) {
                error = QString::fromUtf8(failed.read(64 * 1024)); cleanup(); return false;
            }
            QThread::msleep(25);
        }
        // The helper checks this cancellation marker before any parent wait or
        // mutation. Leave cleanup to it if startup is merely delayed.
        QFile cancelled(qtPath(plan.work / "cancel"));
        if (cancelled.open(QIODevice::WriteOnly)) cancelled.write("cancel\n");
        error = QStringLiteral("The update helper did not become ready in time. The application remains open.");
        cleanup();
        return false;
    } catch (const std::exception& e) {
        error = QString::fromUtf8(e.what()); cleanup(); return false;
    }
}

QString UpdateInstaller::takeFailureReport() {
    const auto path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral(".kega-update-error.txt"));
    QFileInfo info(path);
    if (info.isSymLink() || !info.isFile() || info.size() > 64 * 1024) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto message = QString::fromUtf8(file.readAll());
    file.close(); file.remove();
    return message;
}
