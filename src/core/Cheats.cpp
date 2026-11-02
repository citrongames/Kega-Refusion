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
#include <utility>

namespace Cheats {
bool normalize(const CheatList& entries, Platform platform, CheatList& normalized, QString& error) {
    error.clear();
    if (entries.size() > MaxCodes) {
        error = QStringLiteral("Можно сохранить не больше %1 кодов.").arg(MaxCodes); return false;
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
    QSet<QString> codes;
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const auto code = entry.code.trimmed().toUpper();
        const auto description = entry.description.trimmed();
        if (code.size() > 11 || !pattern.match(code).hasMatch()) {
            error = QStringLiteral("Строка %1: неверный формат кода. Один код на строку.").arg(i+1); return false;
        }
        if (description.size() > MaxDescriptionLength || description.contains(QChar::Null) ||
            description.contains(QLatin1Char('\n')) || description.contains(QLatin1Char('\r'))) {
            error = QStringLiteral("Строка %1: описание должно занимать одну строку, до %2 символов.")
                .arg(i+1).arg(MaxDescriptionLength); return false;
        }
        if (codes.contains(code)) {
            error = QStringLiteral("Строка %1: этот код уже есть в списке.").arg(i+1); return false;
        }
        codes.insert(code); result.append({code,description,entry.enabled});
    }
    normalized = std::move(result);
    return true;
}
QString supportText(Platform platform) {
    if (platform == Platform::MasterSystem)
        return QStringLiteral("SMS / Game Gear / SG-1000: Game Genie XXX-XXX или XXX-XXX-XXX; "
                              "Action Replay XXXX-XXXX; Fusion AAAA:DD или RRAAAA:DD.");
    QString text = QStringLiteral("Game Genie XXXX-XXXX; Action Replay / прямой код AAAAAA:DDDD.");
    if (platform == Platform::SegaCd || platform == Platform::Cd32x)
        text += QStringLiteral("\nSega CD: Work RAM FFxxxx, PRG-RAM 000000–07FFFF, Word-RAM (2M) 200000–23FFFF.");
    if (platform == Platform::Sega32x)
        text += QStringLiteral("\n32X: коды 68000 для ROM и Work RAM. Память SH-2 не поддерживается.");
    else if (platform == Platform::Cd32x)
        text += QStringLiteral("\nCD32X: память SH-2 не поддерживается.");
    return text;
}
void Store::clear() { path_.clear(); entries_.clear(); }
bool Store::load(const QString& path, Platform platform, QString& error) {
    clear(); path_ = path; platform_ = platform; error.clear();
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        const QFileInfo info(path_);
        if (!info.exists() && !info.isSymLink()) return true;
        error = QStringLiteral("Не удалось прочитать читы: %1").arg(file.errorString()); return false;
    }
    if (file.size() > MaxFileBytes) {
        error = QStringLiteral("Файл читов слишком большой (максимум 128 КиБ)."); return false;
    }
    const auto bytes = file.read(MaxFileBytes+1);
    if (bytes.size() > MaxFileBytes || !file.atEnd() || file.error() != QFileDevice::NoError) {
        error = QStringLiteral("Файл читов прочитан не полностью или превышает 128 КиБ."); return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes,&parseError);
    const auto object = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        object.value(QStringLiteral("version")).toInt(-1) != 1 || !object.value(QStringLiteral("codes")).isArray()) {
        error = QStringLiteral("Неверный формат файла читов: %1").arg(path_); return false;
    }
    const auto codes = object.value(QStringLiteral("codes")).toArray();
    if (codes.size() > MaxCodes) {
        error = QStringLiteral("В файле больше %1 кодов.").arg(MaxCodes); return false;
    }
    CheatList decoded;
    decoded.reserve(codes.size());
    for (const auto& value : codes) {
        const auto code = value.toObject();
        if (!value.isObject() || !code.value(QStringLiteral("code")).isString() ||
            !code.value(QStringLiteral("description")).isString() || !code.value(QStringLiteral("enabled")).isBool()) {
            error = QStringLiteral("Неверная запись в файле читов."); return false;
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
    if (path_.isEmpty()) { error = QStringLiteral("Не выбрана игра для читов."); return false; }
    QJsonArray codes;
    for (const auto& entry : normalized)
        codes.append(QJsonObject{{QStringLiteral("code"),entry.code},
                                 {QStringLiteral("description"),entry.description},{QStringLiteral("enabled"),entry.enabled}});
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"),1},{QStringLiteral("codes"),codes}})
        .toJson(QJsonDocument::Indented);
    if (bytes.size() > MaxFileBytes) { error = QStringLiteral("Файл читов превышает 128 КиБ."); return false; }
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
        error = QStringLiteral("Не удалось создать папку читов."); return false;
    }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = QStringLiteral("Не удалось сохранить читы: %1").arg(file.errorString()); return false;
    }
    entries_ = std::move(normalized);
    return true;
}
}
