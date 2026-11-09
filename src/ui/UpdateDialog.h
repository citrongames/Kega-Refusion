#pragma once

#include <QDialog>

class QLabel;
class QCheckBox;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

class UpdateDialog final : public QDialog {
    Q_OBJECT
public:
    explicit UpdateDialog(QWidget* parent = nullptr);
    void setChecking();
    void setRelease(const QString& version, const QString& notes);
    void setNoUpdate();
    void setError(const QString& message, bool canInstall);
    void setProgress(qint64 received, qint64 total);
    void setRestarting();
    void setCheckOnStartup(bool enabled);

signals:
    void installRequested();
    void checkRequested();
    void checkOnStartupChanged(bool enabled);

private:
    QLabel* latestVersion_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* notesLabel_ = nullptr;
    QPlainTextEdit* notes_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* primaryButton_ = nullptr;
    QCheckBox* startupCheck_ = nullptr;
    bool installAction_ = false;
    QString releaseNotes_;
};
