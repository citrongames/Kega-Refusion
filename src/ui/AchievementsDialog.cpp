#include "i18n/Localization.h"
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
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

namespace {
bool sameEntry(const AchievementEntry& a, const AchievementEntry& b) {
    return a.id == b.id && a.points == b.points &&
           a.title == b.title && a.description == b.description &&
           a.badgeUrl == b.badgeUrl && a.lockedBadgeUrl == b.lockedBadgeUrl &&
           a.rarity == b.rarity &&
           a.hardcoreRarity == b.hardcoreRarity &&
           a.softcoreUnlocked == b.softcoreUnlocked &&
           a.hardcoreUnlocked == b.hardcoreUnlocked &&
           a.unsupported == b.unsupported && a.type == b.type;
}
QString achievementType(const AchievementEntry& entry) {
    if (entry.unsupported) return Localization::text(QStringLiteral("Недоступно"));
    switch (entry.type) {
    case 1: return Localization::text(QStringLiteral("Пропускаемое"));
    case 2: return Localization::text(QStringLiteral("Прогресс"));
    case 3: return Localization::text(QStringLiteral("Победа"));
    default: return QStringLiteral("—");
    }
}
}

class AchievementModel final : public QAbstractTableModel {
public:
    explicit AchievementModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : state_.entries.size();
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 8;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
        static const QStringList headers{
            QString(), Localization::text(QStringLiteral("Достижение")), Localization::text(QStringLiteral("Очки")),
            QStringLiteral("Softcore"), QStringLiteral("Hardcore"),
            Localization::text(QStringLiteral("Открыли %")), QStringLiteral("Hard %"),
            Localization::text(QStringLiteral("Тип"))};
        return section >= 0 && section < headers.size() ? headers[section] : QVariant();
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= state_.entries.size()) return {};
        const AchievementEntry& entry = state_.entries[index.row()];
        if (role == Qt::UserRole) {
            switch (index.column()) {
            case 0: return entry.id;
            case 1: return entry.title;
            case 2: return entry.points;
            case 3: return int(entry.softcoreUnlocked);
            case 4: return int(entry.hardcoreUnlocked);
            case 5: return entry.rarity;
            case 6: return entry.hardcoreRarity;
            case 7: return achievementType(entry);
            default: return {};
            }
        }
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
        case 7: return achievementType(entry);
        default: return {};
        }
    }
    void setState(const AchievementsState& state) {
        const bool newGame = state.gameId != state_.gameId;
        bool sameRows = !newGame && state.entries.size() == state_.entries.size();
        if (sameRows) {
            for (int row = 0; row < state.entries.size(); ++row) {
                if (state.entries[row].id != state_.entries[row].id) {
                    sameRows = false;
                    break;
                }
            }
        }
        if (!sameRows) {
            beginResetModel();
            state_ = state;
            if (newGame) icons_.clear();
            endResetModel();
            return;
        }
        QList<int> changedRows;
        for (int row = 0; row < state.entries.size(); ++row)
            if (!sameEntry(state_.entries[row], state.entries[row]))
                changedRows.append(row);
        state_ = state;
        for (int row : changedRows)
            emit dataChanged(index(row, 0), index(row, 7),
                             {Qt::DisplayRole, Qt::DecorationRole, Qt::ToolTipRole, Qt::UserRole});
    }
    QString badgeUrl(int row) const {
        if (row < 0 || row >= state_.entries.size()) return {};
        const AchievementEntry& entry = state_.entries[row];
        return (!entry.softcoreUnlocked && !entry.hardcoreUnlocked &&
                !entry.lockedBadgeUrl.isEmpty()) ? entry.lockedBadgeUrl : entry.badgeUrl;
    }
    bool hasBadge(const QString& url) const { return icons_.contains(url); }
    void clearBadgeImages() {
        icons_.clear();
        if (!state_.entries.isEmpty())
            emit dataChanged(index(0, 0), index(state_.entries.size() - 1, 0),
                             {Qt::DecorationRole});
    }
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

