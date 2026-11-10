#pragma once
#include "core/MovieTape.h"
#include "core/InputDisplay.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QImage>
#include <QString>
#include <functional>
#include <memory>
class LibretroHost;
// Worker-owned GGPO instance. The application supplies authenticated datagrams;
// GGPO owns input queues, retransmission, prediction and rollback scheduling.
class NetRollbackSession final {
public:
    using Send = std::function<void(quint64, const QByteArray&)>;
    enum Result { Run, Wait, Failed };
    NetRollbackSession();
    ~NetRollbackSession();
    bool start(LibretroHost&, quint64 epoch, const QJsonArray& roster,
               quint64 localId, Send send, QString& error);
    void stop();
    void receive(quint64 peer, const QByteArray& packet);
    bool idle(QString& error);
    Result beforeFrame(const MovieInput& local, MovieInput& merged, QString& error);
    bool localInputConsumed() const;
    bool afterFrame(QString& error);
    struct RewindFrame {
        QByteArray state; QImage image; double aspect=1;
        QImage secondImage; double secondAspect=1;
        InputDisplayState input;
    };
    void setHistoryEnabled(bool enabled);
    // Only final, locally earned frames reach the achievement runtime. Predicted
    // frames keep bounded memory snapshots, replaced when GGPO replays them.
    void setAchievementsEnabled(bool enabled);
    void processConfirmedAchievements(const std::function<void(const QByteArray&)>& consume);
    bool takeConfirmedHistory(RewindFrame& frame);
    // At most one snapshot per second, using GGPO's existing UDP quality replies.
    bool takeQuality(QJsonObject& quality,bool realtime);
    bool active() const;
    quint64 position() const;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
