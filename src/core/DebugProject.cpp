#include "DebugProject.h"
#include "i18n/Localization.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <limits>
#include <utility>

namespace {
constexpr qint64 MaxBytes = 2 * 1024 * 1024;
constexpr int Version = 1;
constexpr int MaxBreakpoints = 128;
constexpr int MaxSymbols = 4096;
constexpr int MaxGameLength = 256;
constexpr int MaxLabelLength = 96;
constexpr int MaxCommentLength = 512;
constexpr int MaxConditionLength = 256;
constexpr int MaxWatchNameLength = 128;

const auto formatName = QLatin1String("KegaRefusionDebugProject");
const auto breakpointError = QT_TRANSLATE_NOOP("DebugProject", "Некорректная точка останова №%1.");
const auto symbolError = QT_TRANSLATE_NOOP("DebugProject", "Некорректная метка, заметка или закладка №%1.");
const auto watchError = QT_TRANSLATE_NOOP("DebugProject", "Некорректное наблюдение №%1.");

QString tr(const char* source) {
    return Localization::text(QString::fromUtf8(source));
}

bool validText(const QString& text, qsizetype limit) {
    return text.size() <= limit && text.isValidUtf16();
}

bool validAddress(quint32 cpu, quint32 address) {
    if ((cpu >> 8) > 1) return false;
    switch (cpu & 0xff) {
    case KR_DEBUG_M68K:
    case KR_DEBUG_SUB68K:
        return address <= 0xffffff;
    case KR_DEBUG_Z80:
    case KR_DEBUG_SSP1601:
        return address <= 0xffff;
    case KR_DEBUG_SH2_MASTER:
    case KR_DEBUG_SH2_SLAVE:
        return true;
    default:
        return false;
    }
}

bool validBreakpoint(const DebuggerBreakpoint& bp) {
    return bp.id && bp.events && !(bp.events & ~(KR_DEBUG_EXEC | KR_DEBUG_READ | KR_DEBUG_WRITE)) &&
           bp.first <= bp.last && validAddress(bp.cpu, bp.last) &&
           validText(bp.condition, MaxConditionLength);
}

bool validSymbol(const DebugProjectSymbol& symbol) {
    return validAddress(symbol.cpu, symbol.address) && validText(symbol.label, MaxLabelLength) &&
           validText(symbol.comment, MaxCommentLength) &&
           (symbol.bookmarked || !symbol.label.trimmed().isEmpty() || !symbol.comment.trimmed().isEmpty());
}

bool validWatch(const DebugWatchEntry& watch) {
    return watch.id && watch.instance <= 1 && watch.domain >= KR_MEM_ROM && watch.domain <= KR_MEM_SVP_DRAM &&
           (watch.width == 1 || watch.width == 2 || watch.width == 4) &&
           quint64(watch.offset) + watch.width - 1 <= std::numeric_limits<quint32>::max() &&
           watch.format >= DebugWatchEntry::Hex && watch.format <= DebugWatchEntry::Binary &&
           watch.order >= DebugWatchEntry::Native && watch.order <= DebugWatchEntry::Little &&
           validText(watch.name, MaxWatchNameLength);
}

bool checkCounts(qsizetype breakpoints, qsizetype symbols, qsizetype watches, QString& error) {
    if (breakpoints <= MaxBreakpoints && symbols <= MaxSymbols && watches <= DebugWatchLimit) return true;
    error = tr("Слишком много записей в проекте отладчика.");
    return false;
}

template <typename Entry, typename Key>
bool validateEntries(const QVector<Entry>& entries, bool (*valid)(const Entry&), Key key,
                     const char* errorText, QString& error) {
    QSet<quint64> keys;
    keys.reserve(entries.size());
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const quint64 id = key(entry);
        if (!valid(entry) || keys.contains(id)) {
            error = tr(errorText).arg(i + 1);
            return false;
        }
        keys.insert(id);
    }
    return true;
}

