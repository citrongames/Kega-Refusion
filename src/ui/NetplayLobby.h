#pragma once

#include <QJsonObject>
#include <QHash>
#include <QPair>
#include <QWidget>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QTableWidget;

// A modeless view of an active direct-connection session. The networking layer
// owns all state; this widget only renders snapshots and emits user requests.
// setLobbyState() accepts:
//   active, host, status, connectionWaiting, addressStatus, addressRetry, invite,
//   inviteWithPassword, lanInvite (LAN links include password when configured),
//   connections: [{id, name, ipv4, physical}] for the create dialog;
//   allowCommands, allowDownloads, linkCable, playerCount, roleKinds;
//   participants: [{id, nickname, role, readiness, pingMs, local, host}];
//   rom: {filename, status, message, progress (0..100), canSelect,
//         canDownload, canCancel}.
// Roles are player1 through player8 and spectator, limited by playerCount.
// Link Cable / Gear-to-Gear limits roles to player1 (console A), player2 (console B)
// and spectator. Only the host can assign roles, and any number of peers may
// have the same role. Peer IDs must be stable strings.
class NetplayLobby final : public QWidget {
    Q_OBJECT
public:
    explicit NetplayLobby(QWidget* parent = nullptr);

    void showCreateDialog();
    void showJoinDialog();
    void setLobbyState(const QJsonObject& state);
    // Local measurements from the emulation worker; never part of lobby broadcasts.
    void setNetworkQuality(const QJsonObject& quality);

signals:
    void createRequested(quint16 port, const QString& advertisedAddress,
                         const QString& password, const QString& connectionId);
    void joinRequested(const QString& invite, const QString& password);
    void leaveRequested();
    void stopWaitingRequested();
    void roleRequested(const QString& peerId, const QString& role);
    void selectRomRequested(const QString& path);
    void downloadRomRequested();
    void cancelTransferRequested();
    void kickRequested(const QString& peerId);
    void banRequested(const QString& peerId, bool permanent);
    void allowCommandsRequested(bool allowed);
    void allowDownloadsRequested(bool allowed);
    void linkCableRequested(bool enabled);
    void retryAddressRequested();

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QString selectedPeerId() const;
    void updateModerationButtons();
    void updateParticipants(const QJsonObject& state);
    void updateQualitySummary();
    void updateParticipantPings();

    QJsonObject state_, networkQuality_;
    QHash<QString,QPair<int,int>> qualityPings_;
    QLabel* status_ = nullptr;
    QLabel* addressStatus_ = nullptr;
    QLabel* participantsTitle_ = nullptr;
    QLabel* qualitySummary_ = nullptr;
    QLabel* romName_ = nullptr;
    QLabel* romStatus_ = nullptr;
    QLineEdit* invite_ = nullptr;
    QTableWidget* participants_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* copyInvite_ = nullptr;
    QPushButton* copyWithPassword_ = nullptr;
    QPushButton* copyLanInvite_ = nullptr;
    QPushButton* retryAddress_ = nullptr;
    QPushButton* leave_ = nullptr;
    QPushButton* stopWaiting_ = nullptr;
    QPushButton* selectRom_ = nullptr;
    QPushButton* downloadRom_ = nullptr;
    QPushButton* cancelTransfer_ = nullptr;
    QPushButton* kick_ = nullptr;
    QPushButton* sessionBan_ = nullptr;
    QPushButton* permanentBan_ = nullptr;
    QCheckBox* allowCommands_ = nullptr;
    QCheckBox* allowDownloads_ = nullptr;
    QCheckBox* linkCable_ = nullptr;
};
