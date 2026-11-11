#pragma once
#include "../core/Cheats.h"
#include <QDialog>
class QLabel;
class QPushButton;
class QTableWidget;

class CheatsDialog final : public QDialog {
    Q_OBJECT
public:
    CheatsDialog(const CheatList& entries, Cheats::Platform platform, const QString& gameName,
                 QWidget* parent = nullptr, const QString& directory = {});
    CheatList cheats() const { return result_; }
private:
    void addRow(const CheatEntry& entry);
    void setRow(int row, const CheatEntry& entry);
    void updateButtons();
    void sortRows();
    void importCht();
    void exportCht();
    bool collectValidated(CheatList& entries);
    void setFeedback(const QString& text, bool error = false);
    void save();
    Cheats::Platform platform_;
    QString gameName_;
    QString directory_;
    CheatList result_;
    QTableWidget* table_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* import_ = nullptr;
    QPushButton* export_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* disableAll_ = nullptr;
    QPushButton* clearAll_ = nullptr;
    quint64 nextRowOrder_ = 0;
    bool sortPending_ = false;
    QLabel* feedback_ = nullptr;
};
