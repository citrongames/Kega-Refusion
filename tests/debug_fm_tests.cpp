#include "core/LibretroHost.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonObject>
#include <QLibrary>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool good, const char* why) { if (!good) throw std::runtime_error(why); }
void save(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "fixture write");
}
void longAt(QByteArray& b, int at, quint32 v) {
    for (int i = 0; i < 4; ++i) b[at + i] = char(v >> (24 - 8 * i));
}

// Original 68000 program: silence PSG and key on one YM2612 FM channel.
QByteArray mdFmRom(unsigned channel) {
    QByteArray b(65536, char(0));
    longAt(b, 0, 0xfffff0); longAt(b, 4, 0x200);
    b.replace(0x100, 16, "SEGA GENESIS    ");
    b.replace(0x120, 16, "DEBUG FM CHANNEL");
    longAt(b, 0x1a4, b.size() - 1); longAt(b, 0x1a8, 0xff0000);
    longAt(b, 0x1ac, 0xffffff); b[0x1f0] = 'U';
    int pc = 0x200;
    const auto word = [&](quint16 v) { b[pc++] = char(v >> 8); b[pc++] = char(v); };
    const auto address = [&](quint32 a) { word(a >> 16); word(a); };
    const auto byteWrite = [&](unsigned v, quint32 a) {
        word(0x13fc); word(v); address(a);
    };
    const auto wordWrite = [&](unsigned v, quint32 a) {
        word(0x33fc); word(v); address(a);
    };
    word(0x46fc); word(0x2700); // supervisor, interrupt mask
    wordWrite(0x0100, 0xa11100); // request Z80 bus
    wordWrite(0x0100, 0xa11200); // release YM2612 reset while bus is held
    for (unsigned v : {0x9fu, 0xbfu, 0xdfu, 0xffu}) byteWrite(v, 0xc00011);
    const unsigned port = channel < 3 ? 0 : 2;
    const unsigned local = channel % 3;
    const auto fm = [&](unsigned reg, unsigned value) {
        byteWrite(reg, 0xa04000 + port);
        byteWrite(value, 0xa04001 + port);
        word(0x303c); word(180); // MOVE.W #180,D0
        word(0x51c8); word(0xfffe); // DBRA D0,self: respect YM busy time
    };
    fm(0xb0 + local, 7);    // all four operators are carriers
    fm(0xb4 + local, 0xc0); // both stereo outputs
    for (unsigned slot : {0u, 4u, 8u, 12u}) {
        fm(0x30 + slot + local, 1);    // frequency multiplier
        fm(0x40 + slot + local, 0);    // maximum level
        fm(0x50 + slot + local, 0x1f); // immediate attack
        fm(0x60 + slot + local, 0);    // sustain
        fm(0x70 + slot + local, 0);
        fm(0x80 + slot + local, 0x00);
    }
    fm(0xa4 + local, 0x22);
    fm(0xa0 + local, 0x69);
    byteWrite(0x28, 0xa04000); // key-on register is always on port zero
    byteWrite(0xf0 | (channel < 3 ? channel : channel + 1), 0xa04001);
    wordWrite(0x5a5a, 0xff0000); // execution marker after all FM writes
    word(0x60fe); // hold the note
    return b;
}

// Adapted from the project's original SMS FM fixture; parameter is YM2413
// melody channel number. PSG is silent and FM alone is routed to output.
QByteArray smsFmRom(unsigned channel) {
    QByteArray b(32768, char(0)); int pc = 0;
    const auto byte = [&](unsigned v) { b[pc++] = char(v); };
    const auto out = [&](unsigned v, unsigned port) {
        byte(0x3e); byte(v); byte(0xd3); byte(port);
        for (int delay = 0; delay < 24; ++delay) byte(0x00); // OPLL I/O pipeline
    };
    byte(0xf3); byte(0x31); byte(0xf0); byte(0xdf);
    for (unsigned v : {0x9fu, 0xbfu, 0xdfu, 0xffu}) out(v, 0x7f);
    out(1, 0xf2);
    out(0x30 + channel, 0xf0); out(0x10, 0xf1);
    out(0x10 + channel, 0xf0); out(0x80, 0xf1);
    out(0x20 + channel, 0xf0); out(0x15, 0xf1);
    byte(0x18); byte(0xfe);
    b.replace(0x7ff0, 8, "TMR SEGA"); b[0x7fff] = char(0x4c);
    return b;
}

