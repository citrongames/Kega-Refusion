#include "i18n/Localization.h"
#include "MovieTape.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
constexpr char MovieMagic[8] = {'K','R','M','O','V','I','E','1'};
constexpr char CheckpointMagic[8] = {'K','R','M','C','H','K','0','1'};
constexpr quint32 LegacyFormatVersion = 1;
constexpr quint32 FormatVersion = 2;
constexpr qsizetype LegacyInputBytes = 4 + 3*2 + MaxGunPlayers*8 + 1;
constexpr qsizetype InputBytes = 4 + MaxControllerPlayers*2 + MaxGunPlayers*8 + 1;
static_assert(LegacyInputBytes == 35 && InputBytes == 45,
              "Changing the recording input layout requires a new format version");
bool validVersion(quint32 version) {
    return version == LegacyFormatVersion || version == FormatVersion;
}
qsizetype inputBytes(quint32 version) {
    return version == LegacyFormatVersion ? LegacyInputBytes : InputBytes;
}
unsigned buttonCount(quint32 version) {
    return version == LegacyFormatVersion ? 3u : MaxControllerPlayers;
}
// Header fields: magic, version, metadata length, initial length, run count,
// frame count, UUID, resume length and resume frame. The snapshot follows the
// initial state; all fields and payload are covered by the trailing SHA-256.
constexpr qsizetype MovieHeaderBytes = 8 + 4 + 4 + 4 + 4 + 8 + 16 + 4 + 8;
constexpr qsizetype CheckpointHeaderBytes = 8 + 4 + 8 + 16 + 32 + 4;
constexpr qsizetype HashBytes = 32;
constexpr qsizetype MaxMovieFileBytes = MovieHeaderBytes + MovieTape::MaxMetadataBytes +
    2*MovieTape::MaxInitialBytes + MovieTape::MaxRunBytes + HashBytes;
constexpr qsizetype MaxCheckpointBytes = CheckpointHeaderBytes + MovieTape::MaxRunBytes + HashBytes;

void add16(QByteArray& bytes, quint16 value) {
    const auto le = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char*>(&le), sizeof(le));
}
void add32(QByteArray& bytes, quint32 value) {
    const auto le = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char*>(&le), sizeof(le));
}
void add64(QByteArray& bytes, quint64 value) {
    const auto le = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char*>(&le), sizeof(le));
}
quint16 read16(const char* bytes) { return qFromLittleEndian<quint16>(bytes); }
quint32 read32(const char* bytes) { return qFromLittleEndian<quint32>(bytes); }
quint64 read64(const char* bytes) { return qFromLittleEndian<quint64>(bytes); }

QByteArray sha256(const char* data, qsizetype length) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(data, length));
    return hash.result();
}
QByteArray fingerprint(const QByteArray& metadata, const QByteArray& initial) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(metadata));
    hash.addData(QByteArrayView(initial));
    return hash.result();
}
void seal(QByteArray& bytes) { bytes.append(sha256(bytes.constData(), bytes.size())); }
bool validHash(const QByteArray& bytes) {
    return bytes.size() >= HashBytes &&
        sha256(bytes.constData(), bytes.size()-HashBytes) == bytes.right(HashBytes);
}
void addInput(QByteArray& bytes, quint32 count, const MovieInput& input, quint32 version) {
    add32(bytes, count);
    for (unsigned player = 0; player < buttonCount(version); ++player)
        add16(bytes, input.buttons[player]);
    for (quint64 aim : input.aim) add64(bytes, aim);
    bytes.append(char(input.commands));
}
MovieInput readInput(const char* bytes, quint32 version) {
    MovieInput input;
    const unsigned players = buttonCount(version);
    for (unsigned player = 0; player < players; ++player)
        input.buttons[player] = read16(bytes + 4 + player*2);
    const qsizetype aimOffset = 4 + qsizetype(players)*2;
    for (unsigned player = 0; player < input.aim.size(); ++player)
        input.aim[player] = read64(bytes + aimOffset + qsizetype(player)*8);
    input.commands = quint8(bytes[aimOffset + qsizetype(input.aim.size())*8]);
    return input;
}
bool inputValid(const MovieInput& input) {
    if (input.commands & ~MovieTape::SoftReset) return false;
    return std::all_of(input.aim.begin(), input.aim.end(), [](quint64 aim) {
        return (aim >> 33) == 0;
    });
}
bool writeAtomic(const QString& path, const QByteArray& bytes, QString& error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = Localization::text(QStringLiteral("Не удалось сохранить запись %1: %2")).arg(path, file.errorString());
        return false;
    }
    return true;
}
bool movieEnvelopeValid(const QByteArray& bytes) {
    if (bytes.size() < MovieHeaderBytes + HashBytes || bytes.size() > MaxMovieFileBytes ||
        std::memcmp(bytes.constData(), MovieMagic, 8) || !validVersion(read32(bytes.constData()+8)))
        return false;
    const quint32 version = read32(bytes.constData()+8);
    const qsizetype runBytes = inputBytes(version);
    const quint32 metadata = read32(bytes.constData()+12);
    const quint32 initial = read32(bytes.constData()+16);
    const quint32 runs = read32(bytes.constData()+20);
    const quint64 frames = read64(bytes.constData()+24);
    const quint32 resumeLength = read32(bytes.constData()+48);
    const quint64 resumeFrame = read64(bytes.constData()+52);
    if (metadata > MovieTape::MaxMetadataBytes || initial == 0 ||
        initial > MovieTape::MaxInitialBytes ||
        resumeLength > MovieTape::MaxInitialBytes ||
        (resumeLength == 0 && resumeFrame != 0) || resumeFrame > frames ||
        quint64(runs)*quint64(runBytes) > quint64(MovieTape::MaxRunBytes) ||
        frames > MovieTape::MaxFrames || (runs == 0) != (frames == 0)) return false;
    const quint64 expected = quint64(MovieHeaderBytes) + metadata + initial + resumeLength +
        quint64(runs)*quint64(runBytes) + HashBytes;
    return expected == quint64(bytes.size()) && validHash(bytes);
}
} // namespace

