#include "core/RomArchive.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <archive.h>
#include <archive_entry.h>
#include <cstdlib>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << Qt::endl;
        std::exit(1);
    }
}
struct Fixture {
    QByteArray name;
    QByteArray data;
    qint64 size = -1;
    int type = AE_IFREG;
};
QByteArray pack(bool sevenZip, const QVector<Fixture>& files, bool encrypted = false,
                bool stored = false) {
    QByteArray buffer(8 * 1024 * 1024, '\0');
    size_t used = 0;
    archive* writer = archive_write_new();
    check(writer != nullptr, "allocate writer");
    check((sevenZip ? archive_write_set_format_7zip(writer) :
           archive_write_set_format_zip(writer)) == ARCHIVE_OK, "select fixture format");
    if (encrypted) {
        check(archive_write_set_options(writer, "zip:encryption=zipcrypt") == ARCHIVE_OK,
              "enable encrypted fixture");
        check(archive_write_set_passphrase(writer, "test-password") == ARCHIVE_OK,
              "set fixture password");
    }
    if (stored)
        check(archive_write_set_options(writer, sevenZip ? "7zip:compression=copy" :
              "zip:compression=store") == ARCHIVE_OK, "select stored fixture");
    check(archive_write_open_memory(writer, buffer.data(), size_t(buffer.size()), &used)
          == ARCHIVE_OK, "open fixture memory");
    for (const auto& file : files) {
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, file.name.constData());
        archive_entry_set_filetype(entry, file.type);
        archive_entry_set_perm(entry, 0644);
        const qint64 size = file.size >= 0 ? file.size : file.data.size();
        archive_entry_set_size(entry, size);
        if (file.type == AE_IFLNK) archive_entry_set_symlink(entry, "target.md");
        check(archive_write_header(writer, entry) == ARCHIVE_OK, "write fixture header");
        qint64 left = file.type == AE_IFREG ? size : 0;
        while (left) {
            const qint64 count = qMin(left, qint64(file.data.size()));
            check(count > 0, "fixture has source bytes");
            check(archive_write_data(writer, file.data.constData(), size_t(count)) == count,
                  "write fixture data");
            left -= count;
        }
        archive_entry_free(entry);
    }
    check(archive_write_close(writer) == ARCHIVE_OK, "close fixture");
    archive_write_free(writer);
    buffer.resize(qsizetype(used));
    return buffer;
}
QString save(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "open fixture file");
    check(file.write(bytes) == bytes.size(), "save fixture");
    return path;
}
void rejected(const QString& path) {
    QString error = QStringLiteral("old error");
    check(RomArchive::list(path, &error).isEmpty(), "reject invalid archive");
    check(!error.isEmpty() && error != QStringLiteral("old error"), "explain rejection");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    check(temp.isValid(), "temporary fixture directory");
    const QByteArray payload = QByteArray("ORIGINAL GENERATED CARTRIDGE DATA") + QByteArray(1024, '\0');
    const QByteArray first = QByteArray("first") + QByteArray(512, '\0');
    const QByteArray second = QByteArray("second") + QByteArray(512, '\0');
    const QByteArray memoryOnly = QByteArray("safe memory only") + QByteArray(512, '\0');
    check(RomArchive::isArchive(QStringLiteral("GAME.ZIP")), "case insensitive ZIP suffix");
    check(RomArchive::isArchive(QStringLiteral("game.7z")), "7z suffix");
    check(!RomArchive::isArchive(QStringLiteral("game.zip.md")), "plain ROM suffix");
    for (bool sevenZip : {false, true}) {
        const QString suffix = sevenZip ? QStringLiteral(".7z") : QStringLiteral(".zip");
        const QString path = temp.path() + QChar(0x0410) + suffix;
        const QByteArray bytes = pack(sevenZip, {
            {"manual.txt", "Generated fixture"},
            {"nested/game.MD", payload},
            {"duplicate.bin", first},
            {"duplicate.bin", second},
            {"../outside.gg", memoryOnly},
            {"folder/", {}, 0, AE_IFDIR},
            {"link.md", {}, 0, AE_IFLNK}
        });
        save(path, bytes);
        QString error = QStringLiteral("stale");
        const auto entries = RomArchive::list(path, &error);
        if (entries.size() != 4) QTextStream(stderr) << error << Qt::endl;
        check(entries.size() == 4 && error.isEmpty(), "ZIP/7z filter entries");
        check(entries[0].name == QStringLiteral("nested/game.MD"), "preserve nested member");
        check(entries[0].index == 1, "index includes non-ROM headers");
        check(RomArchive::extract(path, entries[0], &error) == payload && error.isEmpty(),
              "ZIP/7z decode identical original bytes");
        check(RomArchive::extract(path, entries[1], &error) == first, "first duplicate");
        check(RomArchive::extract(path, entries[2], &error) == second, "second duplicate");
        check(RomArchive::extract(path, entries[3], &error) == memoryOnly,
              "traversal member stays in memory");
        check(!QFile::exists(temp.path() + QStringLiteral("/../outside.gg")), "no path extraction");
        auto stale = entries[0];
        stale.name = QStringLiteral("changed.md");
        check(RomArchive::extract(path, stale, &error).isEmpty() && !error.isEmpty(), "stale member");
        stale = entries[0]; stale.index = 1234;
        check(RomArchive::extract(path, stale, &error).isEmpty() && !error.isEmpty(), "missing member");
        rejected(save(temp.path() + "/truncated" + suffix, bytes.left(bytes.size() / 2)));
        auto damaged = bytes;
        damaged[sevenZip ? 8 : 0] = char(damaged[sevenZip ? 8 : 0] ^ 0x7f);
        rejected(save(temp.path() + "/damaged" + suffix, damaged));
        rejected(save(temp.path() + "/no-rom" + suffix, pack(sevenZip, {{"manual.txt", "notes"}})));
        rejected(save(temp.path() + "/empty-rom" + suffix, pack(sevenZip, {{"empty.md", {}}})));
        rejected(save(temp.path() + "/oversized" + suffix,
            pack(sevenZip, {{"large.md", QByteArray(64 * 1024, '\0'), RomArchive::MaxMemberSize + 1}})));
        // The writer repeats this small block; do not allocate a full-size
        // expected ROM just to verify the largest supported cartridge.
        const QByteArray largeBlock(64 * 1024, 'R');
        const QString largePath = save(temp.path() + "/max-rom" + suffix,
            pack(sevenZip, {{"large.32x", largeBlock, CartridgeLimits::MaxRomSize}}));
        const auto largeEntries = RomArchive::list(largePath, &error);
        check(largeEntries.size() == 1 && error.isEmpty() &&
              largeEntries[0].size == CartridgeLimits::MaxRomSize,
              "list 128 MiB cartridge");
        const auto largeRom = RomArchive::extract(largePath, largeEntries[0], &error);
        check(largeRom.size() == CartridgeLimits::MaxRomSize && error.isEmpty() &&
              largeRom.startsWith(largeBlock) && largeRom.endsWith(largeBlock),
              "extract 128 MiB cartridge without truncation");
        QVector<Fixture> recognized;
        for (const auto* extension : {"md", "gen", "bin", "smd", "sms", "gg", "sg", "68k", "32x"})
            recognized.append({QByteArray("game.") + extension, payload});
        check(RomArchive::list(save(temp.path() + "/extensions" + suffix,
              pack(sevenZip, recognized)), &error).size() == 9, "all cartridge extensions");
    }
    rejected(save(temp.path() + "/encrypted.zip", pack(false, {{"game.md", payload}}, true)));
    auto crcBad = pack(false, {{"game.md", payload}}, false, true);
    const auto offset = crcBad.indexOf(payload);
    check(offset >= 0, "find stored fixture data");
    crcBad[offset] = char(crcBad[offset] ^ 0x40);
    rejected(save(temp.path() + "/bad-crc.zip", crcBad));

    QVector<Fixture> tooMany;
    for (int index = 0; index <= RomArchive::MaxEntries; ++index)
        tooMany.append({QByteArray("notes/") + QByteArray::number(index) + ".txt", "x"});
    rejected(save(temp.path() + "/many.zip", pack(false, tooMany)));
    rejected(save(temp.path() + "/many.7z", pack(true, tooMany)));

    QVector<Fixture> tooMuch;
    for (int index = 0; index < 5; ++index)
        tooMuch.append({QByteArray::number(index) + ".md", QByteArray(64 * 1024, '\0'), 27LL * 1024 * 1024});
    rejected(save(temp.path() + "/total-limit.zip", pack(false, tooMuch)));

    QFile sparse(temp.path() + "/outer-limit.zip");
    check(sparse.open(QIODevice::WriteOnly), "create sparse archive fixture");
    check(sparse.resize(RomArchive::MaxArchiveSize + 1), "resize sparse archive fixture");
    sparse.close();
    rejected(sparse.fileName());
    rejected(temp.path() + "/missing.zip");
    rejected(save(temp.path() + "/wrong.zip", "not an archive"));
    QTextStream(stdout) << "ZIP/7z archive fixtures, CRC, encryption, selection and size limits passed."
                        << Qt::endl;
    return 0;
}
