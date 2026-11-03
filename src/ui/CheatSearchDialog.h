#pragma once

#include "../core/Cheats.h"
#include <QByteArray>
#include <QDialog>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class CheatSearchDialog final : public QDialog {
    Q_OBJECT
public:
    explicit CheatSearchDialog(QWidget* parent = nullptr);
    void setResults(quint64 requestId, const QByteArray& json);
    void setCheats(const CheatList& entries);
    void setSaveError(const QString& error);
    void resetSearch();

signals:
    void searchRequested(quint64 requestId, int width, int comparison,
                         quint32 value, bool restart);
    void clearRequested();
    void saveRequested(const CheatEntry& entry);

protected:
    void done(int result) override;

private:
    void requestSearch(bool restart);
    void addSelectedCheat();
    void updateActions();
    void setStatus(const QString& text, bool error = false);
    bool parseValue(const QLineEdit* edit, int width, quint32& value) const;
    static QString addressText(quint32 address, quint32 base);

    QComboBox* widthBox_ = nullptr;
    QComboBox* comparisonBox_ = nullptr;
    QLineEdit* searchValue_ = nullptr;
    QPushButton* newSearch_ = nullptr;
    QPushButton* filter_ = nullptr;
    QLabel* scope_ = nullptr;
    QLabel* status_ = nullptr;
    QTableWidget* results_ = nullptr;
    QLineEdit* desiredValue_ = nullptr;
    QLineEdit* description_ = nullptr;
    QCheckBox* freeze_ = nullptr;
    QPushButton* addCheat_ = nullptr;
    quint64 pendingRequestId_ = 0;
    std::optional<CheatEntry> pendingSave_;
    quint32 base_ = 0;
    int width_ = 0;
    bool bigEndian_ = false;
    bool started_ = false;
};