void MovieTape::clear() {
    metadata_ = {};
    metadataBytes_.clear(); initialState_.clear(); invalidateResume();
    fingerprint_.clear(); id_.clear();
    std::deque<Run>().swap(runs_);
    frameCount_ = 0;
}

qsizetype MovieTape::bytesUsed() const {
    return metadataBytes_.capacity() + initialState_.capacity() + resumeState_.capacity() +
        fingerprint_.capacity() +
        id_.capacity() + qsizetype(runs_.size()) * qsizetype(sizeof(Run) + 16);
}

bool MovieTape::setResumeState(const QByteArray& state, quint64 cursor, QString& error) {
    error.clear();
    if (!isValid() || state.isEmpty() || state.size() > MaxInitialBytes || cursor > frameCount_) {
        error = Localization::text(QStringLiteral("Недопустимое состояние для продолжения записи."));
        return false;
    }
    resumeState_ = state;
    resumeFrame_ = cursor;
    return true;
}

bool MovieTape::create(const QJsonObject& metadata, const QByteArray& initialState, QString& error) {
    error.clear();
    const QByteArray encoded = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    if (encoded.isEmpty() || encoded.size() > MaxMetadataBytes ||
        initialState.isEmpty() || initialState.size() > MaxInitialBytes) {
        error = Localization::text(QStringLiteral("Недопустимый размер описания или начального состояния записи."));
        return false;
    }
    clear();
    metadata_ = metadata;
    metadataBytes_ = encoded;
    initialState_ = initialState;
    fingerprint_ = fingerprint(metadataBytes_, initialState_);
    id_ = QUuid::createUuid().toRfc4122();
    return true;
}

bool MovieTape::parseRuns(const char* bytes, quint32 count, quint64 frames, quint32 version,
                          std::deque<Run>& out, QString& error) {
    if (count > quint32(MaxRuns) || frames > MaxFrames || (count == 0) != (frames == 0)) {
        error = Localization::text(QStringLiteral("Превышен предел длины записи."));
        return false;
    }
    quint64 end = 0;
    MovieInput previous;
    for (quint32 i = 0; i < count; ++i) {
        const char* entry = bytes + qsizetype(i)*inputBytes(version);
        const quint32 repeat = read32(entry);
        const MovieInput input = readInput(entry, version);
        if (!repeat || repeat > MaxFrames-end || !inputValid(input) ||
            (i && input == previous)) {
            error = Localization::text(QStringLiteral("Повреждённые данные ввода в записи."));
            return false;
        }
        end += repeat;
        out.push_back({input, end});
        previous = input;
    }
    if (end != frames) {
        error = Localization::text(QStringLiteral("Число кадров записи не совпадает с данными ввода."));
        return false;
    }
    return true;
}

void MovieTape::appendRuns(QByteArray& bytes, quint64 frames, quint32 version) const {
    quint64 start = 0;
    for (const Run& run : runs_) {
        if (start == frames) break;
        const quint64 end = std::min(run.endFrame, frames);
        addInput(bytes, quint32(end-start), run.input, version);
        start = end;
    }
}

