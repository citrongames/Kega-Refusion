#include "NetplayLobby.h"

#include "i18n/Localization.h"
#include "input/ControllerType.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
QString t(const char* source) { return Localization::text(QString::fromUtf8(source)); }

bool validManualAddress(const QString& address) {
    if (address.isEmpty()) return true;
    QHostAddress ip;
    if (ip.setAddress(address)) return true;
    if (address.size() > 253) return false;
    static const QRegularExpression label(
        QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9_-]{0,61}[A-Za-z0-9])?$"));
    for (const QString& part : address.split(QLatin1Char('.')))
        if (!label.match(part).hasMatch()) return false;
    return true;
}

QString roleName(const QString& role,const QJsonArray& kinds,int jcartBase) {
    if (role.startsWith(QLatin1String("player"))) {
        bool ok=false;const int player=role.mid(6).toInt(&ok);
        if(ok && player>=1 && player<=8 && role==QStringLiteral("player%1").arg(player)) {
            if(jcartBase>=2 && player>jcartBase && player<=jcartBase+2)
                return t("Игрок %1 · J-Cart (порт %2)").arg(player).arg(player-jcartBase);
            const int kind=player<=kinds.size()?kinds[player-1].toInt(0):0;
            if(kind==int(ControllerType::LightPhaser) && player<=2)
                return t("Игрок %1 · Light Phaser (порт %2)").arg(player).arg(player);
            if(kind==int(ControllerType::Menacer) && player==2)
                return t("Игрок %1 · Menacer (порт 2)").arg(player);
            if(kind==int(ControllerType::Justifier) && player>=2 && player<=3)
                return t("Игрок %1 · Justifier %2 (порт 2)").arg(player).arg(player-1);
            if(kind==int(ControllerType::Mouse) && player<=2)
                return t("Игрок %1 · MD Mouse (порт %2)").arg(player).arg(player);
            if(kind==int(ControllerType::Paddle) && player<=2)
                return t("Игрок %1 · SMS Paddle (порт %2)").arg(player).arg(player);
            return t("Игрок %1").arg(player);
        }
    }
    if (role == QLatin1String("spectator")) return t("Зритель");
    return t("Не выбрана");
}

QString readinessName(const QString& readiness) {
    if (readiness == QLatin1String("ready")) return t("Готов");
    if (readiness == QLatin1String("waiting")) return t("Ожидание");
    if (readiness == QLatin1String("rom-missing")) return t("Нет ROM");
    if (readiness == QLatin1String("rom-mismatch")) return t("ROM не совпадает");
    if (readiness == QLatin1String("transferring")) return t("Передача ROM");
    if (readiness == QLatin1String("reconnecting")) return t("Переподключение");
    if (readiness == QLatin1String("syncing")) return t("Синхронизация");
    if (readiness == QLatin1String("disconnected")) return t("Отключён");
    return readiness.isEmpty() ? t("Ожидание") : readiness;
}

QString romStatusName(const QString& status) {
    if (status == QLatin1String("matched")) return t("ROM совпадает");
    if (status == QLatin1String("missing")) return t("Выберите ROM или запросите загрузку");
    if (status == QLatin1String("mismatch")) return t("ROM не совпадает с игрой хоста");
    if (status == QLatin1String("downloading")) return t("Загрузка ROM");
    if (status == QLatin1String("uploading")) return t("Отправка ROM");
    if (status == QLatin1String("verifying")) return t("Проверка ROM");
    if (status == QLatin1String("cancelled")) return t("Передача отменена");
    if (status == QLatin1String("failed")) return t("Не удалось передать ROM");
    return status.isEmpty() ? t("ROM не выбран") : status;
}

void setCell(QTableWidget* table, int row, int column, const QString& value) {
    auto* item = table->item(row, column);
    if (!item) {
        item = new QTableWidgetItem(value);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, column, item);
    } else if (item->text() != value) {
        item->setText(value);
    }
}
}

