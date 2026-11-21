#include "core/DebugProject.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
void require(bool ok, const QString& detail) {
    if (!ok) throw std::runtime_error(detail.toStdString());
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    return file.readAll();
}

void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(),
            file.errorString());
}

DebugProject exampleProject() {
    DebugProject project;
    project.game = QStringLiteral("Original cartridge — 日本語");
    DebuggerBreakpoint bp;
    bp.id = std::numeric_limits<quint32>::max();
    bp.cpu = 0x100 | KR_DEBUG_Z80;
    bp.events = KR_DEBUG_EXEC | KR_DEBUG_READ | KR_DEBUG_WRITE;
    bp.first = 0xfff0;
    bp.last = 0xffff;
    bp.oneShot = true;
    bp.enabled = false;
    bp.condition = QStringLiteral("ADDR & $FF == $F2");
    bp.ignoreHits = std::numeric_limits<quint64>::max();
    bp.hits = 123;
    project.breakpoints.push_back(bp);
    project.symbols = {
        {KR_DEBUG_Z80, 0xfff0, QStringLiteral("Вход A"), QStringLiteral("Текст\n\"1.25e-2\" \\ в кавычках"), true},
        {0x100 | KR_DEBUG_Z80, 0xfff0, QStringLiteral("入口 B"), {}, false},
        {KR_DEBUG_SH2_MASTER, 0xffffffffu, {}, QStringLiteral("Last SH-2 address"), false},
        {KR_DEBUG_SSP1601, 0xffff, {}, {}, true},
    };
    DebugWatchEntry watch;
    watch.id = 15;
    watch.instance = 1;
    watch.domain = KR_MEM_SVP_DRAM;
    watch.offset = 0xfffffffc;
    watch.width = 4;
    watch.format = DebugWatchEntry::Binary;
    watch.order = DebugWatchEntry::Little;
    watch.name = QStringLiteral("Скорость 🧭");
    project.watches.push_back(watch);
    watch.id = 16;
    watch.instance = 0;
    watch.domain = KR_MEM_RAM;
    watch.offset = 0x42;
    watch.width = 2;
    watch.format = DebugWatchEntry::Signed;
    watch.order = DebugWatchEntry::Big;
    watch.name = QStringLiteral("Δ");
    project.watches.push_back(watch);
    return project;
}

void checkRoundTrip(const DebugProject& loaded, const DebugProject& expected) {
    require(loaded.game == expected.game &&
            loaded.breakpoints.size() == expected.breakpoints.size() &&
            loaded.symbols.size() == expected.symbols.size() &&
            loaded.watches.size() == expected.watches.size(), "Game and list lengths round-trip");
    for (qsizetype i = 0; i < loaded.breakpoints.size(); ++i) {
        const auto& a = loaded.breakpoints[i];
        const auto& b = expected.breakpoints[i];
        require(a.id == b.id && a.cpu == b.cpu && a.events == b.events && a.first == b.first && a.last == b.last &&
                a.condition == b.condition && a.ignoreHits == b.ignoreHits && a.enabled == b.enabled &&
                a.oneShot == b.oneShot && a.hits == 0, "Breakpoint definitions round-trip and runtime hits reset");
    }
    for (qsizetype i = 0; i < loaded.symbols.size(); ++i) {
        const auto& a = loaded.symbols[i];
        const auto& b = expected.symbols[i];
        require(a.cpu == b.cpu && a.address == b.address && a.label == b.label && a.comment == b.comment &&
                a.bookmarked == b.bookmarked, "Labels, notes and bookmarks round-trip in both CPU namespaces");
    }
    for (qsizetype i = 0; i < loaded.watches.size(); ++i) {
        const auto& a = loaded.watches[i];
        const auto& b = expected.watches[i];
        require(a.id == b.id && a.instance == b.instance && a.domain == b.domain && a.offset == b.offset &&
                a.width == b.width && a.format == b.format && a.order == b.order && a.name == b.name,
                "Physical watch definitions round-trip");
    }
}

void testRoundTrip(const QString& path) {
    QString error;
    const auto expected = exampleProject();
    require(saveDebugProject(path, expected, error), error);
    const auto bytes = readFile(path);
    const auto root = QJsonDocument::fromJson(bytes).object();
    const auto bp = root.value("breakpoints").toArray().at(0).toObject();
    require(root.value("format") == "KegaRefusionDebugProject" && root.value("version").toInt() == 1 &&
            bp.value("ignoreHits") == "18446744073709551615" && !bp.contains("hits") &&
            bytes.contains(QStringLiteral("入口 B").toUtf8()), "Versioned UTF-8 data preserves all 64 counter bits");
    DebugProject loaded;
    require(loadDebugProject(path, loaded, error), error);
    checkRoundTrip(loaded, expected);
}

QJsonObject changeFirst(QJsonObject root, const char* arrayKey, const char* key, QJsonValue value) {
    auto rows = root.value(QLatin1String(arrayKey)).toArray();
    auto first = rows.at(0).toObject();
    first.insert(QLatin1String(key), std::move(value));
    rows.replace(0, first);
    root.insert(QLatin1String(arrayKey), rows);
    return root;
}

