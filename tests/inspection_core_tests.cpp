#include "core/DebugInspectionAbi.h"
#include "core/LibretroHost.h"
#include "core/SegaCdBios.h"

#include <QCoreApplication>
#include <QFile>
#include <QLibrary>
#include <QSet>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const QString& detail) {
    if (!condition) throw std::runtime_error(detail.toStdString());
}

void writeFixture(const QString& path, const QByteArray& contents) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
            file.write(contents) == contents.size(), QStringLiteral("Write fixture %1").arg(path));
}

enum class Cartridge { MegaDrive, MasterSystem, GameGear, Sg1000, Firecore, FirecoreBitmap, X32 };

// Original 68000 code writes asymmetric data to work RAM, VDP memories and 32X video.
QByteArray makeMdRom(Cartridge kind) {
    QByteArray rom(512 * 1024, char(0xff));
    qToBigEndian<quint32>(0x00fffff0, rom.data());
    qToBigEndian<quint32>(0x00000200, rom.data() + 4);
    rom.replace(0x100, 16, kind == Cartridge::X32 ? "SEGA 32X        " : "SEGA GENESIS    ");
    rom.replace(0x120, 16, "INSPECT TEST ROM");
    rom.replace(0x150, 16, "INSPECT TEST ROM");
    rom.replace(0x180, 2, "GM");
    qToBigEndian<quint32>(quint32(rom.size() - 1), rom.data() + 0x1a4);
    qToBigEndian<quint32>(0x00ff0000, rom.data() + 0x1a8);
    qToBigEndian<quint32>(0x00ffffff, rom.data() + 0x1ac);
    rom[0x1f0] = 'U';
    rom[rom.size() - 1] = char(0x6d);

    int pc = 0x200;
    const auto word = [&](quint16 value) {
        qToBigEndian(value, rom.data() + pc);
        pc += 2;
    };
    const auto longWord = [&](quint32 value) {
        word(quint16(value >> 16));
        word(quint16(value));
    };
    const auto writeWord = [&](quint16 value, quint32 address) {
        word(0x33fc); word(value); longWord(address); // MOVE.W #value,(address).L
    };
    const auto writeLong = [&](quint32 value, quint32 address) {
        word(0x23fc); longWord(value); longWord(address); // MOVE.L #value,(address).L
    };
    word(0x46fc); word(0x2700); // Mask interrupts while setting up the test.
    if (kind == Cartridge::X32) {
        writeWord(1, 0xa15100); // Enable 32X and keep the SH-2s in reset.
        writeWord(0, 0xa15180);
        writeWord(0x83e0, 0xa15202);
        writeWord(0x293c, 0x840000);
    }
    writeLong(0x1234abcd, 0xff0000);
    writeWord(0x8004, 0xc00004);
    writeWord(0x8104, 0xc00004);
    writeWord(0x8f02, 0xc00004);
    writeLong(0x40000000, 0xc00004); // VRAM write at $0000.
    writeLong(0x2468ace1, 0xc00000);
    writeLong(0xc0000000, 0xc00004); // CRAM write at $0000.
    writeWord(0x0eee, 0xc00000);
    if (kind == Cartridge::Firecore || kind == Cartridge::FirecoreBitmap)
        writeWord(kind == Cartridge::FirecoreBitmap ? 0x98c0 : 0x9880, 0xc00004);
    writeWord(0x8144, 0xc00004);
    for (quint16 data : {0x8d, 0x12, 0x93}) {
        word(0x13fc); word(data); longWord(0xc00011); // MOVE.B #data,(PSG).L
    }
    word(0x60fe); // BRA.S *
    return rom;
}

