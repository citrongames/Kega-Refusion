#include "core/DebugCoverageMap.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
void require(bool ok, const QString& message) {
    if (!ok) throw std::runtime_error(message.toStdString());
}

kr_cov_source source(quint32 id, quint32 size, const char* name) {
    kr_cov_source result{};
    result.id = id;
    result.size = size;
    result.exec_cpu_mask = result.read_cpu_mask = KR_DEBUG_M68K;
    result.flags = KR_COV_SOURCE_ROM;
    std::strncpy(result.name, name, sizeof(result.name) - 1);
    return result;
}

void waitForExport(const DebugCoverageMap& map) {
    QElapsedTimer timer;
    timer.start();
    while (map.writing() && timer.elapsed() < 5000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    require(!map.writing(), "Coverage export timed out");
}

QByteArray readFile(QString path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    return file.readAll();
}

void checkCoverageFile(const QByteArray& contents, const QByteArray& hash) {
    require(contents.size() >= 12 && contents.startsWith("KRCDL1\r\n"), "Portable KCDL file header");
    const auto headerBytes = qFromLittleEndian<quint32>(contents.constData() + 8);
    require(headerBytes <= quint64(contents.size() - 12), "Bounded JSON header");
    const auto json = QJsonDocument::fromJson(contents.mid(12, headerBytes)).object();
    require(json.value("version").toInt() == KR_COV_VERSION && json.value("game") == "Original coverage" &&
            json.value("romSha256") == QString::fromLatin1(hash.toHex()) &&
            json.value("unknownCode").toInt() == 4 && json.value("unknownData").toInt() == 4,
            "Export captures game, hash and unknown access counts");
    const auto descriptors = json.value("sources").toArray();
    require(descriptors.size() == 2, "Export includes both physical sources");
    const auto bank1 = descriptors[0].toObject();
    const auto bank2 = descriptors[1].toObject();
    require(bank1.value("id").toInt() == 7 && bank1.value("size").toInt() == 5 &&
            bank1.value("packedOffset").toInt() == 0 && bank1.value("packedBytes").toInt() == 2 &&
            bank1.value("code").toInt() == 4 && bank1.value("data").toInt() == 4 &&
            bank1.value("both").toInt() == 3 && bank1.value("inspectDomain").toInt() == 17 &&
            bank1.value("inspectOffset").toInt() == 321,
            "Export sorts sources and records canonical offsets and flag counts");
    require(bank2.value("id").toInt() == 41 && bank2.value("size").toInt() == 302 &&
            bank2.value("packedOffset").toInt() == 2 && bank2.value("packedBytes").toInt() == 76 &&
            bank2.value("code").toInt() == 1 && bank2.value("data").toInt() == 1 &&
            bank2.value("both").toInt() == 0, "Second source remains distinct");
    QByteArray expected(78, char(0));
    expected[0] = char(0xde);
    expected[1] = char(0x03);
    expected[2] = char(0x08);
    expected[77] = char(0x04);
    require(contents.mid(12 + headerBytes) == expected,
            "Payload has two bits per physical byte, independent source tails and no ROM data");
}

void testMap(const QTemporaryDir& directory) {
    DebugCoverageMap map;
    QString error;
    require(!map.exportFile(directory.filePath("empty.kcdl"), error) && !error.isEmpty(),
            "Require a captured map before export");
    auto firstBank = source(7, 5, "First bank");
    firstBank.inspect_domain = 17;
    firstBank.inspect_offset = 321;
    const auto secondBank = source(41, 302, "Second bank");
    const QByteArray hash(32, char(0x8a));
    require(map.start({secondBank, firstBank}, "Original coverage", hash, error), error);
    require(map.allocatedBytes() == 78 && map.active(), "Round each region up to a packed byte");
    const auto revision = map.revision();
    require(!map.start({firstBank, firstBank}, {}, {}, error) && map.active() &&
            map.revision() == revision, "Reject duplicate source IDs without clearing a capture");
    require(!map.start({source(63, 256 * 1024 * 1024, "Limit"), firstBank}, {}, {}, error) &&
            map.allocatedBytes() == 78, "Reject total packed allocation above 64 MiB");
    require(!map.start({source(64, 1, "Invalid")}, {}, {}, error) &&
            map.active(), "Reject out-of-range source ID");

    map.mark({KR_DEBUG_M68K, KR_COV_EXEC, 7, 1, 4});
    map.mark({KR_DEBUG_M68K, KR_COV_READ, 7, 3, 2});
    map.mark({KR_DEBUG_M68K, KR_COV_READ, 7, 0, 2});
    map.mark({KR_DEBUG_M68K, KR_COV_READ, 41, 1, 1});
    map.mark({KR_DEBUG_M68K, KR_COV_EXEC, 41, 301, 1});
    const auto marked = map.revision();
    map.mark({KR_DEBUG_M68K, KR_COV_EXEC, 7, 2, 2});
    map.mark({KR_DEBUG_M68K, KR_COV_READ, 41, 1, 1});
    require(marked == revision + 5 && map.revision() == marked,
            "Repeated visits do not change flags, totals or revision");
    require(map.page(7, 0) == QByteArray::fromHex("0203010303"), "Combine read and execution flags byte by byte");
    require(map.page(41, 1, 2) == QByteArray::fromHex("0200"),
            "Equal offsets at distinct physical sources are independent");
    require(map.page(41, 0, std::numeric_limits<unsigned>::max()).size() == 256 &&
            map.page(41, 300) == QByteArray::fromHex("0001") && map.page(41, 302).isEmpty() &&
            map.page(0, 0).isEmpty() && map.page(64, 0).isEmpty(), "Clamp previews to region and page bounds");

    map.mark({KR_DEBUG_M68K, KR_COV_EXEC, 0, 0, 2});
    map.mark({KR_DEBUG_M68K, KR_COV_EXEC, 7, 4, 2});
    map.mark({KR_DEBUG_M68K, KR_COV_READ, 999, 0, 1});
    map.mark({KR_DEBUG_Z80, KR_COV_READ, 41, 0, 3});
    require(map.unknownCode() == 4 && map.unknownData() == 4 && map.revision() == marked + 4,
            "Count bytes of unknown, out-of-range and unsupported spans without marking any part");
    const auto stats = map.sources();
    require(stats.size() == 2 && stats[0].native.id == 7 &&
            stats[0].code == 4 && stats[0].data == 4 && stats[0].both == 3 &&
            stats[1].code == 1 && stats[1].data == 1 && stats[1].both == 0,
            "Unique code and data byte counts both include shared bytes");

    const auto path = directory.filePath("observed.kcdl");
    require(map.exportFile(path, error), error);
    map.clear();
    const auto cleared = map.revision();
    require(map.active() && map.allocatedBytes() == 78 && map.page(7, 0) == QByteArray(5, char(0)) &&
            map.unknownCode() == 0 && map.unknownData() == 0 && map.sources()[0].both == 0,
            "Clear resets flags and counters without stopping capture");
    map.clear();
    require(map.revision() == cleared, "Clearing an empty map is a no-op");
    waitForExport(map);
    const auto completeStatus = map.status();
    const auto contents = readFile(path);
    checkCoverageFile(contents, hash);

    require(map.exportFile(path, error), error);
    waitForExport(map);
    require(!map.status().isEmpty() && map.status() != completeStatus && readFile(path) == contents,
            "Existing file is preserved and the asynchronous export error is visible");
    map.stop();
    map.mark({KR_DEBUG_M68K, KR_COV_EXEC, 7, 1, 1});
    require(!map.active() && map.revision() == cleared, "Stop retains data and rejects later callbacks");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "Create coverage test directory");
        testMap(directory);
        std::puts("PASS: physical coverage sources, bounded packed flags, clear and portable async export");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
