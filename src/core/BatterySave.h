#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>

// Called only by the emulation thread, at most once a second during play.
class BatterySave final {
public:
    static constexpr qint64 SettleMs = 2000;
    static constexpr qint64 MinimumWriteMs = 30000;
    bool update(const QString& path, const void* data, qsizetype size, qint64 now,
                bool force, bool skipBlank, QString& error);
    void rememberUnwritten(const QString& path, const void* formatted, qsizetype size);
    void clear() { entries_.clear(); }
private:
    struct Entry {
        QByteArray persisted, observed;
        qint64 changedAt = 0, dirtySince = -1, lastAttempt = -MinimumWriteMs;
        bool initialized = false;
    };
    QHash<QString, Entry> entries_;
};