// Original Z80 code writes the same four VRAM bytes through the port.
QByteArray make8BitRom(Cartridge kind) {
    const bool gg = kind == Cartridge::GameGear;
    const bool sg = kind == Cartridge::Sg1000;
    QByteArray rom(32 * 1024, char(0));
    int pc = 0;
    const auto emitBytes = [&](std::initializer_list<unsigned> bytes) {
        for (unsigned value : bytes) rom[pc++] = char(value);
    };
    const auto out = [&](unsigned port, unsigned value) {
        emitBytes({0x3e, value, 0xd3, port}); // LD A,value; OUT (port),A
    };
    emitBytes({0xf3, 0x31, 0xf0, 0xc3, 0x3e, 0x5a, 0x32, 0, 0xc0}); // DI; SP; ($C000)=$5A
    out(0xbf, sg ? 0 : 4); out(0xbf, 0x80);
    out(0xbf, sg ? 0xc0 : 0x40); out(0xbf, 0x81);
    out(0xbf, 0); out(0xbf, 0x40);
    for (unsigned value : {0x24, 0x68, 0xac, 0xe1}) out(0xbe, value);
    if (!sg) {
        out(0xbf, 0); out(0xbf, 0xc0);
        out(0xbe, gg ? 0x4a : 0x2a);
        if (gg) out(0xbe, 0x0b);
    }
    for (unsigned data : {0x8d, 0x12, 0x93}) out(0x7f, data);
    emitBytes({0x18, 0xfe}); // JR *
    if (!sg) {
        rom.replace(0x7ff0, 8, "TMR SEGA");
        rom[0x7fff] = char(gg ? 0x6c : 0x4c);
    }
    return rom;
}

struct Inspection {
    using Info = bool (*)(unsigned, unsigned, kr_inspect_info*, size_t);
    using Read = size_t (*)(unsigned, uint32_t, void*, size_t);
    Info info;
    Read read;

    explicit Inspection(QLibrary& library)
        : info(reinterpret_cast<Info>(library.resolve("retro_kegarefusion_inspect_info"))),
          read(reinterpret_cast<Read>(library.resolve("retro_kegarefusion_inspect_read"))) {
        require(info && read, "Core inspection exports");
    }

    kr_inspect_info catalog(unsigned parts) const {
        kr_inspect_info result{};
        require(info(KR_INSPECT_VERSION, parts, &result, sizeof(result)), "Inspection catalog");
        require(result.version == KR_INSPECT_VERSION && result.struct_size == sizeof(result) &&
                result.domain_count <= KR_INSPECT_MAX_DOMAINS && result.value_count <= KR_INSPECT_MAX_VALUES &&
                result.system[0] && std::memchr(result.system, '\0', sizeof(result.system)), "Catalog header");
        return result;
    }

    QByteArray bytes(unsigned id, quint32 offset, qsizetype count) const {
        QByteArray result(count, Qt::Uninitialized);
        require(read(id, offset, result.data(), size_t(count)) == size_t(count),
                QStringLiteral("Read %1 bytes from domain %2 at %3").arg(count).arg(id).arg(offset));
        return result;
    }
};

constexpr unsigned allParts = KR_INSPECT_MEMORY | KR_INSPECT_VIDEO | KR_INSPECT_AUDIO | KR_INSPECT_HARDWARE;

const kr_inspect_domain* findDomain(const kr_inspect_info& info, unsigned id) {
    const auto* end = info.domains + info.domain_count;
    const auto* found = std::find_if(info.domains, end, [id](const auto& value) { return value.id == id; });
    return found == end ? nullptr : found;
}

