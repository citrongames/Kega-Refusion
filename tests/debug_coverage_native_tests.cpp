/* Original synthetic ROMs exercise physical bank changes without game data. */
#include "core/DebugCoverageAbi.h"
#include "core/DebugExecutionAbi.h"
#include "core/DebugInspectionAbi.h"
#include "libretro.h"

#include <QCoreApplication>
#include <QFile>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtEndian>

#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
void require(bool success, const char* message) {
    if (!success) throw std::runtime_error(message);
}

QByteArray systemPath;

bool environment(unsigned command, void* value) {
    switch (command) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(value) = systemPath.constData();
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return true;
    default:
        return false;
    }
}

void video(const void*, unsigned, unsigned, size_t) {}
void audio(int16_t, int16_t) {}
size_t audioBatch(const int16_t*, size_t frames) { return frames; }
void poll() {}
int16_t input(unsigned, unsigned, unsigned, unsigned) { return 0; }

struct Observed {
    struct Region {
        kr_cov_source source{};
        std::vector<uint8_t> bytes;
    };
    std::array<Region, KR_COV_MAX_SOURCES> regions;
    uint64_t unknownExec = 0, unknownRead = 0, events = 0;
    unsigned malformed = 0;

    static void mark(void* opaque, const kr_cov_event* event) {
        auto& observed = *static_cast<Observed*>(opaque);
        ++observed.events;
        if (event->kind != KR_COV_EXEC && event->kind != KR_COV_READ) {
            ++observed.malformed;
            return;
        }
        if (!event->source) {
            auto& count = event->kind == KR_COV_EXEC ? observed.unknownExec : observed.unknownRead;
            count += event->size;
            return;
        }
        if (event->source >= observed.regions.size()) {
            ++observed.malformed;
            return;
        }
        auto& region = observed.regions[event->source];
        const auto mask = event->kind == KR_COV_EXEC ? region.source.exec_cpu_mask :
                                                       region.source.read_cpu_mask;
        if (!(mask & event->cpu) || !event->size || event->offset >= region.bytes.size() ||
            event->size > region.bytes.size() - event->offset) {
            ++observed.malformed;
            return;
        }
        for (unsigned i = 0; i < event->size; ++i)
            region.bytes[event->offset + i] |= uint8_t(event->kind);
    }

    const Region& domain(unsigned id) const {
        for (const auto& region : regions)
            if (region.source.id && region.source.inspect_domain == id) return region;
        throw std::runtime_error("Missing physical memory view");
    }
};

using Enumerate = size_t (*)(unsigned, kr_cov_source*, size_t);
using Bind = bool (*)(unsigned, uint32_t, uint32_t, kr_cov_callback, void*);
using Locations = size_t (*)(unsigned,unsigned,const uint32_t*,kr_cov_location*,size_t,unsigned);
using DebugConnect = bool (*)(kr_debug_callback, void*, const unsigned*, size_t);

struct DebugEvents {
    unsigned exec = 0, reads = 0;
    Locations locations = nullptr;
    bool prefetchChecked = false;
    static void record(void* opaque, unsigned cpu, unsigned event, uint32_t address, uint32_t, unsigned) {
        auto& events = *static_cast<DebugEvents*>(opaque);
        if (cpu != KR_DEBUG_M68K) return;
        if (event == KR_DEBUG_EXEC) {
            ++events.exec;
            if (address==0x08010e && events.locations) {
                kr_cov_location mapped{},prefetched{};
                require(events.locations(KR_COV_VERSION,cpu,&address,&mapped,1,0)==1 &&
                        events.locations(KR_COV_VERSION,cpu,&address,&prefetched,1,1)==1 &&
                        mapped.source==1 && mapped.offset==0x18010e &&
                        prefetched.source==1 && prefetched.offset==0x10010e,
                        "Labels distinguish the queued opcode origin from current bank mapping");
                events.prefetchChecked=true;
            }
        }
        if (event == KR_DEBUG_READ) ++events.reads;
    }
};


