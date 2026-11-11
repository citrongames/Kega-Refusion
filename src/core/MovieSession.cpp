#include "i18n/Localization.h"
#include "MovieSession.h"
#include "LibretroHost.h"
#include "VideoFrameGeometry.h"
#include "StateFile.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <utility>

namespace {
constexpr qsizetype MaxSnapshot = 32 * 1024 * 1024;
constexpr qsizetype MaxPrefix = 72 * 1024 * 1024;
constexpr qsizetype MaxPixels = 16 * 1024 * 1024;
constexpr qsizetype MaxSlot = MaxSnapshot + MaxPrefix + MaxPixels + 256;
QByteArray hash(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
// Failure recovery only: successful takeover keeps the current game/disc running.
void restorePreviousSession(LibretroHost& host, const QByteArray& state,
                            int disc, bool wasPrepared, QString& error) {
    QString recovery;
    bool restored = host.restoreMovieConfiguration(recovery);
    if (restored && host.loaded() && host.isCd() && host.discIndex() != disc)
        restored = host.selectDisc(disc, recovery);
    // Reapply the original region/options/ROM patches before restoring its state.
    if (restored && wasPrepared && host.loaded()) restored = host.hardReset(recovery,false);
    if (restored) restored = host.restoreMovieState(state, recovery);
    host.setMoviePlayback(false);
    if (!restored) error += Localization::text(QStringLiteral("\nНе удалось восстановить прежнюю сессию: %1")).arg(recovery);
}
void restoreLegacyClock(LibretroHost& host, const MovieTape& tape, quint64 cursor) {
    const double rate = tape.metadata().value(QStringLiteral("frameRate")).toDouble(host.fps());
    const quint64 frames = tape.framesSinceReset(cursor);
    const quint64 ticksPerFrame = quint64(std::llround(4294967296.0 / rate));
    host.setRunClock({frames,frames * ticksPerFrame});
}
void applyInput(LibretroHost& host, const MovieInput& input) {
    if (input.commands & MovieTape::SoftReset) host.reset();
    for (unsigned p = 0; p < input.buttons.size(); ++p)
        host.setButtons(p, input.buttons[p]);
    for (unsigned p = 0; p < input.aim.size(); ++p) {
        const quint64 aim = input.aim[p];
        if(host.controllerKind(p)==ControllerType::Mouse || host.controllerKind(p)==ControllerType::Paddle)
            host.setPointer(p,qint16(aim),qint16(aim>>16));
        else host.setGunAim(p, qint16(aim & 0xffff), qint16((aim >> 16) & 0xffff), bool((aim >> 32) & 1));
    }
}
}
MovieSession::MovieSession(LibretroHost& host, QString dataPath)
    : host_(host), dataPath_(std::move(dataPath)) {}

bool MovieSession::startRecording(const QString& path, QString& error) {
    if (active() || !host_.loaded()) { error = Localization::text(QStringLiteral("Сначала откройте игру и закончите текущую запись.")); return false; }
    if (!host_.flushSaveRam(error)) return false;
    const int previousDisc = host_.discIndex();
    const auto previous = host_.captureMovieState();
    if (previous.isEmpty()) { error = Localization::text(QStringLiteral("Не удалось сохранить текущее состояние перед началом записи.")); return false; }
    host_.setMoviePlayback(true); // Movie editing must not write imported/rewound battery data to normal saves.
    auto config = host_.movieConfiguration(true);
    // The current cadence can be stale while a user region change is pending.
    // Only an existing recording may pin replay to its saved frame rate.
    config.remove(QStringLiteral("frameRate"));
    if (!host_.prepareMovie(config, error)) {
        restorePreviousSession(host_, previous, previousDisc, false, error);
        return false;
    }
    config = host_.movieConfiguration(); // Power reset has applied the recorded region and actual frame rate.
    const auto initial = host_.captureMovieState();
    MovieTape candidate;
    // Normalize the record start through exactly the same restoration path as replay.
    if (initial.isEmpty() || !host_.restoreMovieState(initial, error) ||
        !candidate.create(config, initial, error) ||
        !candidate.setResumeState(initial, 0, error) || !candidate.save(path, error)) {
        restorePreviousSession(host_, previous, previousDisc, true, error);
        if (error.isEmpty()) error = Localization::text(QStringLiteral("Не удалось подготовить начальное состояние записи."));
        return false;
    }
    tape_ = std::move(candidate); path_ = QFileInfo(path).absoluteFilePath();
    cursor_ = 0; mode_ = Recording;
    return true;
}

bool MovieSession::play(const QString& path, bool resumeRecording, QString& error) {
    if (active() || !host_.loaded()) { error = Localization::text(QStringLiteral("Сначала откройте игру и закончите текущую запись.")); return false; }
    MovieTape candidate;
    if (!candidate.load(path, error)) return false;
    if (resumeRecording && candidate.resumeState().isEmpty()) {
        error = Localization::text(QStringLiteral("В этой записи нет точки продолжения. Воспроизведите его и выберите запись с нужного места.")); return false;
    }
    if (!host_.flushSaveRam(error)) return false;
    const int previousDisc = host_.discIndex();
    const auto previous = host_.captureMovieState();
    if (previous.isEmpty()) { error = Localization::text(QStringLiteral("Не удалось сохранить текущее состояние перед загрузкой записи.")); return false; }
    host_.setMoviePlayback(true); // Preparing replay must not flush imported movie battery data.
    const bool prepared = host_.prepareMovie(candidate.metadata(), error);
    bool restoredClock = false;
    if (!prepared || !host_.restoreMovieState(
            resumeRecording ? candidate.resumeState() : candidate.initialState(),
            error, false, &restoredClock)) {
        restorePreviousSession(host_, previous, previousDisc, prepared, error);
        return false;
    }
    cursor_ = resumeRecording ? candidate.resumeFrame() : 0;
    if (!restoredClock) restoreLegacyClock(host_, candidate, cursor_);
    tape_ = std::move(candidate); path_ = QFileInfo(path).absoluteFilePath();
    mode_ = resumeRecording ? Recording : (tape_.size() ? Playback : Finished);
    host_.setMoviePlayback(true);
    return true;
}

bool MovieSession::rerecord(const QString& path, QString& error) {
    if (!readOnly()) { error = Localization::text(QStringLiteral("Сначала включите воспроизведение записи.")); return false; }
    if (QFileInfo(path).absoluteFilePath() == QFileInfo(path_).absoluteFilePath()) {
        error = Localization::text(QStringLiteral("Выберите новое имя: исходная запись останется без изменений.")); return false;
    }
    MovieTape candidate = tape_;
    if (!candidate.forkAt(cursor_, error) ||
        !candidate.setResumeState(host_.captureMovieState(), cursor_, error) ||
        !candidate.save(path, error)) return false;
    tape_ = std::move(candidate); path_ = QFileInfo(path).absoluteFilePath();
    mode_ = Recording; host_.setMoviePlayback(true);
    return true;
}

bool MovieSession::saveRecording(QString& error) {
    if (!recording()) return true;
    // A session ended after rewinding is a deliberate shorter final timeline.
    if (!tape_.truncate(cursor_) ||
        !tape_.setResumeState(host_.captureMovieState(), cursor_, error)) return false;
    return tape_.save(path_, error);
}
bool MovieSession::finish(bool unload, QString& error) {
    if (!saveRecording(error)) return false;
    if (unload) host_.unload(); // Keep the read-only battery guard until after unloading.
    if (active() && !host_.restoreMovieConfiguration(error,!unload)) return false;
    host_.setMoviePlayback(false);
    mode_ = Off; cursor_ = 0; path_.clear(); tape_.clear();
    return true;
}

bool MovieSession::beforeFrame(const MovieInput& live, QString& error, MovieInput* applied) {
    if (mode_ == Off) {
        applyInput(host_, live);
        if (applied) *applied = live;
        return true;
    }
    if (mode_ == Finished) return false;
    MovieInput input;
    if (recording()) {
        if (cursor_ < tape_.size()) tape_.truncate(cursor_);
        if (!tape_.append(live, error)) return false;
        input = live;
    } else if (!tape_.inputAt(cursor_, input)) {
        error = Localization::text(QStringLiteral("Запись не содержит ожидаемый кадр.")); return false;
    }
    applyInput(host_, input);
    if (applied) *applied = input;
    return true;
}
void MovieSession::afterFrame() {
    if (!active() || mode_ == Finished) return;
    ++cursor_;
    if (mode_ == Playback && cursor_ == tape_.size()) mode_ = Finished;
}
double MovieSession::frameRate() const {
    return active() ? tape_.metadata().value(QStringLiteral("frameRate")).toDouble(host_.fps()) : host_.fps();
}
QByteArray MovieSession::controllers() const {
    if (!active()) return {};
    const QJsonObject metadata = tape_.metadata();
    const QJsonArray controllers = metadata.value(QStringLiteral("controllers")).toArray();
    if (controllers.size() == 3)
        return QJsonDocument(controllers).toJson(QJsonDocument::Compact);
    QJsonObject topology;
    topology.insert(QStringLiteral("adapter"), metadata.value(QStringLiteral("adapter")).toInt());
    topology.insert(QStringLiteral("jcart"), metadata.value(QStringLiteral("jcart")).toBool());
    topology.insert(QStringLiteral("jcartBase"), metadata.value(QStringLiteral("jcartBase")).toInt());
    topology.insert(QStringLiteral("controllers"), controllers);
    return QJsonDocument(topology).toJson(QJsonDocument::Compact);
}
QString MovieSession::slotPath(int slot) const {
    const QString name = QFileInfo(path_).completeBaseName();
    // Readable movie name plus recording identity prevents same-named movies sharing slots.
    return QDir(dataPath_).filePath(QStringLiteral("states/movies/%1_%2/slot%3.krs")
        .arg(name, QString::fromLatin1(tape_.id().toHex())).arg(slot));
}
unsigned MovieSession::occupiedSlots() const {
    unsigned result = 0;
    if (active()) for (int i = 0; i < 10; ++i) if (QFileInfo::exists(slotPath(i))) result |= 1u << i;
    return result;
}

bool MovieSession::saveSlot(int slot, QString& error) {
    if (!active() || slot < 0 || slot > 9) { error = Localization::text(QStringLiteral("Нет активной записи.")); return false; }
    return saveSlotFile(slotPath(slot), error, true);
}
bool MovieSession::saveFile(const QString& path, QString& error) {
    return saveSlotFile(path, error, false);
}
bool MovieSession::saveSlotFile(const QString& path, QString& error, bool keepPrevious) {
    if (!active() || path.isEmpty()) { error = Localization::text(QStringLiteral("Нет активной записи или не выбран файл сохранения.")); return false; }
    const auto state = host_.captureMovieState();
    const auto prefix = tape_.checkpoint(cursor_, error);
    if (state.isEmpty() || state.size() > MaxSnapshot || prefix.isEmpty() || prefix.size() > MaxPrefix) {
        if (error.isEmpty()) error = Localization::text(QStringLiteral("Состояние записи слишком велико или недоступно."));
        return false;
    }
    const auto frame = host_.frame().convertToFormat(QImage::Format_RGB32);
    const auto native = VideoFrameGeometry::nativeSize(host_.frame());
    if (frame.sizeInBytes() > MaxPixels) { error = Localization::text(QStringLiteral("Кадр записи слишком велик.")); return false; }
    const auto pixels = frame.isNull() ? QByteArray() : qCompress(frame.constBits(), frame.sizeInBytes(), 1);
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << quint32(state.size()) << quint32(prefix.size()) << quint32(pixels.size())
           << qint32(frame.width()) << qint32(frame.height()) << qint32(native.width()) << qint32(native.height())
           << host_.aspectRatio();
    stream.writeRawData(state.constData(), state.size());
    stream.writeRawData(prefix.constData(), prefix.size());
    stream.writeRawData(pixels.constData(), pixels.size());
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = Localization::text(QStringLiteral("Не удалось создать папку сохранения записи.")); return false;
    }
    return StateFile::write(path,QByteArrayLiteral("KRMSLOT1") + hash(payload) + payload,keepPrevious,error);
}

