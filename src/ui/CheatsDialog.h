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
                 QWidget* parent = nullptr);
    CheatList cheats() const { return result_; }
private:
    void addRow(const CheatEntry& entry);
    void updateButtons();
    void save();
    Cheats::Platform platform_;
    CheatList result_;
    QTableWidget* table_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* remove_ = nullptr;
    QLabel* feedback_ = nullptr;
};