class AchievementSortProxy final : public QSortFilterProxyModel {
public:
    explicit AchievementSortProxy(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}
protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        const QVariant a = sourceModel()->data(left, Qt::UserRole);
        const QVariant b = sourceModel()->data(right, Qt::UserRole);
        if (left.column() == 0 || (left.column() >= 2 && left.column() <= 6)) {
            const double first = a.toDouble();
            const double second = b.toDouble();
            if (first != second) return first < second;
        } else {
            const int result = QString::localeAwareCompare(a.toString(), b.toString());
            if (result != 0) return result < 0;
        }
        return left.row() < right.row();
    }
};

AchievementsDialog::AchievementsDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("achievementsDialog"));
    setWindowTitle(Localization::text(QStringLiteral("Достижения — Kega Refusion")));
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
    proxy_ = new AchievementSortProxy(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(Qt::UserRole);
    proxy_->setDynamicSortFilter(false);
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("achievementsTable"));
    table_->setModel(proxy_);
    table_->setSortingEnabled(true);
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
    table_->viewport()->installEventFilter(this);
    layout->addWidget(table_, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(Localization::text(QStringLiteral("Закрыть")));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(table_->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &AchievementsDialog::scheduleVisibleBadges);
    connect(table_->horizontalHeader(), &QHeaderView::sortIndicatorChanged,
            this, &AchievementsDialog::scheduleVisibleBadges);
    table_->sortByColumn(-1, Qt::AscendingOrder);
}

void AchievementsDialog::setState(const AchievementsState& state) {
    if (state.gameId != displayedGameId_) {
        requestedBadges_.clear();
        displayedGameId_ = state.gameId;
    }
    model_->setState(state);
    account_->setText(state.loggedIn
        ? QStringLiteral("RetroAchievements: %1%2").arg(state.username,
            state.hardcore ? QStringLiteral(" · Hardcore") : QStringLiteral(" · Softcore"))
        : Localization::text(QStringLiteral("Войдите в RetroAchievements в настройках.")));
    game_->setText(state.gameId
        ? Localization::text(QStringLiteral("%1 · открыто %2 из %3 · %4 очков"))
              .arg(state.gameTitle).arg(state.unlocked).arg(state.total).arg(state.points)
        : Localization::text(QStringLiteral("Игра с достижениями не загружена.")));
    status_->setText(state.status);
    scheduleVisibleBadges();
}

void AchievementsDialog::clearBadgeImages() {
    badgeRequestsPaused_ = true;
    requestedBadges_.clear();
    model_->clearBadgeImages();
}

void AchievementsDialog::setBadge(const QString& url, const QImage& image) {
    if (image.isNull() || !requestedBadges_.remove(url)) return;
    model_->setBadge(url, image);
}

bool AchievementsDialog::eventFilter(QObject* watched, QEvent* event) {
    if (table_ && watched == table_->viewport() && event->type() == QEvent::Resize)
        scheduleVisibleBadges();
    return QDialog::eventFilter(watched, event);
}

void AchievementsDialog::scheduleVisibleBadges() {
    if (visibleScanPending_) return;
    visibleScanPending_ = true;
    QTimer::singleShot(0, this, [this] {
        visibleScanPending_ = false;
        requestVisibleBadges();
    });
}

void AchievementsDialog::requestVisibleBadges() {
    if (badgeRequestsPaused_ || !isVisible() || proxy_->rowCount() == 0) return;
    const int first = qMax(0, table_->rowAt(0));
    int last = table_->rowAt(table_->viewport()->height() - 1);
    if (last < 0) last = proxy_->rowCount() - 1;
    for (int row = first; row <= qMin(last, proxy_->rowCount() - 1); ++row) {
        const int sourceRow = proxy_->mapToSource(proxy_->index(row, 0)).row();
        const QString url = model_->badgeUrl(sourceRow);
        if (!url.isEmpty() && !model_->hasBadge(url) &&
            !requestedBadges_.contains(url)) {
            if (requestedBadges_.size() >= 256) requestedBadges_.clear();
            requestedBadges_.insert(url);
            emit badgeRequested(url);
        }
    }
}
