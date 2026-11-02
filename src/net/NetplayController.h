#pragma once
#include "NetTransport.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QThread>
#include <memory>
class EmulationSession;
class QSettings;
class QWidget;
class NetplayLobby;
class NetTransferWorker;
class NetplayController final : public QObject {
    friend class NetplayScenario;
    Q_OBJECT
public:
    NetplayController(EmulationSession&, const QString& dataPath, QSettings&, QWidget* parentWindow);
    ~NetplayController() override;
    void createSession();
    void joinSession();
    void showLobby();
    void leaveSession();
    bool active() const { return active_; }
    QString nickname() const;
    QJsonArray bannedPeers() const;
    void setNickname(const QString&);
    void unban(const QString&);
signals:
    void activeChanged(bool);
    void openRomRequested(const QString&);
    void notice(const QString&);
private:
    void startHost(quint16, const QString&, const QString&);
    void startJoin(const QString&, const QString&);
    void refreshLobby();
    void receive(quint64,kega::net::MessageKind,const QByteArray&);
    void sendState(quint64 peer);
    void sendInput();
    void acceptCommand(quint64,const QByteArray&);
    void onGameLoaded(const QString&);
    void queueSnapshot(quint64,const QByteArray&);
    void beginIncomingSnapshot(qsizetype size,quint64 epoch);
    void updateWaiting();
    void announceContent();
    void compareContent();
    void prepareSource(const QString&,int archiveIndex=-1);
    EmulationSession& session_;
    QString dataPath_;
    QSettings& settings_;
    NetplayLobby* lobby_ = nullptr;
    kega::net::NetTransport transport_;
    QThread fileThread_;
    NetTransferWorker* files_ = nullptr;
    QTimer inputTimer_, reconnectTimer_, snapshotTimer_;
    bool active_ = false, hosting_ = false, stopping_ = false;
    bool allowCommands_ = true, allowDownloads_ = false;
    bool gameReady_ = false, remoteGameOpen_ = false;
    QString invite_, inviteWithPassword_, gamePath_, gameName_, status_;
    QString joinInvite_, joinPassword_;
    QByteArray identity_, manifest_, localManifest_;
    QHash<quint64,QList<QByteArray>> heldFrames_;
    QHash<quint64,qint64> disconnectedAt_;
    QHash<quint64,qint64> commandTimes_;
    bool snapshotRequested_=false;
    QString storageId_, downloadedPath_;
    int sourceArchiveIndex_=-1;
    quint64 hostEpoch_ = 0;
    quint64 localId_ = 0;
    int localRole_ = 0, reconnectAttempts_ = 0;
    QHash<quint64,QJsonObject> participants_;
    QHash<quint64,QByteArray> inputs_;
    QSet<quint64> readyPeers_, filePending_;
    QHash<quint64,QByteArray> outgoingSnapshots_;
    QHash<quint64,qsizetype> outgoingOffsets_;
    QByteArray incomingSnapshot_;
    qsizetype incomingSize_ = 0, pendingIncomingSize_=0;
    quint64 incomingEpoch_=0, applyingEpoch_=0, pendingIncomingEpoch_=0;
    bool fileChunkExpected_=false;
    qint64 transferDone_ = 0, transferTotal_ = 0;
    bool downloading_ = false;
};
