#pragma once
#include <QByteArrayView>
#include <QString>

namespace CartridgeProfile {
// ATF is an explicit hardware selection for an unchanged, plain BIN image.
// BIN detection is deliberately restricted to verified Firecore code families.
inline bool firecore(const QString& extension, QByteArrayView rom) {
    if (extension.compare(QLatin1String("atf"), Qt::CaseInsensitive) == 0) return true;
    if (extension.compare(QLatin1String("bin"), Qt::CaseInsensitive) != 0 || rom.size() < 512) return false;
    if (rom.sliced(0x100,16) == QByteArrayView("DMC RedKid      ") ||
        rom.sliced(0x100,16) == QByteArrayView("DMC  EMDGAME (C)")) return true;
    // Niutai and ordinary AtGames releases share this system/copyright header.
    // Identify the two complete, adjacent register-$18 setters, including their
    // shared RAM flag address and writes to the VDP control port. Fixed code
    // locations avoid matching isolated register values in graphics or tables.
    if (rom.size() < 0x33ee ||
        rom.sliced(0x100,32) != QByteArrayView("SEGA GENESIS (C) 2004 FVRING    ")) return false;
    constexpr char enable[] =
        "\x10\x10\x00\x00\xff\x80\x10\x80\x02\x40\x98\xff"
        "\x00\x40\x98\x00\x33\xc0\x00\xc0\x00\x04\x4e\x75";
    constexpr char disable[] =
        "\x10\x10\x02\x00\x00\x7f\x10\x80\x02\x40\x98\x7f"
        "\x00\x40\x98\x00\x33\xc0\x00\xc0\x00\x04\x4e\x75";
    return rom.sliced(0x33b2,4) == QByteArrayView("\x41\xf9\x00\xff",4) &&
        rom.sliced(0x33d0,6) == rom.sliced(0x33b2,6) &&
        rom.sliced(0x33b8,24) == QByteArrayView(enable,24) &&
        rom.sliced(0x33d6,24) == QByteArrayView(disable,24);
}
}
