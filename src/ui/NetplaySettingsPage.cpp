#include "NetplaySettingsPage.h"

#include "i18n/Localization.h"

#include <QFormLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

NetplaySettingsPage::NetplaySettingsPage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("netplaySettingsPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 8, 14, 8);
    layout->setSpacing(14);
    auto* heading = new QLabel(Localization::text(QStringLiteral("Сетевая игра")), this);
    auto font = heading->font();
    font.setPointSize(font.pointSize() + 3);
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);
    auto* description = new QLabel(Localization::text(QStringLiteral(
        "Подключения напрямую по адресу и порту. Никнейм и постоянные запреты сохраняются на этом компьютере.")), this);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto* form = new QFormLayout;
    nickname_ = new QLineEdit(this);
    nickname_->setObjectName(QStringLiteral("netplayNickname"));
    nickname_->setMaxLength(32);
    form->addRow(Localization::text(QStringLiteral("Никнейм:")), nickname_);
    layout->addLayout(form);
    connect(nickname_, &QLineEdit::editingFinished, this, [this] {
        const QString nickname = nickname_->text().trimmed();
        if (nickname != nickname_->text()) nickname_->setText(nickname);
        if (!nickname.isEmpty()) emit nicknameChanged(nickname);
    });

    layout->addWidget(new QLabel(Localization::text(QStringLiteral("Постоянно заблокированные участники:")), this));
    bans_ = new QListWidget(this);
    bans_->setObjectName(QStringLiteral("netplayPermanentBans"));
    layout->addWidget(bans_, 1);
    unban_ = new QPushButton(Localization::text(QStringLiteral("Снять запрет")), this);
    unban_->setObjectName(QStringLiteral("netplayUnban"));
    unban_->setEnabled(false);
    layout->addWidget(unban_);
    connect(bans_, &QListWidget::itemSelectionChanged, this, [this] {
        unban_->setEnabled(bans_->currentItem() != nullptr);
    });
    connect(unban_, &QPushButton::clicked, this, [this] {
        if (auto* item = bans_->currentItem()) emit unbanRequested(item->data(Qt::UserRole).toString());
    });
}

QString NetplaySettingsPage::nickname() const { return nickname_->text().trimmed(); }

void NetplaySettingsPage::setNickname(const QString& nickname) {
    const QSignalBlocker blocker(nickname_);
    if (nickname_->text() != nickname) nickname_->setText(nickname);
}

void NetplaySettingsPage::setBannedPeers(const QJsonArray& peers) {
    QString selected;
    if (auto* item = bans_->currentItem()) selected = item->data(Qt::UserRole).toString();
    const QSignalBlocker blocker(bans_);
    bans_->clear();
    for (const QJsonValue& value : peers) {
        const QJsonObject peer = value.toObject();
        if (!peer.value(QStringLiteral("persistent")).toBool()) continue;
        const QString fingerprint = peer.value(QStringLiteral("fingerprint")).toString();
        if (fingerprint.isEmpty()) continue;
        const QString nickname = peer.value(QStringLiteral("nickname")).toString().trimmed();
        const QString shortId = fingerprint.left(12) + (fingerprint.size() > 12 ? QStringLiteral("…") : QString());
        const QString label = nickname.isEmpty() ? fingerprint : nickname + QStringLiteral(" — ") + shortId;
        auto* item = new QListWidgetItem(label, bans_);
        item->setData(Qt::UserRole, fingerprint);
        item->setToolTip(fingerprint);
        if (fingerprint == selected) bans_->setCurrentItem(item);
    }
    unban_->setEnabled(bans_->currentItem() != nullptr);
}
