#include "core/LibretroHost.h"
#include "core/Sega32xBios.h"
#include "core/SegaCdBios.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QLibrary>
#include <QTemporaryDir>

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "cannot create generated ROM");
    require(file.write(bytes) == bytes.size(), "cannot write generated ROM");
}

// Original, synthetic disc header: sufficient for the real core to mount,
// without any copied security or game code.
QByteArray makeDisc() {
    QByteArray bytes(150 * 2048, char(0));
    bytes.replace(0, 14, "SEGADISCSYSTEM");
    bytes[0x20b] = char(0); // NTSC-U
    return bytes;
}

// Minimal original 68000 cartridge, attached to real 32X hardware by option.
QByteArray makeCartridge() {
    QByteArray bytes(65536, char(0xff));
    bytes[0] = char(0x00); bytes[1] = char(0xff);
    bytes[2] = char(0xf0); bytes[3] = char(0x00);
    bytes[4] = char(0x00); bytes[5] = char(0x00);
    bytes[6] = char(0x02); bytes[7] = char(0x00);
    bytes.replace(0x100, 16, "SEGA GENESIS    ");
    bytes.replace(0x120, 16, "KR BIOS PROBE   ");
    bytes.replace(0x150, 16, "KR BIOS PROBE   ");
    bytes[0x1f0] = 'U';
    bytes[0x200] = char(0x60); bytes[0x201] = char(0xfe); // BRA.S *
    return bytes;
}

struct BiosFile {
    const char* canonical;
    const char* renamed;
    const char* alternateSource;
};

constexpr std::array<BiosFile, 4> biosFiles{{
    {"bios_CD_U.bin", "renamed_sega_cd_us.rom", "us_scd1_9210.bin"},
    {"32X_G_BIOS.BIN", "renamed_32x_m68k.rom", nullptr},
    {"32X_M_BIOS.BIN", "renamed_32x_master.rom", nullptr},
    {"32X_S_BIOS.BIN", "renamed_32x_slave.rom", nullptr},
}};

QString sourceFile(const QDir& directory, const BiosFile& file) {
    const QString canonical = directory.filePath(QString::fromLatin1(file.canonical));
    if (QFileInfo(canonical).isFile()) return canonical;
    if (file.alternateSource) {
        const QString alternate = directory.filePath(QString::fromLatin1(file.alternateSource));
        if (QFileInfo(alternate).isFile()) return alternate;
    }
    return {};
}

bool hasBiosSet(const QString& path) {
    const QDir directory(path);
    if (!directory.exists()) return false;
    for (const auto& file : biosFiles)
        if (sourceFile(directory, file).isEmpty()) return false;
    return true;
}

QString findBiosSource(int argc, char** argv) {
    if (argc == 3) return QString::fromLocal8Bit(argv[2]);
    const QStringList localSources{
        QStringLiteral("/mnt/d/emulation/Sega Mega Drive/emu/KegaRefusion/testbuild/data/bios"),
        QStringLiteral("D:/emulation/Sega Mega Drive/emu/KegaRefusion/testbuild/data/bios"),
        QStringLiteral("/mnt/d/emulation/Sega Mega Drive/emu/Fusion364/Bios"),
        QStringLiteral("D:/emulation/Sega Mega Drive/emu/Fusion364/Bios")
    };
    for (const auto& candidate : localSources)
        if (hasBiosSet(candidate)) return candidate;
    throw std::runtime_error("local original BIOS set unavailable; pass BIOS_DIRECTORY");
}

void copyBios(const QString& sourcePath, const QString& biosPath) {
    require(QDir().mkpath(biosPath), "cannot create isolated BIOS directory");
    const QDir source(sourcePath), destination(biosPath);
    for (const auto& file : biosFiles) {
        const QString original = sourceFile(source, file);
        require(!original.isEmpty(), "source lacks an original CD or 32X BIOS");
        require(QFile::copy(original, destination.filePath(QString::fromLatin1(file.canonical))),
                "cannot copy original BIOS into isolated temporary directory");
    }
    QString error;
    require(SegaCdBios::validate(QFileInfo(biosPath).absolutePath(), 2, error),
            "copied NTSC-U CD BIOS was not recognized");
    require(Sega32xBios::validate(QFileInfo(biosPath).absolutePath(), error),
            "copied 32X BIOS set was not recognized");
}