kr_inspect_info checkCatalog(const Inspection& api) {
    kr_inspect_info invalid{};
    invalid.version = 0xfeed;
    require(!api.info(KR_INSPECT_VERSION + 1, allParts, &invalid, sizeof(invalid)) &&
            !api.info(KR_INSPECT_VERSION, allParts, &invalid, sizeof(invalid) - 1) &&
            !api.info(KR_INSPECT_VERSION, allParts, nullptr, sizeof(invalid)) &&
            !api.info(KR_INSPECT_VERSION, allParts | 0x100, &invalid, sizeof(invalid)) &&
            invalid.version == 0xfeed, "Reject incompatible metadata requests");

    const auto info = api.catalog(allParts);
    for (quint32 i = 0; i < info.value_count; ++i) {
        const auto& value = info.values[i];
        require((value.part == KR_INSPECT_VIDEO || value.part == KR_INSPECT_AUDIO ||
                 value.part == KR_INSPECT_HARDWARE) && value.digits <= 16 &&
                value.device[0] && std::memchr(value.device, '\0', sizeof(value.device)) &&
                value.name[0] && std::memchr(value.name, '\0', sizeof(value.name)), "Value metadata");
    }
    QSet<unsigned> ids;
    for (quint32 i = 0; i < info.domain_count; ++i) {
        const auto& domain = info.domains[i];
        require(domain.id && domain.size && domain.big_endian <= 1 &&
                domain.name[0] && std::memchr(domain.name, '\0', sizeof(domain.name)) &&
                !ids.contains(domain.id), "Domain metadata");
        ids.insert(domain.id);
        std::array<char, 6> tail{'x', 'x', 'x', 'x', 'x', 'x'};
        require(api.read(domain.id, domain.size - 1, tail.data() + 1, tail.size() - 2) == 1 &&
                tail[0] == 'x' && std::all_of(tail.begin() + 2, tail.end(), [](char byte) { return byte == 'x'; }),
                QStringLiteral("Read at end of %1 must stop at the domain boundary").arg(QString::fromLatin1(domain.name)));
        require(api.read(domain.id, domain.size, tail.data(), tail.size()) == 0 &&
                api.read(domain.id, std::numeric_limits<quint32>::max(), tail.data(), tail.size()) == 0 &&
                api.read(domain.id, 0, nullptr, tail.size()) == 0 &&
                api.read(domain.id, 0, tail.data(), 0) == 0 && tail[0] == 'x',
                "Out-of-range or empty memory reads");
    }
    char sentinel = 'x';
    require(api.read(std::numeric_limits<unsigned>::max(), 0, &sentinel, 1) == 0 && sentinel == 'x',
            "Unknown domain must return no bytes");

    const auto video = api.catalog(KR_INSPECT_VIDEO);
    require(findDomain(video, KR_MEM_VRAM) && !findDomain(video, KR_MEM_RAM), "Video-only domains");
    const auto audio = api.catalog(KR_INSPECT_AUDIO);
    require(audio.domain_count == 0 && audio.value_count, "Audio-only metadata");
    for (quint32 i = 0; i < audio.value_count; ++i)
        require(audio.values[i].part == KR_INSPECT_AUDIO && audio.values[i].device[0] &&
                audio.values[i].name[0], "Audio snapshot excludes unrequested values");
    const auto metadata = api.catalog(0);
    require(metadata.hardware == info.hardware && metadata.domain_count == 0 && metadata.value_count == 0,
            "Metadata-only request");
    return info;
}