void testFailedLoads(const QString& inputPath, const QString& validPath) {
    const auto baseline = QJsonDocument::fromJson(readFile(validPath)).object();
    const auto expected = exampleProject();
    DebugProject destination;
    QString error;
    require(loadDebugProject(validPath, destination, error), error);
    const auto rejectBytes = [&](const QByteArray& data) {
        writeFile(inputPath, data);
        require(!loadDebugProject(inputPath, destination, error) && !error.isEmpty(),
                "Reject invalid project data with an error");
        checkRoundTrip(destination, expected);
    };
    const auto reject = [&](const QJsonObject& bad) {
        rejectBytes(QJsonDocument(bad).toJson(QJsonDocument::Compact));
    };
    rejectBytes("{\"format\":");
    rejectBytes("[]");
    auto roundedFraction = QJsonDocument(baseline).toJson(QJsonDocument::Compact);
    require(roundedFraction.contains("\"version\":1"), "Fixture has a plain integer version");
    rejectBytes(roundedFraction.replace("\"version\":1", "\"version\":1.000000000000000000000001"));
    rejectBytes(QByteArray(2 * 1024 * 1024 + 1, ' '));

    auto bad = baseline;
    bad["version"] = 2;
    reject(bad);
    bad["version"] = true;
    reject(bad);
    bad = baseline;
    bad["watches"] = QJsonObject{};
    reject(bad);
    bad = baseline;
    bad["rom"] = "extra payload";
    reject(bad);

    reject(changeFirst(baseline, "breakpoints", "cpu", 0x200 | KR_DEBUG_M68K));
    reject(changeFirst(baseline, "breakpoints", "cpu", KR_DEBUG_M68K | KR_DEBUG_Z80));
    reject(changeFirst(baseline, "breakpoints", "last", 0x10000));
    reject(changeFirst(baseline, "breakpoints", "last", 0xffef));
    reject(changeFirst(baseline, "breakpoints", "first", qint64(0x100000000)));
    reject(changeFirst(baseline, "breakpoints", "first", -1));
    reject(changeFirst(baseline, "breakpoints", "id", 1.25));
    reject(changeFirst(baseline, "breakpoints", "events", 0));
    reject(changeFirst(baseline, "breakpoints", "events", 8));
    reject(changeFirst(baseline, "breakpoints", "enabled", 0));
    reject(changeFirst(baseline, "breakpoints", "ignoreHits", "18446744073709551616"));
    reject(changeFirst(baseline, "breakpoints", "ignoreHits", " +5"));
    reject(changeFirst(baseline, "breakpoints", "ignoreHits", 5));
    reject(changeFirst(baseline, "breakpoints", "condition", QString(257, QLatin1Char('x'))));
    // Check the address limit independently by moving that label outside the main CPU range.
    bad = changeFirst(baseline, "symbols", "cpu", KR_DEBUG_M68K);
    reject(changeFirst(bad, "symbols", "address", 0x1000000));
    reject(changeFirst(baseline, "watches", "offset", qint64(0xfffffffd)));
    reject(changeFirst(baseline, "watches", "instance", true));
    reject(changeFirst(baseline, "watches", "instance", 2));
    reject(changeFirst(baseline, "watches", "domain", KR_MEM_SVP_DRAM + 1));
    reject(changeFirst(baseline, "watches", "width", 3));
    reject(changeFirst(baseline, "watches", "order", 3));
    reject(changeFirst(baseline, "watches", "format", 4));

    for (const auto key : {"breakpoints", "symbols", "watches"}) {
        bad = baseline;
        auto rows = baseline.value(QLatin1String(key)).toArray();
        rows.append(rows.at(0));
        bad[QLatin1String(key)] = rows;
        reject(bad);
    }

    bad = baseline;
    QJsonArray tooMany;
    for (int i = 0; i <= 128; ++i) tooMany.append(QJsonObject{});
    bad["breakpoints"] = tooMany;
    reject(bad);
    bad = baseline;
    bad["watches"] = tooMany;
    reject(bad);
    for (int i = 129; i <= 4096; ++i) tooMany.append(QJsonObject{});
    bad = baseline;
    bad["symbols"] = tooMany;
    reject(bad);
}

void testFailedSaves(const QTemporaryDir& directory, const QString& validPath) {
    const auto previous = readFile(validPath);
    QString error;
    const auto reject = [&](const DebugProject& bad) {
        require(!saveDebugProject(validPath, bad, error) && !error.isEmpty() &&
                readFile(validPath) == previous, "Failed save leaves the previous complete file intact");
    };
    const auto old = exampleProject();
    auto bad = old;
    bad.breakpoints.front().id = 0;
    reject(bad);
    bad = old;
    bad.symbols.front().label = QString(97, QLatin1Char('x'));
    reject(bad);
    bad = old;
    bad.symbols.push_back({KR_DEBUG_M68K, 0x20, {}, {}, false});
    reject(bad);
    bad = old;
    bad.watches.last().name = QString(129, QLatin1Char('x'));
    reject(bad);

    bad = old;
    const QString largeComment(512, QChar(0x754c));
    bad.symbols.clear();
    for (unsigned i = 0; i < 1500; ++i)
        bad.symbols.push_back({KR_DEBUG_M68K, i, {}, largeComment, false});
    reject(bad);

    require(QDir(directory.path()).mkdir("existing-directory"), "Create failed output destination");
    const auto invalidPath = directory.filePath("existing-directory");
    const auto sentinel = QDir(invalidPath).filePath("keep");
    writeFile(sentinel, "existing");
    require(!saveDebugProject(invalidPath, old, error) && readFile(sentinel) == "existing",
            "An unwritable destination gets no partial project contents");

    auto replacement = old;
    replacement.game = QStringLiteral("Updated project");
    require(saveDebugProject(validPath, replacement, error), error);
    DebugProject loaded;
    require(loadDebugProject(validPath, loaded, error), error);
    checkRoundTrip(loaded, replacement);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "Create project test directory");
        const auto valid = directory.filePath("valid.json");
        testRoundTrip(valid);
        testFailedLoads(directory.filePath("mutated.json"), valid);
        testFailedSaves(directory, valid);
        std::puts("PASS: debug project round-trip, strict bounds, and transactional load/save");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
