#include "update/UpdateService.h"
#include "update/UpdateInstaller.h"
#include "i18n/Localization.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVersionNumber>
#include <memory>

namespace {
constexpr qint64 maxMetadata = 1024 * 1024;
constexpr qint64 maxDownload = 512LL * 1024 * 1024;
const QString repository = QStringLiteral("citrongames/Kega-Refusion");
#ifdef Q_OS_WIN
const QString suffix = QStringLiteral("windows-x86_64.zip");
#else
const QString suffix = QStringLiteral("linux-x86_64.tar.gz");
#endif

struct Result {
    QString version, notes, error, detail;
    QUrl url;
    QByteArray digest;
    qint64 size = 0;
};

QString versionFromTag(const QString& tag) {
    static const QRegularExpression pattern(QStringLiteral("^v([0-9]{1,8}\\.[0-9]{1,8}\\.[0-9]{1,8})$"));
    return pattern.match(tag).captured(1);
}

void parseRelease(const QByteArray& json, const QString& expectedTag, Result& result) {
    const auto release = QJsonDocument::fromJson(json).object();
    const QString tag = release.value("tag_name").toString();
    if (tag != expectedTag || release.value("draft").toBool(true) ||
        release.value("prerelease").toBool(true) || !release.value("assets").isArray()) {
        result.error = QStringLiteral("Некорректное описание релиза на GitHub.");
        return;
    }
    const QString version = versionFromTag(tag);
    result.notes = release.value("body").toString().trimmed();

    const QString name = QStringLiteral("KegaRefusion-") + version + '-' + suffix;
    const QUrl expected(QStringLiteral("https://github.com/") + repository + "/releases/download/" + tag + '/' + name);
    static const QRegularExpression sha(QStringLiteral("^sha256:([0-9a-fA-F]{64})$"));
    for (const auto& item : release.value("assets").toArray()) {
        const auto asset = item.toObject();
        if (asset.value("name").toString() != name) continue;
        const auto digest = sha.match(asset.value("digest").toString());
        const auto size = asset.value("size").toInteger();
        if (asset.value("state").toString() != QStringLiteral("uploaded") ||
            QUrl(asset.value("browser_download_url").toString()) != expected ||
            !digest.hasMatch() || size <= 0 || size > maxDownload) break;
        result.version = version;
        result.url = expected;
        result.digest = QByteArray::fromHex(digest.captured(1).toLatin1());
        result.size = size;
        return;
    }
    result.error = QStringLiteral("В релизе нет проверенного архива для этой платформы.");
}

// Runs entirely on a worker thread, including all file writes and hashing.
void transfer(UpdateService* service, const QUrl& url, qint64 limit, QFile* file,
              QCryptographicHash* hash, QByteArray* metadata, Result& result, QUrl* tagRedirect = nullptr) {
    QNetworkAccessManager network;
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "KegaRefusion-Update/" + QCoreApplication::applicationVersion().toLatin1());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, tagRedirect
                         ? QNetworkRequest::ManualRedirectPolicy : QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(file ? 30000 : 8000);
    QEventLoop loop;
    QNetworkReply* reply = tagRedirect ? network.head(request) : network.get(request);
    reply->setReadBufferSize(128 * 1024);
    qint64 count = 0;
    auto consume = [&] {
        while (reply->bytesAvailable() && result.error.isEmpty()) {
            const QByteArray bytes = reply->read(64 * 1024);
            if (bytes.isEmpty()) break;
            count += bytes.size();
            if (count > limit) {
                result.error = QStringLiteral("Размер ответа GitHub превышает ожидаемый.");
            } else if (file) {
                if (file->write(bytes) != bytes.size())
                    result.error = QStringLiteral("Не удалось записать архив обновления на диск.");
                else
                    hash->addData(bytes);
            } else if (metadata) {
                metadata->append(bytes);
            }
        }
        if (!result.error.isEmpty() && !reply->isFinished()) reply->abort();
    };
    QObject::connect(reply, &QIODevice::readyRead, &loop, consume);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(service, &UpdateService::stopping, reply, &QNetworkReply::abort);
    if (file) {
        QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
                         [service, limit](qint64 received, qint64) { emit service->progress(received, limit); });
    }
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    deadline.start(file ? 15 * 60 * 1000 : 12000);
    if (QThread::currentThread()->isInterruptionRequested()) reply->abort();
    if (!reply->isFinished()) loop.exec();
    consume();
    if (!result.error.isEmpty()) return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (tagRedirect && reply->error() == QNetworkReply::NoError && status >= 300 && status < 400) {
        *tagRedirect = url.resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
        return;
    }
    if (reply->error() != QNetworkReply::NoError || status != 200 || tagRedirect) {
        result.error = QStringLiteral("Не удалось связаться с GitHub. Проверьте подключение и повторите попытку.");
        result.detail = reply->errorString();
    } else if (file && count != limit) {
        result.error = QStringLiteral("Архив обновления загружен не полностью.");
    }
}
}

