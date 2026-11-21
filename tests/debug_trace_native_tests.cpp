#include "core/DebugExecutionAbi.h"
#include "core/DebugTraceAbi.h"
#include "libretro.h"

#include <QCoreApplication>
#include <QFile>
#include <QLibrary>
#include <QTemporaryDir>

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool success, const char* description) {
    if (!success) throw std::runtime_error(description);
}

using TraceBind = bool (*)(unsigned, uint32_t, uint32_t, kr_trace_callback, void*);
using TraceCapabilities = uint32_t (*)();
using TraceDecode = bool (*)(unsigned, uint32_t, const uint8_t*, size_t, kr_debug_instruction*, size_t);
using DebugConnect = bool (*)(kr_debug_callback, void*, const unsigned*, size_t);
using DebugWriteRegister = bool (*)(unsigned, const char*, uint32_t);

struct TraceLog {
    std::array<kr_trace_record, 64> records{};
    size_t count = 0;
    unsigned instructions = 0;

    void clear() { count = 0; instructions = 0; }

    static void capture(void* user, const kr_trace_record* source) {
        auto& log = *static_cast<TraceLog*>(user);
        // Keep a short instruction prefix and sparse events from the whole frame.
        if (source->kind == KR_TRACE_EXEC && log.instructions++ >= 12) return;
        if (log.count < log.records.size()) log.records[log.count++] = *source;
    }

    const kr_trace_record* first(unsigned kind) const {
        for (size_t i = 0; i < count; ++i)
            if (records[i].kind == kind) return &records[i];
        return nullptr;
    }
};

struct DebugPlan {
    DebugWriteRegister write = nullptr;
    TraceLog* trace = nullptr;
    unsigned visits = 0, idleBeforeUnmask = 0;
    bool redirect = false, lowerMask = false, pcWritten = false, srWritten = false;

    static void execute(void* user, unsigned cpu, unsigned event, uint32_t address,
                        uint32_t, unsigned) {
        auto& plan = *static_cast<DebugPlan*>(user);
        if (cpu != KR_DEBUG_M68K || event != KR_DEBUG_EXEC) return;
        ++plan.visits;
        if (plan.redirect && address == 0x200) {
            plan.redirect = false;
            plan.pcWritten = plan.write(cpu, "PC", 0x220);
        }
        // Leave the reset SR masked through the early VINT edge. Every
        // visit executes one 70-master-tick BRA once tracing continues.
        if (plan.lowerMask && address == 0x270 && ++plan.idleBeforeUnmask == 32) {
            plan.lowerMask = false;
            plan.trace->clear();
            plan.srWritten = plan.write(cpu, "SR", 0x2000);
        }
    }
};

QByteArray systemDirectory;

bool environment(unsigned command, void* value) {
    switch (command) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(value) = systemDirectory.constData();
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        return true;
    default:
        return false;
    }
}

void video(const void*, unsigned, unsigned, size_t) {}
void audio(int16_t, int16_t) {}
size_t batch(const int16_t*, size_t frames) { return frames; }
void poll() {}
int16_t input(unsigned, unsigned, unsigned, unsigned) { return 0; }

class Core {
public:
    explicit Core(const QString& path) : library(path) {
        require(temp.isValid() && library.load(), "Open staged core");
        systemDirectory = QFile::encodeName(temp.path());
        bind = symbol<TraceBind>("retro_kegarefusion_trace_bind_v1");
        caps = symbol<TraceCapabilities>("retro_kegarefusion_trace_capabilities_v1");
        decode = symbol<TraceDecode>("retro_kegarefusion_trace_decode_v1");
        debugConnect = symbol<DebugConnect>("retro_kegarefusion_debug_connect_v2");
        writeRegister = symbol<DebugWriteRegister>("retro_kegarefusion_debug_write_register_v2");
        run = symbol<decltype(&retro_run)>("retro_run");
        symbol<decltype(&retro_set_environment)>("retro_set_environment")(environment);
        symbol<decltype(&retro_set_video_refresh)>("retro_set_video_refresh")(video);
        symbol<decltype(&retro_set_audio_sample)>("retro_set_audio_sample")(audio);
        symbol<decltype(&retro_set_audio_sample_batch)>("retro_set_audio_sample_batch")(batch);
        symbol<decltype(&retro_set_input_poll)>("retro_set_input_poll")(poll);
        symbol<decltype(&retro_set_input_state)>("retro_set_input_state")(input);
        symbol<decltype(&retro_init)>("retro_init")();
    }

    ~Core() {
        if (loaded) unload();
        symbol<decltype(&retro_deinit)>("retro_deinit")();
        library.unload();
    }