bool validate(const DebugProject& project, QString& error) {
    if (!checkCounts(project.breakpoints.size(), project.symbols.size(), project.watches.size(), error))
        return false;
    if (!validText(project.game, MaxGameLength)) {
        error = tr("Некорректное название игры в проекте отладчика.");
        return false;
    }
    return validateEntries(project.breakpoints, validBreakpoint,
                           [](const auto& bp) { return bp.id; }, breakpointError, error) &&
           validateEntries(project.symbols, validSymbol,
                           [](const auto& symbol) { return (quint64(symbol.cpu) << 32) | symbol.address; },
                           symbolError, error) &&
           validateEntries(project.watches, validWatch,
                           [](const auto& watch) { return watch.id; }, watchError, error);
}

// All numeric fields are integer tokens in version 1. Check before building
// the JSON tree so a decimal cannot round to an integer in the Qt parser.
bool integerTokens(const QByteArray& bytes) {
    bool quoted = false;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const char ch = bytes.at(i);
        if (ch == '"') {
            quoted = !quoted;
        } else if (quoted) {
            if (ch == '\\') ++i;
        } else {
            const char previous = i ? bytes.at(i - 1) : 0;
            const bool exponent = (ch == 'e' || ch == 'E') && previous >= '0' && previous <= '9';
            if (ch == '.' || exponent) return false;
        }
    }
    return true;
}

bool readUInt(const QJsonObject& row, const char* key, quint32& out) {
    const auto value = row.value(QLatin1String(key));
    const auto number = value.toInteger(-1);
    if (!value.isDouble() || number < 0 || number > std::numeric_limits<quint32>::max()) return false;
    out = quint32(number);
    return true;
}

bool readText(const QJsonObject& row, const char* key, QString& out) {
    const auto value = row.value(QLatin1String(key));
    if (!value.isString()) return false;
    out = value.toString();
    return true;
}

bool readBool(const QJsonObject& row, const char* key, bool& out) {
    const auto value = row.value(QLatin1String(key));
    if (!value.isBool()) return false;
    out = value.toBool();
    return true;
}

bool readIgnoreHits(const QJsonObject& row, quint64& out) {
    QString text;
    if (!readText(row, "ignoreHits", text) || text.isEmpty() || text.size() > 20 ||
        !std::all_of(text.cbegin(), text.cend(), [](QChar c) {
            return c >= QLatin1Char('0') && c <= QLatin1Char('9');
        })) return false;
    bool ok = false;
    out = text.toULongLong(&ok, 10);
    return ok;
}

bool readBreakpoint(const QJsonObject& row, DebuggerBreakpoint& bp) {
    return row.size() == 9 && readUInt(row, "id", bp.id) && readUInt(row, "cpu", bp.cpu) &&
           readUInt(row, "events", bp.events) && readUInt(row, "first", bp.first) &&
           readUInt(row, "last", bp.last) && readText(row, "condition", bp.condition) &&
           readIgnoreHits(row, bp.ignoreHits) && readBool(row, "enabled", bp.enabled) &&
           readBool(row, "oneShot", bp.oneShot);
}

bool readSymbol(const QJsonObject& row, DebugProjectSymbol& symbol) {
    return row.size() == 5 && readUInt(row, "cpu", symbol.cpu) && readUInt(row, "address", symbol.address) &&
           readText(row, "label", symbol.label) && readText(row, "comment", symbol.comment) &&
           readBool(row, "bookmarked", symbol.bookmarked);
}

bool readWatch(const QJsonObject& row, DebugWatchEntry& watch) {
    quint32 format = 0, order = 0;
    if (row.size() != 8 || !readUInt(row, "id", watch.id) || !readUInt(row, "instance", watch.instance) ||
        !readUInt(row, "domain", watch.domain) || !readUInt(row, "offset", watch.offset) ||
        !readUInt(row, "width", watch.width) || !readUInt(row, "format", format) ||
        !readUInt(row, "order", order) || !readText(row, "name", watch.name) ||
        format > DebugWatchEntry::Binary || order > DebugWatchEntry::Little) return false;
    watch.format = DebugWatchEntry::Format(format);
    watch.order = DebugWatchEntry::Order(order);
    return true;
}

template <typename Entry>
bool readEntries(const QJsonArray& rows, QVector<Entry>& entries,
                 bool (*read)(const QJsonObject&, Entry&), const char* errorText, QString& error) {
    entries.reserve(rows.size());
    for (const auto row : rows) {
        Entry entry{};
        if (!row.isObject() || !read(row.toObject(), entry)) {
            error = tr(errorText).arg(entries.size() + 1);
            return false;
        }
        entries.push_back(std::move(entry));
    }
    return true;
}