bool MovieTape::load(const QString& path, QString& error) {
    error.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < MovieHeaderBytes+HashBytes ||
        file.size() > MaxMovieFileBytes) {
        error = Localization::text(QStringLiteral("Не удалось прочитать файл записи или превышен предел размера: %1")).arg(path);
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() != file.size() || !movieEnvelopeValid(bytes)) {
        error = Localization::text(QStringLiteral("Повреждённый или неподдерживаемый файл записи: %1")).arg(path);
        return false;
    }
    const quint32 version = read32(bytes.constData()+8);
    const quint32 metadataLength = read32(bytes.constData()+12);
    const quint32 initialLength = read32(bytes.constData()+16);
    const quint32 runCount = read32(bytes.constData()+20);
    const quint64 frames = read64(bytes.constData()+24);
    const quint32 resumeLength = read32(bytes.constData()+48);
    const quint64 resumeFrame = read64(bytes.constData()+52);
    if (runCount > quint32(MaxRuns)) {
        error = Localization::text(QStringLiteral("Превышен предел объёма ввода записи."));
        return false;
    }
    const QByteArray metadataBytes(bytes.constData()+MovieHeaderBytes, metadataLength);
    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(metadataBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        error = Localization::text(QStringLiteral("Повреждено описание записи."));
        return false;
    }
    const QByteArray initial(bytes.constData()+MovieHeaderBytes+metadataLength, initialLength);
    const QByteArray resume(bytes.constData()+MovieHeaderBytes+metadataLength+initialLength,
                            resumeLength);
    std::deque<Run> runs;
    if (!parseRuns(bytes.constData()+MovieHeaderBytes+metadataLength+initialLength+resumeLength,
                   runCount, frames, version, runs, error)) return false;
    clear();
    metadata_ = parsed.object(); metadataBytes_ = metadataBytes; initialState_ = initial;
    resumeState_ = resume; resumeFrame_ = resumeFrame;
    fingerprint_ = fingerprint(metadataBytes_, initialState_);
    id_ = QByteArray(bytes.constData()+32, 16);
    runs_.swap(runs); frameCount_ = frames;
    return true;
}

bool MovieTape::save(const QString& path, QString& error) const {
    error.clear();
    if (!isValid() || !path.endsWith(QStringLiteral(".krm"), Qt::CaseInsensitive)) {
        error = Localization::text(QStringLiteral("Для записи нужен путь файла .krm."));
        return false;
    }
    QByteArray bytes;
    bytes.reserve(MovieHeaderBytes + metadataBytes_.size() + initialState_.size() +
                  resumeState_.size() +
                  qsizetype(runs_.size())*InputBytes + HashBytes);
    bytes.append(MovieMagic, 8);
    add32(bytes, FormatVersion);
    add32(bytes, quint32(metadataBytes_.size()));
    add32(bytes, quint32(initialState_.size()));
    add32(bytes, quint32(runs_.size()));
    add64(bytes, frameCount_);
    bytes.append(id_);
    add32(bytes, quint32(resumeState_.size()));
    add64(bytes, resumeFrame_);
    bytes.append(metadataBytes_);
    bytes.append(initialState_);
    bytes.append(resumeState_);
    appendRuns(bytes, frameCount_, FormatVersion);
    seal(bytes);
    if (QFileInfo::exists(path)) {
        QFile old(path);
        if (!old.open(QIODevice::ReadOnly) || old.size() > MaxMovieFileBytes || old.size() < 0) {
            error = Localization::text(QStringLiteral("Не удалось проверить прежний файл записи: %1")).arg(path);
            return false;
        }
        const QByteArray previous = old.readAll();
        if (previous.size() != old.size() || !movieEnvelopeValid(previous)) {
            error = Localization::text(QStringLiteral("Прежний файл записи повреждён; он оставлен без изменений: %1")).arg(path);
            return false;
        }
        if (QByteArray(previous.constData()+32, 16) != id_) {
            error = Localization::text(QStringLiteral("Файл содержит другую запись; выберите новое имя: %1")).arg(path);
            return false;
        }
        if (previous == bytes) return true;
        if (!writeAtomic(path + QStringLiteral(".bak"), previous, error)) return false;
    }
    return writeAtomic(path, bytes, error);
}

bool MovieTape::append(const MovieInput& input, QString& error) {
    error.clear();
    if (!isValid() || !inputValid(input)) {
        error = Localization::text(QStringLiteral("Некорректный ввод кадра записи."));
        return false;
    }
    if (frameCount_ >= MaxFrames) {
        error = Localization::text(QStringLiteral("Достигнут предел числа кадров записи."));
        return false;
    }
    if (!runs_.empty() && runs_.back().input == input) {
        ++runs_.back().endFrame; ++frameCount_;
        invalidateResume();
        return true;
    }
    if (runs_.size() >= size_t(MaxRuns)) {
        error = Localization::text(QStringLiteral("Достигнут предел памяти записи ввода."));
        return false;
    }
    runs_.push_back({input, frameCount_+1});
    ++frameCount_;
    invalidateResume();
    return true;
}