bool MovieSession::loadSlot(int slot, QImage& frame, double& aspect, bool& missing, QString& error) {
    missing = false;
    if (!active() || slot < 0 || slot > 9) { error = Localization::text(QStringLiteral("Нет активной записи.")); return false; }
    const bool loaded = loadFile(slotPath(slot), frame, aspect, missing, error);
    if (missing) error = Localization::text(QStringLiteral("Сохранение записи в слоте %1 не найдено")).arg(slot);
    return loaded;
}
bool MovieSession::loadFile(const QString& path, QImage& frame, double& aspect, bool& missing, QString& error) {
    missing = false;
    if (!active() || path.isEmpty()) { error = Localization::text(QStringLiteral("Нет активной записи или не выбран файл сохранения.")); return false; }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        missing = !QFileInfo::exists(file.fileName());
        error = missing ? Localization::text(QStringLiteral("Файл сохранения записи не найден: %1")).arg(path)
                        : Localization::text(QStringLiteral("Не удалось прочитать сохранение записи: %1")).arg(file.errorString());
        return false;
    }
    const auto bad = [&] { error = Localization::text(QStringLiteral("Сохранение записи повреждено или несовместимо.")); return false; };
    if (file.size() < 76 || file.size() > MaxSlot) return bad();
    const auto data = file.readAll();
    if (data.size() != file.size() || data.left(8) != QByteArrayLiteral("KRMSLOT1") || data.mid(8,32) != hash(data.mid(40))) return bad();
    const auto payload = data.mid(40);
    QDataStream stream(payload); stream.setVersion(QDataStream::Qt_6_0);
    quint32 stateSize = 0, prefixSize = 0, pixelSize = 0;
    qint32 w = 0, h = 0, nw = 0, nh = 0; double ratio = 0;
    stream >> stateSize >> prefixSize >> pixelSize >> w >> h >> nw >> nh >> ratio;
    constexpr qsizetype header = 36;
    if (stream.status() != QDataStream::Ok || !stateSize || stateSize > MaxSnapshot ||
        !prefixSize || prefixSize > MaxPrefix || pixelSize > MaxPixels ||
        quint64(header) + stateSize + prefixSize + pixelSize != quint64(payload.size()) ||
        !std::isfinite(ratio) || ratio <= 0 || ratio > 16 || w < 0 || h < 0 ||
        w > 4096 || h > 4096 || quint64(w)*h*4 > MaxPixels ||
        ((w == 0 || h == 0) && (w != 0 || h != 0 || pixelSize != 0))) return bad();
    QImage savedFrame;
    if (pixelSize) {
        const auto packed = payload.mid(header + stateSize + prefixSize, pixelSize);
        if (packed.size() < 4 || qFromBigEndian<quint32>(packed.constData()) != quint64(w)*h*4) return bad();
        const auto raw = qUncompress(packed);
        if (quint64(raw.size()) != quint64(w)*h*4) return bad();
        savedFrame = QImage(w,h,QImage::Format_RGB32);
        if (savedFrame.isNull()) return bad();
        std::memcpy(savedFrame.bits(), raw.constData(), size_t(raw.size()));
        if (nw > 0 && nh > 0 && nw <= 4096 && nh <= 4096) VideoFrameGeometry::setNativeSize(savedFrame, QSize(nw,nh));
    } else if (w || h) return bad();
    MovieTape candidate = tape_;
    quint64 target = 0;
    if (!candidate.restoreCheckpoint(payload.mid(header + stateSize, prefixSize), target, readOnly(), error)) return false;
    bool restoredClock = false;
    if (!host_.restoreMovieState(payload.mid(header,stateSize), error, false, &restoredClock)) return false;
    if (!restoredClock) restoreLegacyClock(host_, candidate, target);
    tape_ = std::move(candidate); cursor_ = target;
    if (readOnly()) mode_ = cursor_ == tape_.size() ? Finished : Playback;
    frame = savedFrame; aspect = ratio;
    return true;
}

QByteArray MovieSession::captureRewind() const {
    auto state = host_.captureMovieState(false);
    if (state.isEmpty()) return {};
    const auto cursor = qToBigEndian(cursor_);
    state.append(reinterpret_cast<const char*>(&cursor), sizeof(cursor));
    return state;
}
bool MovieSession::restoreRewind(const QByteArray& state, QString& error) {
    if (!active() || state.size() < 8) return false;
    const quint64 cursor = qFromBigEndian<quint64>(state.constData() + state.size() - 8);
    if (cursor > tape_.size() || !restoreRewindCore(state,error)) return false;
    cursor_ = cursor;
    if (readOnly()) mode_ = cursor == tape_.size() ? Finished : Playback;
    return true;
}

bool MovieSession::restoreRewindCore(const QByteArray& state, QString& error) {
    if (!active() || state.size() < 8) return false;
    const quint64 cursor = qFromBigEndian<quint64>(state.constData() + state.size() - 8);
    return cursor <= tape_.size() && host_.restoreMovieState(state.left(state.size()-8),error,true);
}
