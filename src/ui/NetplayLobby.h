#pragma once

#include <QJsonObject>
#include <QWidget>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

// A modeless view of an active direct-connection session. The networking layer
// owns all state; this widget only renders snapshots and emits user requests.
// setLobbyState() accepts:
//   active, host, status, addressStatus, addressRetry, invite,
//   inviteWithPassword, localInvite (includes password when configured),
//   connections: [{id, name, ipv4, physical}] for the create dialog;
//   allowCommands, allowDownloads;
//   participants: [{id, nickname, role, readiness, pingMs, local, host}];
//   rom: {filename, status, message, progress (0..100), canSelect,
//         canDownload, canCancel}.
// Roles are player1, player2, spectator. Only the host can assign roles, and
// any number of peers may have the same role. Peer IDs must be stable strings.
class NetplayLobby final : public QWidget {
    Q_OBJECT
public:
    explicit NetplayLobby(QWidget* parent = nullptr);

    void showCreateDialog();
    void showJoinDialog();
    void setLobbyState(const QJsonObject& state);

signals:
    void createRequested(quint16 port, const QString& advertisedAddress,
                         const QString& password, const QString& connectionId);
    void joinRequested(const QString& invite, const QString& password);
    void leaveRequested();
    void roleRequested(const QString& peerId, const QString& role);
    void selectRomRequested(const QString& path);
    void downloadRomRequested();
    void cancelTransferRequested();
    void kickRequested(const QString& peerId);
    void banRequested(const QString& peerId, bool permanent);
    void allowCommandsRequested(bool allowed);
    void allowDownloadsRequested(bool allowed);
    void retryAddressRequested();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QString selectedPeerId() const;
    void updateModerationButtons();
    void updateParticipants(const QJsonObject& state);

    QJsonObject state_;
    QLabel* status_ = nullptr;
    QLabel* addressStatus_ = nullptr;
    QLabel* participantsTitle_ = nullptr;
    QLabel* romName_ = nullptr;
    QLabel* romStatus_ = nullptr;
    QLineEdit* invite_ = nullptr;
    QComboBox* role_ = nullptr;
    QTableWidget* participants_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* copyInvite_ = nullptr;
    QPushButton* copyWithPassword_ = nullptr;
    QPushButton* copyLocalInvite_ = nullptr;
    QPushButton* retryAddress_ = nullptr;
    QPushButton* leave_ = nullptr;
    QPushButton* selectRom_ = nullptr;
    QPushButton* downloadRom_ = nullptr;
    QPushButton* cancelTransfer_ = nullptr;
    QPushButton* kick_ = nullptr;
    QPushButton* sessionBan_ = nullptr;
    QPushButton* permanentBan_ = nullptr;
    QCheckBox* allowCommands_ = nullptr;
    QCheckBox* allowDownloads_ = nullptr;
};
