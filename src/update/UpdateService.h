#pragma once

#include <QObject>
#include <QUrl>

class QThread;

class UpdateService final : public QObject {
    Q_OBJECT
public:
    explicit UpdateService(QObject* parent = nullptr);
    ~UpdateService() override;
    void check();
    void install();

signals:
    void checked(QString version, QString notes);
    void failed(QString message);
    void progress(qint64 received, qint64 total);
    void restarting();
    void stopping();

private:
    void run(bool download);
    QThread* job_ = nullptr;
    QUrl assetUrl_;
    QByteArray digest_;
    qint64 assetSize_ = 0;
};
