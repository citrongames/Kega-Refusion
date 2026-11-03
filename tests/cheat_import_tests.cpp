#include "core/CheatImport.h"
#include <QCoreApplication>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QStringEncoder>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>

namespace {
void check(bool okay, const char* why) { if (!okay) throw std::runtime_error(why); }
void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
          file.write(bytes) == bytes.size(), "fixture write");
}
QByteArray read(const QString& path) {
    QFile file(path); check(file.open(QIODevice::ReadOnly), "fixture read");
    return file.readAll();
}
bool parse(const QTemporaryDir& dir, const QByteArray& bytes, Cheats::ImportResult& result,
           QString& error, Cheats::Platform platform = Cheats::Platform::MegaDrive) {
    const auto path = dir.filePath(QStringLiteral("fixture.cht"));
    write(path, bytes);
    return Cheats::importCht(path, platform, result, error);
}
void normalization() {
    CheatList out; QString error;
    const CheatList groups{
        {QStringLiteral(" ff0000:0001 + ff0001:0002 "), QStringLiteral(" One "), true},
        {QStringLiteral("FF0000:0001+FF0002:0003"), QStringLiteral("Two"), true}
    };
    check(Cheats::normalize(groups, Cheats::Platform::MegaDrive, out, error) &&
          out.size() == 2 && out[0].code == QStringLiteral("FF0000:0001+FF0001:0002") &&
          out[0].description == QStringLiteral("One"), "group normalization/shared atom");
    check(!Cheats::normalize({groups[0], groups[0]}, Cheats::Platform::MegaDrive,
                             out, error), "duplicate whole group");
    check(!Cheats::normalize({{QStringLiteral("FF0000:0001+XX"),
                               QStringLiteral("Bad"), false}},
                             Cheats::Platform::MegaDrive, out, error), "partial group accepted");
    CheatList catalog;
    for (int i = 0; i < 101; ++i)
        catalog.append({QStringLiteral("FF%1:0001").arg(i, 4, 16, QLatin1Char('0')),
                        QStringLiteral("Catalog"), false});
    check(Cheats::normalize(catalog, Cheats::Platform::MegaDrive, out, error) &&
          out.size() == 101, "disabled catalog hit active cap");
    for (auto& entry : catalog) entry.enabled = true;
    check(!Cheats::normalize(catalog, Cheats::Platform::MegaDrive, out, error),
          "active unique atom cap");
}
void importGroups() {
    QTemporaryDir dir; check(dir.isValid(), "temporary directory");
    const QByteArray text = R"(cheats = 7 # a slightly wrong count
cheat0_desc = "  Hello\tWorld  "
cheat0_code = " ff0000:0001 + ff0001:0002 "
cheat0_enable = true
cheat7_code = "FF0002:0003"
cheat7_enanble = true
cheat9_code = "FF0000:0001+FF0001:0002"
cheat10_code = "FF0004:0005"
cheat10_handler = "1"
cheat11_code = "FF00??:0001"
cheat12_code = "FF0006:0007+XX"
)";
    Cheats::ImportResult result; QString error;
    check(parse(dir, text, result, error), "sparse import");
    check(error.isEmpty() && result.entries.size() == 2 && result.skipped == 4,
          "skip/duplicate accounting");
    check(result.entries[0].code == QStringLiteral("FF0000:0001+FF0001:0002") &&
          result.entries[0].description == QStringLiteral("Hello World") &&
          !result.entries[0].enabled && !result.entries[1].enabled,
          "group canonicalization/disabled import");
    check(result.warnings.size() >= 5, "count/typo/skip warnings");

    QByteArray many("cheats = 25\n");
    for (int i = 0; i < 25; ++i)
        many += "cheat" + QByteArray::number(i) + "_code = \"??\"\n";
    check(parse(dir, many, result, error) && result.entries.isEmpty() &&
          result.skipped == 25 && result.warnings.size() == 20, "warning bound");
}
void encodingsAndFailure() {
    QTemporaryDir dir; check(dir.isValid(), "temporary directory");
    Cheats::ImportResult result; QString error;
    const QByteArray ascii("cheats = \"1\"\ncheat0_code = \"FF0000:0001\"\n");
    check(parse(dir, QByteArray::fromHex("efbbbf") + ascii, result, error) &&
          result.entries.size() == 1 && !result.entries[0].enabled, "UTF-8 BOM");
    const QString unicode = QString::fromUtf8(
        "cheats = 1\ncheat0_desc = \"Тест\"\ncheat0_code = \"FF0001:0002\"\n");
    QStringEncoder le(QStringEncoder::Utf16LE), be(QStringEncoder::Utf16BE);
    const QByteArray leText = le.encode(unicode);
    const QByteArray beText = be.encode(unicode);
    check(parse(dir, QByteArray::fromHex("fffe") + leText,
                result, error) &&
          result.entries[0].description == QString::fromUtf8("Тест"), "UTF-16LE BOM");
    check(parse(dir, QByteArray::fromHex("feff") + beText,
                result, error) &&
          result.entries[0].description == QString::fromUtf8("Тест"), "UTF-16BE BOM");
    const auto prior = result;
    const QByteArray invalid[] = {
        "cheat0_code = \"FF0000:0001\"\n",
        "cheats = 4097\ncheat0_code = \"FF0000:0001\"\n",
        "cheats = 1\nfalse\ncheat0_code = \"FF0000:0001\"\n",
        "cheats = 1\ncheat4096_code = \"FF0000:0001\"\n"
    };
    for (const auto& bytes : invalid) {
        check(!parse(dir, bytes, result, error) && !error.isEmpty(), "invalid structure accepted");
        check(result.entries == prior.entries && result.skipped == prior.skipped &&
              result.warnings == prior.warnings, "failure changed result");
    }
}
void shorthandConversion() {
    QTemporaryDir dir; check(dir.isValid(), "temporary directory");
    const QByteArray md = R"(cheats = 4
cheat0_code = "FF0213:50"
cheat1_code = "FF0100:0001"
cheat2_code = "FF0213:50+FFB78D:15"
cheat3_code = "000100:50"
)";
    Cheats::ImportResult result; QString error;
    check(parse(dir, md, result, error) && result.entries.size() == 3 &&
          result.skipped == 1, "MD byte shorthand accounting");
    const auto first = Cheats::ramCode(0xff0213, 1, true, 0x50);
    const auto second = Cheats::ramCode(0xffb78d, 1, true, 0x15);
    check(!first.isEmpty() && result.entries[0].code == first &&
          result.entries[1].code == QStringLiteral("FF0100:0001") &&
          result.entries[2].code == first + QLatin1Char('+') + second &&
          result.warnings.size() >= 3,
          "MD byte shorthand changed address or full word code");

    const QByteArray sms = R"(cheats = 4
