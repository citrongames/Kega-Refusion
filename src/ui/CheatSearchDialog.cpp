#include "CheatSearchDialog.h"
#include "i18n/Localization.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <limits>

namespace {
constexpr int MaxShownResults = 256;
std::atomic<quint64> nextGlobalRequestId{0};

bool unsignedValue(const QLineEdit* edit, int width, quint32& value) {
    const QString text = edit->text().trimmed();
    const bool hex = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);
    const QString digits = hex ? text.mid(2) : text;
    if (digits.isEmpty()) return false;
    bool okay = false;
    const qulonglong parsed = digits.toULongLong(&okay, hex ? 16 : 10);
    const qulonglong max = width == 1 ? 0xffULL :
                           width == 2 ? 0xffffULL : 0xffffffffULL;
    if (!okay || parsed > max) return false;
    value = quint32(parsed);
    return true;
}
}

CheatSearchDialog::CheatSearchDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                        Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint) {
    setObjectName(QStringLiteral("cheatSearchDialog"));
    setWindowTitle(Localization::text(QStringLiteral("Поиск читов в RAM")));
    setModal(false);
    resize(760, 520);
    setMinimumSize(650, 460);
    setSizeGripEnabled(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    auto* searchRow = new QHBoxLayout;
    widthBox_ = new QComboBox(this);
    widthBox_->setObjectName(QStringLiteral("cheatSearchWidth"));
    for (int width : {1, 2, 4})
        widthBox_->addItem(Localization::text(QStringLiteral("%1 байт")).arg(width), width);
    comparisonBox_ = new QComboBox(this);
    comparisonBox_->setObjectName(QStringLiteral("cheatSearchComparison"));
    comparisonBox_->addItem(Localization::text(QStringLiteral("Равно")), 0);
    comparisonBox_->addItem(Localization::text(QStringLiteral("Изменилось")), 1);
    comparisonBox_->addItem(Localization::text(QStringLiteral("Не изменилось")), 2);
    comparisonBox_->addItem(Localization::text(QStringLiteral("Увеличилось")), 3);
    comparisonBox_->addItem(Localization::text(QStringLiteral("Уменьшилось")), 4);
    comparisonBox_->addItem(Localization::text(QStringLiteral("Неизвестно")), 5);
    comparisonBox_->setCurrentIndex(comparisonBox_->findData(5));
    searchValue_ = new QLineEdit(this);
    searchValue_->setObjectName(QStringLiteral("cheatSearchValue"));
    searchValue_->setPlaceholderText(Localization::text(QStringLiteral("Число или 0xHEX")));
    searchValue_->setMaximumWidth(150);
    newSearch_ = new QPushButton(Localization::text(QStringLiteral("Новый поиск")), this);
    newSearch_->setObjectName(QStringLiteral("cheatSearchNew"));
    filter_ = new QPushButton(Localization::text(QStringLiteral("Отфильтровать")), this);
    filter_->setObjectName(QStringLiteral("cheatSearchFilter"));
    searchRow->addWidget(widthBox_);
    searchRow->addWidget(comparisonBox_);
    searchRow->addWidget(searchValue_);
    searchRow->addWidget(newSearch_);
    searchRow->addWidget(filter_);
    layout->addLayout(searchRow);

    scope_ = new QLabel(Localization::text(QStringLiteral("Область RAM: —")), this);
    scope_->setObjectName(QStringLiteral("cheatSearchScope"));
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("cheatSearchStatus"));
    status_->setWordWrap(true);
    status_->hide();
    auto* information = new QHBoxLayout;
    information->addWidget(scope_);
    information->addStretch(1);
    information->addWidget(status_);
    layout->addLayout(information);

    results_ = new QTableWidget(0, 3, this);
    results_->setObjectName(QStringLiteral("cheatSearchResults"));
    results_->setHorizontalHeaderLabels({
        Localization::text(QStringLiteral("Адрес")),
        Localization::text(QStringLiteral("Было")),
        Localization::text(QStringLiteral("Сейчас"))});
    results_->verticalHeader()->hide();
    results_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_->setSelectionMode(QAbstractItemView::SingleSelection);
    results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    results_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    results_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(results_, 1);

    auto* saveRow = new QHBoxLayout;
    desiredValue_ = new QLineEdit(this);
    desiredValue_->setObjectName(QStringLiteral("cheatSearchDesiredValue"));
    desiredValue_->setPlaceholderText(Localization::text(QStringLiteral("Значение")));
    desiredValue_->setMaximumWidth(125);
    description_ = new QLineEdit(this);
    description_->setObjectName(QStringLiteral("cheatSearchDescription"));
    description_->setMaxLength(Cheats::MaxDescriptionLength);
    description_->setPlaceholderText(Localization::text(QStringLiteral("Описание")));
    freeze_ = new QCheckBox(Localization::text(QStringLiteral("Включить (заморозить)")), this);
    freeze_->setObjectName(QStringLiteral("cheatSearchFreeze"));
    freeze_->setChecked(true);
    addCheat_ = new QPushButton(Localization::text(QStringLiteral("Добавить чит")), this);
    addCheat_->setObjectName(QStringLiteral("cheatSearchAdd"));
    saveRow->addWidget(desiredValue_);
    saveRow->addWidget(description_, 1);
    saveRow->addWidget(freeze_);
    saveRow->addWidget(addCheat_);
    layout->addLayout(saveRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(Localization::text(QStringLiteral("Закрыть")));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(newSearch_, &QPushButton::clicked, this, [this] { requestSearch(true); });
    connect(filter_, &QPushButton::clicked, this, [this] { requestSearch(false); });
    connect(addCheat_, &QPushButton::clicked, this, &CheatSearchDialog::addSelectedCheat);
    connect(results_, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = results_->currentRow();
        if (row >= 0 && row < results_->rowCount()) {
            desiredValue_->setText(results_->item(row, 2)->text());
            const quint32 address = results_->item(row, 0)->data(Qt::UserRole).toUInt();
            description_->setText(Localization::text(QStringLiteral("RAM %1"))
                                  .arg(addressText(address, base_)));
        }
        updateActions();
    });
    connect(comparisonBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        searchValue_->setEnabled(comparisonBox_->currentData().toInt() == 0);
    });
    searchValue_->setEnabled(false);
    updateActions();
}