bool MovieTape::inputAt(quint64 frame, MovieInput& input) const {
    if (!isValid() || frame >= frameCount_) return false;
    const auto it = std::lower_bound(runs_.begin(), runs_.end(), frame+1,
        [](const Run& run, quint64 target) { return run.endFrame < target; });
    if (it == runs_.end()) return false;
    input = it->input;
    return true;
}

bool MovieTape::truncate(quint64 frameCount) {
    if (!isValid() || frameCount > frameCount_) return false;
    if (frameCount == frameCount_) return true;
    if (frameCount == 0) runs_.clear();
    else {
        const auto it = std::lower_bound(runs_.begin(), runs_.end(), frameCount,
            [](const Run& run, quint64 target) { return run.endFrame < target; });
        if (it == runs_.end()) return false;
        it->endFrame = frameCount;
        runs_.erase(std::next(it), runs_.end());
    }
    frameCount_ = frameCount;
    invalidateResume();
    return true;
}

QByteArray MovieTape::checkpoint(quint64 cursor, QString& error) const {
    error.clear();
    if (!isValid() || cursor > frameCount_) {
        error = Localization::text(QStringLiteral("Недопустимая позиция записи."));
        return {};
    }
    quint32 count = 0;
    for (const Run& run : runs_) {
        if (cursor == 0) break;
        ++count;
        if (run.endFrame >= cursor) break;
    }
    QByteArray bytes;
    bytes.reserve(CheckpointHeaderBytes + qsizetype(count)*InputBytes + HashBytes);
    bytes.append(CheckpointMagic, 8);
    add32(bytes, FormatVersion);
    add64(bytes, cursor);
    bytes.append(id_);
    bytes.append(fingerprint_);
    add32(bytes, count);
    appendRuns(bytes, cursor, FormatVersion);
    seal(bytes);
    return bytes;
}

bool MovieTape::prefixMatches(const std::deque<Run>& prefix, quint64 cursor) const {
    if (cursor > frameCount_ || prefix.size() > runs_.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (prefix[i].input != runs_[i].input ||
            prefix[i].endFrame != std::min(runs_[i].endFrame, cursor)) return false;
    }
    return true;
}

bool MovieTape::restoreCheckpoint(const QByteArray& bytes, quint64& cursor,
                                  bool readOnly, QString& error) {
    error.clear();
    if (!isValid() || bytes.size() < CheckpointHeaderBytes+HashBytes ||
        bytes.size() > MaxCheckpointBytes ||
        std::memcmp(bytes.constData(), CheckpointMagic, 8) ||
        !validVersion(read32(bytes.constData()+8)) || !validHash(bytes)) {
        error = Localization::text(QStringLiteral("Повреждена контрольная точка записи."));
        return false;
    }
    const quint32 version = read32(bytes.constData()+8);
    const qsizetype runBytes = inputBytes(version);
    const quint64 savedCursor = read64(bytes.constData()+12);
    const quint32 count = read32(bytes.constData()+68);
    if (QByteArray(bytes.constData()+20, 16) != id_ ||
        QByteArray(bytes.constData()+36, 32) != fingerprint_) {
        error = Localization::text(QStringLiteral("Контрольная точка относится к другой записи или игре."));
        return false;
    }
    if (count > quint32(MaxRuns) || savedCursor > MaxFrames ||
        quint64(bytes.size()) != quint64(CheckpointHeaderBytes) +
            quint64(count)*quint64(runBytes) + HashBytes) {
        error = Localization::text(QStringLiteral("Превышен предел контрольной точки записи."));
        return false;
    }
    std::deque<Run> prefix;
    if (!parseRuns(bytes.constData()+CheckpointHeaderBytes, count, savedCursor, version, prefix, error))
        return false;
    if (readOnly) {
        if (!prefixMatches(prefix, savedCursor)) {
            error = Localization::text(QStringLiteral("Контрольная точка не совпадает с текущей веткой воспроизведения."));
            return false;
        }
    } else {
        runs_.swap(prefix);
        frameCount_ = savedCursor;
        invalidateResume();
    }
    cursor = savedCursor;
    return true;
}

bool MovieTape::forkAt(quint64 frameCount, QString& error) {
    error.clear();
    if (!truncate(frameCount)) {
        error = Localization::text(QStringLiteral("Недопустимая позиция ответвления записи."));
        return false;
    }
    id_ = QUuid::createUuid().toRfc4122();
    invalidateResume();
    return true;
}