QByteArray encode(const DebugProject& project) {
    QJsonArray breakpoints, symbols, watches;
    for (const auto& bp : project.breakpoints) {
        breakpoints.append(QJsonObject{
            {"id", qint64(bp.id)}, {"cpu", qint64(bp.cpu)}, {"events", qint64(bp.events)},
            {"first", qint64(bp.first)}, {"last", qint64(bp.last)}, {"condition", bp.condition},
            {"ignoreHits", QString::number(bp.ignoreHits)}, {"enabled", bp.enabled}, {"oneShot", bp.oneShot},
        });
    }
    for (const auto& symbol : project.symbols) {
        symbols.append(QJsonObject{
            {"cpu", qint64(symbol.cpu)}, {"address", qint64(symbol.address)}, {"label", symbol.label},
            {"comment", symbol.comment}, {"bookmarked", symbol.bookmarked},
        });
    }
    for (const auto& watch : project.watches) {
        watches.append(QJsonObject{
            {"id", qint64(watch.id)}, {"instance", qint64(watch.instance)}, {"domain", qint64(watch.domain)},
            {"offset", qint64(watch.offset)}, {"width", qint64(watch.width)}, {"format", int(watch.format)},
            {"order", int(watch.order)}, {"name", watch.name},
        });
    }
    return QJsonDocument(QJsonObject{
        {"format", QString(formatName)}, {"version", Version}, {"game", project.game},
        {"breakpoints", breakpoints}, {"symbols", symbols}, {"watches", watches},
    }).toJson(QJsonDocument::Compact);
}
} // namespace

bool loadDebugProject(const QString& path, DebugProject& result, QString& error) {
    error.clear();
    if (!QFileInfo(path).isFile()) {
        error = tr("Файл проекта отладчика недоступен.");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = tr("Не удалось открыть проект отладчика (%1).").arg(file.errorString());
        return false;
    }
    if (file.size() > MaxBytes) {
        error = tr("Файл проекта отладчика превышает 2 МиБ.");
        return false;
    }
    const auto bytes = file.read(MaxBytes + 1);
    if (bytes.size() > MaxBytes) {
        error = tr("Файл проекта отладчика превышает 2 МиБ.");
        return false;
    }
    if (!file.atEnd() || file.error() != QFileDevice::NoError) {
        error = tr("Не удалось прочитать проект отладчика (%1).").arg(file.errorString());
        return false;
    }

    const auto root = integerTokens(bytes) ? QJsonDocument::fromJson(bytes).object() : QJsonObject{};
    if (root.value("format") != QString(formatName)) {
        error = tr("Некорректный формат проекта отладчика.");
        return false;
    }
    quint32 version;
    if (!readUInt(root, "version", version) || version != Version) {
        error = tr("Версия проекта отладчика не поддерживается.");
        return false;
    }
    DebugProject project;
    const auto breakpoints = root.value("breakpoints");
    const auto symbols = root.value("symbols");
    const auto watches = root.value("watches");
    if (root.size() != 6 || !readText(root, "game", project.game) || !breakpoints.isArray() ||
        !symbols.isArray() || !watches.isArray()) {
        error = tr("Некорректные параметры проекта отладчика.");
        return false;
    }
    const auto bpRows = breakpoints.toArray();
    const auto symbolRows = symbols.toArray();
    const auto watchRows = watches.toArray();
    if (!checkCounts(bpRows.size(), symbolRows.size(), watchRows.size(), error) ||
        !readEntries(bpRows, project.breakpoints, readBreakpoint, breakpointError, error) ||
        !readEntries(symbolRows, project.symbols, readSymbol, symbolError, error) ||
        !readEntries(watchRows, project.watches, readWatch, watchError, error) ||
        !validate(project, error)) return false;
    result = std::move(project);
    return true;
}

bool saveDebugProject(const QString& path, const DebugProject& project, QString& error) {
    error.clear();
    if (!validate(project, error)) return false;
    const auto bytes = encode(project);
    if (bytes.size() > MaxBytes) {
        error = tr("Файл проекта отладчика превышает 2 МиБ.");
        return false;
    }

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = tr("Не удалось сохранить проект отладчика (%1).").arg(file.errorString());
        return false;
    }
    return true;
}
