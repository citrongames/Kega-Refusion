#pragma once
#include <QList>
#include <QMetaType>
#include <QString>

struct CheatEntry {
    QString code;
    QString description;
    bool enabled = true;
    friend bool operator==(const CheatEntry& a, const CheatEntry& b) {
        return a.code == b.code && a.description == b.description && a.enabled == b.enabled;
    }
};
using CheatList = QList<CheatEntry>;

namespace Cheats {
// GPX uses the MD decoder for CD/32X; this does not add SH-2 memory codes.
enum class Platform { MegaDrive, MasterSystem, SegaCd, Sega32x, Cd32x };
constexpr int MaxCodes = 100; // Enabled native components; below GPX MAX_CHEATS=150.
constexpr int MaxEntries = 4096;
constexpr int MaxCodeLength = 4095;
constexpr int MaxDescriptionLength = 256;
constexpr qint64 MaxFileBytes = 2 * 1024 * 1024;
bool normalize(const CheatList& entries, Platform platform, CheatList& normalized, QString& error);
QString supportText(Platform platform);
QString ramCode(quint32 address, int width, bool bigEndian, quint32 value);

// Emulation-thread owned. Reads once at game load; writes atomically only on
// an explicit change. Entries can group native codes with + under one enabled flag.
class Store final {
public:
    bool load(const QString& path, Platform platform, QString& error);
    bool set(const CheatList& entries, QString& error);
    void clear();
    const CheatList& entries() const { return entries_; }
private:
    QString path_;
    Platform platform_ = Platform::MegaDrive;
    CheatList entries_;
};
}
Q_DECLARE_METATYPE(CheatEntry)
Q_DECLARE_METATYPE(CheatList)
Q_DECLARE_METATYPE(Cheats::Platform)