void renameBios(const QString& biosPath) {
    const QDir directory(biosPath);
    for (const auto& file : biosFiles)
        require(QFile::rename(directory.filePath(QString::fromLatin1(file.canonical)),
                              directory.filePath(QString::fromLatin1(file.renamed))),
                "cannot rename isolated BIOS image");
}

void requireOnlyRenamedBios(const QString& biosPath) {
    const QDir directory(biosPath);
    for (const auto& file : biosFiles) {
        require(!QFileInfo::exists(directory.filePath(QString::fromLatin1(file.canonical))),
                "core created a canonical BIOS copy");
        require(QFileInfo(directory.filePath(QString::fromLatin1(file.renamed))).isFile(),
                "renamed BIOS image disappeared");
    }
    require(directory.entryList(QDir::Files | QDir::NoDotAndDotDot).size() == int(biosFiles.size()),
            "core added an unexpected BIOS file");
}

void requireFrame(LibretroHost& host) {
    host.runFrame();
    require(!host.frame().isNull(), "real core did not produce a video frame");
    require(host.audio().size() % 4 == 0, "real core produced incomplete stereo PCM");
}

struct Identity {
    QByteArray game;
    QByteArray core;
    QJsonObject bios;
};

Identity snapshot(const LibretroHost& host) {
    const Identity identity{host.movieGameIdentity(), host.movieCoreIdentity(),
                            host.movieConfiguration().value(QStringLiteral("bios")).toObject()};
    require(identity.game.size() == 32 && identity.core.size() == 32,
            "loaded core has no stable game/core identity");
    return identity;
}

void sameIdentity(const Identity& actual, const Identity& expected) {
    require(actual.game == expected.game, "renaming BIOS changed the game identity");
    require(actual.core == expected.core, "renaming BIOS changed the core identity");
    require(actual.bios == expected.bios, "renaming BIOS changed recording BIOS fingerprints");
}

Identity exerciseCd(const QString& corePath, const QString& dataPath, const QString& discPath, bool restoreExisting) {
    LibretroHost host(corePath, dataPath);
    QString error;
    if (!host.load(discPath, error)) throw std::runtime_error((QStringLiteral("CD load: ") + error).toStdString());
    require(host.isCd(), "generated disc did not select Sega CD hardware");
    requireFrame(host);
    const Identity beforeReset = snapshot(host);
    require(beforeReset.bios.value(QStringLiteral("bios_CD_U.bin")).toString().size() == 64,
            "active CD BIOS was not fingerprinted");
    if (!host.hardReset(error)) throw std::runtime_error((QStringLiteral("CD hard reset: ") + error).toStdString());
    require(host.isCd(), "CD hardware disappeared on hard reset");
    requireFrame(host);
    sameIdentity(snapshot(host), beforeReset);
    // A quick state includes the CD BIOS-dependent romHash, not movieGameIdentity.
    if (restoreExisting) {
        if (!host.loadState(0, error))
            throw std::runtime_error((QStringLiteral("CD state after BIOS rename: ") + error).toStdString());
    } else if (!host.saveState(0, error)) {
        throw std::runtime_error((QStringLiteral("CD baseline state: ") + error).toStdString());
    }
    return beforeReset;
}

