#include "LibretroHost.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QtEndian>
#include <cstring>
#include <cmath>
#include <limits>
#include <atomic>
#include <algorithm>

thread_local LibretroHost* LibretroHost::active_ = nullptr;
struct LibretroHost::Api {
#define API(name) decltype(&retro_##name) name = nullptr
    API(init); API(deinit); API(api_version); API(get_system_info); API(get_system_av_info);
    API(set_environment); API(set_video_refresh); API(set_audio_sample); API(set_audio_sample_batch);
    API(set_input_poll); API(set_input_state); API(set_controller_port_device);
    API(load_game); API(unload_game); API(run); API(reset); API(serialize_size); API(serialize);
    API(unserialize); API(get_memory_data); API(get_memory_size);
#undef API
};
namespace {
std::atomic<LibretroHost*> coreOwner{nullptr};
constexpr qsizetype maxStateSize = 64 * 1024 * 1024;
constexpr qsizetype stateHeaderSize = 108;
const QByteArray stateMagic("KRSTATE1", 8);
QByteArray digest(const QByteArray& data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256); }
bool atomicWrite(const QString& path, const QByteArray& bytes, QString& error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = QStringLiteral("Не удалось сохранить %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}
}
LibretroHost::LibretroHost(QString corePath, QString dataPath)
    : library_(corePath), api_(std::make_unique<Api>()), dataPath_(QDir(dataPath).absolutePath()) {
    av_.timing.fps = 60.0;
    av_.timing.sample_rate = 48000.0;
}
LibretroHost::~LibretroHost() {
    unload();
    if (initialized_) api_->deinit();
    if (active_ == this) active_ = nullptr;
    LibretroHost* expected = this;
    coreOwner.compare_exchange_strong(expected, nullptr);
    library_.unload();
}
bool LibretroHost::initialize(QString& error) {
    if (initialized_) return true;
    if (active_ && active_ != this) {
        error = QStringLiteral("В одном потоке может работать только одно ядро."); return false;
    }
    for (const auto& name : {"bios", "saves", "states"}) {
        if (!QDir().mkpath(dataPath_ + '/' + name)) {
            error = QStringLiteral("Нет доступа для записи в каталог данных: %1").arg(dataPath_); return false;
        }
    }
    QFile binary(library_.fileName());
    if (!binary.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Не найдено ядро: %1").arg(library_.fileName()); return false;
    }
    QCryptographicHash coreHash(QCryptographicHash::Sha256);
    if (!coreHash.addData(&binary)) { error = QStringLiteral("Не удалось прочитать ядро."); return false; }
    coreHash_ = coreHash.result();
    if (!library_.load()) { error = library_.errorString(); return false; }
#define RESOLVE(name) api_->name = reinterpret_cast<decltype(api_->name)>(library_.resolve("retro_" #name)); \
    if (!api_->name) { error = QStringLiteral("Ядро не поддерживает retro_" #name); return false; }
    RESOLVE(init); RESOLVE(deinit); RESOLVE(api_version); RESOLVE(get_system_info); RESOLVE(get_system_av_info);
    RESOLVE(set_environment); RESOLVE(set_video_refresh); RESOLVE(set_audio_sample); RESOLVE(set_audio_sample_batch);
    RESOLVE(set_input_poll); RESOLVE(set_input_state); RESOLVE(set_controller_port_device);
    RESOLVE(load_game); RESOLVE(unload_game); RESOLVE(run); RESOLVE(reset); RESOLVE(serialize_size); RESOLVE(serialize);
    RESOLVE(unserialize); RESOLVE(get_memory_data); RESOLVE(get_memory_size);
#undef RESOLVE
    if (api_->api_version() != RETRO_API_VERSION) { error = QStringLiteral("Несовместимая версия API ядра."); return false; }
    LibretroHost* expected = nullptr;
    if (!coreOwner.compare_exchange_strong(expected, this)) {
        error = QStringLiteral("В приложении уже работает экземпляр ядра."); return false;
    }
    active_ = this;
    systemPath_ = QDir::toNativeSeparators(dataPath_ + "/bios").toUtf8();
    savePath_ = QDir::toNativeSeparators(dataPath_ + "/saves").toUtf8();
    api_->set_environment(environment);
    api_->set_video_refresh(video);
    api_->set_audio_sample(audioSample);
    api_->set_audio_sample_batch(audioBatch);
    api_->set_input_poll(inputPoll);
    api_->set_input_state(inputState);
    api_->init();
    initialized_ = true;
    return true;
}
bool LibretroHost::load(const QString& path, QString& error) {
    const QFileInfo info(path);
    const auto ext = info.suffix().toLower();
    const QStringList supported{"md", "gen", "bin", "smd", "sms", "gg", "sg", "68k"};
    if (!supported.contains(ext)) {
        error = QStringLiteral("Эта сборка открывает картриджи MD, SMS, Game Gear и SG-1000. CD, 32X и архивы пока не поддерживаются."); return false;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) { error = QStringLiteral("Не удалось открыть игру: %1").arg(file.errorString()); return false; }
    if (file.size() < 512 || file.size() > 32 * 1024 * 1024) {
        error = QStringLiteral("Некорректный размер ROM (ожидается от 512 байт до 32 МиБ)."); return false;
    }
    QByteArray content = file.readAll();
    if (content.size() != file.size()) { error = QStringLiteral("ROM прочитан не полностью."); return false; }
    const auto encodedPath = QDir::toNativeSeparators(info.absoluteFilePath()).toUtf8();
    if (encodedPath.size() >= 256) { error = QStringLiteral("Путь ROM слишком длинный для этого ядра (максимум 255 байт UTF-8)."); return false; }
    if (!initialize(error)) return false;
    // Preserve the previous game's battery saves before switching content.
    if (loaded_ && !flushSaveRam(error)) return false;
    unload();
    content_ = std::move(content);
    encodedGamePath_ = encodedPath;
    retro_game_info game{};
    game.path = encodedGamePath_.constData();
    game.data = content_.constData();
    game.size = size_t(content_.size());
    if (!api_->load_game(&game)) { error = QStringLiteral("Genesis Plus GX не смог загрузить ROM."); content_.clear(); return false; }
    loaded_ = true;
    gamePath_ = info.absoluteFilePath();
    romHash_ = digest(content_);
    gameKey_ = QString::fromLatin1(romHash_.toHex());
    api_->get_system_av_info(&av_);
    if (!std::isfinite(av_.timing.fps) || av_.timing.fps < 1 || av_.timing.fps > 240 ||
        !std::isfinite(av_.timing.sample_rate) || av_.timing.sample_rate < 8000 || av_.timing.sample_rate > 192000) {
        error = QStringLiteral("Ядро вернуло некорректные параметры видео/звука."); unload(); return false;
    }
    // Genesis 6-button pad; Master System/Game Gear/SG-1000 keep plain joypads.
    const bool md = ext != "sms" && ext != "gg" && ext != "sg";
    for (unsigned port = 0; port < 2; ++port)
        api_->set_controller_port_device(port, md ? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1) : RETRO_DEVICE_JOYPAD);
    saveMemorySizes_[0] = api_->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    saveMemorySizes_[1] = api_->get_memory_size(RETRO_MEMORY_RTC);
    QString saveError;
    saveRamWritable_ = restoreSaveRam(saveError);
    if (!saveRamWritable_ && message) message(saveError + QStringLiteral(" Автозапись отключена, чтобы не затереть существующий файл."));
    return true;
}
void LibretroHost::unload() {
    if (loaded_) {
        QString error;
        if (!flushSaveRam(error) && message) message(error);
        api_->unload_game();
    }
    loaded_ = false;
    content_.clear(); audio_.clear(); frame_ = QImage(); gamePath_.clear(); gameKey_.clear(); romHash_.clear();
    buttons_.fill(0);
    saveRamWritable_ = true;
    saveMemorySizes_.fill(0);
}
void LibretroHost::runFrame() { if (loaded_) { audio_.clear(); api_->run(); } }
void LibretroHost::reset() { if (loaded_) { api_->reset(); audio_.clear(); } }
void LibretroHost::setButtons(unsigned port, unsigned mask) { if (port < buttons_.size()) buttons_[port] = mask; }
double LibretroHost::aspectRatio() const {
    if (av_.geometry.aspect_ratio > 0) return av_.geometry.aspect_ratio;
    return av_.geometry.base_height ? double(av_.geometry.base_width) / av_.geometry.base_height : 4.0 / 3.0;
}
QByteArray LibretroHost::serialize() const {
    if (!loaded_) return {};
    const size_t size = api_->serialize_size();
    if (!size || size > size_t(maxStateSize)) return {};
    QByteArray result(qsizetype(size), '\0');
    if (!api_->serialize(result.data(), size)) return {};
    return result;
}
bool LibretroHost::unserialize(const QByteArray& state) {
    return loaded_ && size_t(state.size()) == api_->serialize_size() && api_->unserialize(state.constData(), size_t(state.size()));
}
QString LibretroHost::statePath(int slot) const { return dataPath_ + "/states/" + gameKey_ + QStringLiteral(".slot%1.krs").arg(slot); }
bool LibretroHost::saveState(int slot, QString& error) {
    if (!loaded_ || slot < 0 || slot > 9) { error = QStringLiteral("Нет игры или неверный слот сохранения."); return false; }
    const auto payload = serialize();
    if (payload.isEmpty()) { error = QStringLiteral("Ядро не смогло сохранить состояние."); return false; }
    QByteArray bytes = stateMagic + romHash_ + coreHash_ + digest(payload);
    quint32 size = qToBigEndian(quint32(payload.size()));
    bytes.append(reinterpret_cast<const char*>(&size), sizeof(size));
    bytes.append(payload);
    return atomicWrite(statePath(slot), bytes, error);
}
bool LibretroHost::loadState(int slot, QString& error) {
    if (!loaded_ || slot < 0 || slot > 9) { error = QStringLiteral("Нет игры или неверный слот сохранения."); return false; }
    QFile file(statePath(slot));
    if (!file.open(QIODevice::ReadOnly)) { error = QStringLiteral("Сохранение в слоте %1 не найдено.").arg(slot); return false; }
    if (file.size() < stateHeaderSize || file.size() > stateHeaderSize + maxStateSize) { error = QStringLiteral("Неверный размер сохранения."); return false; }
    const auto bytes = file.readAll();
    if (bytes.size() < stateHeaderSize || bytes.left(8) != stateMagic || bytes.mid(8,32) != romHash_ || bytes.mid(40,32) != coreHash_) {
        error = QStringLiteral("Сохранение относится к другой игре или сборке ядра."); return false;
    }
    const quint32 size = qFromBigEndian<quint32>(bytes.constData() + 104);
    const auto payload = bytes.mid(stateHeaderSize);
    if (size != quint32(payload.size()) || bytes.mid(72,32) != digest(payload)) { error = QStringLiteral("Сохранение повреждено."); return false; }
    if (!unserialize(payload)) { error = QStringLiteral("Ядро отклонило сохранение."); return false; }
    audio_.clear();
    return true;
}
bool LibretroHost::flushSaveRam(QString& error) {
    if (!loaded_ || !saveRamWritable_) return true;
    for (unsigned id : {RETRO_MEMORY_SAVE_RAM, RETRO_MEMORY_RTC}) {
        const auto size = saveMemorySizes_[id == RETRO_MEMORY_RTC ? 1 : 0];
        const auto data = api_->get_memory_data(id);
        if (!size || !data) continue;
        if (size > size_t(maxStateSize)) { error = QStringLiteral("Ядро вернуло неверный размер памяти сохранения."); return false; }
        const auto suffix = id == RETRO_MEMORY_RTC ? ".rtc" : ".srm";
        const auto path = dataPath_ + "/saves/" + gameKey_ + suffix;
        const QByteArray bytes(static_cast<const char*>(data), qsizetype(size));
        QFile existing(path);
        if (id == RETRO_MEMORY_SAVE_RAM && !existing.exists() &&
            std::all_of(bytes.cbegin(), bytes.cend(), [](char v) { return static_cast<unsigned char>(v) == 0xff; })) continue;
        if (existing.open(QIODevice::ReadOnly) && existing.size() == bytes.size() && existing.readAll() == bytes) continue;
        // Windows cannot atomically replace a file while our read handle is open.
        existing.close();
        if (!atomicWrite(path, bytes, error)) return false;
    }
    return true;
}
bool LibretroHost::restoreSaveRam(QString& error) {
    for (unsigned id : {RETRO_MEMORY_SAVE_RAM, RETRO_MEMORY_RTC}) {
        const auto size = saveMemorySizes_[id == RETRO_MEMORY_RTC ? 1 : 0];
        auto data = api_->get_memory_data(id);
        if (!size || !data) continue;
        QFile file(dataPath_ + "/saves/" + gameKey_ + (id == RETRO_MEMORY_RTC ? ".rtc" : ".srm"));
        if (!file.exists()) continue;
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > qint64(size) ||
            (id == RETRO_MEMORY_RTC && file.size() != qint64(size))) {
            error = QStringLiteral("Память сохранения имеет неверный размер или недоступна; файл не загружен."); return false;
        }
        const auto bytes = file.readAll();
        if (bytes.size() != file.size()) { error = QStringLiteral("Ошибка чтения памяти сохранения."); return false; }
        // Genesis Plus GX reports trimmed SRAM after retro_run, but full capacity before it.
        // Accept old trimmed saves and normalize future writes to the captured load-time capacity.
        if (id == RETRO_MEMORY_SAVE_RAM) std::memset(data, 0xff, size);
        std::memcpy(data, bytes.constData(), size_t(bytes.size()));
    }
    return true;
}
bool LibretroHost::environment(unsigned command, void* data) {
    auto* h = active_;
    if (!h) return false;
    switch (command) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *static_cast<const char**>(data) = h->systemPath_.constData(); return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *static_cast<const char**>(data) = h->savePath_.constData(); return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        const auto format = *static_cast<retro_pixel_format*>(data);
        if (format != RETRO_PIXEL_FORMAT_0RGB1555 && format != RETRO_PIXEL_FORMAT_RGB565 && format != RETRO_PIXEL_FORMAT_XRGB8888) return false;
        h->pixelFormat_ = format; return true;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *static_cast<bool*>(data) = true; return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *static_cast<bool*>(data) = false; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *static_cast<unsigned*>(data) = 0; return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        auto* vars = static_cast<const retro_variable*>(data);
        for (; vars && vars->key; ++vars) {
            const QByteArray value(vars->value);
            const auto start = value.indexOf(';');
            if (start >= 0) h->variables_[vars->key] = value.mid(start + 1).trimmed().split('|').first();
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* var = static_cast<retro_variable*>(data);
        const auto found = h->variables_.find(var->key);
        var->value = found == h->variables_.end() ? nullptr : found->second.constData();
        return found != h->variables_.end();
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY: h->av_.geometry = *static_cast<retro_game_geometry*>(data); return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: h->av_ = *static_cast<retro_system_av_info*>(data); return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: *static_cast<int*>(data) = 3; return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE: *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH; return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const auto* msg = static_cast<retro_message*>(data);
        if (h->message && msg && msg->msg) h->message(QString::fromUtf8(msg->msg));
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS: return true;
    default: return false;
    }
}
void LibretroHost::video(const void* data, unsigned width, unsigned height, size_t pitch) {
    auto* h = active_;
    if (!h || !data || data == RETRO_HW_FRAME_BUFFER_VALID || !width || !height || width > 4096 || height > 4096) return;
    const size_t bpp = h->pixelFormat_ == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
    if (pitch < width * bpp) return;
    QImage result(int(width), int(height), QImage::Format_RGB32);
    if (result.isNull()) return;
    for (unsigned y = 0; y < height; ++y) {
        const auto* source = static_cast<const unsigned char*>(data) + pitch * y;
        auto* out = reinterpret_cast<QRgb*>(result.scanLine(int(y)));
        for (unsigned x = 0; x < width; ++x) {
            if (bpp == 4) { quint32 p; std::memcpy(&p, source + x * 4, 4); out[x] = p | 0xff000000u; }
            else {
                quint16 p; std::memcpy(&p, source + x * 2, 2);
                const bool rgb565 = h->pixelFormat_ == RETRO_PIXEL_FORMAT_RGB565;
                unsigned r = (p >> (rgb565 ? 11 : 10)) & 31;
                unsigned g = (p >> 5) & (rgb565 ? 63 : 31);
                unsigned b = p & 31;
                out[x] = qRgb((r << 3) | (r >> 2), rgb565 ? ((g << 2) | (g >> 4)) : ((g << 3) | (g >> 2)), (b << 3) | (b >> 2));
            }
        }
    }
    h->frame_ = std::move(result);
}
void LibretroHost::audioSample(int16_t left, int16_t right) { const int16_t samples[2]{left,right}; audioBatch(samples,1); }
size_t LibretroHost::audioBatch(const int16_t* data, size_t frames) {
    auto* h = active_;
    if (h && data && frames <= 192000 && h->audio_.size() + qsizetype(frames * 4) <= 768000)
        h->audio_.append(reinterpret_cast<const char*>(data), qsizetype(frames * 4));
    return frames;
}
void LibretroHost::inputPoll() {}
int16_t LibretroHost::inputState(unsigned port, unsigned device, unsigned, unsigned id) {
    if (!active_ || port >= 2 || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD) return 0;
    const auto mask = active_->buttons_[port];
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return int16_t(mask);
    return id < 16 && (mask & (1u << id)) ? 1 : 0;
}
