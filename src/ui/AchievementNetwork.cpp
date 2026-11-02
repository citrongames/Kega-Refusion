#include "AchievementNetwork.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <climits>
#include <memory>

namespace {
constexpr qint64 kApiLimit = 8 * 1024 * 1024;
constexpr qint64 kBadgeLimit = 2 * 1024 * 1024;
constexpr int kTimeoutMs = 15000;
constexpr int kBadgeScanPerTurn = 2;
struct Transfer {
    QByteArray body;
    bool tooLarge = false;
};
bool safeUrl(const QUrl& url, bool badge) {
    if (!url.isValid() || url.scheme() != QLatin1String("https") ||
        !url.userInfo().isEmpty() || !url.fragment().isEmpty() ||
        (url.port(-1) != -1 && url.port(-1) != 443))
        return false;
    if (badge)
        return (url.host() == QLatin1String("media.retroachievements.org") &&
                (url.path().startsWith(QLatin1String("/Badge/")) ||
                 url.path().startsWith(QLatin1String("/Images/")))) ||
               (url.host() == QLatin1String("retroachievements.org") &&
                url.path().startsWith(QLatin1String("/Images/")));
    return url.host() == QLatin1String("retroachievements.org");
}
QNetworkRequest makeRequest(const QUrl& url) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "KegaRefusion/0.11.1 rcheevos/12.5.0");
    request.setTransferTimeout(kTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    return request;
}
void drain(QNetworkReply* reply, const std::shared_ptr<Transfer>& transfer, qint64 limit) {
    if (transfer->tooLarge) return;
    const qint64 remaining = limit - transfer->body.size();
    const QByteArray chunk = reply->read(remaining + 1);
    if (chunk.size() > remaining) {
        transfer->tooLarge = true;
        transfer->body.clear();
        reply->abort();
    } else {
        transfer->body += chunk;
    }
}
QImage decodeBadge(QIODevice* device) {
    QImageReader reader(device);
    reader.setDecideFormatFromContent(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > 512 || size.height() > 512)
        return {};
    QImage image = reader.read();
    return image.width() <= 512 && image.height() <= 512 ? image : QImage();
}
QImage loadCachedBadge(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > kBadgeLimit)
        return {};
    return decodeBadge(&file);
}
bool cachedBadgeFilePresent(const QString& path) {
    const QFileInfo info(path);
    return info.isFile() && info.size() > 0 && info.size() <= kBadgeLimit;
}
void saveBadge(const QString& path, const QByteArray& body) {
    if (body.isEmpty() || body.size() > kBadgeLimit ||
        !loadCachedBadge(path).isNull())
        return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    if (file.write(body) != body.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}
} // namespace

AchievementNetwork::AchievementNetwork(const QString& dataPath, QObject* parent)
    : QObject(parent),
      badgeDirectory_(QDir(dataPath).filePath(QStringLiteral("achievements/badges"))) {
    QDir().mkpath(badgeDirectory_);
    badgeIoPool_.setMaxThreadCount(1);
    badgeIoPool_.setExpiryTimeout(5000);
}

AchievementNetwork::~AchievementNetwork() {
    badgeIoPool_.waitForDone();
}

QString AchievementNetwork::badgePath(const QString& url) const {
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(badgeDirectory_).filePath(QString::fromLatin1(hash) + QStringLiteral(".img"));
}

void AchievementNetwork::perform(const AchievementHttpRequest& request) {
    const QUrl url(request.url);
    if (!safeUrl(url, false) || request.postData.size() > 1024 * 1024 ||
        apiInFlight_ >= 8) {
        emit response(request.id, 0, {});
        return;
    }
    QNetworkRequest networkRequest = makeRequest(url);
    QNetworkReply* reply = nullptr;
    if (request.postData.isEmpty()) {
        reply = manager_.get(networkRequest);
    } else {
        networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                                 request.contentType.isEmpty()
                                     ? QStringLiteral("application/x-www-form-urlencoded")
                                     : request.contentType);
        reply = manager_.post(networkRequest, request.postData);
    }
    ++apiInFlight_;
    auto transfer = std::make_shared<Transfer>();
    connect(reply, &QIODevice::readyRead, this, [reply, transfer] {
        drain(reply, transfer, kApiLimit);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, transfer, id = request.id] {
        drain(reply, transfer, kApiLimit);
        const int status = !transfer->tooLarge
            ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
        emit response(id, status, transfer->tooLarge ? QByteArray() : transfer->body);
        --apiInFlight_;
        reply->deleteLater();
    });
}

void AchievementNetwork::requestBadge(const QString& urlText) {
    if (urlText.size() > 2048 || !safeUrl(QUrl(urlText), true)) return;
    if (const QImage* image = images_.object(urlText)) {
        emit badgeReady(urlText, *image);
        return;
    }
    if (badgeFailed_.contains(urlText)) return;
    if (badgePending_.contains(urlText)) {
        badgeWanted_.insert(urlText);
        return;
    }
    if (badgeQueue_.size() >= 64) return;
    badgePending_.insert(urlText);
    badgeQueue_.enqueue(urlText);
    scheduleStartBadges();
}