double energy(LibretroHost& host, int count = 5) {
    double total = 0; size_t samples = 0;
    for (int frame = 0; frame < count; ++frame) {
        host.runFrame();
        const auto& pcm = host.audio();
        require(!pcm.isEmpty() && pcm.size() % 2 == 0, "PCM stream missing");
        if (frame == 0) continue; // allow held output and blip to settle
        const auto* data = reinterpret_cast<const qint16*>(pcm.constData());
        for (qsizetype i = 0; i < pcm.size() / 2; ++i) {
            total += double(data[i]) * data[i]; ++samples;
        }
    }
    return samples ? std::sqrt(total / samples) : 0;
}

void backend(LibretroHost& host, bool sms, bool nuked, QString& error) {
    if (!nuked) return;
    auto config = host.movieConfiguration();
    auto options = config.value(QStringLiteral("options")).toObject();
    options.insert(sms ? QStringLiteral("genesis_plus_gx_ym2413_core")
                       : QStringLiteral("genesis_plus_gx_ym2612"),
                   sms ? QStringLiteral("nuked") : QStringLiteral("nuked (ym2612)"));
    config.insert(QStringLiteral("options"), options);
    require(host.prepareMovie(config, error), qPrintable(error));
}

using Memory = void* (*)(unsigned);
void checkChannel(LibretroHost& host, unsigned channel, unsigned bit, Memory memory) {
    const double normal = energy(host, 10);
    if (bit < 10) {
        const auto* ram = static_cast<const unsigned char*>(memory(RETRO_MEMORY_SYSTEM_RAM));
        require(ram && ram[0] == 0x5a && ram[1] == 0x5a,
                "MD fixture did not complete YM2612 setup");
    }
    host.setDebugMasks(0, quint64(1) << bit);
    const double muted = energy(host, 7);
    host.setDebugMasks(0, quint64(1) << ((bit == 4 || bit == 10) ? bit + 1 : bit - 1));
    const double other = energy(host, 7);
    host.setDebugMasks(0, 0);
    const double resumed = energy(host, 7);
    std::printf("bit=%u ch=%u normal=%.1f muted=%.1f other=%.1f resumed=%.1f\n",
                bit, channel, normal, muted, other, resumed);
    require(normal > 20, "FM fixture produced no signal");
    require(muted < normal * 0.18 + 12, "own FM mask did not silence channel");
    require(other > normal * 0.45, "other FM mask silenced active channel");
    require(resumed > normal * 0.45, "FM signal did not resume");
    if (bit == 4 || bit == 10) {
        host.setDebugAudioGain(bit, 50);
        const double half = energy(host, 7);
        host.setDebugAudioGain(bit, 100);
        const double full = energy(host, 7);
        std::printf("gain bit=%u half=%.1f full=%.1f\n", bit, half, full);
        require(half > normal * 0.30 && half < normal * 0.70, "50% FM gain did not scale output");
        require(full > normal * 0.45, "100% FM gain did not restore output");
    }
}

void run(const QString& core, bool nuked) {
    QTemporaryDir dir; require(dir.isValid(), "temporary directory");
    QString error;
    QLibrary inspect(core);
    auto memory = reinterpret_cast<Memory>(inspect.resolve("retro_get_memory_data"));
    require(memory != nullptr, "core memory API");
    for (unsigned channel = 0; channel < 6; ++channel) {
        const auto path = dir.filePath(QStringLiteral("fm%1.md").arg(channel));
        save(path, mdFmRom(channel));
        LibretroHost host(core, dir.filePath(QStringLiteral("md-data%1").arg(channel)));
        require(host.load(path, error), qPrintable(error));
        backend(host, false, nuked, error);
        checkChannel(host, channel, 4 + channel, memory);
    }
    for (unsigned channel = 0; channel < 9; ++channel) {
        const auto path = dir.filePath(QStringLiteral("fm%1.sms").arg(channel));
        save(path, smsFmRom(channel));
        LibretroHost host(core, dir.filePath(QStringLiteral("sms-data%1").arg(channel)));
        require(host.load(path, error), qPrintable(error));
        backend(host, true, nuked, error);
        checkChannel(host, channel, 10 + channel, memory);
    }
    std::puts(nuked ? "PASS: Nuked MD/SMS FM channel masks" : "PASS: MAME MD/SMS FM channel masks");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        require(argc >= 2, "core path required");
        run(QString::fromLocal8Bit(argv[1]), argc >= 3 && QByteArray(argv[2]) == "--nuked");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
    }
}