UpdateService::UpdateService(QObject* parent) : QObject(parent) {}

UpdateService::~UpdateService() {
    if (job_) {
        job_->requestInterruption();
        emit stopping();
        job_->wait();
    }
}

void UpdateService::check() { if (!job_) run(false); }
void UpdateService::install() { if (!job_ && assetUrl_.isValid()) run(true); }

void UpdateService::run(bool download) {
    if (!download) assetUrl_.clear();
    const QString current = QCoreApplication::applicationVersion();
    const QUrl url = assetUrl_;
    const QByteArray expectedDigest = digest_;
    const qint64 size = assetSize_;
    auto result = std::make_shared<Result>();
    job_ = QThread::create([this, download, current, url, expectedDigest, size, result] {
        if (!download) {
            const QString releases = QStringLiteral("https://github.com/") + repository + "/releases/";
            QUrl tagUrl;
            transfer(this, QUrl(releases + "latest"), maxMetadata, nullptr, nullptr, nullptr, *result, &tagUrl);
            if (!result->error.isEmpty()) return;
            const QString tag = tagUrl.fileName();
            const QString version = versionFromTag(tag);
            if (version.isEmpty() || tagUrl != QUrl(releases + "tag/" + tag)) {
                result->error = QStringLiteral("Некорректное описание релиза на GitHub.");
                return;
            }
            if (QVersionNumber::fromString(version) <= QVersionNumber::fromString(current)) return;
            QByteArray metadata;
            transfer(this, QUrl(releases + "download/" + tag + "/update.json"), maxMetadata,
                     nullptr, nullptr, &metadata, *result);
            if (result->error.isEmpty()) parseRelease(metadata, tag, *result);
            return;
        }
        QTemporaryDir temp;
        QFile file(temp.filePath(suffix));
        if (!temp.isValid() || !file.open(QIODevice::WriteOnly)) {
            result->error = QStringLiteral("Не удалось записать архив обновления на диск.");
            return;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        transfer(this, url, size, &file, &hash, nullptr, *result);
        if (!result->error.isEmpty()) return;
        if (!file.flush()) {
            result->error = QStringLiteral("Не удалось записать архив обновления на диск.");
            return;
        }
        file.close();
        if (hash.result() != expectedDigest) {
            result->error = QStringLiteral("Контрольная сумма обновления не совпадает. Загрузите архив ещё раз.");
            return;
        }
        if (QThread::currentThread()->isInterruptionRequested()) return;
        if (!UpdateInstaller::prepareAndLaunch(file.fileName(), result->detail))
            result->error = QStringLiteral("Не удалось установить обновление.");
    });
    job_->setParent(this);
    connect(job_, &QThread::finished, this, [this, result, download] {
        job_->deleteLater();
        job_ = nullptr;
        if (!result->error.isEmpty()) {
            QString message = Localization::text(result->error);
            if (!result->detail.isEmpty()) message += '\n' + result->detail;
            emit failed(message);
        } else if (download) {
            emit restarting();
        } else {
            assetUrl_ = result->url;
            digest_ = result->digest;
            assetSize_ = result->size;
            emit checked(result->version, result->notes);
        }
    });
    job_->start();
}
