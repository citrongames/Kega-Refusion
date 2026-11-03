#include "CheatImport.h"
#include "i18n/Localization.h"

#include <QByteArrayView>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QMap>
#include <QSet>
#include <QStringConverter>
#include <QStringView>
#include <utility>

namespace Cheats {
namespace {
constexpr qint64 MaxImportBytes = 2 * 1024 * 1024;
constexpr int MaxIndex = 4095;
constexpr int MaxWarnings = 20;
constexpr int MaxImportedCodeLength = 4095;

enum class RowIssue { None, MalformedCode, DuplicateField };

struct Row {
    QString code;
    QString description;
    QString handler;
    bool hasCode = false;
    bool hasDescription = false;
    bool hasHandler = false;
    bool hasEnable = false;
    bool hasEnableTypo = false;
    bool descriptionQuotesRecovered = false;
    RowIssue issue = RowIssue::None;
};

bool hexChar(QChar ch) {
    const ushort value = ch.unicode();
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F');
}
bool allHex(const QString& text) {
    for (const auto ch : text) if (!hexChar(ch)) return false;
    return !text.isEmpty();
}

void warn(ImportResult& result, const QString& message) {
    if (result.warnings.size() < MaxWarnings && !result.warnings.contains(message))
        result.warnings.append(message);
}

bool parseValue(QStringView source, QString& value, bool description, bool& recovered) {
    recovered = false;
    source = source.trimmed();
    if (source.isEmpty()) {
        value.clear();
        return true;
    }
    if (source.front() != QLatin1Char('"')) {
        const auto comment = source.indexOf(QLatin1Char('#'));
        if (comment >= 0) source = source.left(comment);
        value = source.trimmed().toString();
        return true;
    }

    // RetroArch descriptions in the wild contain literal, unescaped quotes.
    // Only descriptions may use the last unescaped quote as the terminator.
    qsizetype closing = -1;
    bool escaped = false;
    for (qsizetype i = 1; i < source.size(); ++i) {
        const auto ch = source[i];
        if (escaped) escaped = false;
        else if (ch == QLatin1Char('\\')) escaped = true;
        else if (ch == QLatin1Char('"')) {
            closing = i;
            if (!description) break;
        }
    }
    if (closing < 0) return false;
    const auto trailing = source.mid(closing + 1).trimmed();
    if (!trailing.isEmpty() && trailing.front() != QLatin1Char('#')) return false;

    QString decoded;
    decoded.reserve(closing - 1);
    escaped = false;
    for (qsizetype i = 1; i < closing; ++i) {
        const auto ch = source[i];
        if (escaped) {
            switch (ch.unicode()) {
            case 'n': decoded.append(QLatin1Char('\n')); break;
            case 'r': decoded.append(QLatin1Char('\r')); break;
            case 't': decoded.append(QLatin1Char('\t')); break;
            default: decoded.append(ch); break;
            }
            escaped = false;
        } else if (ch == QLatin1Char('\\')) {
            escaped = true;
        } else {
            if (ch == QLatin1Char('"')) recovered = true;
            decoded.append(ch);
        }
    }
    if (escaped) return false;
    value = std::move(decoded);
    return true;
}

bool readText(const QString& path, QString& content, bool& latin1Fallback, QString& error) {
    latin1Fallback = false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = Localization::text(QStringLiteral("Не удалось прочитать файл RetroArch: %1"))
                    .arg(file.errorString());
        return false;
    }
    if (file.size() > MaxImportBytes) {
        error = Localization::text(QStringLiteral("Файл RetroArch .cht слишком большой (максимум 2 МиБ)."));
        return false;
    }
    const QByteArray bytes = file.read(MaxImportBytes + 1);
    if (bytes.size() > MaxImportBytes) {
        error = Localization::text(QStringLiteral("Файл RetroArch .cht слишком большой (максимум 2 МиБ)."));
        return false;
    }
    if (!file.atEnd() || file.error() != QFileDevice::NoError) {
        error = Localization::text(QStringLiteral("Файл RetroArch .cht прочитан не полностью."));
        return false;
    }

