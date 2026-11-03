#include "i18n/Localization.h"
#include "CheatsDialog.h"
#include "../core/CheatImport.h"
#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace {
class CheatDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override {
        auto* editor = new QLineEdit(parent);
        editor->setMaxLength(index.column() == 1 ? Cheats::MaxCodeLength : Cheats::MaxDescriptionLength);
        return editor;
    }
};
}
CheatsDialog::CheatsDialog(const CheatList& entries, Cheats::Platform platform, const QString& gameName, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint), platform_(platform), gameName_(gameName), result_(entries) {
    setWindowTitle(Localization::text(QStringLiteral("Читы — %1")).arg(gameName));
    resize(640,380); setMinimumSize(500,300); setSizeGripEnabled(true);
    auto* layout = new QVBoxLayout(this);
    auto* formats = new QLabel(Cheats::supportText(platform),this);
    formats->setWordWrap(true); layout->addWidget(formats);
    table_ = new QTableWidget(0,3,this);
    table_->setHorizontalHeaderLabels({Localization::text(QStringLiteral("Вкл.")),Localization::text(QStringLiteral("Код")),Localization::text(QStringLiteral("Описание"))});
    table_->verticalHeader()->hide(); table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setWordWrap(false);
    table_->setTextElideMode(Qt::ElideMiddle);
    table_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Interactive);
    table_->setColumnWidth(1,170);
    table_->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch);
    table_->setItemDelegateForColumn(1,new CheatDelegate(table_));
    table_->setItemDelegateForColumn(2,new CheatDelegate(table_));
    layout->addWidget(table_,1);
    auto* actions = new QHBoxLayout;
    add_ = new QPushButton(Localization::text(QStringLiteral("Добавить")),this);
    import_ = new QPushButton(Localization::text(QStringLiteral("Загрузить CHT…")),this);
    import_->setObjectName(QStringLiteral("importChtButton"));
    export_ = new QPushButton(Localization::text(QStringLiteral("Сохранить CHT…")),this);
    export_->setObjectName(QStringLiteral("exportChtButton"));
    remove_ = new QPushButton(Localization::text(QStringLiteral("Удалить")),this);
    actions->addWidget(add_); actions->addWidget(remove_);
    actions->addStretch();
    actions->addWidget(import_); actions->addWidget(export_);
    layout->addLayout(actions);
    auto* capacity = new QLabel(Localization::text(QStringLiteral("До %1 записей · %2 активных кодов"))
        .arg(Cheats::MaxEntries).arg(Cheats::MaxCodes),this);
    capacity->setAlignment(Qt::AlignRight);
    layout->addWidget(capacity);
    feedback_ = new QLabel(this);
    feedback_->setWordWrap(true);
    feedback_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    feedback_->setMaximumHeight(90);
    feedback_->hide();
    layout->addWidget(feedback_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this);
    buttons->button(QDialogButtonBox::Save)->setText(Localization::text(QStringLiteral("Применить")));
    buttons->button(QDialogButtonBox::Cancel)->setText(Localization::text(QStringLiteral("Отмена")));
    layout->addWidget(buttons);
    table_->setRowCount(entries.size());
    for (int row = 0; row < entries.size(); ++row) setRow(row, entries[row]);
    connect(add_,&QPushButton::clicked,this,[this] {
        if (table_->rowCount() >= Cheats::MaxEntries) return;
        addRow({}); table_->setCurrentCell(table_->rowCount()-1,1);
        table_->editItem(table_->item(table_->rowCount()-1,1)); updateButtons();
    });
    connect(remove_,&QPushButton::clicked,this,[this] {
        auto rows = table_->selectionModel()->selectedRows();
        std::sort(rows.begin(),rows.end(),[](const QModelIndex& a,const QModelIndex& b){return a.row()>b.row();});
        for (const auto& index : rows) table_->removeRow(index.row());
        setFeedback({}); updateButtons();
    });
    connect(import_, &QPushButton::clicked, this, &CheatsDialog::importCht);
    connect(export_, &QPushButton::clicked, this, &CheatsDialog::exportCht);
    connect(table_,&QTableWidget::itemSelectionChanged,this,&CheatsDialog::updateButtons);
    connect(table_,&QTableWidget::itemChanged,this,[this] { setFeedback({}); });
    connect(buttons,&QDialogButtonBox::accepted,this,&CheatsDialog::save);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    updateButtons();
}
void CheatsDialog::addRow(const CheatEntry& entry) {
    const int row = table_->rowCount();
    table_->insertRow(row);
    setRow(row, entry);
}
void CheatsDialog::setRow(int row, const CheatEntry& entry) {
    auto* enabled = new QTableWidgetItem;
    enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    enabled->setCheckState(entry.enabled ? Qt::Checked : Qt::Unchecked);
    table_->setItem(row, 0, enabled);
    table_->setItem(row, 1, new QTableWidgetItem(entry.code));
    table_->setItem(row, 2, new QTableWidgetItem(entry.description));
}
void CheatsDialog::updateButtons() {
    add_->setEnabled(table_->rowCount() < Cheats::MaxEntries);
    remove_->setEnabled(!table_->selectionModel()->selectedRows().isEmpty());
}
void CheatsDialog::setFeedback(const QString& text, bool error) {
    feedback_->setText(text);
    feedback_->setToolTip(text);
    feedback_->setVisible(!text.isEmpty());
    feedback_->setStyleSheet(error ? QStringLiteral("color: #c84a3a;") : QString());
}
void CheatsDialog::importCht() {
    const QString path = QFileDialog::getOpenFileName(this,
        Localization::text(QStringLiteral("Загрузить RetroArch CHT")),
        {}, Localization::text(QStringLiteral("Файлы RetroArch CHT (*.cht);;Все файлы (*)")));
    if (path.isEmpty()) return;
    Cheats::ImportResult imported;
    QString error;
    if (!Cheats::importCht(path, platform_, imported, error)) {
        setFeedback(error, true);
        return;
    }

    table_->setCurrentItem(nullptr); // Finish an active edit before deduplicating.
    const auto canonical = [](const QString& code) {
        QStringList parts = code.split(QLatin1Char('+'), Qt::KeepEmptyParts);
        for (auto& part : parts) part = part.trimmed().toUpper();
        return parts.join(QLatin1Char('+'));
    };
    QSet<QString> seen;
    for (int row = 0; row < table_->rowCount(); ++row) {
        const QString code = canonical(table_->item(row, 1)->text());
        if (!code.isEmpty()) seen.insert(code);
    }
    CheatList fresh;
    fresh.reserve(imported.entries.size());
    int duplicates = 0;
    for (auto entry : imported.entries) {
        const QString code = canonical(entry.code);
        if (seen.contains(code)) { ++duplicates; continue; }
        seen.insert(code);
        entry.enabled = false;
        fresh.append(std::move(entry));
    }
    if (fresh.size() > Cheats::MaxEntries - table_->rowCount()) {
        setFeedback(Localization::text(QStringLiteral(
            "Импорт не выполнен: %1 новых записей не помещаются в список из %2."))
            .arg(fresh.size()).arg(Cheats::MaxEntries), true);
        return;
    }
    const int first = table_->rowCount();
    {
        const QSignalBlocker blocker(table_);
        table_->setUpdatesEnabled(false);
        table_->setRowCount(first + fresh.size());
        for (int index = 0; index < fresh.size(); ++index)
            setRow(first + index, fresh[index]);
        table_->setUpdatesEnabled(true);
    }
    updateButtons();
    QString summary = Localization::text(QStringLiteral("Импортировано: %1; пропущено: %2."))
        .arg(fresh.size()).arg(imported.skipped + duplicates);
    if (duplicates)
        summary += QLatin1Char(' ') + Localization::text(QStringLiteral("Уже в списке: %1.")).arg(duplicates);
    const int shown = std::min(2, int(imported.warnings.size()));
    for (int index = 0; index < shown; ++index)
        summary += QLatin1Char('\n') + imported.warnings[index];
    if (imported.warnings.size() > shown)
        summary += QLatin1Char('\n') +
            Localization::text(QStringLiteral("Ещё предупреждений: %1."))
                .arg(imported.warnings.size() - shown);
    setFeedback(summary);
    feedback_->setToolTip(summary + QLatin1Char('\n') + imported.warnings.join(QLatin1Char('\n')));
}
bool CheatsDialog::collectValidated(CheatList& normalized) {
    table_->setCurrentItem(nullptr); // Commit an active cell editor before collecting its text.
    CheatList entries;
    entries.reserve(table_->rowCount());
    for (int row = 0; row < table_->rowCount(); ++row)
        entries.append({table_->item(row, 1)->text(), table_->item(row, 2)->text(),
                        table_->item(row, 0)->checkState() == Qt::Checked});
    QString error;
    if (!Cheats::normalize(entries, platform_, normalized, error)) {
        setFeedback(error, true);
        return false;
    }
    return true;
}
void CheatsDialog::exportCht() {
    CheatList entries;
    if (!collectValidated(entries)) return;

    QString stem = gameName_.trimmed();
    for (QChar& character : stem) {
        if (character.unicode() < 0x20 ||
            QStringLiteral("\\/:*?\"<>|").contains(character))
            character = QLatin1Char('_');
    }
    stem = stem.trimmed().left(80);
    if (stem.isEmpty()) stem = QStringLiteral("cheats");
    QFileDialog dialog(this, Localization::text(QStringLiteral("Сохранить RetroArch CHT")),
                       QDir::home().filePath(stem + QStringLiteral(".cht")),
                       Localization::text(QStringLiteral("Файлы RetroArch CHT (*.cht);;Все файлы (*)")));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QStringLiteral("cht"));
    if (dialog.exec() != QDialog::Accepted) return;
    const QStringList selected = dialog.selectedFiles();
    if (selected.isEmpty()) return;

    QString error;
    if (!Cheats::exportCht(selected.front(), platform_, entries, error)) {
        setFeedback(error, true);
        return;
    }
    setFeedback(Localization::text(QStringLiteral("CHT сохранён: %1")).arg(selected.front()));
}
void CheatsDialog::save() {
    CheatList entries;
    if (!collectValidated(entries)) return;
    result_ = std::move(entries);
    accept();
}
