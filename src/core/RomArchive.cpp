#include "i18n/Localization.h"
#include "RomArchive.h"
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cerrno>
#include <limits>
#include <memory>

namespace {
void fail(QString* error, const QString& message) {
    if (error) *error = message;
}

QString archiveError(archive* reader) {
    const char* detail = archive_error_string(reader);
    return Localization::text(QStringLiteral("Не удалось прочитать архив (шифрование, повреждение или неподдерживаемое сжатие): %1"))
        .arg(detail ? QString::fromUtf8(detail) : Localization::text(QStringLiteral("неверный формат архива")));
}

struct Reader {
    QFile file;
    std::array<char, 64 * 1024> buffer{};
    std::unique_ptr<archive, decltype(&archive_read_free)> handle{archive_read_new(), archive_read_free};

    explicit Reader(const QString& path) : file(path) {}
    static la_ssize_t read(archive* a, void* opaque, const void** block) {
        auto& self = *static_cast<Reader*>(opaque);
        const qint64 count = self.file.read(self.buffer.data(), qint64(self.buffer.size()));
        if (count < 0) archive_set_error(a, EIO, "Cannot read archive file");
        *block = self.buffer.data();
        return la_ssize_t(count);
    }
    static la_int64_t seek(archive* a, void* opaque, la_int64_t offset, int whence) {
        auto& self = *static_cast<Reader*>(opaque);
        qint64 base = 0;
        if (whence == SEEK_CUR) base = self.file.pos();
        else if (whence == SEEK_END) base = self.file.size();
        else if (whence != SEEK_SET) return -1;
        if ((offset > 0 && base > std::numeric_limits<qint64>::max() - offset) ||
            (offset < 0 && offset < -base) ||
            base + offset > self.file.size() || !self.file.seek(base + offset)) {
            archive_set_error(a, EIO, "Invalid archive seek");
            return -1;
        }
        return self.file.pos();
    }
    bool open(QString* error) {
        if (!file.open(QIODevice::ReadOnly)) {
            fail(error, Localization::text(QStringLiteral("Не удалось открыть архив: %1")).arg(file.errorString()));
            return false;
        }
        if (file.size() > RomArchive::MaxArchiveSize) {
            fail(error, Localization::text(QStringLiteral("Архив превышает допустимый размер 512 МиБ.")));
            return false;
        }
        if (!handle) {
            fail(error, Localization::text(QStringLiteral("Недостаточно памяти для чтения архива.")));
            return false;
        }
        archive_read_support_filter_none(handle.get());
        archive_read_support_format_zip_seekable(handle.get());
        archive_read_support_format_7zip(handle.get());
        // Disable AppleDouble resource-fork handling: no metadata needs decoding.
        archive_read_set_format_option(handle.get(), "zip", "mac-ext", "0");
        archive_read_set_seek_callback(handle.get(), seek);
        if (archive_read_open(handle.get(), this, nullptr, read, nullptr) != ARCHIVE_OK) {
            fail(error, archiveError(handle.get()));
            return false;
        }
        return true;
    }
};

bool cartridge(const QString& name) {
    static const QStringList extensions = {
        QStringLiteral("md"), QStringLiteral("gen"), QStringLiteral("bin"),
        QStringLiteral("smd"), QStringLiteral("sms"), QStringLiteral("gg"),
        QStringLiteral("sg"), QStringLiteral("68k"), QStringLiteral("32x")};
    return extensions.contains(QFileInfo(name).suffix().toLower());
}

bool scan(const QString& path, QVector<RomArchive::Entry>* members,
          const RomArchive::Entry* selected, QByteArray* output, QString* error) {
    if (error) error->clear();
    if (!RomArchive::isArchive(path)) {
        fail(error, Localization::text(QStringLiteral("Поддерживаются только архивы ZIP и 7z.")));
        return false;
    }
    Reader reader(path);
    if (!reader.open(error)) return false;
    archive* a = reader.handle.get();
    archive_entry* header = nullptr;
    int index = 0;
    qint64 total = 0;
    bool found = false;
    std::array<char, 64 * 1024> block{};
    for (;;) {
        const int result = archive_read_next_header(a, &header);
        if (result == ARCHIVE_EOF) break;
        if (result != ARCHIVE_OK) {
            fail(error, archiveError(a));
            return false;
        }
        if (index >= RomArchive::MaxEntries) {
            fail(error, Localization::text(QStringLiteral("Архив содержит более 4096 файлов.")));
            return false;
        }
        if (archive_entry_is_encrypted(header) > 0 ||
            archive_read_has_encrypted_entries(a) > 0) {
            fail(error, Localization::text(QStringLiteral("Зашифрованные архивы не поддерживаются.")));
            return false;
        }
        const char* name = archive_entry_pathname_utf8(header);
        if (!name) name = archive_entry_pathname(header);
        const QString entryName = name ? QString::fromUtf8(name) : QString();
        if (entryName.isEmpty() || entryName.size() > 4096) {
            fail(error, Localization::text(QStringLiteral("Архив содержит неверное или слишком длинное имя файла.")));
            return false;
        }
        const qint64 expected = archive_entry_size_is_set(header) ? archive_entry_size(header) : -1;
        if (expected < 0 || expected > RomArchive::MaxMemberSize) {
            fail(error, Localization::text(QStringLiteral("Файл в архиве превышает 128 МиБ или имеет неверный размер.")));
            return false;
        }
        if (expected > RomArchive::MaxDecodedSize - total) {
            fail(error, Localization::text(QStringLiteral("Общий размер распакованных данных превышает 128 МиБ.")));
            return false;
        }
        const bool isCartridge = archive_entry_filetype(header) == AE_IFREG &&
            !archive_entry_symlink(header) && !archive_entry_hardlink(header) && cartridge(entryName);
        const bool wanted = selected && selected->index == index;
        if (wanted && (!isCartridge || selected->name != entryName || selected->size != expected)) {
            fail(error, Localization::text(QStringLiteral("Выбранный файл в архиве изменился. Откройте архив заново.")));
            return false;
        }
        if (isCartridge && expected < 512) {
            fail(error, Localization::text(QStringLiteral("Размер ROM в архиве меньше 512 байт.")));
            return false;
        }
        if (wanted) output->reserve(qsizetype(expected));
        qint64 decoded = 0;
        for (;;) {
            const la_ssize_t count = archive_read_data(a, block.data(), block.size());
            if (count < 0) {
                fail(error, archiveError(a));
                return false;
            }
            if (count == 0) break;
            if (count > RomArchive::MaxMemberSize - decoded ||
                count > RomArchive::MaxDecodedSize - total) {
                fail(error, Localization::text(QStringLiteral("Архив превышает допустимый размер распакованных данных.")));
                return false;
            }
            decoded += count;
            total += count;
            if (wanted) output->append(block.data(), qsizetype(count));
        }
        if (archive_entry_filetype(header) == AE_IFREG && decoded != expected) {
            fail(error, Localization::text(QStringLiteral("Файл в архиве обрезан или имеет неверный распакованный размер.")));
            return false;
        }
        if (isCartridge && members) members->append({entryName, index, expected});
        found = found || wanted;
        ++index;
    }
    if (selected && !found) {
        fail(error, Localization::text(QStringLiteral("Выбранный ROM не найден в архиве.")));
        return false;
    }
    if (members && members->isEmpty()) {
        fail(error, Localization::text(QStringLiteral("В архиве нет поддерживаемых файлов ROM.")));
        return false;
    }
    return true;
}
}

bool RomArchive::isArchive(const QString& path) {
    const QString suffix = QFileInfo(path).suffix();
    return suffix.compare(QStringLiteral("zip"), Qt::CaseInsensitive) == 0 ||
           suffix.compare(QStringLiteral("7z"), Qt::CaseInsensitive) == 0;
}

QVector<RomArchive::Entry> RomArchive::list(const QString& path, QString* error) {
    QVector<Entry> members;
    if (!scan(path, &members, nullptr, nullptr, error)) members.clear();
    return members;
}

QByteArray RomArchive::extract(const QString& path, const Entry& member, QString* error) {
    QByteArray output;
    if (!scan(path, nullptr, &member, &output, error)) output.clear();
    return output;
}
