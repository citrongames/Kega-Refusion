#pragma once

#include <QWidget>
#include <QJsonArray>

class QLineEdit;
class QListWidget;
class QPushButton;

// Embeddable page for the emulator's settings dialog. The owner persists the
// nickname and ban list; this widget never writes settings by itself.
class NetplaySettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit NetplaySettingsPage(QWidget* parent = nullptr);
    QString nickname() const;
    void setNickname(const QString& nickname);
    // Objects use {fingerprint, nickname, persistent}; session-only bans are hidden.
    void setBannedPeers(const QJsonArray& peers);

signals:
    void nicknameChanged(const QString& nickname);
    void unbanRequested(const QString& peerId);

private:
    QLineEdit* nickname_ = nullptr;
    QListWidget* bans_ = nullptr;
    QPushButton* unban_ = nullptr;
};
