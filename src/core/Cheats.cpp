#include "i18n/Localization.h"
#include "Cheats.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QtEndian>
#include <utility>

namespace Cheats {
bool normalize(const CheatList& entries, Platform platform, CheatList& normalized, QString& error) {
    error.clear();
    if (entries.size() > MaxEntries) {
        error = Localization::text(QStringLiteral("Можно сохранить не больше %1 кодов.")).arg(MaxEntries); return false;
    }
    // Formats from GPX f27655b libretro.c:decode_cheat. Require complete
    // payloads: its MD PAR decoder otherwise accepts truncated values.
    static const QRegularExpression md(QStringLiteral(
        "\\A(?:[ABCDEFGHJKLMNPRSTVWXYZ0-9]{4}-[ABCDEFGHJKLMNPRSTVWXYZ0-9]{4}|[0-9A-F]{6}:[0-9A-F]{4})\\z"));
    static const QRegularExpression sms(QStringLiteral(
        "\\A(?:[0-9A-F]{3}-[0-9A-F]{3}(?:-[0-9A-F]{3})?|[0-9A-F]{4}-[0-9A-F]{4}|[0-9A-F]{4}:[0-9A-F]{2}|[0-9A-F]{6}:[0-9A-F]{2})\\z"));
    const auto& pattern = platform == Platform::MasterSystem ? sms : md;
    CheatList result;
    result.reserve(entries.size());
    QSet<QString> codes, active;
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        auto code = entry.code.trimmed().toUpper();
        const auto description = entry.description.trimmed();
        if (code.isEmpty() || code.size() > MaxCodeLength) {
            error = Localization::text(QStringLiteral("Строка %1: неверный формат кода.")).arg(i+1); return false;
        }
        auto parts = code.split(QLatin1Char('+'));
        for (auto& part : parts) {
            part = part.trimmed();
            if (part.size() > 11 || !pattern.match(part).hasMatch()) {
                error = Localization::text(QStringLiteral("Строка %1: неверный формат кода.")).arg(i+1); return false;
            }
            if (entry.enabled) active.insert(part);
        }
        if (active.size() > MaxCodes) {
            error = Localization::text(QStringLiteral("Одновременно можно включить не больше %1 кодов с учётом составных читов.")).arg(MaxCodes); return false;
        }
        code = parts.join(QLatin1Char('+'));
        if (description.size() > MaxDescriptionLength || description.contains(QChar::Null) ||
            description.contains(QLatin1Char('\n')) || description.contains(QLatin1Char('\r'))) {
            error = Localization::text(QStringLiteral("Строка %1: описание должно занимать одну строку, до %2 символов."))
                .arg(i+1).arg(MaxDescriptionLength); return false;
        }
        if (codes.contains(code)) {
            error = Localization::text(QStringLiteral("Строка %1: этот код уже есть в списке.")).arg(i+1); return false;
        }
        codes.insert(code); result.append({code,description,entry.enabled});
    }
    normalized = std::move(result);
    return true;
}
QString ramCode(quint32 address, int width, bool bigEndian, quint32 value) {
    if ((width != 1 && width != 2 && width != 4) ||
        (width < 4 && value >= (quint32(1) << (width*8)))) return {};
    const quint32 base = bigEndian ? 0xff0000u : 0xc000u;
    const quint32 end = bigEndian ? 0x1000000u : 0xe000u;
    if (address < base || quint64(address)+width > end) return {};
    QStringList parts;
    for (int i=0; i<width; ++i) {
        quint32 target = address+quint32(i);
        // GPX native 68000 byte cheats address its word-swapped RAM buffer.
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
        if (bigEndian) target ^= 1u;
#endif
        const unsigned shift = unsigned(bigEndian ? width-1-i : i)*8;
        parts.append(QStringLiteral("%1:%2").arg(target,bigEndian?6:4,16,QLatin1Char('0'))
            .arg((value>>shift)&255u,bigEndian?4:2,16,QLatin1Char('0')).toUpper());
    }
    return parts.join(QLatin1Char('+'));
}
QString supportText(Platform platform) {
    if (platform == Platform::MasterSystem)
        return Localization::text(QStringLiteral("SMS / Game Gear / SG-1000: Game Genie XXX-XXX или XXX-XXX-XXX; "
                              "Action Replay XXXX-XXXX; Fusion AAAA:DD или RRAAAA:DD."));
    QString text = Localization::text(QStringLiteral("Game Genie XXXX-XXXX; Action Replay / прямой код AAAAAA:DDDD."));
    if (platform == Platform::SegaCd || platform == Platform::Cd32x)
        text += QStringLiteral("\nSega CD: Work RAM FFxxxx, PRG-RAM 000000–07FFFF, Word-RAM (2M) 200000–23FFFF.");
    if (platform == Platform::Sega32x)
        text += Localization::text(QStringLiteral("\n32X: коды 68000 для ROM и Work RAM. Память SH-2 не поддерживается."));
    else if (platform == Platform::Cd32x)
        text += Localization::text(QStringLiteral("\nCD32X: память SH-2 не поддерживается."));
    return text;
}
void Store::clear() { path_.clear(); entries_.clear(); }
bool Store::load(const QString& path, Platform platform, QString& error) {
    clear(); path_ = path; platform_ = platform; error.clear();
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        const QFileInfo info(path_);
        if (!info.exists() && !info.isSymLink()) return true;
        error = Localization::text(QStringLiteral("Не удалось прочитать читы: %1")).arg(file.errorString()); return false;
    }
    if (file.size() > MaxFileBytes) {
        error = Localization::text(QStringLiteral("Файл читов слишком большой (максимум 2 МиБ).")); return false;
    }
    const auto bytes = file.read(MaxFileBytes+1);
    if (bytes.size() > MaxFileBytes || !file.atEnd() || file.error() != QFileDevice::NoError) {
        error = Localization::text(QStringLiteral("Файл читов прочитан не полностью или превышает 2 МиБ.")); return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes,&parseError);
    const auto object = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        object.value(QStringLiteral("version")).toInt(-1) != 1 || !object.value(QStringLiteral("codes")).isArray()) {
        error = Localization::text(QStringLiteral("Неверный формат файла читов: %1")).arg(path_); return false;
    }
    const auto codes = object.value(QStringLiteral("codes")).toArray();
    if (codes.size() > MaxEntries) {
        error = Localization::text(QStringLiteral("В файле больше %1 кодов.")).arg(MaxEntries); return false;
    }
    CheatList decoded;
    decoded.reserve(codes.size());
    for (const auto& value : codes) {
        const auto code = value.toObject();
        if (!value.isObject() || !code.value(QStringLiteral("code")).isString() ||
            !code.value(QStringLiteral("description")).isString() || !code.value(QStringLiteral("enabled")).isBool()) {
            error = Localization::text(QStringLiteral("Неверная запись в файле читов.")); return false;
        }
        decoded.append({code.value(QStringLiteral("code")).toString(),
                        code.value(QStringLiteral("description")).toString(),code.value(QStringLiteral("enabled")).toBool()});
    }
    return normalize(decoded,platform_,entries_,error);
}
bool Store::set(const CheatList& entries, QString& error) {
    CheatList normalized;
    if (!normalize(entries,platform_,normalized,error)) return false;
    if (normalized == entries_) return true;
    if (path_.isEmpty()) { error = Localization::text(QStringLiteral("Не выбрана игра для читов.")); return false; }
    QJsonArray codes;
    for (const auto& entry : normalized)
        codes.append(QJsonObject{{QStringLiteral("code"),entry.code},
                                 {QStringLiteral("description"),entry.description},{QStringLiteral("enabled"),entry.enabled}});
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"),1},{QStringLiteral("codes"),codes}})
        .toJson(QJsonDocument::Indented);
    if (bytes.size() > MaxFileBytes) { error = Localization::text(QStringLiteral("Файл читов превышает 2 МиБ.")); return false; }
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
        error = Localization::text(QStringLiteral("Не удалось создать папку читов.")); return false;
    }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = Localization::text(QStringLiteral("Не удалось сохранить читы: %1")).arg(file.errorString()); return false;
    }
    entries_ = std::move(normalized);
    return true;
}
}