QString CheatSearchDialog::addressText(quint32 address, quint32 base) {
    const int digits = base > 0xffffu ? 6 : 4;
    return QStringLiteral("0x%1").arg(address, digits, 16, QLatin1Char('0')).toUpper();
}

bool CheatSearchDialog::parseValue(const QLineEdit* edit, int width, quint32& value) const {
    return unsignedValue(edit, width, value);
}

void CheatSearchDialog::setStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setVisible(!text.isEmpty());
    status_->setStyleSheet(error ? QStringLiteral("color: #c84a3a;") : QString());
}

void CheatSearchDialog::updateActions() {
    newSearch_->setEnabled(pendingRequestId_ == 0);
    filter_->setEnabled(started_ && pendingRequestId_ == 0);
    addCheat_->setEnabled(started_ && pendingRequestId_ == 0 &&
                          results_->currentRow() >= 0);
}

void CheatSearchDialog::requestSearch(bool restart) {
    if (pendingRequestId_) return;
    if (!restart && !started_) {
        setStatus(Localization::text(QStringLiteral("Сначала начните новый поиск.")), true);
        return;
    }
    int comparison = comparisonBox_->currentData().toInt();
    if (restart && comparison != 0 && comparison != 5) {
        comparison = 5;
        comparisonBox_->setCurrentIndex(comparisonBox_->findData(5));
    }
    quint32 value = 0;
    const int width = widthBox_->currentData().toInt();
    if (comparison == 0 && !parseValue(searchValue_, width, value)) {
        setStatus(Localization::text(QStringLiteral("Введите беззнаковое число в диапазоне выбранной ширины.")), true);
        searchValue_->setFocus();
        return;
    }
    if (restart) {
        started_ = false;
        results_->setRowCount(0);
        scope_->setText(Localization::text(QStringLiteral("Область RAM: —")));
        desiredValue_->clear();
        description_->clear();
    }
    setStatus(Localization::text(QStringLiteral("Поиск…")));
    pendingRequestId_ = nextGlobalRequestId.fetch_add(1, std::memory_order_relaxed) + 1;
    updateActions();
    emit searchRequested(pendingRequestId_, width, comparison, value, restart);
}