void checkCartridge(LibretroHost& host, const Inspection& api, const QString& path, Cartridge kind,
                    unsigned hardware, unsigned videoFormat) {
    const bool md = (hardware & KR_HW_MD) != 0;
    const auto rom = md ? makeMdRom(kind) : make8BitRom(kind);
    writeFixture(path, rom);
    QString error;
    const bool loaded = host.load(path, error);
    require(loaded, error);
    for (int frame = 0; frame < 12; ++frame) host.runFrame();
    const auto state = host.serialize();
    require(!state.isEmpty(), "Serialize before inspection");

    const auto info = checkCatalog(api);
    require(info.hardware == hardware && info.video_format == videoFormat,
            QStringLiteral("Actual hardware and VDP mode (%1/%2)").arg(info.hardware).arg(info.video_format));
    const unsigned ramId = md ? KR_MEM_RAM : KR_MEM_ZRAM;
    const auto* ram = findDomain(info, ramId);
    const auto* vram = findDomain(info, KR_MEM_VRAM);
    require(ram && ram->big_endian == unsigned(md) && ram->address_base == (md ? 0xff0000u : 0xc000u) &&
            vram && vram->big_endian == unsigned(md), "Work RAM and VRAM address/endianness metadata");
    require(findDomain(info, KR_MEM_ZRAM) && bool(findDomain(info, KR_MEM_RAM)) == md &&
            !findDomain(info, KR_MEM_CD_PRG), "Main RAM and expansion domains follow hardware");
    require(api.bytes(KR_MEM_ROM, md ? 0x201 : 1, 7) == rom.mid(md ? 0x201 : 1, 7),
            "ROM inspection returns original byte order at odd addresses");
    require(api.bytes(KR_MEM_VRAM, 1, 3) == QByteArray::fromHex("68ace1"), "Canonical VRAM odd-offset read");

    if (md) {
        require(api.bytes(KR_MEM_RAM, 0, 4) == QByteArray::fromHex("1234abcd") &&
                api.bytes(KR_MEM_RAM, 1, 2) == QByteArray::fromHex("34ab"),
                "Canonical 68000 RAM bytes");
        QByteArray capped(KR_INSPECT_MAX_READ + 1, 'x');
        require(api.read(KR_MEM_ROM, 0, capped.data(), size_t(capped.size())) == KR_INSPECT_MAX_READ &&
                capped.back() == 'x' && api.bytes(KR_MEM_ROM, quint32(rom.size()) - 1, 1) == QByteArray(1, char(0x6d)),
                "Per-read limit and final cartridge byte");
        if (kind == Cartridge::MegaDrive)
            require(api.bytes(KR_MEM_CRAM, 0, 2) == QByteArray::fromHex("01ff"), "Packed MD CRAM");
        if (kind == Cartridge::Firecore || kind == Cartridge::FirecoreBitmap)
            require(info.vdp_regs[24] == (kind == Cartridge::FirecoreBitmap ? 0xc0 : 0x80), "Firecore extended VDP mode");
    } else {
        require(api.bytes(ramId, 0, 1) == QByteArray(1, char(0x5a)), "Canonical Z80 RAM byte");
        if (kind == Cartridge::GameGear)
            require(api.bytes(KR_MEM_CRAM, 0, 2) == QByteArray::fromHex("4a0b"), "Game Gear CRAM byte order");
        if (kind == Cartridge::MasterSystem)
            require(api.bytes(KR_MEM_CRAM, 0, 1) == QByteArray::fromHex("2a"), "Master System CRAM");
        if (kind == Cartridge::Sg1000)
            require(!findDomain(info, KR_MEM_CRAM), "SG-1000 has a fixed palette");
    }

    const auto* valueEnd = info.values + info.value_count;
    const auto psgValue = [&](const char* name) {
        const auto* value = std::find_if(info.values, valueEnd, [name](const kr_inspect_value& item) {
            return item.part == KR_INSPECT_AUDIO && std::strcmp(item.device, "PSG") == 0 &&
                   std::strcmp(item.name, name) == 0;
        });
        require(value != valueEnd, QStringLiteral("Missing PSG %1").arg(QString::fromLatin1(name)));
        return value->value;
    };
    require(psgValue("Tone 1 period") == 0x12d && psgValue("Tone 1 attenuation") == 3,
            "PSG snapshot reports original fixture registers");

    if (kind == Cartridge::X32) {
        require(findDomain(info, KR_MEM_32X_SDRAM) && findDomain(info, KR_MEM_32X_FB0) &&
                findDomain(info, KR_MEM_32X_FB1) && findDomain(info, KR_MEM_32X_CRAM), "32X memory domains");
        require(api.bytes(KR_MEM_32X_CRAM, 2, 2) == QByteArray::fromHex("83e0"), "Canonical 32X palette");
        const auto marker = QByteArray::fromHex("293c");
        require(api.bytes(KR_MEM_32X_FB0, 0, 2) == marker || api.bytes(KR_MEM_32X_FB1, 0, 2) == marker,
                "Canonical 32X framebuffer word");
    } else {
        require(!findDomain(info, KR_MEM_32X_SDRAM), "Non-32X cartridge has no 32X SDRAM");
    }
    DebugInspectionRequest request;
    request.serial = 7;
    request.parts = KR_INSPECT_MEMORY | KR_INSPECT_VIDEO;
    request.domain = KR_MEM_ROM;
    request.offset = 1;
    request.length = std::numeric_limits<quint32>::max();
    const auto snapshot = host.inspect(request);
    require(snapshot.available() && snapshot.serial == request.serial && snapshot.frame == 12 &&
            snapshot.generation == host.inspectionGeneration() && snapshot.parts == request.parts &&
            snapshot.info.hardware == hardware && snapshot.info.value_count <= info.value_count,
            "Host snapshot metadata");
    require(snapshot.memoryDomain == request.domain && snapshot.memoryOffset == request.offset &&
            snapshot.memory.size() == 4096 && snapshot.memory == rom.mid(1, 4096) &&
            snapshot.blocks.value(KR_MEM_VRAM).size() == vram->size &&
            snapshot.blocks.value(KR_MEM_VRAM).left(4) == QByteArray::fromHex("2468ace1"),
            "Host caps memory and captures complete requested graphics domains");
    request.parts = KR_INSPECT_MEMORY;
    request.domain = KR_MEM_CD_WORD;
    const auto stale = host.inspect(request);
    require(stale.available() && stale.memory.isEmpty() && stale.blocks.isEmpty(),
            "Stale domain selection does not read an unrelated domain");
    require(host.serialize() == state, "Inspection changed the serialized machine");
    std::printf("PASS: %s domains, bounded reads, canonical bytes, native-state invariance\n", qPrintable(path));
}