void AchievementNetwork::prefetchBadges(const QStringList& urls) {
    prefetchUrls_ = urls;
    prefetchCursor_ = 0;
    ++prefetchGeneration_;
    prefetchSeen_.clear();
    badgeFailed_.clear();
    for (const QString& queued : badgeQueue_)
        badgePending_.remove(queued);
    badgeQueue_.clear();
    scheduleStartBadges();
}

void AchievementNetwork::scheduleStartBadges() {
    if (badgeStartScheduled_) return;
    badgeStartScheduled_ = true;
    QTimer::singleShot(1, this, [this] {
        badgeStartScheduled_ = false;
        startBadges();
    });
}

void AchievementNetwork::finishBadge(const QString& url, const QImage& image) {
    if (!image.isNull()) {
        images_.insert(url, new QImage(image),
                       int(qMin<qint64>(image.sizeInBytes(), INT_MAX)));
        emit badgeReady(url, image);
    } else {
        if (badgeFailed_.size() >= 128) badgeFailed_.clear();
        badgeFailed_.insert(url);
    }
    badgeWanted_.remove(url);
    badgePending_.remove(url);
    --badgeInFlight_;
    scheduleStartBadges();
}

void AchievementNetwork::startBadgeNetwork(const QString& url, quint64 generation) {
    QNetworkReply* reply = manager_.get(makeRequest(QUrl(url)));
    auto transfer = std::make_shared<Transfer>();
    connect(reply, &QIODevice::readyRead, this, [reply, transfer] {
        drain(reply, transfer, kBadgeLimit);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, transfer, url, generation] {
        drain(reply, transfer, kBadgeLimit);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool transportFailure = !transfer->tooLarge &&
            reply->error() != QNetworkReply::NoError && status == 0;
        if (generation == prefetchGeneration_ &&
            (transportFailure || status == 429 || status >= 500))
            prefetchCursor_ = prefetchUrls_.size();
        const bool validBody = !transfer->tooLarge &&
            reply->error() == QNetworkReply::NoError && status == 200;
        reply->deleteLater();
        if (!validBody) {
            finishBadge(url, {});
            return;
        }
        const QString path = badgePath(url);
        QByteArray body = std::move(transfer->body);
        badgeIoPool_.start([this, url, path, body = std::move(body)]() mutable {
            QBuffer buffer(&body);
            buffer.open(QIODevice::ReadOnly);
            QImage image = decodeBadge(&buffer);
            if (!image.isNull()) saveBadge(path, body);
            QMetaObject::invokeMethod(this, [this, url, image = std::move(image)] {
                finishBadge(url, image);
            }, Qt::QueuedConnection);
        });
    });
}

void AchievementNetwork::startBadges() {
    int scanned = 0;
    while (badgeInFlight_ < 2 && scanned < kBadgeScanPerTurn &&
           (!badgeQueue_.isEmpty() || prefetchCursor_ < prefetchUrls_.size())) {
        const bool urgent = !badgeQueue_.isEmpty();
        const QString urlText = urgent ? badgeQueue_.dequeue()
                                      : prefetchUrls_.at(prefetchCursor_++);
        ++scanned;
        if (urlText.size() > 2048 || !safeUrl(QUrl(urlText), true)) {
            if (urgent) badgePending_.remove(urlText);
            continue;
        }
        if (!urgent) {
            if (prefetchSeen_.contains(urlText)) continue;
            prefetchSeen_.insert(urlText);
            if (badgePending_.contains(urlText) || badgeFailed_.contains(urlText))
                continue;
        }
        if (const QImage* inMemory = images_.object(urlText)) {
            if (urgent) {
                const QImage image = *inMemory;
                badgePending_.remove(urlText);
                emit badgeReady(urlText, image);
            }
            continue;
        }
        if (!urgent) badgePending_.insert(urlText);
        ++badgeInFlight_;
        const QString path = badgePath(urlText);
        const quint64 generation = prefetchGeneration_;
        badgeIoPool_.start([this, urlText, path, urgent, generation] {
            const bool present = !urgent && cachedBadgeFilePresent(path);
            QImage cached = urgent ? loadCachedBadge(path) : QImage();
            QMetaObject::invokeMethod(this, [this, urlText, urgent, generation,
                                             present, cached = std::move(cached)] {
                if (!present && cached.isNull()) {
                    startBadgeNetwork(urlText, generation);
                    return;
                }
                if (urgent) {
                    images_.insert(urlText, new QImage(cached),
                                   int(qMin<qint64>(cached.sizeInBytes(), INT_MAX)));
                    emit badgeReady(urlText, cached);
                }
                const bool wanted = badgeWanted_.remove(urlText);
                badgePending_.remove(urlText);
                --badgeInFlight_;
                if (present && wanted) requestBadge(urlText);
                scheduleStartBadges();
            }, Qt::QueuedConnection);
        });
    }
    if (badgeInFlight_ < 2 &&
        (!badgeQueue_.isEmpty() || prefetchCursor_ < prefetchUrls_.size()))
        scheduleStartBadges();
}