Identity exercise32x(const QString& corePath, const QString& dataPath, const QString& cartridgePath, bool restoreExisting) {
    LibretroHost host(corePath, dataPath);
    host.setExpansionDevices(false, true);
    QString error;
    if (!host.load(cartridgePath, error))
        throw std::runtime_error((QStringLiteral("32X cartridge load: ") + error).toStdString());
    QLibrary core(corePath);
    using GetFeatures = unsigned (*)();
    const auto features = reinterpret_cast<GetFeatures>(core.resolve("retro_kegarefusion_core_features"));
    require(features && (features() & 2u), "real 32X hardware was not active");
    requireFrame(host);
    const Identity beforeReset = snapshot(host);
    for (const auto& file : biosFiles)
        if (QString::fromLatin1(file.canonical).startsWith(QStringLiteral("32X_")))
            require(beforeReset.bios.value(QString::fromLatin1(file.canonical)).toString().size() == 64,
                    "active 32X BIOS piece was not fingerprinted");
    if (!host.hardReset(error)) throw std::runtime_error((QStringLiteral("32X hard reset: ") + error).toStdString());
    require(features() & 2u, "32X hardware disappeared on hard reset");
    requireFrame(host);
    sameIdentity(snapshot(host), beforeReset);
    if (restoreExisting) {
        if (!host.loadState(0, error))
            throw std::runtime_error((QStringLiteral("32X state after BIOS rename: ") + error).toStdString());
    } else if (!host.saveState(0, error)) {
        throw std::runtime_error((QStringLiteral("32X baseline state: ") + error).toStdString());
    }
    return beforeReset;
}

