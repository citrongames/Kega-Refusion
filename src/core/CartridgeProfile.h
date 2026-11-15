#pragma once
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QString>

namespace CartridgeProfile {
// These verified standalone dumps use shared Sega/Devworks/TecToy headers.
// Match the complete unchanged image; names and generic copyright strings
// cannot distinguish this supplied Firecore collection from ordinary MD ROMs.
struct FirecoreDump {
    qsizetype size;
    const char* sha256;
};

inline constexpr FirecoreDump knownFirecoreDumps[] = {
    {65536, "55e7ab6a80fa999c8eb3ce80d476c2430c3a5c9fc70da141ee64346af9ae9a76"}, // Mahjong Solitaire (Alt).bin
    {65536, "9e400861c3b1146190ee385ed030486e43b899adda7315c847d077a8abd2264b"}, // Mahjong Solitaire.bin
    {65536, "9d9e64647e4f7b7285e270dfefbeec633bf64504c27518ec14cdb582a614c613"}, // Memory.bin
    {65536, "4851f1ae4a0d0af959f43ae9b7512d5f347cb2a039e5bb45d22e047fe8938ca9"}, // Snake.bin
    {65536, "2ec319238314cef6e7445b48398900b891ef9fb287c4db23aa51e9900abb06dd"}, // Warehouse Keeper.bin
    {113698, "8cc33556c0aae465f7b0e464c2813d3e8321cb4894240d407b5c9d17cb78f7c4"}, // Air Hockey (Brazil).bin
    {117648, "514209014e850d0edf7b1cf8567a19c6a86f6fd4927db8257223843a2ea53b1e"}, // Air Hockey (USA).bin
    {262144, "906cd9c9fad7a3c5df0b488d4da2e3759810a04dcf93f510c63aad44f45faa1d"}, // Alien Force.bin
    {87834, "ef65adb19cf40ca63a19ef5be1bb2936228691303dc43ad4daf042a31e3225f0"}, // Anaconda.bin
    {88070, "09a3951ac570c6767dbd3faa34585c84fd9009966c16ed538f8cad62e02f1b13"}, // Bottle Taps Race (Brazil).bin
    {88070, "d63ec4a7f7340634a2f18dad7733d42a8cb1b530fbf0fae806b880d3c2ea7055"}, // Bottle Taps Race (USA).bin
    {115358, "263d537c8d36160ee25532b82a9e1c3a8159242e5e9a1d0d0d650841dee7e993"}, // Cannon (Brazil).bin
    {150718, "78d4d1556883715875e2dc6835708bb373b536110c4d0c3fe99053ddb010b3bd"}, // Cannon (USA).bin
    {262144, "790f7854d4fec34b3c7de780c3f3b45eaeb2191c7994bc1c7a85061e4d5b4942"}, // Checker (Brazil).bin
    {291348, "1a2fe210c5e7deafed9977aa1101283d6b5d853c382e21c5b880136a3dd5e051"}, // Checker (USA).bin
    {79010, "22bbb99d119e585e8545ba6520396aa24dc247430576a60d58bb6ded7b83c8ea"}, // Fight or Lose (Brazil).bin
    {106192, "7b728c90045074ac661e10956b6442cf30a37e76a953a243f4ed4e3f01a42617"}, // Fight or Lose (USA).bin
    {125206, "689c2d3e49efb8dd09f23a2f3f0d123a2615790116505a539639cb5097fa0a50"}, // Hang Gliding (Brazil).bin
    {262144, "f085a4267cbee4cb9496f0b814295d4324669d243d41123ea4b8f5d6da06dad8"}, // Join 4.bin
    {93402, "af2d524cca42c59c4be60bbbfa5657b4b2c189d0ce79c8747f191b6540b9446c"}, // Mosaic.bin
    {131072, "e86bac091a84bd74c9e0bbaada5668f4d5091900a20490cec54d56f28cda5650"}, // Mr. Balls (Brazil).bin
    {158044, "922e53f123ef3dd52e39afb1d2ebf91f5889e45282898fe22432167a3f2291c8"}, // Mr. Balls (USA).bin
    {82792, "c32c3c1a6f2d281521759b3328c184cf2b1daf809ea11d1d88f995c61b4797c1"}, // Naval Power (Brazil).bin
    {109856, "437bbea197b112890b1af3933cc923a936c313ec1f58cc5f5d13462bedee8b4e"}, // Naval Power (USA).bin
    {296524, "6f153fc681ff69bc71d2a8eb48c587b20a5f37f76945cf07c4d1fe03d2d1c437"}, // Polar Mason.bin
    {119916, "943e19fb01160814ea1a090a79093acd813ad2960be533cb85ca05bf5b86c810"}, // Spider (Alt).bin
    {81316, "c25cce008bdac00e6f92c729d9a8e9b2566dd9f31d879861bc16d605852ba5f0"}, // Spider (Brazil).bin
    {120848, "7799cd65b4fb441e604c3fd38a3c7b94778a4cda7986a170c00523535e1f8972"}, // Spider (USA).bin
    {1208218, "c6d8a0ab4c565669c21581fa0c560e44eea34ba8ecc57e40c49c958406e797ec"}, // Think Good 2004.bin
    {156588, "25b4502be64dc68d7f78810d0e593eaf60fc5ed5b7d2e02e654284691a1647bf"}, // War of the Monsters.bin
    {917504, "7455f9f62fca2cdb1684dd54c8da27da336c0d3501fb188461223fff8be44939"}, // Academy of Knowledge.bin
    {131072, "861653df0bdfc8b4131d69bd6af07f55c5fe054dcec895d850cdf3f1f35a3825"}, // Aquaduct.bin
    {131072, "232974cd33421308e59295ea3241d768827b2a118b76942194f2b3b60b5c92e0"}, // Bomber (Brazil).bin
    {131072, "a7069cc52c34a15eb6dc7ba9cab799b69101719d0807645a6c9fb1068bb0d376"}, // Bomber (USA).bin
    {524288, "a76b9394f11575f9a8271e91185bc4787273c36068679cbfaeb4e5c9388a113d"}, // Chess.bin
    {532394, "75d546cd6377bb76fdd7814730a5c4de587d1b738c411d572cc694190a259c0b"}, // Fantasy Pinball.bin
    {131072, "aa0d157e5c45633b3f7d191a0c45ee45439c82a0638cdb79e0d3835bcd9ab259"}, // Hexagonos (Brazil).bin
    {131072, "fdd9bd4d43944c8063b24a1a64eea5962f15d01ae9f469b25bf4c6b59254b02c"}, // Hexagonos (USA).bin
};

inline bool knownFirecoreDumpSize(qsizetype size) {
    for (const auto& dump : knownFirecoreDumps) {
        if (dump.size == size) return true;
    }
    return false;
}

inline bool knownFirecoreDump(QByteArrayView rom, QByteArrayView sourceSha256 = {}) {
    if (!knownFirecoreDumpSize(rom.size())) return false;
    // The host already hashes the original bytes for state identity. Standalone
    // callers compute it here only after the inexpensive exact-size filter.
    const QByteArray digest = sourceSha256.size() == 32
        ? QByteArray::fromRawData(sourceSha256.data(), sourceSha256.size())
        : QCryptographicHash::hash(rom, QCryptographicHash::Sha256);
    const QByteArray hex = digest.toHex();
    for (const auto& dump : knownFirecoreDumps) {
        if (dump.size == rom.size() && hex == dump.sha256) return true;
    }
    return false;
}

// ATF is an explicit hardware selection for an unchanged, plain BIN image.
// BIN detection is deliberately restricted to verified Firecore families and known dumps.
inline bool firecore(const QString& extension, QByteArrayView rom, QByteArrayView sourceSha256 = {}) {
    if (extension.compare(QLatin1String("atf"), Qt::CaseInsensitive) == 0) return true;
    if (extension.compare(QLatin1String("bin"), Qt::CaseInsensitive) != 0 || rom.size() < 512) return false;
    if (rom.sliced(0x100,16) == QByteArrayView("DMC RedKid      ") ||
        rom.sliced(0x100,16) == QByteArrayView("DMC  EMDGAME (C)")) return true;
    if (knownFirecoreDump(rom, sourceSha256)) return true;
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
