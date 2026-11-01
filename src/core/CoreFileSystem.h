#pragma once

#include <QByteArray>
#include <QString>
#include <libretro.h>

// File access for the single libretro core on its emulation thread. Virtual
// paths are thread-local; open handles own their data or backing file.
class CoreFileSystem final {
public:
    static retro_vfs_interface* interface();
    static void clearVirtualFiles();
    static void addMemoryFile(QString virtualPath, QByteArray bytes);
    static void addAlias(QString virtualPath, QString actualPath, bool mp3 = false);
    static QString lastError();
    static void clearError();
    static quint64 mp3PcmBytesRead();
};
