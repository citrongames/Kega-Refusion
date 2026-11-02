#include "AchievementsDialog.h"

#include <QAbstractTableModel>
#include <QCache>
#include <QEvent>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

class AchievementModel final : public QAbstractTableModel {
public:
    explicit AchievementModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : state_.entries.size();
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 9;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
        static const QStringList headers{
            QString(), QStringLiteral("Достижение"), QStringLiteral("Очки"),
            QStringLiteral("Softcore"), QStringLiteral("Hardcore"),
            QStringLiteral("Открыли %"), QStringLiteral("Hard %"),
            QStringLiteral("Тип"), QStringLiteral("Прогресс")};
        return section >= 0 && section < headers.size() ? headers[section] : QVariant();
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= state_.entries.size()) return {};
        const AchievementEntry& entry = state_.entries[index.row()];
        if (role == Qt::DecorationRole && index.column() == 0) {
            const QPixmap* image = icons_.object(badgeUrl(index.row()));
            return image ? QVariant(*image) : QVariant();
        }
        if (role == Qt::TextAlignmentRole)
            return int(index.column() == 1 ? Qt::AlignLeft | Qt::AlignVCenter
                                           : Qt::AlignHCenter | Qt::AlignVCenter);
        if (role == Qt::ToolTipRole)
            return entry.title + QLatin1Char('\n') + entry.description;
        if (role != Qt::DisplayRole) return {};
        switch (index.column()) {
        case 1: return entry.title + QLatin1Char('\n') + entry.description;
        case 2: return entry.points;
        case 3: return entry.softcoreUnlocked ? QStringLiteral("✓") : QStringLiteral("—");
        case 4: return entry.hardcoreUnlocked ? QStringLiteral("✓") : QStringLiteral("—");
        case 5: return QStringLiteral("%1%").arg(entry.rarity, 0, 'f', 2);
        case 6: return QStringLiteral("%1%").arg(entry.hardcoreRarity, 0, 'f', 2);
        case 7:
            if (entry.unsupported) return QStringLiteral("Недоступно");
            switch (entry.type) {
            case 1: return QStringLiteral("Пропускаемое");
            case 2: return QStringLiteral("Прогресс");
            case 3: return QStringLiteral("Победа");
            default: return QStringLiteral("—");
            }
        case 8: return entry.progress.isEmpty() ? QStringLiteral("—") : entry.progress;
        default: return {};
        }
    }
    void setState(const AchievementsState& state) {
        const bool newGame = state.gameId != state_.gameId;
        beginResetModel();
        state_ = state;
        if (newGame) icons_.clear();
        endResetModel();
    }
    QString badgeUrl(int row) const {
        if (row < 0 || row >= state_.entries.size()) return {};
        const AchievementEntry& entry = state_.entries[row];
        return (!entry.softcoreUnlocked && !entry.hardcoreUnlocked &&
                !entry.lockedBadgeUrl.isEmpty()) ? entry.lockedBadgeUrl : entry.badgeUrl;
    }
    bool hasBadge(const QString& url) const { return icons_.contains(url); }
    void setBadge(const QString& url, const QImage& image) {
        if (image.isNull()) return;
        auto* icon = new QPixmap(QPixmap::fromImage(image).scaled(48, 48, Qt::KeepAspectRatio,
                                                               Qt::SmoothTransformation));
        icons_.insert(url, icon, icon->width() * icon->height() * 4);
        for (int row = 0; row < state_.entries.size(); ++row) {
            if (badgeUrl(row) == url)
                emit dataChanged(index(row, 0), index(row, 0), {Qt::DecorationRole});
        }
    }

private:
    AchievementsState state_;
    QCache<QString, QPixmap> icons_{8 * 1024 * 1024};
};

AchievementsDialog::AchievementsDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("achievementsDialog"));
    setWindowTitle(QStringLiteral("Достижения — Kega Refusion"));
    setMinimumSize(780, 460);
    resize(1120, 670);
    setSizeGripEnabled(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);
    account_ = new QLabel(this);
    account_->setObjectName(QStringLiteral("achievementsAccount"));
    game_ = new QLabel(this);
    game_->setObjectName(QStringLiteral("achievementsGame"));
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("achievementsStatus"));
    status_->setWordWrap(true);
    layout->addWidget(account_);
    layout->addWidget(game_);
    layout->addWidget(status_);
    model_ = new AchievementModel(this);
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("achievementsTable"));
    table_->setModel(model_);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setWordWrap(true);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(68);
    table_->setColumnWidth(0, 60);
    table_->setColumnWidth(2, 54);
    table_->setColumnWidth(3, 80);
    table_->setColumnWidth(4, 80);
    table_->setColumnWidth(5, 84);
    table_->setColumnWidth(6, 74);
    table_->setColumnWidth(7, 110);
    table_->setColumnWidth(8, 110);
    table_->viewport()->installEventFilter(this);
    layout->addWidget(table_, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Закрыть"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(table_->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &AchievementsDialog::requestVisibleBadges);
}

void AchievementsDialog::setState(const AchievementsState& state) {
    model_->setState(state);
    account_->setText(state.loggedIn
        ? QStringLiteral("RetroAchievements: %1%2").arg(state.username,
            state.hardcore ? QStringLiteral(" · Hardcore") : QStringLiteral(" · Softcore"))
        : QStringLiteral("Войдите в RetroAchievements в настройках."));
    game_->setText(state.gameId
        ? QStringLiteral("%1 · открыто %2 из %3 · %4 очков")
              .arg(state.gameTitle).arg(state.unlocked).arg(state.total).arg(state.points)
        : QStringLiteral("Игра с достижениями не загружена."));
    status_->setText(state.status);
    QTimer::singleShot(0, this, &AchievementsDialog::requestVisibleBadges);
}

void AchievementsDialog::setBadge(const QString& url, const QImage& image) {
    model_->setBadge(url, image);
}

bool AchievementsDialog::eventFilter(QObject* watched, QEvent* event) {
    if (table_ && watched == table_->viewport() && event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, &AchievementsDialog::requestVisibleBadges);
    return QDialog::eventFilter(watched, event);
}

void AchievementsDialog::requestVisibleBadges() {
    if (!isVisible() || model_->rowCount() == 0) return;
    const int first = qMax(0, table_->rowAt(0));
    int last = table_->rowAt(table_->viewport()->height() - 1);
    if (last < 0) last = model_->rowCount() - 1;
    for (int row = first; row <= qMin(last, model_->rowCount() - 1); ++row) {
        const QString url = model_->badgeUrl(row);
        if (!url.isEmpty() && !model_->hasBadge(url)) emit badgeRequested(url);
    }
}