    void load(const char* name, const QByteArray& image) {
        const QString path = temp.filePath(QString::fromLatin1(name));
        QFile rom(path);
        require(rom.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                rom.write(image) == image.size(), "Write original test ROM");
        rom.close();
        const QByteArray encoded = QFile::encodeName(path);
        retro_game_info game{encoded.constData(), nullptr, 0, nullptr};
        loaded = symbol<decltype(&retro_load_game)>("retro_load_game")(&game);
        require(loaded, "Load original test ROM");
    }

    void unload() {
        symbol<decltype(&retro_unload_game)>("retro_unload_game")();
        loaded = false;
    }

    TraceBind bind = nullptr;
    TraceCapabilities caps = nullptr;
    TraceDecode decode = nullptr;
    DebugConnect debugConnect = nullptr;
    DebugWriteRegister writeRegister = nullptr;
    void (*run)() = nullptr;

private:
    template<typename Function> Function symbol(const char* name) {
        const auto address = library.resolve(name);
        require(address != nullptr, name);
        return reinterpret_cast<Function>(address);
    }

    QTemporaryDir temp;
    QLibrary library;
    bool loaded = false;
};

void word(QByteArray& rom, unsigned offset, unsigned value) {
    rom[offset] = char(value >> 8);
    rom[offset + 1] = char(value);
}

void longword(QByteArray& rom, unsigned offset, uint32_t value) {
    word(rom, offset, value >> 16);
    word(rom, offset + 2, value);
}

QByteArray mdImage() {
    QByteArray rom(0x40000, '\0');
    longword(rom, 0, 0x00ffff00);
    longword(rom, 4, 0x200);
    longword(rom, 30 * 4, 0x280); // IRQ 6 autovector.
    rom.replace(0x100, 16, "SEGA GENESIS    ", 16);
    rom[0x1f0] = 'U';
    word(rom, 0x200, 0x7007); // PC edit skips MOVEQ #7,D0.
    word(rom, 0x202, 0x60fe);
    unsigned pc = 0x220;
    auto next = [&](unsigned value) { word(rom, pc, value); pc += 2; };
    auto writeVdp = [&](unsigned value) {
        next(0x33fc); next(value); next(0x00c0); next(4);
    };
    writeVdp(0x8134); // Mode 5, DMA and vertical IRQ enabled.
    writeVdp(0x8f02); // Destination increment.
    writeVdp(0x9302); // DMA length.
    writeVdp(0x9780); // VRAM fill, triggered by the next data write.
    next(0x23fc); next(0x4000); next(0x0080); next(0x00c0); next(4);
    next(0x33fc); next(0xa55a); next(0x00c0); next(0);
    while (pc < 0x270) next(0x4e71);
    word(rom, 0x270, 0x60fe);
    word(rom, 0x280, 0x4e73); // RTE.
    return rom;
}

QByteArray smsImage() {
    QByteArray rom(0x8000, '\0');
    const unsigned char code[] = {
        0x3e, 0x36,       // LD A,$36
        0xd3, 0xbf,       // OUT ($BF),A
        0x3e, 0x80,       // LD A,$80
        0xd3, 0xbf,       // Commit VDP register 0.
        0x76,             // HALT refresh is not instruction execution.
    };
    rom.replace(qsizetype(0), sizeof(code), reinterpret_cast<const char*>(code), sizeof(code));
    rom.replace(0x7ff0, 8, "TMR SEGA", 8);
    rom[0x7fff] = char(0x4c);
    return rom;
}