    QStringDecoder::Encoding encoding = QStringDecoder::Utf8;
    qsizetype offset = 0;
    if (bytes.startsWith("\xEF\xBB\xBF")) {
        offset = 3;
    } else if (bytes.startsWith("\xFF\xFE")) {
        encoding = QStringDecoder::Utf16LE;
        offset = 2;
    } else if (bytes.startsWith("\xFE\xFF")) {
        encoding = QStringDecoder::Utf16BE;
        offset = 2;
    }
    QStringDecoder decoder(encoding);
    content = decoder.decode(QByteArrayView(bytes.constData() + offset, bytes.size() - offset));
    if (decoder.hasError() && offset == 0) {
        content = QString::fromLatin1(bytes);
        latin1Fallback = true;
    } else if (decoder.hasError()) {
        error = Localization::text(QStringLiteral("Неверная кодировка файла RetroArch .cht."));
        return false;
    }
    if (content.contains(QChar::Null)) {
        error = Localization::text(QStringLiteral("Неверная кодировка файла RetroArch .cht."));
        return false;
    }
    return true;
}

bool parseIndex(const QString& key, int& index, QString& field) {
    if (!key.startsWith(QStringLiteral("cheat"))) return false;
    const auto separator = key.indexOf(QLatin1Char('_'), 5);
    if (separator <= 5) return false;
    const auto number = QStringView(key).mid(5, separator - 5);
    for (const auto digit : number) {
        if (!digit.isDigit() || digit.unicode() > '9') return false;
    }
    bool ok = false;
    index = number.toInt(&ok);
    if (!ok) index = MaxIndex + 1;
    field = key.mid(separator + 1);
    return !field.isEmpty();
}

bool parseCount(const QString& value, int& count) {
    if (value.isEmpty()) return false;
    for (const auto digit : value) {
        if (!digit.isDigit() || digit.unicode() > '9') return false;
    }
    bool ok = false;
    count = value.toInt(&ok);
    return ok && count >= 0 && count <= MaxIndex + 1;
}

} // namespace