void verifyMixedBiosGuards(const QString& corePath, const QString& dataPath,
                           const QString& cartridgePath, const QString& biosPath,
                           const QString& biosSource) {
    QJsonObject originalConfig;
    QByteArray networkIdentity;
    {
        LibretroHost original(corePath, dataPath);
        original.setExpansionDevices(false, true);
        QString error;
        require(original.load(cartridgePath, error), "cannot load 32X with original BIOS");
        originalConfig = original.movieConfiguration();
        networkIdentity = original.networkCompatibility();
        require(networkIdentity.size() == 32, "real core lacks network compatibility");
    }
    require(QFile::remove(QDir(biosPath).filePath(QStringLiteral("renamed_32x_master.rom"))),
            "cannot isolate a missing Master SH2 BIOS");
    LibretroHost mixed(corePath, dataPath);
    mixed.setExpansionDevices(false, true);
    QString error;
    require(mixed.load(cartridgePath, error), "mixed original/built-in 32X BIOS failed to load");
    const auto mixedConfig = mixed.movieConfiguration();
    const auto bios = mixedConfig.value(QStringLiteral("bios")).toObject();
    require(bios.value(QStringLiteral("32X_G_BIOS.BIN")).toString().size() == 64,
            "original 68000 BIOS fingerprint disappeared");
    require(bios.value(QStringLiteral("32X_M_BIOS.BIN")).toString() ==
                QStringLiteral("builtin-32x-m-v1"),
            "built-in Master SH2 BIOS lacks a role-specific identity");
    require(bios.value(QStringLiteral("32X_S_BIOS.BIN")).toString().size() == 64,
            "original Slave SH2 BIOS fingerprint disappeared");
    require(!mixed.prepareMovie(originalConfig, error) && error.contains(QStringLiteral("BIOS")),
            "recording accepted different 32X BIOS");
    auto originalNetwork = originalConfig;
    originalNetwork.insert(QStringLiteral("networkCompatibility"),
                           QString::fromLatin1(networkIdentity.toHex()));
    error.clear();
    require(!mixed.prepareNetwork(originalNetwork, error) && error.contains(QStringLiteral("BIOS")),
            "netplay accepted different 32X BIOS");
    auto oldMissing = mixedConfig;
    auto oldBios = bios;
    oldBios.insert(QStringLiteral("32X_M_BIOS.BIN"), QStringLiteral("missing"));
    oldMissing.insert(QStringLiteral("bios"), oldBios);
    error.clear();
    require(!mixed.prepareMovie(oldMissing, error) && error.contains(QStringLiteral("BIOS")),
            "recording treated legacy missing as built-in 32X BIOS");
    oldMissing.insert(QStringLiteral("networkCompatibility"),
                      QString::fromLatin1(networkIdentity.toHex()));
    error.clear();
    require(!mixed.prepareNetwork(oldMissing, error) && error.contains(QStringLiteral("BIOS")),
            "netplay treated legacy missing as built-in 32X BIOS");
    error.clear();
    require(mixed.prepareMovie(mixedConfig, error),
            "matching mixed 32X BIOS configuration was rejected");
    require(mixed.restoreMovieConfiguration(error), "cannot end matching recording setup");

    // An import changes the next power cycle, not the BIOS executing now.
    const auto originalMaster = sourceFile(QDir(biosSource), biosFiles[2]);
    require(!originalMaster.isEmpty(), "original Master SH2 BIOS source disappeared");
    const auto installedMaster = QDir(biosPath).filePath(QStringLiteral("32X_M_BIOS.BIN"));
    require(QFile::copy(originalMaster, installedMaster), "cannot import Master SH2 BIOS");
    require(mixed.movieConfiguration().value(QStringLiteral("bios")).toObject() == bios,
            "import changed the running built-in BIOS identity");
    error.clear();
    require(!mixed.prepareMovie(mixedConfig, error) && error.contains(QStringLiteral("BIOS")),
            "recording ignored the imported BIOS on its next power cycle");
    auto mixedNetwork = mixedConfig;
    mixedNetwork.insert(QStringLiteral("networkCompatibility"),
                        QString::fromLatin1(networkIdentity.toHex()));
    error.clear();
    require(!mixed.prepareNetwork(mixedNetwork, error) && error.contains(QStringLiteral("BIOS")),
            "netplay ignored the imported BIOS on its next power cycle");

    error.clear();
    require(mixed.hardReset(error), "cannot power cycle into imported Master SH2 BIOS");
    const auto originalMasterConfig = mixed.movieConfiguration();
    require(originalMasterConfig.value(QStringLiteral("bios")).toObject()
                .value(QStringLiteral("32X_M_BIOS.BIN")).toString().size() == 64,
            "power cycle did not select imported Master SH2 BIOS");
    require(QFile::remove(installedMaster), "cannot remove imported Master SH2 BIOS");
    require(mixed.movieConfiguration().value(QStringLiteral("bios")).toObject()
                .value(QStringLiteral("32X_M_BIOS.BIN")).toString().size() == 64,
            "removal changed the running original BIOS identity");
    error.clear();
    require(!mixed.prepareMovie(originalMasterConfig, error) && error.contains(QStringLiteral("BIOS")),
            "recording ignored the removed BIOS on its next power cycle");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: bios_host_tests CORE [BIOS_DIRECTORY]\n");
        return 2;
    }
    try {
        QTemporaryDir temp;
        require(temp.isValid(), "cannot create isolated BIOS test directory");
        const QString corePath = QString::fromLocal8Bit(argv[1]);
        const QString source = findBiosSource(argc, argv);
        const QString dataPath = temp.filePath(QStringLiteral("data"));
        const QString biosPath = QDir(dataPath).filePath(QStringLiteral("bios"));
        const QString discPath = temp.filePath(QStringLiteral("original_cd.iso"));
        const QString cartridgePath = temp.filePath(QStringLiteral("original_cart.md"));
        writeFile(discPath, makeDisc());
        writeFile(cartridgePath, makeCartridge());
        copyBios(source, biosPath);
        const Identity canonicalCd = exerciseCd(corePath, dataPath, discPath, false);
        const Identity canonical32x = exercise32x(corePath, dataPath, cartridgePath, false);

        renameBios(biosPath);
        requireOnlyRenamedBios(biosPath);
        sameIdentity(exerciseCd(corePath, dataPath, discPath, true), canonicalCd);
        requireOnlyRenamedBios(biosPath);
        sameIdentity(exercise32x(corePath, dataPath, cartridgePath, true), canonical32x);
        requireOnlyRenamedBios(biosPath);
        verifyMixedBiosGuards(corePath, dataPath, cartridgePath, biosPath, source);
        std::puts("Real-core CD and 32X BIOS loads, hard resets, quick-state compatibility, identities and no-copy rename passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}