void testMd(Core& core) {
    const auto image = mdImage();
    core.load("trace-original.md", image);
    TraceLog log;
    DebugPlan plan;
    plan.write = core.writeRegister;
    plan.trace = &log;
    plan.redirect = true;
    constexpr unsigned filters = KR_TRACE_INSTRUCTION | KR_TRACE_VIDEO | KR_TRACE_DMA | KR_TRACE_IRQ;
    const unsigned masks[] = {KR_DEBUG_EXEC};
    require((core.caps() & filters) == filters, "MD trace capabilities");
    require(!core.bind(KR_TRACE_VERSION + 1, KR_DEBUG_M68K, filters, TraceLog::capture, &log),
            "Trace rejects an unsupported ABI version");
    require(!core.bind(KR_TRACE_VERSION, KR_DEBUG_SUB68K, filters, TraceLog::capture, &log),
            "Trace rejects unavailable CPU");
    require(core.debugConnect(DebugPlan::execute, &plan, masks, 1), "Attach independent breakpoint hook");
    require(core.bind(KR_TRACE_VERSION, KR_DEBUG_M68K, filters, TraceLog::capture, &log),
            "Attach MD trace and hardware events");
    core.run();
    const auto* first = log.first(KR_TRACE_EXEC);
    require(plan.pcWritten && first && first->address == 0x220 && first->value == 0x33fc,
            "First traced opcode is actual opcode after debug PC redirect");
    require(first->width == 2 && first->valid_bytes >= 8 && (first->flags & KR_TRACE_PEEK_TAIL) &&
            first->bytes[0] == 0x33 && first->bytes[1] == 0xfc &&
            first->bytes[2] == 0x81 && first->bytes[3] == 0x34,
            "Trace keeps actual first opcode and tags bounded peek suffix");
    const kr_trace_record captured = *first;
    const auto* vdp = log.first(KR_TRACE_VDP_WRITE);
    if (!vdp || vdp->cpu != KR_DEBUG_M68K || vdp->address != 1 || vdp->value != 0x34) {
        for (size_t i = 0; i < log.count; ++i) {
            const auto& row = log.records[i];
            std::fprintf(stderr, "trace kind=%u cpu=%u pc/port=%x value=%x aux=%x\n",
                         row.kind, row.cpu, row.address, row.value, row.aux);
        }
    }
    require(vdp && vdp->cpu == KR_DEBUG_M68K && vdp->address == 1 && vdp->value == 0x34,
            "Trace shows accepted VDP port transaction");
    const auto* dmaStart = log.first(KR_TRACE_DMA_START);
    const auto* dmaEnd = log.first(KR_TRACE_DMA_END);
    require(dmaStart && dmaEnd && dmaStart < dmaEnd && (dmaStart->aux & 0xff) == 2 &&
            dmaStart->value == 2 && dmaEnd->mclk >= dmaStart->mclk,
            "Trace shows actual VRAM fill start, length, and completion");

    log.clear();
    plan.lowerMask = true;
    core.run();
    const auto* irq = log.first(KR_TRACE_IRQ_ACCEPT);
    const auto* afterIrq = log.first(KR_TRACE_EXEC);
    require(plan.srWritten && irq && afterIrq && irq < afterIrq && irq->address == 0x270 &&
            irq->value == 30 && afterIrq->address == 0x280 && afterIrq->value == 0x4e73,
            "Unmasking pending IRQ traces exception and first handler opcode before idle opcode");

    const unsigned visits = plan.visits;
    log.clear();
    require(core.bind(KR_TRACE_VERSION, 0, 0, nullptr, nullptr), "Detach trace");
    core.run();
    require(log.count == 0 && plan.visits > visits, "Trace detach leaves execution hook active");
    require(core.bind(KR_TRACE_VERSION, KR_DEBUG_M68K, KR_TRACE_INSTRUCTION, TraceLog::capture, &log),
            "Reattach trace without hardware events");
    require(core.debugConnect(nullptr, nullptr, nullptr, 0), "Detach execution hook");
    const unsigned detachedVisits = plan.visits;
    core.run();
    require(log.instructions && plan.visits == detachedVisits,
            "Execution hook detach leaves trace active");
    core.unload();
    require(core.caps() == 0, "Unload clears trace capabilities");
    kr_debug_instruction decoded{};
    require(core.decode(KR_DEBUG_M68K, captured.address, captured.bytes, captured.valid_bytes,
                        &decoded, sizeof(decoded)) && decoded.valid && decoded.length == 8,
            "Captured opcode bytes decode after unload");
    require(!core.decode(KR_DEBUG_M68K, captured.address, captured.bytes, 1, &decoded, sizeof(decoded)),
            "Cold decoder rejects truncated opcode");
}

void testSms(Core& core) {
    const auto image = smsImage();
    core.load("trace-original.sms", image);
    TraceLog log;
    require(core.bind(KR_TRACE_VERSION, KR_DEBUG_Z80, KR_TRACE_INSTRUCTION | KR_TRACE_VIDEO,
                      TraceLog::capture, &log), "Attach SMS trace");
    core.run();
    const auto* first = log.first(KR_TRACE_EXEC);
    require(first && first->cpu == KR_DEBUG_Z80 && first->address == 0 && first->width == 1 &&
            first->bytes[0] == 0x3e && first->bytes[1] == 0x36 && log.instructions == 5,
            "SMS traces five executed opcodes, including HALT once");
    const auto* vdp = log.first(KR_TRACE_VDP_WRITE);
    require(vdp && vdp->cpu == KR_DEBUG_Z80 && vdp->address == 0 && vdp->value == 0x36,
            "SMS emits committed VDP register write from Z80");
    core.unload();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        require(app.arguments().size() == 2, "Pass path to current libretro core");
        Core core(app.arguments()[1]);
        testMd(core);
        testSms(core);
        std::puts("PASS: native opcode, IRQ, VDP, DMA, detach, cold decoding and HALT trace");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