NetplayLobby::NetplayLobby(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                      Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint) {
    setObjectName(QStringLiteral("netplayLobby"));
    setWindowTitle(t("Сетевое лобби — Kega Refusion"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(600, 500);
    resize(660, 560);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    auto* top = new QHBoxLayout;
    status_ = new QLabel(t("Нет сетевого сеанса"), this);
    status_->setObjectName(QStringLiteral("netplayStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    top->addWidget(status_, 1);
    stopWaiting_ = new QPushButton(t("Продолжить без отключившихся"), this);
    stopWaiting_->setObjectName(QStringLiteral("netplayStopWaiting"));
    stopWaiting_->hide();
    top->addWidget(stopWaiting_);
    leave_ = new QPushButton(t("Выйти из сеанса"), this);
    leave_->setObjectName(QStringLiteral("netplayLeave"));
    top->addWidget(leave_);
    layout->addLayout(top);
    connect(leave_, &QPushButton::clicked, this, &NetplayLobby::leaveRequested);
    connect(stopWaiting_, &QPushButton::clicked, this, &NetplayLobby::stopWaitingRequested);

    auto* inviteBox = new QGroupBox(t("Приглашение"), this);
    auto* inviteLayout = new QVBoxLayout(inviteBox);
    auto* inviteRow = new QHBoxLayout;
    invite_ = new QLineEdit(inviteBox);
    invite_->setObjectName(QStringLiteral("netplayInvite"));
    invite_->setReadOnly(true);
    invite_->setEchoMode(QLineEdit::Password);
    invite_->setPlaceholderText(t("Доступно после создания сеанса"));
    inviteRow->addWidget(invite_, 1);
    copyInvite_ = new QPushButton(t("Копировать"), inviteBox);
    copyInvite_->setObjectName(QStringLiteral("netplayCopyInvite"));
    inviteRow->addWidget(copyInvite_);
    copyWithPassword_ = new QPushButton(t("С паролем"), inviteBox);
    copyWithPassword_->setObjectName(QStringLiteral("netplayCopyWithPassword"));
    copyWithPassword_->setToolTip(t("Копировать приглашение вместе с паролем доступа"));
    inviteRow->addWidget(copyWithPassword_);
    inviteLayout->addLayout(inviteRow);
    auto* addressRow = new QHBoxLayout;
    addressStatus_ = new QLabel(inviteBox);
    addressStatus_->setObjectName(QStringLiteral("netplayAddressStatus"));
    addressStatus_->setTextFormat(Qt::PlainText);
    addressStatus_->setWordWrap(true);
    addressRow->addWidget(addressStatus_, 1);
    retryAddress_ = new QPushButton(t("Повторить"), inviteBox);
    retryAddress_->setObjectName(QStringLiteral("netplayRetryAddress"));
    addressRow->addWidget(retryAddress_);
    inviteLayout->addLayout(addressRow);
    auto* lanInviteRow = new QHBoxLayout;
    lanInviteRow->addStretch();
    copyLanInvite_ = new QPushButton(t("Копировать для локальной сети"), inviteBox);
    copyLanInvite_->setObjectName(QStringLiteral("netplayCopyLanInvite"));
    lanInviteRow->addWidget(copyLanInvite_);
    inviteLayout->addLayout(lanInviteRow);
    layout->addWidget(inviteBox);
    connect(copyInvite_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(state_.value(QStringLiteral("invite")).toString());
    });
    connect(copyWithPassword_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(state_.value(QStringLiteral("inviteWithPassword")).toString());
    });
    connect(copyLanInvite_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(state_.value(QStringLiteral("lanInvite")).toString());
    });
    connect(retryAddress_, &QPushButton::clicked, this, &NetplayLobby::retryAddressRequested);

    participantsTitle_ = new QLabel(t("Участники"), this);
    auto headingFont = participantsTitle_->font();
    headingFont.setBold(true);
    participantsTitle_->setFont(headingFont);
    auto* participantsHeading = new QHBoxLayout;
    participantsHeading->addWidget(participantsTitle_,1);
    qualitySummary_ = new QLabel(this);
    qualitySummary_->setObjectName(QStringLiteral("netplayQuality"));
    qualitySummary_->setTextFormat(Qt::PlainText);
    qualitySummary_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    participantsHeading->addWidget(qualitySummary_);
    layout->addLayout(participantsHeading);
    participants_ = new QTableWidget(0, 4, this);
    participants_->setObjectName(QStringLiteral("netplayParticipants"));
    participants_->setHorizontalHeaderLabels({t("Имя"), t("Роль"), t("Готовность"), t("Пинг")});
    participants_->horizontalHeaderItem(3)->setToolTip(
        t("Пинг игрового соединения (UDP), если он уже измерен на этом компьютере. Если игрового измерения нет, показан служебный пинг лобби (TCP)."));
    participants_->horizontalHeaderItem(1)->setToolTip(
        t("Участник управляет назначенным ему портом с помощью своего первого контроллера."));
    participants_->verticalHeader()->hide();
    participants_->horizontalHeader()->setStretchLastSection(false);
    participants_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    participants_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    participants_->setColumnWidth(1, 300);
    participants_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    participants_->setColumnWidth(2, 105);
    participants_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    participants_->setSelectionBehavior(QAbstractItemView::SelectRows);
    participants_->setSelectionMode(QAbstractItemView::SingleSelection);
    participants_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    participants_->setMinimumHeight(110);
    layout->addWidget(participants_, 1);
    connect(participants_, &QTableWidget::itemSelectionChanged,
            this, &NetplayLobby::updateModerationButtons);

    auto* moderation = new QHBoxLayout;
    kick_ = new QPushButton(t("Отключить"), this);
    kick_->setObjectName(QStringLiteral("netplayKick"));
    sessionBan_ = new QPushButton(t("Запретить до конца сеанса"), this);
    sessionBan_->setObjectName(QStringLiteral("netplaySessionBan"));
    permanentBan_ = new QPushButton(t("Запретить навсегда"), this);
    permanentBan_->setObjectName(QStringLiteral("netplayPermanentBan"));
    moderation->addWidget(kick_);
    moderation->addWidget(sessionBan_);
    moderation->addWidget(permanentBan_);
    moderation->addStretch();
    layout->addLayout(moderation);
    connect(kick_, &QPushButton::clicked, this, [this] {
        const QString id = selectedPeerId();
        if (!id.isEmpty()) emit kickRequested(id);
    });
    connect(sessionBan_, &QPushButton::clicked, this, [this] {
        const QString id = selectedPeerId();
        if (!id.isEmpty()) emit banRequested(id, false);
    });
    connect(permanentBan_, &QPushButton::clicked, this, [this] {
        const QString id = selectedPeerId();
        if (!id.isEmpty()) emit banRequested(id, true);
    });

    auto* romBox = new QGroupBox(t("Игра и ROM"), this);
    auto* romLayout = new QVBoxLayout(romBox);
    romName_ = new QLabel(t("Игра не выбрана"), romBox);
    romName_->setObjectName(QStringLiteral("netplayRomName"));
    romName_->setTextFormat(Qt::PlainText);
    romName_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    romName_->setWordWrap(true);
    romStatus_ = new QLabel(t("ROM не выбран"), romBox);
    romStatus_->setObjectName(QStringLiteral("netplayRomStatus"));
    romStatus_->setTextFormat(Qt::PlainText);
    romStatus_->setWordWrap(true);
    romLayout->addWidget(romName_);
    romLayout->addWidget(romStatus_);
    linkCable_ = new QCheckBox(t("Link Cable (две консоли Mega Drive)"), romBox);
    linkCable_->setObjectName(QStringLiteral("netplayLinkCable"));
    linkCable_->setToolTip(t("Хост может включить Link Cable в этом лобби. Включение перезапускает игру на двух консолях; выключение продолжает игру на консоли A. Игроки 1 и 2 управляют первым геймпадом консолей A и B."));
    romLayout->addWidget(linkCable_);
    connect(linkCable_, &QCheckBox::toggled, this, &NetplayLobby::linkCableRequested);
    progress_ = new QProgressBar(romBox);
    progress_->setObjectName(QStringLiteral("netplayTransferProgress"));
    progress_->setRange(0, 100);
    romLayout->addWidget(progress_);
    auto* romButtons = new QHBoxLayout;
    selectRom_ = new QPushButton(t("Выбрать свой ROM…"), romBox);
    selectRom_->setObjectName(QStringLiteral("netplaySelectRom"));
    downloadRom_ = new QPushButton(t("Запросить загрузку"), romBox);
    downloadRom_->setObjectName(QStringLiteral("netplayDownloadRom"));
    cancelTransfer_ = new QPushButton(t("Отменить передачу"), romBox);
    cancelTransfer_->setObjectName(QStringLiteral("netplayCancelTransfer"));
    romButtons->addWidget(selectRom_);
    romButtons->addWidget(downloadRom_);
    romButtons->addWidget(cancelTransfer_);
    romButtons->addStretch();
    romLayout->addLayout(romButtons);
    layout->addWidget(romBox);
    connect(selectRom_, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, t("Выбрать ROM для сетевой игры"), {},
            t("Игры (*.md *.gen *.bin *.smd *.sms *.gg *.sg *.68k *.zip *.7z *.cue *.iso *.chd *.m3u);;Все файлы (*)"));
        if (!path.isEmpty()) emit selectRomRequested(path);
    });
    connect(downloadRom_, &QPushButton::clicked, this, &NetplayLobby::downloadRomRequested);
    connect(cancelTransfer_, &QPushButton::clicked, this, &NetplayLobby::cancelTransferRequested);

    auto* hostBox = new QGroupBox(t("Права участников"), this);
    auto* hostLayout = new QVBoxLayout(hostBox);
    allowCommands_ = new QCheckBox(t("Разрешить игровые команды всем игрокам"), hostBox);
    allowCommands_->setObjectName(QStringLiteral("netplayAllowCommands"));
    allowCommands_->setToolTip(t("Если выключено, игровые команды доступны только хосту"));
    allowDownloads_ = new QCheckBox(t("Разрешить скачивание ROM у хоста"), hostBox);
    allowDownloads_->setObjectName(QStringLiteral("netplayAllowDownloads"));
    hostLayout->addWidget(allowCommands_);
    hostLayout->addWidget(allowDownloads_);
    layout->addWidget(hostBox);
    connect(allowCommands_, &QCheckBox::toggled, this, &NetplayLobby::allowCommandsRequested);
    connect(allowDownloads_, &QCheckBox::toggled, this, &NetplayLobby::allowDownloadsRequested);

    setLobbyState({});
}