bool importCht(const QString& path, Platform platform, ImportResult& result, QString& error) {
    error.clear();
    QString content;
    bool latin1Fallback = false;
    if (!readText(path, content, latin1Fallback, error)) return false;

    QMap<int, Row> rows;
    int declaredCount = -1;
    qsizetype position = 0;
    int lineNumber = 0;
    while (position < content.size()) {
        auto next = content.indexOf(QLatin1Char('\n'), position);
        if (next < 0) next = content.size();
        auto line = QStringView(content).mid(position, next - position).trimmed();
        position = next + 1;
        ++lineNumber;
        if (line.isEmpty() || line.front() == QLatin1Char('#') ||
            line.front() == QLatin1Char(';')) continue;

        const auto equal = line.indexOf(QLatin1Char('='));
        if (equal <= 0) {
            error = Localization::text(QStringLiteral("Строка %1: неверная запись RetroArch .cht."))
                        .arg(lineNumber);
            return false;
        }
        const QString key = line.left(equal).trimmed().toString().toLower();
        int index = -1;
        QString field;
        const bool cheatField = parseIndex(key, index, field);
        if (cheatField && index > MaxIndex) {
            error = Localization::text(QStringLiteral("Строка %1: индекс чита вне диапазона 0–4095."))
                        .arg(lineNumber);
            return false;
        }
        QString value;
        bool recovered = false;
        if (!parseValue(line.mid(equal + 1), value,
                        cheatField && field == QStringLiteral("desc"), recovered)) {
            if (cheatField && field == QStringLiteral("code")) {
                rows[index].issue = RowIssue::MalformedCode;
                continue;
            }
            error = Localization::text(QStringLiteral("Строка %1: неверная запись RetroArch .cht."))
                        .arg(lineNumber);
            return false;
        }
        if (key == QStringLiteral("cheats")) {
            if (declaredCount >= 0 || !parseCount(value, declaredCount)) {
                error = Localization::text(QStringLiteral("Неверное число cheats в файле RetroArch .cht."));
                return false;
            }
            continue;
        }
        if (!cheatField) continue;
        auto& row = rows[index];
        bool* present = nullptr;
        QString* destination = nullptr;
        if (field == QStringLiteral("code")) {
            present = &row.hasCode;
            destination = &row.code;
        } else if (field == QStringLiteral("desc")) {
            present = &row.hasDescription;
            destination = &row.description;
        } else if (field == QStringLiteral("handler")) {
            present = &row.hasHandler;
            destination = &row.handler;
        } else if (field == QStringLiteral("enable")) {
            present = &row.hasEnable;
        } else if (field == QStringLiteral("enanble")) {
            present = &row.hasEnableTypo;
        }
        if (!present) continue;
        if (*present) {
            row.issue = RowIssue::DuplicateField;
            continue;
        }
        *present = true;
        if (destination) *destination = std::move(value);
        if (field == QStringLiteral("desc"))
            row.descriptionQuotesRecovered = recovered;
    }

    if (declaredCount < 0 || (declaredCount > 0 && rows.isEmpty())) {
        error = Localization::text(QStringLiteral("Файл не похож на RetroArch .cht: отсутствуют записи cheats."));
        return false;
    }

    ImportResult imported;
    if (latin1Fallback)
        warn(imported, Localization::text(QStringLiteral(
                 "Файл без BOM прочитан как Latin-1 из-за неверной кодировки UTF-8.")));
    if (declaredCount != rows.size()) {
        warn(imported, Localization::text(QStringLiteral("В файле указано %1 читов, найдено %2."))
                           .arg(declaredCount).arg(rows.size()));
    }
    imported.entries.reserve(rows.size());
    QSet<QString> seenCodes;
    for (auto it = rows.cbegin(); it != rows.cend(); ++it) {
        const int number = it.key();
        const Row& row = it.value();
        if (row.descriptionQuotesRecovered)
            warn(imported, Localization::text(QStringLiteral(
                     "Чит %1: восстановлены кавычки в описании.")).arg(number));
        if (row.issue != RowIssue::None) {
            ++imported.skipped;
            warn(imported, row.issue == RowIssue::MalformedCode
                 ? Localization::text(QStringLiteral(
                       "Чит %1: незакрытая кавычка в коде; запись пропущена.")).arg(number)
                 : Localization::text(QStringLiteral(
                       "Чит %1: повторное поле; неоднозначная запись пропущена.")).arg(number));
            continue;
        }
        if (row.hasEnableTypo) {
            warn(imported, Localization::text(QStringLiteral(
                     "Чит %1: поле enanble проигнорировано; импортирован выключенным.")).arg(number));
        }
        if (row.hasHandler && row.handler.trimmed() != QStringLiteral("0")) {
            ++imported.skipped;
            warn(imported, Localization::text(QStringLiteral(
                     "Чит %1: обработчик памяти или вибрации не поддерживается.")).arg(number));
            continue;
        }
        if (!row.hasCode || row.code.trimmed().isEmpty()) {
            ++imported.skipped;
            warn(imported, Localization::text(QStringLiteral("Чит %1: код отсутствует.")).arg(number));
            continue;
        }
        QString description = row.description.simplified();
        if (description.isEmpty()) {
            description = Localization::text(QStringLiteral("Чит %1")).arg(number + 1);
            warn(imported, Localization::text(QStringLiteral(
                     "Чит %1: описание отсутствует; задано имя по номеру.")).arg(number));
        } else if (description.size() > MaxDescriptionLength) {
            description.truncate(MaxDescriptionLength);
            warn(imported, Localization::text(QStringLiteral(
                     "Чит %1: описание сокращено до %2 символов."))
                     .arg(number).arg(MaxDescriptionLength));
        }

        if (row.code.size() > MaxImportedCodeLength) {
            ++imported.skipped;
            warn(imported, Localization::text(QStringLiteral(
                     "Чит %1: неверный или неподдерживаемый код.")).arg(number));
            continue;
        }
        const QStringList parts = row.code.split(QLatin1Char('+'), Qt::KeepEmptyParts);
        QStringList canonicalParts;
        canonicalParts.reserve(parts.size());
        for (const auto& part : parts) canonicalParts.append(part.trimmed().toUpper());
        bool converted = false;
        if (platform == Platform::MasterSystem) {
            bool triples = !canonicalParts.isEmpty() && canonicalParts.size() % 3 == 0;
            for (const auto& part : canonicalParts)
                triples = triples && part.size() == 3 && allHex(part);
            if (triples) {
                QStringList rebuilt;
                rebuilt.reserve(canonicalParts.size() / 3);
                for (qsizetype i = 0; i < canonicalParts.size(); i += 3)
                    rebuilt.append(canonicalParts[i] + QLatin1Char('-') +
                                   canonicalParts[i + 1] + QLatin1Char('-') +
                                   canonicalParts[i + 2]);
                canonicalParts = std::move(rebuilt);
                converted = true;
            }
        } else {
            for (auto& part : canonicalParts) {
                if (part.size() != 9 || part[6] != QLatin1Char(':') ||
                    !allHex(part.left(6)) || !allHex(part.right(2))) continue;
                const quint32 address = part.left(6).toUInt(nullptr, 16);
                if (address < 0xff0000u) continue;
                const auto native = ramCode(address, 1, true, part.right(2).toUInt(nullptr, 16));
                if (!native.isEmpty()) {
                    part = native;
                    converted = true;
                }
            }
        }
        const QString code = canonicalParts.join(QLatin1Char('+'));
        if (code.size() > MaxImportedCodeLength) {
            ++imported.skipped;
            warn(imported, Localization::text(QStringLiteral(
                     "Чит %1: неверный или неподдерживаемый код.")).arg(number));
            continue;
        }

        CheatList normalized;
        QString normalizeError;
        const CheatList candidate{{code, description, false}};
        if (!normalize(candidate, platform, normalized, normalizeError) || normalized.size() != 1) {
            ++imported.skipped;
            warn(imported, Localization::text(QStringLiteral(
                     "Чит %1: неверный или неподдерживаемый код.")).arg(number));
            continue;
        }
        if (seenCodes.contains(normalized.front().code)) {
            ++imported.skipped;
            warn(imported, Localization::text(QStringLiteral(
                     "Чит %1: такой набор кодов уже импортирован.")).arg(number));
            continue;
        }
        seenCodes.insert(normalized.front().code);
        imported.entries.append(normalized.front());
        if (converted)
            warn(imported, Localization::text(QStringLiteral("Формат кода приведён к формату ядра.")));
    }
    result = std::move(imported);
    return true;
}

