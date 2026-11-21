#pragma once

#include "core/DebugInspection.h"

#include <QDialog>

class DebugMemoryPageModel;
class DebugStateInspector;
class QComboBox;
class QHideEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableView;
class QTableWidget;
class QTabWidget;

class DebugMemoryDialog final : public QDialog {
    Q_OBJECT
public:
    explicit DebugMemoryDialog(QWidget* parent = nullptr);
    DebugInspectionRequest request() const;
    void setSnapshot(const DebugInspectionSnapshot& snapshot);
    void navigateTo(quint32 domain, quint32 offset);
    void navigateTo(quint32 instance, quint32 domain, quint32 offset);
    void finishMemoryWrite(const QString& error);

signals:
    void inspectionChanged();
    void writeRequested(const DebugMemoryWrite& request);
    void watchRequested(const DebugWatchEntry& entry);
    void inspectionInstanceChanged();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void changeOffset(quint32 offset);
    void changeInstance(quint32 instance);
    bool canWrite(int width) const;
    void editMemory(int width);
    bool hasSample(int width) const;
    void addWatch();
    void showMemoryMenu(const QPoint& point);
    void copySelectedBytes();
    bool watchBigEndian() const;
    void updatePage(const QByteArray& bytes, bool comparePrevious = true);
    void resizePageColumns();
    void updateNavigation();
    void updateWatch();

    QTabWidget* tabs_ = nullptr;
    DebugStateInspector* hardware_ = nullptr;
    QLabel* status_ = nullptr;
    QComboBox* domains_ = nullptr;
    QComboBox* instances_ = nullptr;
    QLineEdit* offset_ = nullptr;
    QPushButton* previous_ = nullptr;
    QPushButton* next_ = nullptr;
    QPushButton* edit_ = nullptr;
    QPushButton* watch_ = nullptr;
    QLabel* editStatus_ = nullptr;
    QLabel* value_ = nullptr;
    QComboBox* width_ = nullptr;
    QComboBox* order_ = nullptr;
    QTableView* hex_ = nullptr;
    DebugMemoryPageModel* pageModel_ = nullptr;
    quint64 generation_ = 0;
    quint32 domain_ = KR_MEM_RAM;
    quint32 size_ = 0;
    quint32 selection_ = 0;
    quint32 page_ = 0;
    quint32 instance_ = 0;
    bool canEdit_ = false, writable_ = false, writePending_ = false;
    bool bigEndian_ = false;
    bool available_ = false;
};

class DebugStateInspector final : public QWidget {
public:
    explicit DebugStateInspector(quint32 part, QWidget* parent = nullptr);
    void setSnapshot(const DebugInspectionSnapshot& snapshot);

private:
    QLabel* status_ = nullptr;
    QTableWidget* table_ = nullptr;
    quint32 part_;
};

class DebugVideoImage;

class DebugVideoInspector final : public QWidget {
    Q_OBJECT
public:
    enum View { Tiles, Map, Sprites, Palette, Framebuffer };

    explicit DebugVideoInspector(View view, QWidget* parent = nullptr);
    void setSnapshot(const DebugInspectionSnapshot& snapshot);

signals:
    void memoryRequested(quint32 domain, quint32 offset);

private:
    void redraw();
    void updateDetail();

    const View view_;
    QLabel* status_ = nullptr;
    QLabel* detail_ = nullptr;
    QComboBox* selection_ = nullptr;
    QComboBox* palette_ = nullptr;
    DebugVideoImage* image_ = nullptr;
    QTableWidget* sprites_ = nullptr;
    DebugInspectionSnapshot snapshot_;
    QString modeCaption_;
    QPoint point_{-1, -1};
    int selectedSprite_ = 0;
};