class NativeCore {
public:
    explicit NativeCore(const QString& path) : library(path) {
        require(temp.isValid() && library.load(), "Open staged core");
        systemPath = QFile::encodeName(temp.path());
        symbol<decltype(&retro_set_environment)>("retro_set_environment")(environment);
        symbol<decltype(&retro_set_video_refresh)>("retro_set_video_refresh")(video);
        symbol<decltype(&retro_set_audio_sample)>("retro_set_audio_sample")(audio);
        symbol<decltype(&retro_set_audio_sample_batch)>("retro_set_audio_sample_batch")(audioBatch);
        symbol<decltype(&retro_set_input_poll)>("retro_set_input_poll")(poll);
        symbol<decltype(&retro_set_input_state)>("retro_set_input_state")(input);
        symbol<decltype(&retro_init)>("retro_init")();
        locations = symbol<Locations>("retro_kegarefusion_debug_locations_v1");
        sources = symbol<Enumerate>("retro_kegarefusion_coverage_sources_v1");
        bind = symbol<Bind>("retro_kegarefusion_coverage_bind_v1");
        debugConnect = symbol<DebugConnect>("retro_kegarefusion_debug_connect_v2");
        run = symbol<decltype(&retro_run)>("retro_run");
    }

    ~NativeCore() {
        unload();
        symbol<decltype(&retro_deinit)>("retro_deinit")();
    }

    void load(const char* name, const QByteArray& image) {
        const auto filename = temp.filePath(QString::fromLatin1(name));
        QFile file(filename);
        require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                file.write(image) == image.size(), "Write original cartridge");
        file.close();
        const auto path = QFile::encodeName(filename);
        const retro_game_info game{path.constData(), nullptr, 0, nullptr};
        loaded = symbol<decltype(&retro_load_game)>("retro_load_game")(&game);
        require(loaded, "Load original cartridge");
    }

    void unload() {
        if (!loaded) return;
        symbol<decltype(&retro_unload_game)>("retro_unload_game")();
        loaded = false;
    }

    void arm(Observed& observation, unsigned cpu) {
        std::array<kr_cov_source, KR_COV_MAX_SOURCES> descriptors{};
        const auto count = sources(KR_COV_VERSION, descriptors.data(), descriptors.size());
        require(count && count < descriptors.size(), "Enumerate active physical allocations");
        for (size_t i = 0; i < count; ++i) {
            const auto& source = descriptors[i];
            require(source.id && source.id < descriptors.size() && source.size &&
                    source.size <= 32 * 1024 * 1024, "Bounded source metadata");
            auto& region = observation.regions[source.id];
            require(!region.source.id, "Stable unique source ID");
            region.source = source;
            region.bytes.resize(source.size);
        }
        require(bind(KR_COV_VERSION, cpu, cpu, Observed::mark, &observation), "Bind native coverage capture");
    }

    Locations locations = nullptr;
    Enumerate sources = nullptr;
    Bind bind = nullptr;
    DebugConnect debugConnect = nullptr;
    void (*run)() = nullptr;

private:
    template<typename T> T symbol(const char* name) {
        const auto ptr = library.resolve(name);
        require(ptr != nullptr, name);
        return reinterpret_cast<T>(ptr);
    }
    QTemporaryDir temp;
    QLibrary library;
    bool loaded = false;
};

QByteArray smsRom() {
    QByteArray rom(0x10000, '\0');
    const char program[] = {
        char(0xf3), 0x31, char(0xf0), char(0xdf),       // DI; LD SP,$DFF0
        0x3e, 1, 0x32, char(0xfe), char(0xff),        // Bank 1 at $4000
        char(0xcd), 0, 0x41,
        0x3e, 3, 0x32, char(0xfe), char(0xff),        // Bank 3 at $4000
        char(0xcd), 0, 0x41,
        0x3e, 1, 0x32, char(0xff), char(0xff),        // Bank 1 also at $8000
        0x3a, 1, char(0x82), 0x76,                    // LD A,($8201); HALT
    };
    const char routine[] = {0x3a, 0, 0x42, char(0xc9)}; // LD A,($4200); RET
    rom.replace(qsizetype(0), sizeof(program), program, sizeof(program));
    rom.replace(0x4100, sizeof(routine), routine, sizeof(routine));
    rom.replace(0xc100, sizeof(routine), routine, sizeof(routine));
    rom[0x4200] = 0x51;
    rom[0xc200] = 0x23;
    rom[0x4201] = 0x62;
    rom.replace(0x7ff0, 8, "TMR SEGA", 8);
    rom[0x7fff] = 0x4e;
    return rom;
}

