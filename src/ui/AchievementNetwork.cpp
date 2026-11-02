#include "AchievementNetwork.h"

#include <QBuffer>
#include <QDir>
#include <QImageReader>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <climits>
#include <memory>

namespace {
constexpr qint64 kApiLimit = 8 * 1024 * 1024;
constexpr qint64 kBadgeLimit = 2 * 1024 * 1024;
constexpr int kTimeoutMs = 15000;
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
        return url.host() == QLatin1String("media.retroachievements.org") &&
               url.path().startsWith(QLatin1String("/Badge/"));
    return url.host() == QLatin1String("retroachievements.org");
}
QNetworkRequest makeRequest(const QUrl& url, bool badge) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "KegaRefusion/0.11.0 rcheevos/12.5.0");
    request.setTransferTimeout(kTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         badge ? QNetworkRequest::PreferCache : QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, badge);
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
} // namespace

AchievementNetwork::AchievementNetwork(const QString& dataPath, QObject* parent)
    : QObject(parent) {
    badgeCache_ = new QNetworkDiskCache(this);
    const QString directory = QDir(dataPath).filePath(QStringLiteral("achievements/cache"));
    QDir().mkpath(directory);
    badgeCache_->setCacheDirectory(directory);
    badgeCache_->setMaximumCacheSize(32 * 1024 * 1024);
    manager_.setCache(badgeCache_);
}

void AchievementNetwork::perform(const AchievementHttpRequest& request) {
    const QUrl url(request.url);
    if (!safeUrl(url, false) || request.postData.size() > 1024 * 1024 ||
        apiInFlight_ >= 8) {
        emit response(request.id, 0, {});
        return;
    }
    QNetworkRequest networkRequest = makeRequest(url, false);
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
    const QUrl url(urlText);
    if (!safeUrl(url, true) || badgeFailed_.contains(urlText)) return;
    if (const QImage* image = images_.object(urlText)) {
        emit badgeReady(urlText, *image);
        return;
    }
    if (badgePending_.contains(urlText) || badgeQueue_.size() >= 64) return;
    badgePending_.insert(urlText);
    badgeQueue_.enqueue(urlText);
    startBadges();
}

void AchievementNetwork::startBadges() {
    while (badgeInFlight_ < 2 && !badgeQueue_.isEmpty()) {
        const QString urlText = badgeQueue_.dequeue();
        QNetworkReply* reply = manager_.get(makeRequest(QUrl(urlText), true));
        ++badgeInFlight_;
        auto transfer = std::make_shared<Transfer>();
        connect(reply, &QIODevice::readyRead, this, [reply, transfer] {
            drain(reply, transfer, kBadgeLimit);
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, transfer, urlText] {
            drain(reply, transfer, kBadgeLimit);
            QImage image;
            if (!transfer->tooLarge && reply->error() == QNetworkReply::NoError &&
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
                QBuffer buffer(&transfer->body);
                buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer);
                const QSize size = reader.size();
                if (size.isValid() && size.width() <= 512 && size.height() <= 512)
                    image = reader.read();
                if (image.width() > 512 || image.height() > 512) image = {};
            }
            if (!image.isNull()) {
                images_.insert(urlText, new QImage(image),
                               int(qMin<qint64>(image.sizeInBytes(), INT_MAX)));
                emit badgeReady(urlText, image);
            } else {
                if (badgeFailed_.size() >= 128) badgeFailed_.clear();
                badgeFailed_.insert(urlText);
            }
            badgePending_.remove(urlText);
            --badgeInFlight_;
            reply->deleteLater();
            startBadges();
        });
    }
}