void CheatSearchDialog::setResults(quint64 requestId, const QByteArray& json) {
    if (!pendingRequestId_ || requestId != pendingRequestId_) return;
    pendingRequestId_ = 0;
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject()) {
        setStatus(Localization::text(QStringLiteral("Не удалось прочитать результаты поиска.")), true);
        updateActions();
        return;
    }
    const QJsonObject object = document.object();
    const QString error = object.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        setStatus(Localization::text(error), true);
        updateActions();
        return;
    }
    const qint64 count = object.value(QStringLiteral("count")).toInteger(-1);
    const qint64 base = object.value(QStringLiteral("base")).toInteger(-1);
    const int width = object.value(QStringLiteral("width")).toInt();
    const QJsonValue endianValue = object.value(QStringLiteral("bigEndian"));
    const QJsonValue matchesValue = object.value(QStringLiteral("results"));
    if (count < 0 || base < 0 || base > std::numeric_limits<quint32>::max() ||
        (width != 1 && width != 2 && width != 4) ||
        !endianValue.isBool() || !matchesValue.isArray()) {
        setStatus(Localization::text(QStringLiteral("Не удалось прочитать результаты поиска.")), true);
        updateActions();
        return;
    }
    const QJsonArray matches = matchesValue.toArray();
    const int shown = std::min(int(matches.size()), MaxShownResults);
    const quint32 nextBase = quint32(base);
    const bool nextBigEndian = endianValue.toBool();
    struct Match { quint32 address, previous, value; };
    QVector<Match> parsed;
    parsed.reserve(shown);
    for (int index = 0; index < shown; ++index) {
        const QJsonObject match = matches[index].toObject();
        const qint64 address = match.value(QStringLiteral("address")).toInteger(-1);
        const qint64 previous = match.value(QStringLiteral("previous")).toInteger(-1);
        const qint64 value = match.value(QStringLiteral("value")).toInteger(-1);
        if (!matches[index].isObject() || address < 0 || previous < 0 || value < 0 ||
            address > std::numeric_limits<quint32>::max() ||
            previous > std::numeric_limits<quint32>::max() ||
            value > std::numeric_limits<quint32>::max()) {
            setStatus(Localization::text(QStringLiteral("Не удалось прочитать результаты поиска.")), true);
            updateActions();
            return;
        }
        parsed.append({quint32(address), quint32(previous), quint32(value)});
    }

    width_ = width;
    base_ = nextBase;
    bigEndian_ = nextBigEndian;
    started_ = true;
    {
        const QSignalBlocker blocker(results_);
        results_->setUpdatesEnabled(false);
        results_->setRowCount(parsed.size());
        for (int row = 0; row < parsed.size(); ++row) {
            auto* address = new QTableWidgetItem(addressText(parsed[row].address, base_));
            address->setData(Qt::UserRole, parsed[row].address);
            results_->setItem(row, 0, address);
            results_->setItem(row, 1, new QTableWidgetItem(QString::number(parsed[row].previous)));
            results_->setItem(row, 2, new QTableWidgetItem(QString::number(parsed[row].value)));
        }
        results_->setUpdatesEnabled(true);
    }
    scope_->setText(Localization::text(QStringLiteral("Область RAM: %1"))
                    .arg(addressText(base_, base_)));
    setStatus(count > parsed.size()
        ? Localization::text(QStringLiteral("Найдено: %1 · показаны первые %2"))
            .arg(count).arg(parsed.size())
        : Localization::text(QStringLiteral("Найдено: %1")).arg(count));
    if (!parsed.isEmpty()) {
        results_->setCurrentCell(0, 0);
        desiredValue_->setText(QString::number(parsed.front().value));
        description_->setText(Localization::text(QStringLiteral("RAM %1"))
                              .arg(addressText(parsed.front().address, base_)));
    } else {
        desiredValue_->clear();
        description_->clear();
    }
    updateActions();
}

void CheatSearchDialog::addSelectedCheat() {
    const int row = results_->currentRow();
    if (!started_ || pendingRequestId_ || row < 0 || row >= results_->rowCount()) return;
    quint32 value = 0;
    if (!parseValue(desiredValue_, width_, value)) {
        setStatus(Localization::text(QStringLiteral("Введите беззнаковое число в диапазоне выбранной ширины.")), true);
        desiredValue_->setFocus();
        return;
    }
    const quint32 address = results_->item(row, 0)->data(Qt::UserRole).toUInt();
    const QString code = Cheats::ramCode(address, width_, bigEndian_, value);
    if (code.isEmpty()) {
        setStatus(Localization::text(QStringLiteral("Этот адрес RAM нельзя сохранить как чит.")), true);
        return;
    }
    pendingSave_ = CheatEntry{code, description_->text().trimmed(), freeze_->isChecked()};
    setStatus(Localization::text(QStringLiteral("Применение чита…")));
    emit saveRequested(*pendingSave_);
}

void CheatSearchDialog::setCheats(const CheatList& entries) {
    if (!pendingSave_) return;
    if (std::find(entries.cbegin(), entries.cend(), *pendingSave_) == entries.cend()) return;
    pendingSave_.reset();
    setStatus(Localization::text(QStringLiteral("Чит сохранён.")));
}

void CheatSearchDialog::setSaveError(const QString& error) {
    if (!pendingSave_) return;
    pendingSave_.reset();
    setStatus(error, true);
}

void CheatSearchDialog::resetSearch() {
    pendingRequestId_ = 0;
    pendingSave_.reset();
    started_ = false;
    base_ = 0;
    width_ = 0;
    bigEndian_ = false;
    results_->setRowCount(0);
    results_->clearSelection();
    desiredValue_->clear();
    description_->clear();
    freeze_->setChecked(true);
    scope_->setText(Localization::text(QStringLiteral("Область RAM: —")));
    setStatus({});
    updateActions();
    emit clearRequested();
}

void CheatSearchDialog::done(int result) {
    resetSearch();
    QDialog::done(result);
}