void checkCd(LibretroHost& host, const Inspection& api, const QString& discPath, bool withX32) {
    QString error;
    host.setExpansionDevices(ExpansionMode::Auto, withX32 ? ExpansionMode::Connected : ExpansionMode::Auto);
    const bool loaded = host.load(discPath, error);
    require(loaded && host.isCd(), QStringLiteral("Mount generated disc: %1").arg(error));
    for (int frame = 0; frame < 12; ++frame) host.runFrame();
    const auto state = host.serialize();
    require(!state.isEmpty(), "Serialize CD before inspection");
    const auto info = checkCatalog(api);
    require(info.hardware == (KR_HW_MD | KR_HW_CD | (withX32 ? KR_HW_32X : 0u)) &&
            findDomain(info, KR_MEM_CD_PRG) && findDomain(info, KR_MEM_CD_WORD) &&
            findDomain(info, KR_MEM_CD_PCM) && findDomain(info, KR_MEM_CD_BRAM) &&
            bool(findDomain(info, KR_MEM_32X_SDRAM)) == withX32, "Sega CD expansion domains");
    require(host.serialize() == state, "Inspection changed serialized CD state");
    std::printf("PASS: generated Sega CD%s mount, domains, bounded reads, native-state invariance\n",
                withX32 ? " + 32X" : "");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 2 && args.size() != 3) {
        std::fprintf(stderr, "Usage: inspection_core_tests CORE [BIOS_DIRECTORY]\n");
        return 2;
    }
    try {
        QTemporaryDir temp;
        require(temp.isValid(), "Fixture directory");
        QLibrary library(args[1]);
        const Inspection api(library);
        LibretroHost host(args[1], temp.filePath("data"));
        struct TestCase { const char* name; Cartridge kind; unsigned hardware, video; };
        constexpr TestCase cases[] = {
            {"original.md", Cartridge::MegaDrive, KR_HW_MD, KR_VIDEO_MD},
            {"original.sms", Cartridge::MasterSystem, KR_HW_SMS, KR_VIDEO_SMS},
            {"original.gg", Cartridge::GameGear, KR_HW_GG, KR_VIDEO_SMS},
            {"original.sg", Cartridge::Sg1000, KR_HW_SG, KR_VIDEO_SG},
            {"original.atf", Cartridge::Firecore, KR_HW_MD | KR_HW_FIRECORE, KR_VIDEO_MD},
            {"original-bitmap.atf", Cartridge::FirecoreBitmap, KR_HW_MD | KR_HW_FIRECORE, KR_VIDEO_FIRECORE_BITMAP},
            {"original.32x", Cartridge::X32, KR_HW_MD | KR_HW_32X, KR_VIDEO_MD}
        };
        for (const auto& test : cases)
            checkCartridge(host, api, temp.filePath(QLatin1String(test.name)), test.kind, test.hardware, test.video);
        if (args.size() == 3) {
            QString error;
            SegaCdBios::importDirectory(args[2], temp.filePath("data"), error);
            const bool hasBios = SegaCdBios::validate(temp.filePath("data"), 2, error);
            require(hasBios, error);
            const QString discPath = temp.filePath("original.iso");
            QByteArray disc(150 * 2048, char(0));
            disc.replace(0, 14, "SEGADISCSYSTEM");
            writeFixture(discPath, disc);
            checkCd(host, api, discPath, false);
            checkCd(host, api, discPath, true);
        }
        host.unload();
        kr_inspect_info info{};
        char sentinel = 'x';
        require(!api.info(KR_INSPECT_VERSION, allParts, &info, sizeof(info)) &&
                api.read(KR_MEM_RAM, 0, &sentinel, 1) == 0 && sentinel == 'x', "Unloaded core exposes no stale state");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
