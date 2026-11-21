#include "core/DebugTraceBuffer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
void require(bool ok, const QString& message) {
    if (!ok) throw std::runtime_error(message.toStdString());
}

DebugTraceRecord recordAt(quint64 sequence) {
    DebugTraceRecord record;
    record.sequence = sequence;
    record.frame = 12;
    record.epoch = 2;
    record.instance = 1;
    record.native.mclk = 940;
    record.native.local_ticks = -15;
    record.native.cpu = KR_DEBUG_M68K;
    record.native.kind = KR_TRACE_EXEC;
    record.native.address = 0x246;
    record.native.bytes[0] = 0x4e;
    record.native.bytes[1] = 0x71;
    record.native.valid_bytes = 2;
    record.native.width = 2;
    return record;
}

void waitForFile(DebugTraceBuffer& buffer) {
    QElapsedTimer timer;
    timer.start();
    while (buffer.writing() && timer.elapsed() < 5000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    require(!buffer.writing(), QStringLiteral("Timeout writing the trace"));
}

void testRing() {
    DebugTraceBuffer buffer;
    DebugTraceConfig config;
    QString error;
    config.memoryMiB = 0;
    require(!buffer.start(config, {}, error) && !error.isEmpty(), "Reject an empty ring");
    config.memoryMiB = 65;
    require(!buffer.start(config, {}, error), "Reject an oversized ring");
    config.memoryMiB = 1;
    require(buffer.start(config, {}, error), error);
    const auto capacity = quint64(buffer.capacity());
    require(capacity == 1024 * 1024 / sizeof(DebugTraceRecord), "Ring fits its configured byte limit");
    for (quint64 i = 0; i < capacity + 23; ++i) buffer.append(recordAt(i));
    require(buffer.overwritten() == 23 && buffer.total() == capacity + 23, "Count ring overwrites");
    const auto recent = buffer.latest(std::numeric_limits<unsigned>::max());
    require(recent.size() == 256, "Bound each preview to 256 records");
    for (qsizetype i = 0; i < recent.size(); ++i)
        require(recent[i].sequence == capacity + 23 - recent.size() + i, "Return recent entries in capture order");
    require(buffer.clear(error) && buffer.active() && buffer.total() == 0 &&
        buffer.latest(256).isEmpty(), "Clearing resets ring history without stopping capture");
    buffer.append(recordAt(500));
    require(buffer.latest(1).front().sequence == 500 && buffer.overwritten() == 0, "Capture after clear");
    buffer.stop();
    buffer.append(recordAt(501));
    require(!buffer.active() && !buffer.writing() && buffer.total() == 1, "Ignore callbacks after stop");
}

quint64 checkFile(const QString& path, quint64 first, bool overflow) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    require(file.readLine() == "# Kega Refusion trace v1\n", "TSV version header");
    require(file.readLine() == "# game\tOriginal trace\n", "Escape tabs and line breaks in game title");
    require(file.readLine().endsWith("\twidth\tinstruction_bytes_hex\n"), "TSV columns");
    quint64 count = 0;
    for (;;) {
        const auto line = file.readLine();
        if (line.startsWith('#')) {
            require(line == (overflow ? "# incomplete: writer queue full\n" : "# complete\n"),
                    "Footer reports capture completeness");
            require(file.atEnd(), "Footer follows the complete prefix");
            break;
        }
        const auto fields = line.trimmed().split('\t');
        require(fields.size() == 15, "TSV keeps all record fields");
        bool ok = false;
        const auto sequence = fields[0].toULongLong(&ok);
        require(ok && sequence == first + count, "Writer keeps capture order across batches");
        require(fields[1] == "12" && fields[2] == "2" && fields[3] == "1" &&
            fields[4] == "940" && fields[5] == "-15" &&
            fields[6].toUInt() == KR_DEBUG_M68K && fields[7] == "EXEC" &&
            fields[9] == "246" && fields[13] == "2" && fields[14] == "4E71",
            "Writer keeps timeline identity, signed ticks and fetched bytes");
        ++count;
    }
    return count;
}

void testFiles(const QTemporaryDir& directory) {
    DebugTraceBuffer buffer;
    DebugTraceConfig config;
    config.memoryMiB = 1;
    config.filePath = directory.filePath("live.tsv");
    QString error;
    const auto game = QStringLiteral("Original\ttrace");
    require(buffer.start(config, game, error), error);
    constexpr quint64 records = 1553;
    for (quint64 i = 0; i < records; ++i) buffer.append(recordAt(i));
    require(!buffer.exportRecent(directory.filePath("busy.tsv"), error) && !error.isEmpty(), "One writer at a time");
    QElapsedTimer timer;
    timer.start();
    buffer.stop();
    require(timer.elapsed() < 250 && !buffer.active(), "Stop returns promptly on the producer");
    waitForFile(buffer);
    require(buffer.status().isEmpty(), buffer.status());
    require(checkFile(config.filePath, 0, false) == records, "Stop drains the last partial batch");

    config.filePath.clear();
    require(buffer.start(config, game, error), error);
    for (quint64 i = 10; i < 15; ++i) buffer.append(recordAt(i));
    const auto exportPath = directory.filePath("recent.tsv");
    require(buffer.exportRecent(exportPath, error), error);
    buffer.append(recordAt(15));
    waitForFile(buffer);
    require(buffer.status().isEmpty(), buffer.status());
    require(checkFile(exportPath, 10, false) == 5 && buffer.total() == 6 && buffer.active(),
            "Export copies the recent ring while capture continues");
    buffer.stop();

    config.filePath = exportPath;
    require(buffer.start(config, game, error), error);
    waitForFile(buffer);
    require(!buffer.active() && !buffer.status().isEmpty(), "File failure stops live capture and reports the error");
    require(checkFile(exportPath, 10, false) == 5, "File failure preserves an existing capture");
}

void testOverflow(const QTemporaryDir& directory) {
    DebugTraceBuffer buffer;
    DebugTraceConfig config;
    config.memoryMiB = 1;
    config.filePath = directory.filePath("overflow.tsv");
    QString error;
    require(buffer.start(config, QStringLiteral("Original\ttrace"), error), error);
    for (quint64 i = 0; i < 260000 && buffer.active(); ++i) buffer.append(recordAt(i));
    require(!buffer.active() && !buffer.status().isEmpty(),
            "A producer burst fills the bounded queue and stops capture");
    waitForFile(buffer);
    const quint64 written = checkFile(config.filePath, 0, true);
    require(written > 0 && written + 1 == buffer.total(),
            "Backpressure writes every queued record and retains the overflow trigger in the ring");
    require(buffer.latest(1).front().sequence == written, "Most recent record survives overflow");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "Create the temporary trace directory");
        testRing();
        testFiles(directory);
        testOverflow(directory);
        std::puts("PASS: bounded ring, async capture/export, file failure, and overflow prefix");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