cheat0_code = "00F+12E+5D4"
cheat1_code = "FF0+4B7+E6A+FF9+607+E6A"
cheat2_code = "FF0+4B7+??"
cheat3_code = "FF0+4B7+E6A+00C0-9505"
)";
    check(parse(dir, sms, result, error, Cheats::Platform::MasterSystem) &&
          result.entries.size() == 2 && result.skipped == 2,
          "SMS triple shorthand accounting");
    check(result.entries[0].code == QStringLiteral("00F-12E-5D4") &&
          result.entries[1].code == QStringLiteral("FF0-4B7-E6A+FF9-607-E6A"),
          "SMS triple shorthand or whole-group rejection");
}
void recoveryAndExport() {
    QTemporaryDir dir; check(dir.isValid(), "temporary directory");
    const QByteArray quirks = R"(cheats = 4
cheat0_desc = "Start With Letters "X" & "T""
cheat0_code = "FF0000:0001"
cheat1_desc = "Broken code quote"
cheat1_code = "FF0001:0002
cheat2_code = "FF0002:0003"
cheat2_code = "FF0003:0004"
cheat3_code = "FF0004:0005"
)";
    Cheats::ImportResult result; QString error;
    check(parse(dir, quirks, result, error) && result.entries.size() == 2 &&
          result.skipped == 2 && result.warnings.size() >= 3,
          "description quote recovery or whole-row skips");
    check(result.entries[0].description == QStringLiteral("Start With Letters \"X\" & \"T\"") &&
          result.entries[1].code == QStringLiteral("FF0004:0005"),
          "quote recovery changed valid rows");

    QByteArray latin1("cheats = 1\ncheat0_desc = \"Caf");
    latin1.append(char(0xe9));
    latin1 += "\"\ncheat0_code = \"FF0005:0006\"\n";
    check(parse(dir, latin1, result, error) && result.entries.size() == 1 &&
          result.entries[0].description == QStringLiteral("Café") &&
          !result.warnings.isEmpty(), "unmarked Latin-1 fallback");
    const auto previous = result;
    check(!parse(dir, QByteArray::fromHex("efbbbf") + latin1, result, error) &&
          !error.isEmpty() && result.entries == previous.entries &&
          result.skipped == previous.skipped && result.warnings == previous.warnings,
          "invalid BOM-marked UTF-8 was silently downgraded");

    const auto path = dir.filePath(QStringLiteral("export.cht"));
    const CheatList exported{
        {QStringLiteral("FF0000:0001+FF0001:0002"),
         QStringLiteral("A \"quoted\" \\ path"), true},
        {QStringLiteral("FF0002:0003"), QStringLiteral("Disabled"), false}
    };
    check(Cheats::exportCht(path, Cheats::Platform::MegaDrive, exported, error),
          "CHT export");
    const auto bytes = read(path);
    check(bytes.contains("cheats = 2") && bytes.contains("cheat0_enable = true") &&
          bytes.contains("cheat1_enable = false") &&
          bytes.contains("\\\"quoted\\\"") && bytes.contains("\\\\ path") &&
          bytes.contains("FF0000:0001+FF0001:0002"),
          "CHT export flags, escaping or group");
    Cheats::ImportResult imported;
    check(Cheats::importCht(path, Cheats::Platform::MegaDrive, imported, error) &&
          imported.entries.size() == 2 &&
          imported.entries[0].description == exported[0].description &&
          imported.entries[0].code == exported[0].code &&
          !imported.entries[0].enabled && !imported.entries[1].enabled,
          "export/import round trip or disabled import policy");

    QFile file(path); check(file.open(QIODevice::ReadWrite), "export timestamp open");
    check(file.setFileTime(QDateTime::fromSecsSinceEpoch(946684800),
                           QFileDevice::FileModificationTime), "export timestamp set");
    file.close();
    const auto before = QFileInfo(path).lastModified();
    check(Cheats::exportCht(path, Cheats::Platform::MegaDrive, exported, error) &&
          read(path) == bytes && QFileInfo(path).lastModified() == before,
          "identical CHT export rewrote disk");
    CheatList invalid = exported;
    invalid[0].code = QStringLiteral("FF0000:0001+XX");
    check(!Cheats::exportCht(path, Cheats::Platform::MegaDrive, invalid, error) &&
          !error.isEmpty() && read(path) == bytes &&
          QFileInfo(path).lastModified() == before,
          "invalid CHT export damaged existing file");
}
void storeRoundTrip() {
    QTemporaryDir dir; check(dir.isValid(), "temporary directory");
    const auto path = dir.filePath(QStringLiteral("saved.json"));
    QString error; Cheats::Store store;
    check(store.load(path, Cheats::Platform::MegaDrive, error), "store load");
    const CheatList entries{
        {QStringLiteral("FF0000:0001+FF0001:0002"), QStringLiteral("Grouped"), true},
        {QStringLiteral("FF0002:0003"), QStringLiteral("Disabled"), false}
    };
    check(store.set(entries, error), "store group save");
    const auto saved = read(path);
    check(saved.contains("FF0000:0001+FF0001:0002"), "group serialization");
    QFile file(path); check(file.open(QIODevice::ReadWrite), "timestamp open");
    check(file.setFileTime(QDateTime::fromSecsSinceEpoch(946684800),
                           QFileDevice::FileModificationTime), "timestamp set");
    file.close();
    const auto before = QFileInfo(path).lastModified();
    check(store.set(entries, error) && read(path) == saved &&
          QFileInfo(path).lastModified() == before, "unchanged save rewrote disk");
    Cheats::Store restored;
    check(restored.load(path, Cheats::Platform::MegaDrive, error) &&
          restored.entries() == entries, "group store round trip");
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc == 2) {
            QDirIterator files(QString::fromLocal8Bit(argv[1]),{QStringLiteral("*.cht")},QDir::Files,QDirIterator::Subdirectories);
            int total=0, parsed=0, entries=0, skipped=0, warnings=0;
            QJsonArray failures;
            while(files.hasNext()) {
                const auto path=files.next(); ++total;
                const bool sms=path.contains(QStringLiteral("Master System")) || path.contains(QStringLiteral("Game Gear"));
                Cheats::ImportResult result; QString error;
                if (!Cheats::importCht(path,sms?Cheats::Platform::MasterSystem:Cheats::Platform::MegaDrive,result,error)) {
                    failures.append(QJsonObject{{"file",QDir(QString::fromLocal8Bit(argv[1])).relativeFilePath(path)},{"error",error}});
                    continue;
                }
                ++parsed; entries+=result.entries.size(); skipped+=result.skipped; warnings+=!result.warnings.isEmpty();
            }
            const auto json=QJsonDocument(QJsonObject{{"files",total},{"parsed",parsed},{"entries",entries},
                {"skippedEntries",skipped},{"filesWithWarnings",warnings},{"failures",failures}}).toJson();
            std::fwrite(json.constData(),1,size_t(json.size()),stdout);
            return 0;
        }
        normalization();
        importGroups();
        encodingsAndFailure();
        shorthandConversion();
        recoveryAndExport();
        storeRoundTrip();
        std::puts("PASS: RetroArch CHT import, grouped codes and store round trip");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