struct MdProgram {
    QByteArray rom = QByteArray(0x200000, '\0');
    unsigned pc = 0x200;
    void word(uint16_t word) {
        qToBigEndian(word, rom.data() + pc);
        pc += 2;
    }
    void address(uint32_t address) {
        qToBigEndian(address, rom.data() + pc);
        pc += 4;
    }
    void immediateByte(uint8_t value, uint32_t addressValue) {
        word(0x13fc);
        word(value);
        address(addressValue);
    }
    void readByte(unsigned dn, uint32_t addressValue) {
        word(0x1039 | (dn << 9));
        address(addressValue);
    }
};

QByteArray mdRom() {
    MdProgram code;
    qToBigEndian<uint32_t>(0xffff00, code.rom.data());
    qToBigEndian<uint32_t>(0x200, code.rom.data() + 4);
    code.rom.replace(0x100, 16, "SEGA SSF        ", 16);
    qToBigEndian<uint32_t>(code.rom.size() - 1, code.rom.data() + 0x1a4);
    code.rom[0x1f0] = 'U';

    code.immediateByte(2, 0xa130f3);
    code.word(0x4eb9); code.address(0x080100); // JSR bank 2.
    code.immediateByte(2, 0xa130f7);           // Mirror bank 2 into logical $180000.
    code.readByte(2, 0x180201);
    code.word(0x33fc); code.word(0x1234); code.address(0xff0000);
    code.word(0x3a39); code.address(0xff0000);
    code.word(0x3239); code.address(0xc00004); // VDP status is an unmapped physical origin.
    code.word(0x60fe);

    code.pc = 0x100100;
    code.readByte(0, 0x080200);
    code.immediateByte(3, 0xa130f3);           // Self-bank-switch; next NOP was fetched already.
    code.word(0x4e71);
    code.word(0x60fe);                         // The stale bank must not execute this BRA.

    code.pc = 0x18010e;
    code.word(0x60fe);                         // This word is underneath the old prefetched NOP.
    code.readByte(1, 0x080200);
    code.word(0x4e75);                         // Return via the fixed base bank.
    code.rom[0x100200] = 0x14;
    code.rom[0x100201] = 0x35;
    code.rom[0x180200] = 0x26;
    return code.rom;
}

bool marked(const Observed::Region& region, unsigned kind, unsigned start, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        if (!(region.bytes.at(start + i) & kind)) return false;
    return true;
}

void testSms(NativeCore& core) {
    core.load("coverage-original.sms", smsRom());
    const uint32_t addresses[]={0x4100,0x8100};
    kr_cov_location mapped[2]{};
    require(core.locations(KR_COV_VERSION,KR_DEBUG_Z80,addresses,mapped,2,0)==2 &&
            mapped[0].source==1 && mapped[0].offset==0x4100,"Read-only bank resolution works with coverage disabled");
    require(core.locations(KR_COV_VERSION,KR_DEBUG_Z80,addresses,mapped,4097,0)==0,"Reject oversized map request");

    Observed observed;
    core.arm(observed, KR_DEBUG_Z80);
    core.run();
    require(core.locations(KR_COV_VERSION,KR_DEBUG_Z80,addresses,mapped,2,0)==2 &&
            mapped[0].source==1 && mapped[0].offset==0xc100 &&
            mapped[1].source==1 && mapped[1].offset==0x4100,"Resolve switched bank and distinct alias");
    const auto& rom = observed.domain(KR_MEM_ROM);
    require(rom.source.size == 0x10000 && !observed.malformed, "Sega 8-bit cartridge bounds");
    require(marked(rom, KR_COV_EXEC, 0x4100, 4) && marked(rom, KR_COV_EXEC, 0xc100, 4),
            "Z80 consumes identical logical instructions in two physical banks, including operands");
    require(marked(rom, KR_COV_READ, 0x4200, 2) && marked(rom, KR_COV_READ, 0xc200, 1),
            "Z80 data reads follow bank 1, bank 3 and a second window onto bank 1");
    require(!(rom.bytes[0x8201] & KR_COV_READ) && !(rom.bytes[0x4200] & KR_COV_EXEC) &&
            !(rom.bytes[0x4104] & KR_COV_EXEC), "No logical, data, or unfetched operand contamination");
    core.unload();
}

