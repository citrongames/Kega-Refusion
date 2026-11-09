#include "UpdateDialog.h"
#include "i18n/Localization.h"

#include <QApplication>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

UpdateDialog::UpdateDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("updateDialog"));
    setWindowTitle(Localization::text(QStringLiteral("Обновление Kega Refusion")));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::NonModal);
    setMinimumWidth(420);
    resize(520, 340);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);
    auto* currentVersion = new QLabel(Localization::text(QStringLiteral("Текущая версия: %1"))
        .arg(QApplication::applicationVersion()), this);
    currentVersion->setObjectName(QStringLiteral("updateCurrentVersion"));
    currentVersion->setTextFormat(Qt::PlainText);
    currentVersion->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(currentVersion);
    latestVersion_ = new QLabel(this);
    latestVersion_->setObjectName(QStringLiteral("updateNewVersion"));
    latestVersion_->setTextFormat(Qt::PlainText);
    latestVersion_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(latestVersion_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("updateStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    notesLabel_ = new QLabel(Localization::text(QStringLiteral("Изменения в выпуске:")), this);
    layout->addWidget(notesLabel_);
    notes_ = new QPlainTextEdit(this);
    notes_->setObjectName(QStringLiteral("updateReleaseNotes"));
    notes_->setReadOnly(true);
    notes_->setMaximumBlockCount(500);
    notes_->setMinimumHeight(100);
    layout->addWidget(notes_, 1);
    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("updateProgress"));
    layout->addWidget(progress_);
    startupCheck_ = new QCheckBox(Localization::text(QStringLiteral("Проверять обновления при запуске")), this);
    startupCheck_->setObjectName(QStringLiteral("updateCheckOnStartup"));
    startupCheck_->setChecked(true);
    connect(startupCheck_, &QCheckBox::toggled, this, &UpdateDialog::checkOnStartupChanged);
    layout->addWidget(startupCheck_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(Localization::text(QStringLiteral("Закрыть")));
    primaryButton_ = buttons->addButton(QString(), QDialogButtonBox::ActionRole);
    primaryButton_->setObjectName(QStringLiteral("updatePrimaryButton"));
    primaryButton_->setDefault(true);
    connect(primaryButton_, &QPushButton::clicked, this, [this] {
        if (installAction_) emit installRequested();
        else emit checkRequested();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
    setChecking();
}

void UpdateDialog::setChecking() {
    status_->setText(Localization::text(QStringLiteral("Проверка обновлений…")));
    latestVersion_->hide();
    notesLabel_->hide();
    notes_->hide();
    progress_->show();
    progress_->setRange(0, 0);
    primaryButton_->setText(Localization::text(QStringLiteral("Проверить снова")));
    primaryButton_->setEnabled(false);
    installAction_ = false;
}

void UpdateDialog::setRelease(const QString& version, const QString& notes) {
    latestVersion_->setText(Localization::text(QStringLiteral("Новая версия: %1")).arg(version));
    latestVersion_->show();
    status_->setText(Localization::text(QStringLiteral(
        "Доступно обновление. После установки приложение перезапустится.")));
    const QString text = notes.trimmed().left(32768);
    const QString display = text.isEmpty()
        ? Localization::text(QStringLiteral("Описание выпуска отсутствует.")) : text;
    if (display != releaseNotes_) {
        releaseNotes_ = display;
        notes_->setPlainText(display);
    }
    notesLabel_->show();
    notes_->show();
    progress_->hide();
    primaryButton_->setText(Localization::text(QStringLiteral("Скачать и установить")));
    primaryButton_->setEnabled(true);
    installAction_ = true;
}

void UpdateDialog::setNoUpdate() {
    latestVersion_->hide();
    notesLabel_->hide();
    notes_->hide();
    progress_->hide();
    status_->setText(Localization::text(QStringLiteral("Установлена последняя версия.")));
    primaryButton_->setText(Localization::text(QStringLiteral("Проверить снова")));
    primaryButton_->setEnabled(true);
    installAction_ = false;
}

void UpdateDialog::setError(const QString& message, bool canInstall) {
    if (!canInstall) {
        latestVersion_->hide();
        notesLabel_->hide();
        notes_->hide();
    }
    progress_->hide();
    status_->setText(message);
    primaryButton_->setText(Localization::text(canInstall
        ? QStringLiteral("Скачать и установить") : QStringLiteral("Проверить снова")));
    primaryButton_->setEnabled(true);
    installAction_ = canInstall;
}

void UpdateDialog::setProgress(qint64 received, qint64 total) {
    progress_->show();
    primaryButton_->setEnabled(false);
    if (total > 0) {
        progress_->setRange(0, 100);
        progress_->setValue(int(qBound(0.0, 100.0 * double(received) / double(total), 100.0)));
        status_->setText(Localization::text(QStringLiteral("Загрузка: %1 / %2 МиБ"))
            .arg(QString::number(double(received) / (1024 * 1024), 'f', 1),
                 QString::number(double(total) / (1024 * 1024), 'f', 1)));
    } else {
        progress_->setRange(0, 0);
        status_->setText(Localization::text(QStringLiteral("Загрузка обновления…")));
    }
}

void UpdateDialog::setRestarting() {
    status_->setText(Localization::text(QStringLiteral("Обновление подготовлено. Перезапуск…")));
    primaryButton_->setEnabled(false);
    progress_->setRange(0, 0);
    progress_->show();
}

void UpdateDialog::setCheckOnStartup(bool enabled) {
    const QSignalBlocker blocker(startupCheck_);
    startupCheck_->setChecked(enabled);
}