void NetplayLobby::showCreateDialog() {
    auto* dialog = new QDialog(this, Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setWindowTitle(t("Создать сетевой сеанс"));
    auto* layout = new QVBoxLayout(dialog);
    auto* form = new QFormLayout;
    auto* password = new QLineEdit(dialog);
    password->setObjectName(QStringLiteral("netplayCreatePassword"));
    password->setEchoMode(QLineEdit::Password);
    form->addRow(t("Пароль (необязательно):"), password);
    layout->addLayout(form);
    auto* hint = new QLabel(t("Адрес для приглашения определится автоматически. Подключение между игроками прямое; настройка порта на маршрутизаторе может потребоваться."), dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* advancedToggle = new QToolButton(dialog);
    advancedToggle->setObjectName(QStringLiteral("netplayAdvancedOptions"));
    advancedToggle->setText(t("Дополнительные параметры"));
    advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedToggle->setArrowType(Qt::RightArrow);
    advancedToggle->setCheckable(true);
    layout->addWidget(advancedToggle, 0, Qt::AlignLeft);
    auto* advanced = new QWidget(dialog);
    auto* advancedForm = new QFormLayout(advanced);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    auto* connection = new QComboBox(advanced);
    connection->setObjectName(QStringLiteral("netplayCreateConnection"));
    connection->addItem(t("Автоматически — Ethernet / Wi-Fi"), QString());
    const QJsonArray connections = state_.value(QStringLiteral("connections")).toArray();
    for (int i = 0; i < qMin(connections.size(), 64); ++i) {
        const QJsonObject entry = connections.at(i).toObject();
        const QString id = entry.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        const QString name = entry.value(QStringLiteral("name")).toString().trimmed();
        const QString ipv4 = entry.value(QStringLiteral("ipv4")).toString().trimmed();
        QString label = name.isEmpty() ? ipv4 : name;
        if (!ipv4.isEmpty() && ipv4 != label) label += QStringLiteral(" · ") + ipv4;
        if (!entry.value(QStringLiteral("physical")).toBool()) label += t(" (VPN / виртуальное)");
        connection->addItem(label, id);
    }
    advancedForm->addRow(t("Подключение для определения адреса:"), connection);
    auto* address = new QLineEdit(advanced);
    address->setObjectName(QStringLiteral("netplayAdvertisedAddress"));
    address->setPlaceholderText(t("Автоматически"));
    advancedForm->addRow(t("Адрес для приглашения:"), address);
    auto* port = new QSpinBox(dialog);
    port->setObjectName(QStringLiteral("netplayCreatePort"));
    port->setRange(1, 65535);
    port->setValue(33445);
    advancedForm->addRow(t("Порт:"), port);
    advanced->setVisible(false);
    layout->addWidget(advanced);
    connect(advancedToggle, &QToolButton::toggled, dialog, [advancedToggle, advanced](bool shown) {
        advanced->setVisible(shown);
        advancedToggle->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
    });
    auto* error = new QLabel(dialog);
    error->setWordWrap(true);
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(t("Создать"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog,
            [this, dialog, port, address, password, connection, error, advancedToggle] {
        const QString advertised = address->text().trimmed();
        if (!validManualAddress(advertised)) {
            error->setText(t("Укажите только IP-адрес или имя хоста, без схемы и порта."));
            address->setFocus();
            advancedToggle->setChecked(true);
            return;
        }
        emit createRequested(quint16(port->value()), advertised, password->text(),
                             connection->currentData().toString());
        dialog->accept();
        show();
        raise();
        activateWindow();
    });
    dialog->show();
    password->setFocus();
}

void NetplayLobby::showJoinDialog() {
    auto* dialog = new QDialog(this, Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setWindowTitle(t("Присоединиться к сетевой игре"));
    auto* layout = new QVBoxLayout(dialog);
    auto* form = new QFormLayout;
    auto* invite = new QLineEdit(dialog);
    invite->setObjectName(QStringLiteral("netplayJoinInvite"));
    invite->setEchoMode(QLineEdit::Password);
    invite->setPlaceholderText(t("Вставьте приглашение от хоста"));
    form->addRow(t("Приглашение:"), invite);
    auto* password = new QLineEdit(dialog);
    password->setObjectName(QStringLiteral("netplayJoinPassword"));
    password->setEchoMode(QLineEdit::Password);
    form->addRow(t("Пароль (если нужен):"), password);
    layout->addLayout(form);
    auto* error = new QLabel(dialog);
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(t("Присоединиться"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, invite, password, error] {
        if (invite->text().trimmed().isEmpty()) {
            error->setText(t("Вставьте приглашение от хоста."));
            invite->setFocus();
            return;
        }
        emit joinRequested(invite->text().trimmed(), password->text());
        dialog->accept();
        show();
        raise();
        activateWindow();
    });
    dialog->show();
    invite->setFocus();
}

void NetplayLobby::setLobbyState(const QJsonObject& state) {
    state_ = state;
    const bool active = state.value(QStringLiteral("active")).toBool();
    const bool host = active && state.value(QStringLiteral("host")).toBool();
    const QString status = state.value(QStringLiteral("status")).toString();
    status_->setText(status.isEmpty() ? (active ? t("Сетевой сеанс") : t("Нет сетевого сеанса"))
                                       : Localization::text(status));
    leave_->setEnabled(active);
    const bool connectionWaiting = host && state.value(QStringLiteral("connectionWaiting")).toBool();
    stopWaiting_->setVisible(connectionWaiting);
    stopWaiting_->setEnabled(connectionWaiting);
    const QString invite = host ? state.value(QStringLiteral("invite")).toString() : QString();
    invite_->setText(invite);
    invite_->setPlaceholderText(host && invite.isEmpty() ? t("Определение адреса…")
                                                      : t("Доступно после создания сеанса"));
    copyInvite_->setEnabled(!invite.isEmpty());
    const QString inviteWithPassword = state.value(QStringLiteral("inviteWithPassword")).toString();
    const bool hasPasswordInvite = host && !invite.isEmpty() &&
        !inviteWithPassword.isEmpty() && inviteWithPassword != invite;
    copyWithPassword_->setEnabled(hasPasswordInvite);
    copyWithPassword_->setVisible(hasPasswordInvite);
    const QString lanInvite = state.value(QStringLiteral("lanInvite")).toString();
    copyLanInvite_->setVisible(host);
    copyLanInvite_->setEnabled(host && !lanInvite.isEmpty());
    const QString addressStatus = host ? state.value(QStringLiteral("addressStatus")).toString() : QString();
    addressStatus_->setText(Localization::text(addressStatus));
    addressStatus_->setVisible(!addressStatus.isEmpty());
    const bool retryAddress = host && state.value(QStringLiteral("addressRetry")).toBool();
    retryAddress_->setVisible(retryAddress);
    retryAddress_->setEnabled(retryAddress);

    updateParticipants(state);
    updateQualitySummary();

    const QJsonObject rom = state.value(QStringLiteral("rom")).toObject();
    const QString fileName = rom.value(QStringLiteral("filename")).toString();
    romName_->setText(fileName.isEmpty() ? t("Игра не выбрана") : fileName);
    const QString romMessage = rom.value(QStringLiteral("message")).toString();
    romStatus_->setText(romMessage.isEmpty() ? romStatusName(rom.value(QStringLiteral("status")).toString())
                                          : Localization::text(romMessage));
    const bool transferring = rom.value(QStringLiteral("canCancel")).toBool();
    progress_->setVisible(transferring);
    if (transferring) progress_->setValue(qBound(0, rom.value(QStringLiteral("progress")).toInt(), 100));
    selectRom_->setEnabled(active && rom.value(QStringLiteral("canSelect")).toBool());
    downloadRom_->setEnabled(active && rom.value(QStringLiteral("canDownload")).toBool());
    cancelTransfer_->setEnabled(active && transferring);

    {
        const QSignalBlocker commandsBlocker(allowCommands_);
        const QSignalBlocker downloadsBlocker(allowDownloads_);
        const QSignalBlocker linkBlocker(linkCable_);
        allowCommands_->setChecked(state.value(QStringLiteral("allowCommands")).toBool());
        allowDownloads_->setChecked(state.value(QStringLiteral("allowDownloads")).toBool());
        linkCable_->setChecked(state.value(QStringLiteral("linkCable")).toBool());
    }
    allowCommands_->setEnabled(host);
    allowDownloads_->setEnabled(host);
    linkCable_->setEnabled(host);
    updateModerationButtons();
}

void NetplayLobby::updateParticipants(const QJsonObject& state) {
    const QJsonArray entries = state.value(QStringLiteral("participants")).toArray();
    const QJsonArray kinds = state.value(QStringLiteral("roleKinds")).toArray();
    const int jcartBase=state.value(QStringLiteral("jcart")).toBool()?
        state.value(QStringLiteral("jcartBase")).toInt():0;
    const int count = qMin(entries.size(), 32);
    participantsTitle_->setText(t("Участники (%1)").arg(entries.size()));
    const QString selection = selectedPeerId();
    const QSignalBlocker blocker(participants_);
    if (participants_->rowCount() != count) participants_->setRowCount(count);
    int selectedRow = -1;
    for (int row = 0; row < count; ++row) {
        const QJsonObject peer = entries.at(row).toObject();
        const QString id = peer.value(QStringLiteral("id")).toString();
        QString name = peer.value(QStringLiteral("nickname")).toString();
        if (peer.value(QStringLiteral("host")).toBool()) name += t(" (хост)");
        if (peer.value(QStringLiteral("local")).toBool()) name += t(" (вы)");
        setCell(participants_, row, 0, name);
        auto* nameCell = participants_->item(row, 0);
        const bool local = peer.value(QStringLiteral("local")).toBool();
        const QString role = peer.value(QStringLiteral("role")).toString();
        if (nameCell->data(Qt::UserRole).toString() != id) nameCell->setData(Qt::UserRole, id);
        if (nameCell->data(Qt::UserRole + 1).toBool() != local)
            nameCell->setData(Qt::UserRole + 1, local);
        setCell(participants_, row, 1, roleName(role,kinds,jcartBase));
        auto* roleBox = qobject_cast<QComboBox*>(participants_->cellWidget(row, 1));
        if (state.value(QStringLiteral("active")).toBool() &&
            state.value(QStringLiteral("host")).toBool() && !id.isEmpty()) {
            const int playerCount=qBound(2,state.value(QStringLiteral("playerCount")).toInt(2),8);
            if (!roleBox || roleBox->property("peerId").toString() != id || roleBox->count()!=playerCount+1) {
                if(roleBox)participants_->removeCellWidget(row,1);
                roleBox = new QComboBox(participants_);
                roleBox->setObjectName(QStringLiteral("netplayParticipantRole"));
                roleBox->setProperty("peerId", id);
                roleBox->addItem(t("Зритель"), QStringLiteral("spectator"));
                for(int player=1;player<=playerCount;++player)
                    roleBox->addItem(roleName(QStringLiteral("player%1").arg(player),kinds,jcartBase),QStringLiteral("player%1").arg(player));
                participants_->setCellWidget(row, 1, roleBox);
                connect(roleBox, &QComboBox::currentIndexChanged, this, [this, roleBox, id](int index) {
                    if (index >= 0 && state_.value(QStringLiteral("active")).toBool() &&
                        state_.value(QStringLiteral("host")).toBool())
                        emit roleRequested(id, roleBox->itemData(index).toString());
                });
            }
            const QSignalBlocker roleBlocker(roleBox);
            for(int player=1;player<=playerCount;++player) {
                const auto label=roleName(QStringLiteral("player%1").arg(player),kinds,jcartBase);
                if(roleBox->itemText(player)!=label)roleBox->setItemText(player,label);
            }
            const int roleIndex = roleBox->findData(role);
            if (roleBox->currentIndex() != roleIndex) roleBox->setCurrentIndex(roleIndex);
        } else if (roleBox) {
            participants_->removeCellWidget(row, 1);
        }
        const QString readiness = readinessName(peer.value(QStringLiteral("readiness")).toString());
        setCell(participants_, row, 2, readiness);
        participants_->item(row, 2)->setToolTip(readiness);
        if (!selection.isEmpty() && id == selection) selectedRow = row;
    }
    if (selectedRow >= 0 && participants_->currentRow() != selectedRow)
        participants_->selectRow(selectedRow);
    else if (selectedRow < 0)
        participants_->clearSelection();
    updateParticipantPings();
}

void NetplayLobby::setNetworkQuality(const QJsonObject& quality) {
    if(networkQuality_==quality)return;
    networkQuality_=quality;qualityPings_.clear();
    if(quality.value(QStringLiteral("active")).toBool()) {
        const auto peers=quality.value(QStringLiteral("peers")).toArray();
        for(int i=0;i<qMin(peers.size(),32);++i) {
            const auto peer=peers[i].toObject();const auto id=peer.value(QStringLiteral("id")).toString();
            const int ping=peer.value(QStringLiteral("pingMs")).toInt(-1);
            if(!id.isEmpty() && ping>=0)qualityPings_.insert(id,{ping,qMax(0,peer.value(QStringLiteral("jitterMs")).toInt())});
        }
    }
    if(!isVisible())return; // Cache the latest sample while the modeless lobby is closed.
    updateQualitySummary();updateParticipantPings();
}

void NetplayLobby::updateQualitySummary() {
    const bool active=state_.value(QStringLiteral("active")).toBool();
    qualitySummary_->setVisible(active);
    if(!active)return;
    QString text,tooltip;
    if(networkQuality_.value(QStringLiteral("active")).toBool()) {
        const int ping=networkQuality_.value(QStringLiteral("pingMs")).toInt(-1);
        const int delay=networkQuality_.value(QStringLiteral("delayFrames")).toInt(-1);
        const auto value=[](int number){return number<0?QStringLiteral("—"):QString::number(number);};
        text=t("У вас: пинг %1 · разброс %2\nВвод: %3 кадр. (авто) · Откаты: %4 кадр./с")
            .arg(ping<0?QStringLiteral("—"):t("%1 мс").arg(ping),
                 ping<0?QStringLiteral("—"):t("%1 мс").arg(qMax(0,networkQuality_.value(QStringLiteral("jitterMs")).toInt())),
                 value(delay),value(qMax(0,networkQuality_.value(QStringLiteral("rollbackFrames")).toInt())));
        tooltip=t("Наибольшие пинг и разброс задержки среди активных игроков, измеренные этим компьютером. Задержка ввода выбирается автоматически; у зрителя собственного ввода нет. Откаты — повторно рассчитанные кадры в секунду, а не частота кадров игры.\nЗа последний интервал: %1 откатов; самый длинный — %2 кадр.")
            .arg(qMax(0,networkQuality_.value(QStringLiteral("rollbackEvents")).toInt()))
            .arg(qMax(0,networkQuality_.value(QStringLiteral("maxRollbackFrames")).toInt()));
    } else {
        text=t("Качество связи появится после запуска игры");
        tooltip=t("Здесь появятся местные показатели игрового соединения и автоматической задержки ввода. Если игровое измерение недоступно, в таблице показан служебный пинг лобби (TCP).");
    }
    if(qualitySummary_->text()!=text)qualitySummary_->setText(text);
    if(qualitySummary_->toolTip()!=tooltip)qualitySummary_->setToolTip(tooltip);
}

void NetplayLobby::updateParticipantPings() {
    const auto entries=state_.value(QStringLiteral("participants")).toArray();
    const int count=qMin(entries.size(),participants_->rowCount());
    const QSignalBlocker blocker(participants_);
    for(int row=0;row<count;++row) {
        const auto peer=entries[row].toObject();const auto id=peer.value(QStringLiteral("id")).toString();
        const auto measured=qualityPings_.constFind(id);
        int ping=peer.value(QStringLiteral("pingMs")).toInt(-1);QString tooltip;
        if(peer.value(QStringLiteral("local")).toBool()) {
            ping=-1;tooltip=t("Этот компьютер.");
        } else if(measured!=qualityPings_.cend()) {
            ping=measured->first;
            tooltip=t("Пинг игрового соединения (UDP), измеренный на этом компьютере. Разброс задержки: %1 мс.").arg(measured->second);
        } else tooltip=t("Служебный пинг лобби (TCP). Измерение игрового пинга сейчас недоступно.");
        setCell(participants_,row,3,ping<0?QStringLiteral("—"):t("%1 мс").arg(ping));
        auto* cell=participants_->item(row,3);
        if(cell->toolTip()!=tooltip)cell->setToolTip(tooltip);
    }
}

void NetplayLobby::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    updateQualitySummary();updateParticipantPings();
}

QString NetplayLobby::selectedPeerId() const {
    const int row = participants_->currentRow();
    auto* item = row >= 0 ? participants_->item(row, 0) : nullptr;
    return item && item->isSelected() ? item->data(Qt::UserRole).toString() : QString();
}

void NetplayLobby::updateModerationButtons() {
    const int row = participants_->currentRow();
    auto* item = row >= 0 ? participants_->item(row, 0) : nullptr;
    const bool allowed = state_.value(QStringLiteral("active")).toBool() &&
        state_.value(QStringLiteral("host")).toBool() && item && item->isSelected() &&
        !item->data(Qt::UserRole).toString().isEmpty() &&
        !item->data(Qt::UserRole + 1).toBool();
    kick_->setEnabled(allowed);
    sessionBan_->setEnabled(allowed);
    permanentBan_->setEnabled(allowed);
}

void NetplayLobby::closeEvent(QCloseEvent* event) {
    hide();
    event->ignore();
}