void testMd(NativeCore& core) {
    core.load("coverage-original.md", mdRom());
    Observed observed;
    core.arm(observed, KR_DEBUG_M68K);
    DebugEvents debug;
    debug.locations=core.locations;
    const unsigned masks[] = {KR_DEBUG_EXEC | KR_DEBUG_READ};
    require(core.debugConnect(DebugEvents::record, &debug, masks, 1), "Arm both native observers");
    core.run();
    require(debug.exec && debug.reads && debug.prefetchChecked, "Execution and read callbacks coexist with coverage");
    const auto& rom = observed.domain(KR_MEM_ROM);
    require(rom.source.size == 0x200000 && !observed.malformed, "MD cartridge bounds");
    require(marked(rom, KR_COV_EXEC, 0x100100, 16) && marked(rom, KR_COV_EXEC, 0x180110, 8),
            "68000 executed old-bank operands and NOP, then new-bank instructions");
    require(!(rom.bytes[0x18010e] & KR_COV_EXEC) && !(rom.bytes[0x18010f] & KR_COV_EXEC) &&
            !(rom.bytes[0x100110] & KR_COV_EXEC), "Prefetch is attributed to the bank that actually supplied it");
    require(marked(rom, KR_COV_READ, 0x100200, 2) && marked(rom, KR_COV_READ, 0x180200, 1) &&
            !(rom.bytes[0x180201] & KR_COV_READ), "Physical bank and mirrored read share the correct source");
    require(marked(observed.domain(KR_MEM_RAM), KR_COV_READ, 0, 2) && observed.unknownRead,
            "RAM read is observed and VDP MMIO is counted as unknown");
    const auto whileAttached = observed.events;
    debug = {};
    require(core.debugConnect(nullptr, nullptr, nullptr, 0), "Detach only the execution debugger");
    core.run();
    require(observed.events > whileAttached && !debug.exec && !debug.reads,
            "Debugger detach leaves physical coverage running");

    require(core.debugConnect(DebugEvents::record, &debug, masks, 1), "Reattach execution debugger");
    require(core.bind(KR_COV_VERSION, 0, 0, nullptr, nullptr), "Detach only native coverage");
    const auto whileCovered = observed.events;
    core.run();
    require(observed.events == whileCovered && debug.exec,
            "Coverage detach leaves the execution debugger running");
    core.unload();
    std::array<kr_cov_source, KR_COV_MAX_SOURCES> descriptors{};
    require(core.sources(KR_COV_VERSION, descriptors.data(), descriptors.size()) == 0,
            "Unload clears native source enumeration and capture");
}

void test32xBootWindow(NativeCore& core) {
    MdProgram code;
    qToBigEndian<uint32_t>(0xffff00, code.rom.data());
    qToBigEndian<uint32_t>(0x200, code.rom.data() + 4);
    code.rom.replace(0x100, 16, "SEGA 32X        ", 16);
    qToBigEndian<uint32_t>(code.rom.size() - 1, code.rom.data() + 0x1a4);
    code.rom[0x1f0] = 'U';
    code.word(0x46fc); code.word(0x2700);          // Mask interrupts.
    code.word(0x33fc); code.word(1); code.address(0xa15100); // Adapter on, SH-2 reset.
    code.word(0x60fe);
    core.load("coverage-boot.32x", code.rom);
    core.run();

    // 32X overlays the first 256 bytes, then shadows cartridge page zero.
    const uint32_t addresses[]{0xfe, 0x100, 0x200, 0x880200};
    kr_cov_location locations[std::size(addresses)]{};
    require(core.locations(KR_COV_VERSION, KR_DEBUG_M68K, addresses, locations, std::size(addresses), 0) ==
                std::size(addresses),
            "Read 32X boot-window mapping");
    require(locations[0].source > 1 && locations[0].offset == 0xfe, "32X bootstrap prefix has its own source");
    require(locations[3].source == 1 && locations[3].offset == 0x200, "32X cartridge window resolves to ROM");
    require(locations[1].source == 1 && locations[1].offset == 0x100 &&
            locations[2].source == locations[3].source && locations[2].offset == locations[3].offset,
            "32X boot shadow tail aliases cartridge ROM");
    core.unload();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        require(app.arguments().size() == 2, "Pass the freshly built libretro core path");
        NativeCore core(app.arguments()[1]);
        testSms(core);
        testMd(core);
        test32xBootWindow(core);
        std::puts("PASS: Z80 and 68000 physical code/read banking, operands, mirrors, prefetch and 32X boot overlay");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