bool exportCht(const QString& path, Platform platform, const CheatList& entries, QString& error) {
    error.clear();
    CheatList normalized;
    if (!normalize(entries, platform, normalized, error)) return false;

    const auto quoted = [](const QString& value) {
        QString escaped = value;
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        return escaped.toUtf8();
    };
    QByteArray bytes = "cheats = ";
    bytes += QByteArray::number(normalized.size());
    bytes += "\n\n";
    for (qsizetype i = 0; i < normalized.size(); ++i) {
        const auto prefix = QByteArray("cheat") + QByteArray::number(i);
        const auto& entry = normalized[i];
        bytes += prefix + "_desc = \"" + quoted(entry.description) + "\"\n";
        bytes += prefix + "_code = \"" + quoted(entry.code) + "\"\n";
        bytes += prefix + (entry.enabled ? "_enable = true\n\n" : "_enable = false\n\n");
        if (bytes.size() > MaxImportBytes) {
            error = Localization::text(QStringLiteral("Файл RetroArch .cht превышает 2 МиБ."));
            return false;
        }
    }
    QFile current(path);
    if (current.open(QIODevice::ReadOnly) && current.size() == bytes.size() &&
        current.readAll() == bytes) return true;
    current.close();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = Localization::text(QStringLiteral("Не удалось создать папку для файла RetroArch .cht."));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = Localization::text(QStringLiteral("Не удалось сохранить файл RetroArch .cht: %1"))
                    .arg(file.errorString());
        return false;
    }
    return true;
}

} // namespace Cheats
