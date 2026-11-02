#include "i18n/Localization.h"
#include "CheatsDialog.h"
#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {
class CheatDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override {
        auto* editor = new QLineEdit(parent);
        editor->setMaxLength(index.column() == 1 ? 11 : Cheats::MaxDescriptionLength);
        return editor;
    }
};
}
CheatsDialog::CheatsDialog(const CheatList& entries, Cheats::Platform platform, const QString& gameName, QWidget* parent)
    : QDialog(parent), platform_(platform), result_(entries) {
    setWindowTitle(Localization::text(QStringLiteral("Читы — %1")).arg(gameName));
    resize(640,380); setMinimumSize(500,300); setSizeGripEnabled(true);
    auto* layout = new QVBoxLayout(this);
    auto* formats = new QLabel(Cheats::supportText(platform),this);
    formats->setWordWrap(true); layout->addWidget(formats);
    table_ = new QTableWidget(0,3,this);
    table_->setHorizontalHeaderLabels({Localization::text(QStringLiteral("Вкл.")),Localization::text(QStringLiteral("Код")),Localization::text(QStringLiteral("Описание"))});
    table_->verticalHeader()->hide(); table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Interactive);
    table_->setColumnWidth(1,170);
    table_->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch);
    table_->setItemDelegateForColumn(1,new CheatDelegate(table_));
    table_->setItemDelegateForColumn(2,new CheatDelegate(table_));
    layout->addWidget(table_,1);
    auto* actions = new QHBoxLayout;
    add_ = new QPushButton(Localization::text(QStringLiteral("Добавить")),this);
    remove_ = new QPushButton(Localization::text(QStringLiteral("Удалить")),this);
    actions->addWidget(add_); actions->addWidget(remove_); actions->addStretch();
    actions->addWidget(new QLabel(Localization::text(QStringLiteral("До %1 кодов")).arg(Cheats::MaxCodes),this));
    layout->addLayout(actions);
    feedback_ = new QLabel(this); feedback_->setWordWrap(true); layout->addWidget(feedback_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this);
    buttons->button(QDialogButtonBox::Save)->setText(Localization::text(QStringLiteral("Применить")));
    buttons->button(QDialogButtonBox::Cancel)->setText(Localization::text(QStringLiteral("Отмена")));
    layout->addWidget(buttons);
    for (const auto& entry : entries) addRow(entry);
    connect(add_,&QPushButton::clicked,this,[this] {
        if (table_->rowCount() >= Cheats::MaxCodes) return;
        addRow({}); table_->setCurrentCell(table_->rowCount()-1,1);
        table_->editItem(table_->item(table_->rowCount()-1,1)); updateButtons();
    });
    connect(remove_,&QPushButton::clicked,this,[this] {
        auto rows = table_->selectionModel()->selectedRows();
        std::sort(rows.begin(),rows.end(),[](const QModelIndex& a,const QModelIndex& b){return a.row()>b.row();});
        for (const auto& index : rows) table_->removeRow(index.row());
        feedback_->clear(); updateButtons();
    });
    connect(table_,&QTableWidget::itemSelectionChanged,this,&CheatsDialog::updateButtons);
    connect(table_,&QTableWidget::itemChanged,this,[this] {feedback_->clear();});
    connect(buttons,&QDialogButtonBox::accepted,this,&CheatsDialog::save);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    updateButtons();
}
void CheatsDialog::addRow(const CheatEntry& entry) {
    const int row = table_->rowCount(); table_->insertRow(row);
    auto* enabled = new QTableWidgetItem;
    enabled->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsUserCheckable);
    enabled->setCheckState(entry.enabled ? Qt::Checked : Qt::Unchecked);
    table_->setItem(row,0,enabled);
    table_->setItem(row,1,new QTableWidgetItem(entry.code));
    table_->setItem(row,2,new QTableWidgetItem(entry.description));
}
void CheatsDialog::updateButtons() {
    add_->setEnabled(table_->rowCount()<Cheats::MaxCodes);
    remove_->setEnabled(!table_->selectionModel()->selectedRows().isEmpty());
}
void CheatsDialog::save() {
    table_->setCurrentItem(nullptr); // Commit an active cell editor before collecting its text.
    CheatList entries;
    entries.reserve(table_->rowCount());
    for (int row=0;row<table_->rowCount();++row)
        entries.append({table_->item(row,1)->text(),table_->item(row,2)->text(),table_->item(row,0)->checkState()==Qt::Checked});
    QString error;
    if (!Cheats::normalize(entries,platform_,result_,error)) { feedback_->setText(error); return; }
    accept();
}
