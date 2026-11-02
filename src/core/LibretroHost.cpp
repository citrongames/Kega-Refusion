#include "LibretroHost.h"
#include <QSysInfo>
#include <rc_hash.h>
#include <rc_consoles.h>
#include "RomArchive.h"
#include "VideoFrameGeometry.h"
#include "SegaCdBios.h"
#include "Sega32xBios.h"
#include "CoreFileSystem.h"
#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QSaveFile>
#include <QtEndian>
#include <cstring>
#include <cmath>
#include <limits>
#include <atomic>
#include <algorithm>
#include <chrono>

thread_local LibretroHost* LibretroHost::active_ = nullptr;
struct LibretroHost::Api {
#define API(name) decltype(&retro_##name) name = nullptr
    API(init); API(deinit); API(api_version); API(get_system_info); API(get_system_av_info);
    API(set_environment); API(set_video_refresh); API(set_audio_sample); API(set_audio_sample_batch);
    API(set_input_poll); API(set_input_state); API(set_controller_port_device);
    API(load_game); API(unload_game); API(run); API(reset); API(serialize_size); API(serialize);
    API(unserialize); API(get_memory_data); API(get_memory_size);
    API(cheat_reset); API(cheat_set);
#undef API
    void (*manage_bram)(bool) = nullptr;
    bool (*get_bram)(unsigned, const void**, size_t*, const char**) = nullptr;
    const char* (*state_abi)() = nullptr;
    const char* (*recording_abi)() = nullptr;
    unsigned (*features)() = nullptr;
    unsigned (*system_hw)() = nullptr;
    bool (*get_cd_header)(void*, size_t) = nullptr;
    void (*invalidate_code)() = nullptr;
    using PapriumMusicCallback = std::uint64_t (*)(void*, unsigned, std::uint64_t,
                                                    std::int16_t*, std::size_t);
    void (*set_paprium_music)(PapriumMusicCallback, void*) = nullptr;
    int (*paprium_active)() = nullptr;
    int (*paprium_cd_active)() = nullptr;
};
namespace {
std::atomic<LibretroHost*> coreOwner{nullptr};
constexpr qsizetype maxStateSize = 64 * 1024 * 1024;
constexpr qsizetype maxAchievementProgressSize = 2 * 1024 * 1024;
const QByteArray achievementStateMagic("KRHOST01", 8);
constexpr qsizetype stateHeaderSize = 108;
const QByteArray stateMagic("KRSTATE1", 8);
bool validGeometry(const retro_game_geometry& geometry) {
    return geometry.base_width && geometry.base_height && geometry.base_width <= 4096 &&
        geometry.base_height <= 4096 && std::isfinite(geometry.aspect_ratio) &&
        geometry.aspect_ratio >= 0 && geometry.aspect_ratio <= 10;
}
bool validAvInfo(const retro_system_av_info& av) {
    return validGeometry(av.geometry) && std::isfinite(av.timing.fps) &&
        av.timing.fps >= 1 && av.timing.fps <= 240 && std::isfinite(av.timing.sample_rate) &&
        av.timing.sample_rate >= 8000 && av.timing.sample_rate <= 192000;
}
QByteArray digest(const QByteArray& data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256); }
QString readableSaveStem(QString fileName) {
    fileName.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString stem = QFileInfo(fileName).completeBaseName().left(256);
    QString name; name.reserve(180);
    int bytes = 0;
    for (uint cp : stem.toUcs4()) {
        if (cp < 32 || cp == 127 || cp == '<' || cp == '>' || cp == ':' || cp == '"' ||
            cp == '/' || cp == '\\' || cp == '|' || cp == '?' || cp == '*') cp = '_';
        const int length = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (bytes + length > 180) break;
        bytes += length;
        if (cp <= 0xffff) name.append(QChar(ushort(cp)));
        else { name.append(QChar(QChar::highSurrogate(cp))); name.append(QChar(QChar::lowSurrogate(cp))); }
    }
    name = name.trimmed();
    while (!name.isEmpty() && (name.back() == QLatin1Char('.') || name.back().isSpace())) name.chop(1);
    if (name.isEmpty()) name = QStringLiteral("game");
    const QString device = name.section(QLatin1Char('.'), 0, 0).toUpper();
    const bool numbered = device.size() == 4 &&
        (device.startsWith(QLatin1String("COM")) || device.startsWith(QLatin1String("LPT"))) &&
        ((device[3] >= QLatin1Char('1') && device[3] <= QLatin1Char('9')) ||
         device[3].unicode() == 0xb9 || device[3].unicode() == 0xb2 || device[3].unicode() == 0xb3);
    if (numbered || device == QLatin1String("CON") || device == QLatin1String("PRN") ||
        device == QLatin1String("AUX") || device == QLatin1String("NUL") ||
        device == QLatin1String("CONIN$") || device == QLatin1String("CONOUT$") || device == QLatin1String("CLOCK$"))
        name.prepend(QLatin1Char('_'));
    return name;
}

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
std::uint64_t LibretroHost::papriumMusicRead(void* user, unsigned track,
                                             std::uint64_t frame, std::int16_t* interleaved,
                                             std::size_t frames) {
    if (!user) return 0;
    auto& music = *static_cast<PapriumMusic*>(user);
    if (!frames) {
        if (frame == 0) return music.frames(track);
        if (frame == 1) return std::uint64_t(music.sampleRate(track));
        return 0;
    }
    return music.read(track, frame, interleaved, frames);
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
    exactCoreHash_ = coreHash_;
    if (!library_.load()) { error = library_.errorString(); return false; }
#define RESOLVE(name) api_->name = reinterpret_cast<decltype(api_->name)>(library_.resolve("retro_" #name)); \
    if (!api_->name) { error = QStringLiteral("Ядро не поддерживает retro_" #name); return false; }
    RESOLVE(init); RESOLVE(deinit); RESOLVE(api_version); RESOLVE(get_system_info); RESOLVE(get_system_av_info);
    RESOLVE(set_environment); RESOLVE(set_video_refresh); RESOLVE(set_audio_sample); RESOLVE(set_audio_sample_batch);
    RESOLVE(set_input_poll); RESOLVE(set_input_state); RESOLVE(set_controller_port_device);
    RESOLVE(load_game); RESOLVE(unload_game); RESOLVE(run); RESOLVE(reset); RESOLVE(serialize_size); RESOLVE(serialize);
    RESOLVE(unserialize); RESOLVE(get_memory_data); RESOLVE(get_memory_size);
#undef RESOLVE
    api_->features = reinterpret_cast<decltype(api_->features)>(library_.resolve("retro_kegarefusion_core_features"));
    api_->system_hw = reinterpret_cast<decltype(api_->system_hw)>(library_.resolve("retro_kegarefusion_system_hw"));
    api_->get_cd_header = reinterpret_cast<decltype(api_->get_cd_header)>(library_.resolve("retro_kegarefusion_cd_header"));
    api_->cheat_reset = reinterpret_cast<decltype(api_->cheat_reset)>(library_.resolve("retro_cheat_reset"));
    api_->cheat_set = reinterpret_cast<decltype(api_->cheat_set)>(library_.resolve("retro_cheat_set"));
    api_->invalidate_code = reinterpret_cast<decltype(api_->invalidate_code)>(library_.resolve("retro_kegarefusion_invalidate_code"));
    api_->manage_bram = reinterpret_cast<decltype(api_->manage_bram)>(library_.resolve("kegarefusion_manage_bram"));
    api_->get_bram = reinterpret_cast<decltype(api_->get_bram)>(library_.resolve("kegarefusion_get_bram"));
    api_->state_abi = reinterpret_cast<decltype(api_->state_abi)>(library_.resolve("kegarefusion_state_abi"));
    api_->recording_abi = reinterpret_cast<decltype(api_->recording_abi)>(library_.resolve("retro_kegarefusion_recording_abi"));
    if (api_->recording_abi) {
        const char* abi = api_->recording_abi();
        if (abi) recordingAbi_ = QByteArray(abi);
    }
    // Native serialization is platform/architecture dependent. The core's explicit
    // contract covers both the state layout and deterministic execution behavior.
    recordingCoreHash_ = recordingAbi_.isEmpty() ? exactCoreHash_
        : digest(QByteArrayLiteral("KegaRefusion-recording-contract:") + recordingAbi_ + ':'
                 + QSysInfo::kernelType().toUtf8() + ':' + QSysInfo::buildAbi().toUtf8());
    api_->set_paprium_music = reinterpret_cast<decltype(api_->set_paprium_music)>(library_.resolve("retro_kegarefusion_set_paprium_music"));
    api_->paprium_active = reinterpret_cast<decltype(api_->paprium_active)>(library_.resolve("retro_kegarefusion_paprium_active"));
    api_->paprium_cd_active = reinterpret_cast<decltype(api_->paprium_cd_active)>(library_.resolve("retro_kegarefusion_paprium_cd_active"));
    if (api_->manage_bram && api_->get_bram && api_->state_abi &&
        QByteArray(api_->state_abi()) == "kegarefusion-gpx-f27655b-battery-v1") {
        // This extension only exposes battery memory; the pinned core's state
        // layout is unchanged. Keep existing quick states compatible.
#ifdef Q_OS_WIN
        coreHash_ = QByteArray::fromHex("c48159c8f49c4266e872077eb5c461471ab63cb7555d2eb7a8df4768b69ebe8a");
#else
        coreHash_ = QByteArray::fromHex("34f2639a8980394875078be54aa6ba0750b8d40a26b7b07eeda4f85f5756d56f");
#endif
        api_->manage_bram(true);
    } else {
        api_->get_bram = nullptr;
        api_->manage_bram = nullptr;
    }
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
    if (api_->set_paprium_music)
        api_->set_paprium_music(papriumMusicRead, &papriumMusic_);
    initialized_ = true;
    return true;
}
QStringList LibretroHost::discPaths() const {
    QStringList result;
    if (isCd()) for (const auto& disc : discs_) result.append(disc.path);
    return result;
}
bool LibretroHost::discEjected() const {
    return isCd() && disk_.get_eject_state && disk_.get_eject_state();
}
bool LibretroHost::checkDiscBios(const DiscImage& image, QString& error) const {
    int region = image.region;
    const auto found = optionOverrides_.find("genesis_plus_gx_region_detect");
    if (found != optionOverrides_.end()) {
        if (found->second == "ntsc-j") region = 1;
        else if (found->second == "ntsc-u") region = 2;
        else if (found->second == "pal") region = 3;
    }
    if (region) return SegaCdBios::validate(dataPath_,region,error);
    // CHD's compressed header is decoded by the core. At least one installed
    // BIOS is needed; a failed core load reports the available region statuses.
    for (int candidate : {1,2,3}) if (SegaCdBios::validate(dataPath_,candidate,error)) { error.clear(); return true; }
    error = QStringLiteral("Не найден BIOS Sega CD. Импортируйте BIOS через Настройки → Настройки эмулятора… → BIOS → Sega CD.");
    return false;
}
void LibretroHost::setDiscIdentity() {
    const QFileInfo info(discs_[discIndex_].runtimePath);
    encodedGamePath_ = QDir::toNativeSeparators(info.absoluteFilePath()).toUtf8();
    contentDirectory_ = QDir::toNativeSeparators(info.absolutePath()).toUtf8();
    contentBaseName_ = info.completeBaseName().toUtf8();
    contentExtension_ = info.suffix().toLower().toUtf8();
    contentInfo_ = {};
    contentInfo_.full_path = encodedGamePath_.constData();
    contentInfo_.dir = contentDirectory_.constData();
    contentInfo_.name = contentBaseName_.constData();
    contentInfo_.ext = contentExtension_.constData();
    // Disc CPU states also depend on the selected region and installed BIOS.
    // Keep them separate from states made with another boot ROM/region.
    const QFileInfo original(discs_[discIndex_].path);
    gameKey_ = readableSaveStem(original.fileName());
    legacyGameKey_.clear(); // Never read/hash the whole disc to locate old saves.
    QByteArray identity = QByteArrayLiteral("KegaRefusion-CD-name-v1:") + original.completeBaseName().toUtf8() + '\0';
    const auto region = optionOverrides_.find("genesis_plus_gx_region_detect");
    identity += region == optionOverrides_.end() ? QByteArray("auto") : region->second;
    identity += '\0';
    for (const auto* code : {"J","U","E"}) {
        QFile bios(dataPath_ + "/bios/bios_CD_" + code + ".bin");
        identity += bios.open(QIODevice::ReadOnly) && bios.size() == 131072 ? digest(bios.readAll()) : QByteArray(32,'\0');
    }
    romHash_ = digest(identity);
}
bool LibretroHost::refreshDiscFiles(QString& error) {
    CoreFileSystem::clearVirtualFiles();
    for (auto& disc : discs_) if (!DiscMedia::prepare(disc,dataPath_,error)) return false;
    return true;
}
bool LibretroHost::registerDiscs(QString& error) {
    if (!disk_.get_num_images || !disk_.set_eject_state || !disk_.get_eject_state ||
        !disk_.set_image_index || !disk_.replace_image_index || !disk_.add_image_index ||
        !disk_.get_num_images()) {
        error = QStringLiteral("Ядро не распознало образ Sega CD."); return false;
    }
    CoreFileSystem::clearError();
    if (!disk_.set_eject_state(true)) { error = QStringLiteral("Не удалось открыть привод."); return false; }
    for (int i = 0; i < discs_.size(); ++i) {
        if (unsigned(i) >= disk_.get_num_images() && !disk_.add_image_index()) {
            error = QStringLiteral("Не удалось добавить диск."); return false;
        }
        // A freshly loaded disc 0 is already mounted. Replacing/selecting it
        // again would reopen every audio track and repeat MP3 indexing.
        if (i == 0 && discIndex_ == 0) continue;
        const auto path = QDir::toNativeSeparators(discs_[i].runtimePath).toUtf8();
        retro_game_info game{}; game.path = path.constData();
        if (!disk_.replace_image_index(unsigned(i),&game)) {
            error = QStringLiteral("Не удалось подключить диск: %1").arg(discs_[i].path); return false;
        }
    }
    if ((discIndex_ != 0 && !disk_.set_image_index(unsigned(discIndex_))) || !disk_.set_eject_state(false)) {
        error = QStringLiteral("Не удалось закрыть привод Sega CD."); return false;
    }
    if (!CoreFileSystem::lastError().isEmpty()) { error = CoreFileSystem::lastError(); return false; }
    return true;
}
bool LibretroHost::loadCd(const QString& path, QString& error) {
    QVector<DiscImage> images;
    if (!DiscMedia::read(path,images,error) || !checkDiscBios(images.first(),error)) return false;
    if (!initialize(error)) return false;
    const bool needs32x = images.first().needs32x;
    if (needs32x) {
        if (!api_->features || !(api_->features() & 1u)) {
            error = QStringLiteral("Этот диск требует CD32X, но загруженное ядро не поддерживает 32X.");
            return false;
        }
        if (!Sega32xBios::validate(dataPath_, error)) {
            error.prepend(QStringLiteral("Для этого диска CD32X нужны три BIOS 32X.\n"));
            return false;
        }
    }
    if (loaded_ && !flushSaveRam(error)) return false;
    unload();
    discs_ = images; discIndex_ = 0;
    if (!refreshDiscFiles(error)) { unload(); return false; }
    CoreFileSystem::clearError();
    setDiscIdentity();
    retro_game_info game{}; game.path = encodedGamePath_.constData();
    contentInfoActive_ = true;
    const bool accepted = api_->load_game(&game);
    contentInfoActive_ = false;
    if (!accepted) {
        error = QStringLiteral("Не удалось загрузить Sega CD. Проверьте образ, дорожки и BIOS региона.\n%1\n%2\n%3")
            .arg(SegaCdBios::status(dataPath_,1),SegaCdBios::status(dataPath_,2),SegaCdBios::status(dataPath_,3));
        if (needs32x) error += QLatin1Char('\n') + Sega32xBios::status(dataPath_);
        unload(); return false;
    }
    loaded_ = true;
    if (!CoreFileSystem::lastError().isEmpty()) { error = CoreFileSystem::lastError(); unload(); return false; }
    gamePath_ = QFileInfo(path).absoluteFilePath(); contentName_ = QFileInfo(path).fileName();
    api_->get_system_av_info(&av_);
    if (!validAvInfo(av_) || !registerDiscs(error)) {
        if (error.isEmpty()) error = QStringLiteral("Некорректные параметры Sega CD.");
        unload(); return false;
    }
    applyControllerPorts(true);
    // The battery extension exposes formatted BRAM and its regional path.
    // GPX still owns initialization/formatting; the host owns atomic writes.
    saveMemorySizes_.fill(0);
    loadCheats();
    return true;
}
bool LibretroHost::setDiscEjected(bool ejected, QString& error) {
    error.clear();
    if (!isCd() || !disk_.set_eject_state || !disk_.set_eject_state(ejected)) {
        error = QStringLiteral("Не удалось изменить состояние привода Sega CD."); return false;
    }
    audio_.clear(); return true;
}
bool LibretroHost::selectDisc(int index, QString& error) {
    error.clear();
    if (!isCd() || index < 0 || index >= discs_.size()) {
        error = QStringLiteral("Неверный номер диска."); return false;
    }
    if (discs_[index].needs32x && (!api_->features || !(api_->features() & 2u))) {
        error = QStringLiteral("Для CD32X откройте образ через «Открыть игру»."); return false;
    }
    if (!DiscMedia::unchanged(discs_[index],error)) return false;
    CoreFileSystem::clearError();
    const auto state = serialize();
    const bool wasEjected = discEjected();
    if (!setDiscEjected(true,error)) return false;
    if (!disk_.set_image_index(unsigned(index)) || !CoreFileSystem::lastError().isEmpty()) {
        disk_.set_image_index(unsigned(discIndex_));
        if (!state.isEmpty()) unserialize(state);
        disk_.set_eject_state(wasEjected);
        error = QStringLiteral("Ядро не смогло прочитать выбранный диск."); return false;
    }
    discIndex_ = index; setDiscIdentity();
    achievementHashCache_.clear();
    loadCheats();
    return setDiscEjected(false,error);
}
bool LibretroHost::changeDisc(const QString& path, QString& error) {
    error.clear();
    if (!isCd()) { error = QStringLiteral("Игра Sega CD не запущена."); return false; }
    DiscImage image;
    if (!DiscMedia::inspect(path,image,error)) return false;
    if (image.needs32x && (!api_->features || !(api_->features() & 2u))) {
        error = QStringLiteral("Для CD32X откройте образ через «Открыть игру»."); return false;
    }
    if (!DiscMedia::prepare(image,dataPath_,error)) return false;
    CoreFileSystem::clearError();
    const auto previous = discs_[discIndex_];
    const auto state = serialize();
    const bool wasEjected = discEjected();
    if (!setDiscEjected(true,error)) return false;
    const auto encoded = QDir::toNativeSeparators(image.runtimePath).toUtf8();
    retro_game_info game{}; game.path = encoded.constData();
    if (!disk_.replace_image_index(unsigned(discIndex_),&game) || !CoreFileSystem::lastError().isEmpty()) {
        const auto oldPath = QDir::toNativeSeparators(previous.runtimePath).toUtf8(); game.path = oldPath.constData();
        const bool restored = disk_.replace_image_index(unsigned(discIndex_),&game);
        if (restored && !state.isEmpty()) unserialize(state);
        disk_.set_eject_state(restored ? wasEjected : true);
        QString ignored; refreshDiscFiles(ignored);
        error = QStringLiteral("Не удалось вставить диск. %1").arg(restored ? QStringLiteral("Предыдущий диск восстановлен.") : QStringLiteral("Привод оставлен открытым."));
        return false;
    }
    discs_[discIndex_] = image;
    achievementHashCache_.clear();
    if (!refreshDiscFiles(error)) return false;
    setDiscIdentity();
    loadCheats();
    return setDiscEjected(false,error);
}
bool LibretroHost::load(const QString& path, QString& error, int archiveIndex,
                        const RomArchive::Entry* archiveEntry) {
    error.clear();
    if (DiscMedia::isDisc(path)) return loadCd(path,error);
    const QFileInfo info(path);
    QString name = info.fileName();
    QByteArray content;
    if (RomArchive::isArchive(path)) {
        RomArchive::Entry selected;
        if (archiveEntry) {
            // The UI already validated/listed this archive. Extraction still
            // revalidates the selected index, name and size against the file.
            selected = *archiveEntry;
        } else {
            const auto entries = RomArchive::list(path, &error);
            if (!error.isEmpty()) return false;
            auto found = std::find_if(entries.begin(), entries.end(), [archiveIndex](const auto& e) {
                return e.index == archiveIndex;
            });
            if (archiveIndex < 0 && entries.size() == 1) found = entries.begin();
            if (found == entries.end()) {
                error = entries.isEmpty() ? QStringLiteral("В архиве нет поддерживаемых игр.") :
                    QStringLiteral("Выберите игру внутри архива.");
                return false;
            }
            selected = *found;
        }
        name = selected.name;
        content = RomArchive::extract(path, selected, &error);
        if (!error.isEmpty()) return false;
    } else {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) { error = QStringLiteral("Не удалось открыть игру: %1").arg(file.errorString()); return false; }
        if (file.size() < 512 || file.size() > 32 * 1024 * 1024) {
            error = QStringLiteral("Некорректный размер ROM (ожидается от 512 байт до 32 МиБ)."); return false;
        }
        content = file.readAll();
        if (content.size() != file.size()) { error = QStringLiteral("ROM прочитан не полностью."); return false; }
    }
    const auto ext = QFileInfo(name).suffix().toLower();
    const QStringList supported{"md", "gen", "bin", "smd", "sms", "gg", "sg", "68k", "32x"};
    if (!supported.contains(ext) || content.size() < 512 || content.size() > 32 * 1024 * 1024) {
        error = QStringLiteral("Неподдерживаемый картридж или неверный размер ROM."); return false;
    }
    const auto encodedPath = QDir::toNativeSeparators(info.absoluteFilePath()).toUtf8();
    if (!initialize(error)) return false;
    // Read only the eight console-header bytes in each supported layout.
    // In SMD, odd/even bytes occupy separate halves of the first 16 KiB block.
    const auto header32x = [&content](int layout) {
        constexpr char signature[] = "SEGA 32X";
        for (qsizetype i = 0; i < 8; ++i) {
            const qsizetype logical = 0x100 + i;
            const qsizetype physical = layout == 2 ? 512 + logical / 2 + ((logical & 1) ? 0 : 0x2000)
                                                  : layout == 1 ? (logical ^ 1) : logical;
            if (physical >= content.size() || content[physical] != signature[i]) return false;
        }
        return true;
    };
    const bool smdLayout = content.size() >= 512 + 0x4000 &&
        ((content.size() / 512) & 1) && !(content.size() % 512) && !((content.size() - 512) % 0x4000);
    const bool needs32x = ext == "32x" || header32x(0) || header32x(1) || (smdLayout && header32x(2));
    if (needs32x) {
        if (!api_->features || !(api_->features() & 1u)) {
            error = QStringLiteral("Это ядро ещё не поддерживает 32X.");
            return false;
        }
        if (!Sega32xBios::validate(dataPath_, error)) return false;
    }
    // Preserve the previous game's battery saves before switching content.
    if (loaded_ && !flushSaveRam(error)) return false;
    unload();
    content_ = std::move(content);
    encodedGamePath_ = encodedPath;
    contentDirectory_ = QDir::toNativeSeparators(info.absolutePath()).toUtf8();
    contentBaseName_ = QFileInfo(name).completeBaseName().toUtf8();
    contentExtension_ = ext.toUtf8();
    archiveMember_ = name.toUtf8();
    const bool archived = RomArchive::isArchive(path);
    prepareCoreContent();
    contentInfo_ = {};
    contentInfo_.full_path = archived ? nullptr : encodedGamePath_.constData();
    contentInfo_.archive_path = archived ? encodedGamePath_.constData() : nullptr;
    contentInfo_.archive_file = archived ? archiveMember_.constData() : nullptr;
    contentInfo_.dir = contentDirectory_.constData();
    contentInfo_.name = contentBaseName_.constData();
    contentInfo_.ext = contentExtension_.constData();
    contentInfo_.data = coreContent_.constData();
    contentInfo_.size = size_t(coreContent_.size());
    contentInfo_.file_in_archive = archived;
    contentInfo_.persistent_data = false;
    retro_game_info game{};
    game.path = encodedGamePath_.constData();
    game.data = discs_.isEmpty() ? coreContent_.constData() : nullptr;
    game.size = size_t(coreContent_.size());
    contentInfoActive_ = true;
    const bool accepted = api_->load_game(&game);
    contentInfoActive_ = false;
    if (!accepted) { error = QStringLiteral("Genesis Plus GX не смог загрузить ROM."); contentInfo_ = {}; coreContent_.clear(); content_.clear(); return false; }
    loaded_ = true;
    gamePath_ = info.absoluteFilePath();
    if (api_->set_paprium_music && api_->paprium_active && api_->paprium_active())
        papriumMusic_.configure(gamePath_);
    contentName_ = name;
    romHash_ = digest(content_);
    legacyGameKey_ = QString::fromLatin1(romHash_.toHex());
    gameKey_ = readableSaveStem(name);
    api_->get_system_av_info(&av_);
    if (!validAvInfo(av_)) {
        error = QStringLiteral("Ядро вернуло некорректные параметры видео/звука."); unload(); return false;
    }
    applyControllerPorts(true);
    saveMemorySizes_[0] = isCd() ? 0 : api_->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    saveMemorySizes_[1] = isCd() ? 0 : api_->get_memory_size(RETRO_MEMORY_RTC);
    QString saveError;
    saveRamWritable_ = restoreSaveRam(saveError);
    if (!saveRamWritable_ && message) message(saveError + QStringLiteral(" Автозапись отключена, чтобы не затереть существующий файл."));
    loadCheats();
    return true;
}
void LibretroHost::unload() {
    resetAchievementMemory();
    achievementDescriptorCount_ = 0;
    achievementConsoleCache_ = 0;
    achievementHashCache_.clear();
    achievementCdHeader_.clear();
    if (loaded_) {
        QString error;
        if (!flushSaveRam(error) && message) message(error);
        clearCoreCheats();
        api_->unload_game();
    }
    papriumMusic_.clear();
    loaded_ = false;
    CoreFileSystem::clearVirtualFiles();
    discs_.clear(); discIndex_ = 0;
    contentInfo_ = {}; contentInfoActive_ = false;
    content_.clear(); coreContent_.clear(); audio_.clear(); frame_ = QImage(); gamePath_.clear(); gameKey_.clear(); legacyGameKey_.clear(); contentName_.clear(); romHash_.clear();
    buttons_.fill(0);
    controllerDevices_.fill(~0u);
    saveRamWritable_ = true;
    saveMemorySizes_.fill(0);
    batterySave_.clear(); cheats_.clear(); movieCheats_.clear(); movieCheatsActive_ = false; activeCheatCodes_.clear();
}
void LibretroHost::runFrame(bool renderVideo, bool captureAudio) {
    if (!loaded_) return;
    videoEnabled_ = renderVideo; audioEnabled_ = captureAudio;
    audio_.clear(); api_->run();
    // Serialization and normal calls retain accurate audio state: never set
    // libretro's fast-savestate or hard-disable-audio flags.
    videoEnabled_ = audioEnabled_ = true;
}
void LibretroHost::reset() {
    if (loaded_) { clearCoreCheats(); api_->reset(); resetAchievementMemory(); applyCheats(); audio_.clear(); }
}
bool LibretroHost::hardReset(QString& error) {
    error.clear();
    if (!loaded_) { error = QStringLiteral("Нет загруженной игры."); return false; }
    achievementHashCache_.clear();
    if (isCd() && !checkDiscBios(discs_[discIndex_],error)) return false;
    // The active attachment also covers CHD, whose 32X header is core-detected.
    // Recheck user BIOS before unloading a working game for its power cycle.
    if (api_->features && (api_->features() & 2u) && !Sega32xBios::validate(dataPath_,error)) return false;
    // Reuse the selected content already in memory: archives need no second
    // selection and moved/deleted ROM files do not affect a power cycle.
    if (!flushSaveRam(error)) return false;
    resetAchievementMemory();
    achievementDescriptorCount_ = 0;
    clearCoreCheats();
    api_->unload_game();
    batterySave_.clear();
    loaded_ = false;
    audio_.clear(); frame_ = QImage(); buttons_.fill(0);
    if (!discs_.isEmpty()) setDiscIdentity();
    prepareCoreContent();
    CoreFileSystem::clearError();
    contentInfo_.data = discs_.isEmpty() ? coreContent_.constData() : nullptr;
    contentInfo_.size = size_t(coreContent_.size());
    retro_game_info game{};
    game.path = encodedGamePath_.constData();
    game.data = discs_.isEmpty() ? coreContent_.constData() : nullptr;
    game.size = size_t(coreContent_.size());
    contentInfoActive_ = true;
    const bool accepted = api_->load_game(&game);
    contentInfoActive_ = false;
    if (!accepted) {
        error = QStringLiteral("Genesis Plus GX не смог перезапустить ROM.");
        unload(); return false;
    }
    loaded_ = true;
    if (!discs_.isEmpty() && !CoreFileSystem::lastError().isEmpty()) { error = CoreFileSystem::lastError(); unload(); return false; }
    if (!discs_.isEmpty() && !registerDiscs(error)) { unload(); return false; }
    saveRamWritable_ = false;
    api_->get_system_av_info(&av_);
    if (!validAvInfo(av_)) {
        error = QStringLiteral("Ядро вернуло некорректные параметры видео/звука.");
        unload(); return false;
    }
    applyControllerPorts(true);
    saveMemorySizes_[0] = isCd() ? 0 : api_->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    saveMemorySizes_[1] = isCd() ? 0 : api_->get_memory_size(RETRO_MEMORY_RTC);
    QString saveError;
    saveRamWritable_ = restoreSaveRam(saveError);
    if (!saveRamWritable_ && message) message(saveError + QStringLiteral(" Автозапись отключена, чтобы не затереть существующий файл."));
    applyCheats();
    return true;
}
Cheats::Platform LibretroHost::cheatPlatform() const {
    const bool x32 = loaded_ && api_->features && (api_->features() & 2u);
    if (isCd()) return x32 ? Cheats::Platform::Cd32x : Cheats::Platform::SegaCd;
    if (x32) return Cheats::Platform::Sega32x;
    return contentExtension_ == "sms" || contentExtension_ == "gg" || contentExtension_ == "sg"
        ? Cheats::Platform::MasterSystem : Cheats::Platform::MegaDrive;
}
bool LibretroHost::cheatsSupported() const {
    return loaded_ && api_->cheat_reset && api_->cheat_set &&
        (!(api_->features && (api_->features() & 2u)) || api_->invalidate_code);
}
QString LibretroHost::cheatSupportText() const {
    if (!loaded_) return QStringLiteral("Сначала откройте игру.");
    if (!cheatsSupported()) return QStringLiteral("Для читов требуется обновлённое ядро эмуляции.");
    return Cheats::supportText(cheatPlatform());
}
void LibretroHost::clearCoreCheats() {
    if (api_->cheat_reset) api_->cheat_reset();
    activeCheatCodes_.clear();
}
void LibretroHost::loadCheats() {
    clearCoreCheats();
    QString error;
    if (!cheats_.load(dataPath_ + "/cheats/" + gameKey_ + ".json",cheatPlatform(),error) && message)
        message(error);
    if (cheatsSupported()) applyCheats();
    else if (message && std::any_of(cheats_.entries().begin(),cheats_.entries().end(),
                                   [](const CheatEntry& entry) { return entry.enabled; }))
        message(cheatSupportText());
}
void LibretroHost::applyCheats() {
    if (!cheatsSupported()) return;
    QList<QByteArray> codes;
    for (const auto& entry : cheats()) if (entry.enabled) codes.append(entry.code.toLatin1());
    if (codes == activeCheatCodes_) return;
    clearCoreCheats();
    for (qsizetype i = 0; i < codes.size(); ++i) api_->cheat_set(unsigned(i),true,codes[i].constData());
    // GPX patches its own ROM copy; invalidate translated SH-2 instructions
    // after both enabling and disabling ROM patches on an active 32X.
    if (api_->invalidate_code) api_->invalidate_code();
    activeCheatCodes_ = std::move(codes);
}
bool LibretroHost::setCheats(const CheatList& entries, QString& error, bool* activeChanged) {
    error.clear();
    if (activeChanged) *activeChanged = false;
    if (!cheatsSupported()) { error = cheatSupportText(); return false; }
    if (!cheats_.set(entries,error)) return false;
    movieCheatsActive_ = false; movieCheats_.clear();
    const auto previous = activeCheatCodes_;
    applyCheats();
    if (activeChanged) *activeChanged = previous != activeCheatCodes_;
    return true;
}
void LibretroHost::prepareCoreContent() {
    coreContent_ = content_;
    if (!discs_.isEmpty()) return;
    if (!autoFixChecksum_ || contentExtension_ == "sms" || contentExtension_ == "gg" || contentExtension_ == "sg") return;
    enum class Layout { Plain, Swapped, Smd };
    Layout layout = Layout::Plain;
    QByteArray normalized = content_;
    const auto segaHeader = [](const QByteArray& bytes) {
        return bytes.size() >= 512 && bytes.mid(0x100,4) == "SEGA";
    };
    if (!segaHeader(normalized)) {
        const bool swapped = normalized.mid(0x100,16) == "ESAGM GE ARDVI E" ||
                             normalized.mid(0x100,12) == "ESAGG NESESI";
        if (swapped && !(normalized.size() & 1)) {
            layout = Layout::Swapped;
            for (qsizetype i = 0; i < normalized.size(); i += 2) std::swap(normalized[i], normalized[i+1]);
        } else if (normalized.size() > 512 && ((normalized.size()/512) & 1) &&
                   !(normalized.size()%512) && !((normalized.size()-512)%0x4000)) {
            layout = Layout::Smd;
            normalized = content_.mid(512);
            for (qsizetype offset = 0; offset < normalized.size(); offset += 0x4000) {
                const auto block = normalized.mid(offset,0x4000);
                for (qsizetype i = 0; i < 0x2000; ++i) {
                    normalized[offset+2*i] = block[0x2000+i];
                    normalized[offset+2*i+1] = block[i];
                }
            }
        } else return;
    }
    // Ambiguous BIN and unsupported layouts remain untouched. Only a valid
    // MD header in the detected byte order authorizes the two-byte patch.
    if (!segaHeader(normalized) || (normalized.size() & 1)) return;
    quint16 checksum = 0;
    for (qsizetype i = 0x200; i < normalized.size(); i += 2)
        checksum = quint16(checksum + (quint16(quint8(normalized[i])) << 8) + quint8(normalized[i+1]));
    const auto sourceOffset = [layout](qsizetype offset) {
        if (layout == Layout::Swapped) return offset ^ 1;
        if (layout == Layout::Smd) return 512 + (offset/0x4000)*0x4000 +
            ((offset & 1) ? 0 : 0x2000) + (offset%0x4000)/2;
        return offset;
    };
    coreContent_[sourceOffset(0x18e)] = char(checksum >> 8);
    coreContent_[sourceOffset(0x18f)] = char(checksum);
}
bool LibretroHost::setAutoFixChecksum(bool enabled, QString& error) {
    error.clear(); autoFixChecksum_ = enabled; return true;
}
bool LibretroHost::setCoreOption(const QByteArray& key, const QByteArray& value, QString& error) {
    error.clear();
    if (initialized_) {
        const auto choices = variableValues_.find(key);
        if (choices == variableValues_.end() || !choices->second.contains(value)) {
            error = QStringLiteral("Ядро не поддерживает выбранный параметр: %1").arg(QString::fromUtf8(key));
            return false;
        }
    }
    optionOverrides_[key] = value;
    if (variables_[key] != value) {
        variables_[key] = value;
        variablesUpdated_ = true;
    }
    return true;
}
bool LibretroHost::setRegion(int region, QString& error) {
    if (isCd()) {
        const int needed = region ? region : discs_[discIndex_].region;
        if (needed && !SegaCdBios::validate(dataPath_,needed,error)) return false;
    }
    static const std::array<QByteArray,4> values{"auto", "ntsc-j", "ntsc-u", "pal"};
    if (region < 0 || region >= int(values.size())) {
        error = QStringLiteral("Неверный регион."); return false;
    }
    return setCoreOption("genesis_plus_gx_region_detect", values[size_t(region)], error);
}
bool LibretroHost::setRemoveSpriteLimit(bool enabled, QString& error) {
    return setCoreOption("genesis_plus_gx_no_sprite_limit", enabled ? "enabled" : "disabled", error);
}
bool LibretroHost::setFastCdLoading(bool enabled, QString& error) {
    // Keep the drive's sector/audio clock intact; GPX retains its minimum
    // BIOS handshake delay while omitting simulated mechanical seek time.
    return setCoreOption("genesis_plus_gx_cd_latency", enabled ? "disabled" : "enabled", error);
}
bool LibretroHost::setBlarggFilter(int filter, QString& error) {
    static const std::array<QByteArray,5> values{"disabled", "monochrome", "composite", "svideo", "rgb"};
    if (filter < 0 || filter >= int(values.size())) {
        error = QStringLiteral("Неверный фильтр Blargg."); return false;
    }
    return setCoreOption("genesis_plus_gx_blargg_ntsc_filter", values[size_t(filter)], error);
}
void LibretroHost::setButtons(unsigned port, unsigned mask) {
    if (port >= buttons_.size()) return;
    if (controllerKinds_[port] == ControllerType::Pad && !controllerSixButton_[port])
        mask &= ~((1u << 2) | (1u << 9) | (1u << 10) | (1u << 11)); // Mode/X/Y/Z.
    buttons_[port] = controllerConnected_[port] ? mask : 0u;
}
bool LibretroHost::controllerConnected(unsigned port) const {
    return port < controllerConnected_.size() && controllerConnected_[port];
}
bool LibretroHost::setControllerConnected(unsigned port, bool connected) {
    if (port >= controllerConnected_.size() || controllerConnected_[port] == connected) return false;
    controllerConnected_[port] = connected;
    buttons_[port] = 0;
    applyControllerPorts();
    return true;
}
bool LibretroHost::controllerSixButton(unsigned port) const {
    return port < controllerSixButton_.size() && controllerSixButton_[port];
}
bool LibretroHost::setControllerSixButton(unsigned port, bool sixButton) {
    if (port >= controllerSixButton_.size() || controllerSixButton_[port] == sixButton) return false;
    controllerSixButton_[port] = sixButton;
    buttons_[port] = 0;
    applyControllerPorts();
    return true;
}
bool LibretroHost::setControllerKind(unsigned player, ControllerType type) {
    if (player >= controllerKinds_.size() || int(type) < 0 || int(type) > int(ControllerType::Justifier) ||
        (player == 0 && (type == ControllerType::Menacer || type == ControllerType::Justifier)) ||
        (player == 2 && type != ControllerType::Justifier && type != ControllerType::Pad) ||
        controllerKinds_[player] == type) return false;
    controllerKinds_[player] = type;
    buttons_[player] = 0; gunAim_[player] = {};
    applyControllerPorts();
    return true;
}
void LibretroHost::setGunAim(unsigned player, int x, int y, bool offscreen) {
    if (player < gunAim_.size()) gunAim_[player] = {int16_t(std::clamp(x,-32767,32767)),
        int16_t(std::clamp(y,-32767,32767)),offscreen};
}
void LibretroHost::applyControllerPorts(bool force) {
    if (!loaded_) return;
    const bool sms = contentExtension_ == "sms";
    const bool md = !sms && contentExtension_ != "gg" && contentExtension_ != "sg";
    std::array<unsigned,2> devices{{RETRO_DEVICE_NONE,RETRO_DEVICE_NONE}};
    callbackPlayers_.fill(-1);
    unsigned callback = 0;
    auto pad = [&](unsigned owner) -> unsigned {
        return md ? (controllerSixButton_[owner] ? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1)
            : RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0)) : RETRO_DEVICE_JOYPAD;
    };
    for (unsigned port=0;port<2;++port) if (controllerConnected_[port]) {
        const auto kind = controllerKinds_[port];
        if (md && port == 1 && (kind == ControllerType::Menacer || kind == ControllerType::Justifier)) {
            devices[port] = kind == ControllerType::Menacer
                ? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 1)
                : RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 2);
            callbackPlayers_[callback++] = int(port);
            // The second Justifier is chained to the first on port B; port A
            // remains an independently configured pad, never a hidden device.
            if (kind == ControllerType::Justifier && controllerConnected_[2] &&
                controllerKinds_[2] == ControllerType::Justifier) callbackPlayers_[callback] = 2;
        } else {
            devices[port] = sms && kind == ControllerType::LightPhaser
                ? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0) : pad(port);
            callbackPlayers_[callback++] = int(port);
        }
    }
    // With hardware port A empty, GPX numbers port B's pad as player zero.
    // Seed that pad subtype before restoring the empty A port; this happens
    // only when the topology/type changes, never on state restore or per frame.
    if (devices[0] == RETRO_DEVICE_NONE && (devices[1] & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD &&
        (force || controllerDevices_ != devices)) {
        api_->set_controller_port_device(0,devices[1]);
        controllerDevices_[0] = ~0u;
    }
    for (unsigned port=0;port<2;++port) {
        // Unchanged topology must not reset the emulated gun/pad protocol.
        if (force || controllerDevices_[port] != devices[port]) {
            api_->set_controller_port_device(port,devices[port]);
            controllerDevices_[port] = devices[port];
        }
    }
}
double LibretroHost::aspectRatio() const {
    if (av_.geometry.aspect_ratio > 0) return av_.geometry.aspect_ratio;
    return av_.geometry.base_height ? double(av_.geometry.base_width) / av_.geometry.base_height : 4.0 / 3.0;
}
QSize LibretroHost::nativeVideoSize() const { return VideoFrameGeometry::nativeSize(frame_); }
QByteArray LibretroHost::serialize() const {
    if (!loaded_) return {};
    const size_t size = api_->serialize_size();
    if (!size || size > size_t(maxStateSize)) return {};
    QByteArray result(qsizetype(size), '\0');
    if (!api_->serialize(result.data(), size)) return {};
    return result;
}
bool LibretroHost::unserialize(const QByteArray& state) {
    if (!loaded_ || size_t(state.size()) != api_->serialize_size() ||
        !api_->unserialize(state.constData(), size_t(state.size()))) return false;
    applyControllerPorts();
    return true;
}
QString LibretroHost::statePath(int slot) const { return dataPath_ + "/states/" + gameKey_ + QStringLiteral(".slot%1.krs").arg(slot); }
QString LibretroHost::saveReadPath(const QString& directory, const QString& suffix) const {
    const QString prefix = dataPath_ + QLatin1Char('/') + directory + QLatin1Char('/');
    const QString preferred = prefix + gameKey_ + suffix;
    const QFileInfo existing(preferred);
    // A present but invalid new file must be reported, never masked by a legacy
    // fallback. No legacy source file is renamed, overwritten or removed.
    return !existing.exists() && !existing.isSymLink() && !legacyGameKey_.isEmpty()
        ? prefix + legacyGameKey_ + suffix : preferred;
}

unsigned LibretroHost::occupiedStateSlots() const {
    unsigned mask = 0;
    if (loaded_) for (int slot = 0; slot < 10; ++slot)
        if (QFileInfo(saveReadPath(QStringLiteral("states"), QStringLiteral(".slot%1.krs").arg(slot))).isFile()) mask |= 1u << slot;
    return mask;
}
QByteArray LibretroHost::captureRewindState() const {
    const auto core = serialize();
    if (core.isEmpty()) return {};
    QByteArray result("KRREW001", 8);
    qsizetype total = core.size() + 20;
    for (auto size : saveMemorySizes_) {
        if (size > size_t(maxStateSize) || total + qsizetype(size) > maxStateSize) return {};
        total += qsizetype(size);
    }
    for (quint32 size : {quint32(core.size()), quint32(saveMemorySizes_[0]), quint32(saveMemorySizes_[1])}) {
        const quint32 encoded = qToBigEndian(size);
        result.append(reinterpret_cast<const char*>(&encoded), 4);
    }
    result += core;
    for (unsigned i = 0; i < 2; ++i) {
        const auto size = saveMemorySizes_[i];
        const void* data = api_->get_memory_data(i ? RETRO_MEMORY_RTC : RETRO_MEMORY_SAVE_RAM);
        if (size && !data) return {};
        if (size) result.append(static_cast<const char*>(data), qsizetype(size));
    }
    return result;
}
bool LibretroHost::restoreRewindState(const QByteArray& state) {
    if (!loaded_ || state.size() < 20 || state.size() > maxStateSize || state.left(8) != "KRREW001") return false;
    const auto coreSize = qFromBigEndian<quint32>(state.constData() + 8);
    const auto ramSize = qFromBigEndian<quint32>(state.constData() + 12);
    const auto rtcSize = qFromBigEndian<quint32>(state.constData() + 16);
    if (coreSize != api_->serialize_size() || ramSize != saveMemorySizes_[0] || rtcSize != saveMemorySizes_[1] ||
        quint64(coreSize) + ramSize + rtcSize + 20 != quint64(state.size())) return false;
    void* ram = api_->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    void* rtc = api_->get_memory_data(RETRO_MEMORY_RTC);
    if ((ramSize && !ram) || (rtcSize && !rtc)) return false;
    if (!unserialize(state.mid(20, coreSize))) return false;
    if (ramSize) std::memcpy(ram, state.constData() + 20 + coreSize, ramSize);
    if (rtcSize) std::memcpy(rtc, state.constData() + 20 + coreSize + ramSize, rtcSize);
    audio_.clear();
    return true;
}
QByteArray LibretroHost::captureMovieState(bool checksum) const {
    const auto core = serialize();
    if (core.isEmpty() || exactCoreHash_.size() != 32 || romHash_.size() != 32) return {};
    std::array<const void*,4> memory{};
    std::array<size_t,5> sizes{size_t(core.size()),saveMemorySizes_[0],saveMemorySizes_[1],0,0};
    memory[0] = api_->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    memory[1] = api_->get_memory_data(RETRO_MEMORY_RTC);
    if ((sizes[1] && !memory[0]) || (sizes[2] && !memory[1])) return {};
    quint32 bramMask = 0;
    for (unsigned index = 0; index < 2; ++index) {
        if (!api_->get_bram) break;
        const void* data = nullptr; size_t size = 0; const char* path = nullptr;
        if (!api_->get_bram(index,&data,&size,&path)) continue;
        if (!data || !size || size > 1024 * 1024) return {};
        memory[index+2] = data; sizes[index+3] = size; bramMask |= 1u << index;
    }
    const bool cd = isCd() || (api_->paprium_cd_active && api_->paprium_cd_active());
    if (cd && !(bramMask & 1u)) return {};
    quint64 total = 128;
    for (size_t size : sizes) total += size;
    constexpr quint64 maxMovieStateSize = 128ull * 1024 * 1024;
    if (total > maxMovieStateSize) return {};
    QByteArray bytes("KRMOV002",8);
    bytes += romHash_;
    bytes += recordingCoreHash_;
    for (size_t size : sizes) {
        const quint32 encoded = qToBigEndian(quint32(size));
        bytes.append(reinterpret_cast<const char*>(&encoded),4);
    }
    const quint32 encodedMask = qToBigEndian(bramMask);
    bytes.append(reinterpret_cast<const char*>(&encodedMask),4);
    bytes.append(32,'\0'); // Filled after the payload is complete.
    bytes += core;
    for (unsigned index = 0; index < 4; ++index)
        if (sizes[index+1]) bytes.append(static_cast<const char*>(memory[index]),qsizetype(sizes[index+1]));
    if (checksum) {
        QCryptographicHash payloadHash(QCryptographicHash::Sha256);
        payloadHash.addData(QByteArrayView(bytes.constData()+128,bytes.size()-128));
        bytes.replace(96,32,payloadHash.result());
    }
    return bytes;
}
bool LibretroHost::restoreMovieState(const QByteArray& state, QString& error, bool trusted) {
    error.clear();
    constexpr quint64 maxMovieStateSize = 128ull * 1024 * 1024;
    const bool legacy = state.startsWith("KRMOV001");
    if (!loaded_ || state.size() < 128 || quint64(state.size()) > maxMovieStateSize ||
        (!legacy && !state.startsWith("KRMOV002")) || state.mid(8,32) != romHash_) {
        error = QStringLiteral("Состояние записи относится к другой игре или имеет неизвестный формат."); return false;
    }
    std::array<quint32,5> sizes{};
    quint64 total = 128;
    for (unsigned i = 0; i < sizes.size(); ++i) {
        sizes[i] = qFromBigEndian<quint32>(state.constData()+72+4*i);
        total += sizes[i];
    }
    const quint32 bramMask = qFromBigEndian<quint32>(state.constData()+92);
    const bool hashValid = trusted
        ? std::all_of(state.constData()+96,state.constData()+128,[](char byte) { return byte == 0; })
        : [&state] {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(QByteArrayView(state.constData()+128,state.size()-128));
            return state.mid(96,32) == hash.result();
        }();
    if (total != quint64(state.size()) || bramMask > 3 ||
        sizes[0] != api_->serialize_size() || sizes[1] != saveMemorySizes_[0] ||
        sizes[2] != saveMemorySizes_[1] ||
        (bool(bramMask & 1u) != bool(sizes[3])) ||
        (bool(bramMask & 2u) != bool(sizes[4])) || !hashValid) {
        error = QStringLiteral("Состояние записи повреждено или несовместимо."); return false;
    }
    std::array<void*,4> memory{};
    memory[0] = api_->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    memory[1] = api_->get_memory_data(RETRO_MEMORY_RTC);
    if ((sizes[1] && !memory[0]) || (sizes[2] && !memory[1])) {
        error = QStringLiteral("Память сохранения записи недоступна."); return false;
    }
    const bool cd = isCd() || (api_->paprium_cd_active && api_->paprium_cd_active());
    if (cd && !(bramMask & 1u)) {
        error = QStringLiteral("В состоянии записи отсутствует BRAM Sega CD."); return false;
    }
    for (unsigned index = 0; index < 2; ++index) {
        const void* data = nullptr; size_t size = 0; const char* path = nullptr;
        const bool present = api_->get_bram && api_->get_bram(index,&data,&size,&path);
        if (present != bool(bramMask & (1u << index)) ||
            (present && (!data || size != sizes[index+3]))) {
            error = QStringLiteral("BRAM записи несовместима с текущим ядром."); return false;
        }
        memory[index+2] = const_cast<void*>(data);
    }
    const auto previous = serialize();
    if (previous.isEmpty()) { error = QStringLiteral("Не удалось сохранить текущее состояние перед загрузкой записи."); return false; }
    if (!api_->unserialize(state.constData()+128,size_t(sizes[0]))) {
        if (!unserialize(previous)) error = QStringLiteral("Ядро отклонило состояние записи; восстановить прежнее состояние не удалось.");
        else error = QStringLiteral("Ядро отклонило состояние записи.");
        return false;
    }
    applyControllerPorts();
    // Native deserialization may rebuild memory regions; never use pointers
    // obtained before it. A failed postcheck leaves battery bytes untouched.
    memory[0] = api_->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    memory[1] = api_->get_memory_data(RETRO_MEMORY_RTC);
    bool memoryValid = (!sizes[1] || memory[0]) && (!sizes[2] || memory[1]);
    for (unsigned index = 0; index < 2; ++index) {
        const void* data = nullptr; size_t size = 0; const char* path = nullptr;
        const bool present = api_->get_bram && api_->get_bram(index,&data,&size,&path);
        memoryValid &= present == bool(bramMask & (1u << index)) &&
            (!present || (data && size == sizes[index+3]));
        memory[index+2] = const_cast<void*>(data);
    }
    if (!memoryValid) {
        if (!unserialize(previous)) error = QStringLiteral("Память записи изменилась; восстановить прежнее состояние не удалось.");
        else error = QStringLiteral("Память записи изменилась при загрузке состояния.");
        return false;
    }
    qsizetype offset = 128 + sizes[0];
    for (unsigned index = 0; index < 4; ++index) {
        if (sizes[index+1]) std::memcpy(memory[index],state.constData()+offset,sizes[index+1]);
        offset += sizes[index+1];
    }
    audio_.clear();
    return true;
}
QByteArray LibretroHost::movieCoreIdentity() const { return loaded_ ? recordingCoreHash_ : QByteArray(); }

void LibretroHost::resetAchievementMemory() {
    if (achievementMemoryInitialized_)
        rc_libretro_memory_destroy(&achievementMemory_);
    achievementMemoryInitialized_ = false;
    achievementMemoryConsole_ = 0;
}
void LibretroHost::achievementCoreMemoryInfo(uint32_t id, rc_libretro_core_memory_info_t* info) {
    if (!info) return;
    info->data = nullptr;
    info->size = 0;
    auto* host = active_;
    if (!host || !host->loaded_ || !host->api_->get_memory_data || !host->api_->get_memory_size) return;
    info->data = static_cast<uint8_t*>(host->api_->get_memory_data(id));
    if (!info->data) return;
    info->size = id == RETRO_MEMORY_SAVE_RAM && host->saveMemorySizes_[0]
        ? host->saveMemorySizes_[0] : host->api_->get_memory_size(id);
}
unsigned LibretroHost::achievementConsoleId() const {
    if (!loaded_) return 0;
    if (achievementConsoleCache_) return achievementConsoleCache_;
    unsigned id = 0;
    if (isCd()) id = RC_CONSOLE_SEGA_CD;
    else if (api_->features && (api_->features() & 2u)) id = RC_CONSOLE_SEGA_32X;
    else if (api_->system_hw) {
        switch (api_->system_hw()) {
        case 0x01: case 0x02: case 0x03: id = RC_CONSOLE_SG1000; break;
        case 0x10: case 0x20: case 0x21: id = RC_CONSOLE_MASTER_SYSTEM; break;
        case 0x40: case 0x41: id = RC_CONSOLE_GAME_GEAR; break;
        case 0x80: case 0x81: case 0x82: id = RC_CONSOLE_MEGA_DRIVE; break;
        case 0x84: id = RC_CONSOLE_SEGA_CD; break;
        default: break;
        }
    } else if (contentExtension_ == "sms") id = RC_CONSOLE_MASTER_SYSTEM;
    else if (contentExtension_ == "gg") id = RC_CONSOLE_GAME_GEAR;
    else if (contentExtension_ == "sg") id = RC_CONSOLE_SG1000;
    else id = RC_CONSOLE_MEGA_DRIVE;
    achievementConsoleCache_ = id;
    return id;
}
void* LibretroHost::achievementCdOpenTrack(const char*, uint32_t track) {
    auto* host = active_;
    return host && track == 1 && host->achievementCdHeader_.size() == 512
        ? host->achievementCdHeader_.data() : nullptr;
}
size_t LibretroHost::achievementCdReadSector(void* track, uint32_t sector, void* buffer, size_t count) {
    if (!track || !buffer || sector != 0 || count > 512) return 0;
    std::memcpy(buffer, track, count);
    return count;
}
void LibretroHost::achievementCdCloseTrack(void*) {}
uint32_t LibretroHost::achievementCdFirstSector(void*) { return 0; }
QString LibretroHost::achievementHash(QString& error) const {
    error.clear();
    if (!loaded_) { error = QStringLiteral("Нет загруженной игры."); return {}; }
    if (!achievementHashCache_.isEmpty()) return achievementHashCache_;
    const unsigned console = achievementConsoleId();
    if (!console) { error = QStringLiteral("Не удалось определить систему для достижений."); return {}; }
    const QByteArray path = isCd() ? QDir::toNativeSeparators(discs_[discIndex_].path).toUtf8()
        : (archiveMember_.isEmpty() ? contentName_.toUtf8() : archiveMember_);
    rc_hash_iterator_t iterator{};
    rc_hash_initialize_iterator(&iterator, path.constData(),
        isCd() ? nullptr : reinterpret_cast<const uint8_t*>(content_.constData()),
        isCd() ? 0 : size_t(content_.size()));
    if (isCd() && api_->get_cd_header) {
        achievementCdHeader_.resize(512);
        if (!api_->get_cd_header(achievementCdHeader_.data(), 512)) {
            achievementCdHeader_.clear();
            rc_hash_destroy_iterator(&iterator);
            error = QStringLiteral("Ядро не предоставило заголовок текущего диска.");
            return {};
        }
        iterator.callbacks.cdreader.open_track_iterator = nullptr;
        iterator.callbacks.cdreader.open_track = achievementCdOpenTrack;
        iterator.callbacks.cdreader.read_sector = achievementCdReadSector;
        iterator.callbacks.cdreader.close_track = achievementCdCloseTrack;
        iterator.callbacks.cdreader.first_track_sector = achievementCdFirstSector;
    } else if (isCd() && QFileInfo(discs_[discIndex_].path).suffix().compare("chd", Qt::CaseInsensitive) == 0) {
        rc_hash_destroy_iterator(&iterator);
        error = QStringLiteral("Для достижений CHD требуется ядро с доступом к заголовку диска.");
        return {};
    }
    char hash[33]{};
    const bool success = rc_hash_generate(hash, console, &iterator) != 0;
    rc_hash_destroy_iterator(&iterator);
    achievementCdHeader_.clear();
    if (!success) {
        error = QStringLiteral("Не удалось вычислить хеш RetroAchievements для этой игры.");
        return {};
    }
    achievementHashCache_ = QString::fromLatin1(hash);
    return achievementHashCache_;
}
uint32_t LibretroHost::readAchievementMemory(uint32_t address, uint8_t* buffer, uint32_t count) {
    if (!loaded_ || !buffer || !count) return 0;
    const unsigned console = achievementConsoleId();
    if (!console) return 0;
    if (!achievementMemoryInitialized_ || achievementMemoryConsole_ != console) {
        resetAchievementMemory();
        retro_memory_map map{};
        map.descriptors = achievementDescriptorCount_ ? achievementDescriptors_.data() : nullptr;
        map.num_descriptors = achievementDescriptorCount_;
        rc_libretro_memory_init(&achievementMemory_, &map, achievementCoreMemoryInfo, console);
        achievementMemoryInitialized_ = true;
        achievementMemoryConsole_ = console;
    }
    return rc_libretro_memory_read(&achievementMemory_, address, buffer, count);
}
QString LibretroHost::achievementHardcoreBlockReason() const {
    if (!loaded_) return QStringLiteral("Сначала откройте игру.");
    if (!activeCheatCodes_.isEmpty())
        return QStringLiteral("Отключите активные читы для хардкорных достижений.");
    if (discs_.isEmpty() && coreContent_ != content_)
        return QStringLiteral("Отключите исправление контрольной суммы ROM для хардкорных достижений.");
    const auto fastCd = variables_.find("genesis_plus_gx_cd_latency");
    if (isCd() && fastCd != variables_.end() && fastCd->second == "disabled")
        return QStringLiteral("Отключите быструю загрузку CD для хардкорных достижений.");
    const auto spriteLimit = variables_.find("genesis_plus_gx_no_sprite_limit");
    if (spriteLimit != variables_.end() && spriteLimit->second == "enabled")
        return QStringLiteral("Включите ограничение спрайтов для хардкорных достижений.");
    const auto* disallowed = rc_libretro_get_disallowed_settings("Genesis Plus GX");
    for (const auto& [key,value] : variables_) {
        if (disallowed && !rc_libretro_is_setting_allowed(disallowed, key.constData(), value.constData()))
            return QStringLiteral("Параметр ядра %1 = %2 несовместим с хардкорными достижениями.")
                .arg(QString::fromUtf8(key), QString::fromUtf8(value));
    }
    return {};
}

bool LibretroHost::acceptsLegacyMovieCore(const QByteArray& identity) const {
    if (identity == exactCoreHash_) return true;
    if (recordingAbi_ != "kegarefusion-recording-v1") return false;
    // Released 0.10.0 binaries used a build hash. These are the verified original
    // implementations of v1, not a blanket acceptance of arbitrary old cores.
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    return identity == QByteArray::fromHex("454dc6e41f9c7ebd22bfc2fa082ac2013dcd46889bb82ff9ca3f4124f3026b0e");
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    return identity == QByteArray::fromHex("83c593f3138f24439520fd91f7b9f1251b35cd0d63087cdc4f9dc9476e3b529a");
#else
    return false;
#endif
}
QByteArray LibretroHost::movieGameIdentity() const {
    if (!loaded_) return {};
    if (discs_.isEmpty()) return digest(QByteArrayLiteral("KRMOV-CART1") + content_);
    // CD identity samples each source file at both ends. This is bounded even
    // for multi-gigabyte CHDs; it is not a whole-disc cryptographic guarantee.
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("KRMOV-CD1", 9));
    auto addNumber = [&hash](quint64 number) {
        const quint64 encoded = qToBigEndian(number);
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(&encoded), sizeof(encoded)));
    };
    addNumber(quint64(discs_.size()));
    for (const auto& disc : discs_) {
        addNumber(quint64(disc.files.size()));
        for (const auto& file : disc.files) {
            QFile source(file.path);
            if (!source.open(QIODevice::ReadOnly) || source.size() <= 0) return {};
            const auto size = source.size();
            addNumber(quint64(size));
            constexpr qint64 sample = 64 * 1024;
            const QByteArray first = source.read(std::min(size, sample));
            if (first.size() != std::min(size, sample)) return {};
            hash.addData(first);
            if (size > sample) {
                if (!source.seek(std::max(qint64(0), size - sample))) return {};
                const QByteArray last = source.read(sample);
                if (last.size() != sample) return {};
                hash.addData(last);
            }
        }
    }
    return hash.result();
}
QJsonObject LibretroHost::movieBiosFingerprints() const {
    QJsonObject result;
    const bool cd = isCd() || (api_->paprium_cd_active && api_->paprium_cd_active());
    const bool x32 = api_->features && (api_->features() & 2u);
    const auto fingerprint = [this,&result](const char* name, qint64 expectedSize) {
        QFile source(dataPath_ + "/bios/" + QString::fromLatin1(name));
        if (!source.open(QIODevice::ReadOnly)) {
            result.insert(QLatin1String(name), QStringLiteral("missing")); return;
        }
        if (source.size() != expectedSize) {
            result.insert(QLatin1String(name), QStringLiteral("invalid")); return;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&source)) {
            result.insert(QLatin1String(name), QStringLiteral("unreadable")); return;
        }
        result.insert(QLatin1String(name), QString::fromLatin1(hash.result().toHex()));
    };
    if (cd) for (const auto* name : {"bios_CD_J.bin", "bios_CD_U.bin", "bios_CD_E.bin"})
        fingerprint(name, 131072);
    if (x32) {
        fingerprint("32X_G_BIOS.BIN", 256);
        fingerprint("32X_M_BIOS.BIN", 2048);
        fingerprint("32X_S_BIOS.BIN", 1024);
    }
    return result;
}
QJsonObject LibretroHost::movieConfiguration() const {
    if (!loaded_) return {};
    const auto game = movieGameIdentity();
    if (game.size() != 32 || exactCoreHash_.size() != 32) return {};
    QJsonObject options;
    for (const auto& [key,value] : variables_)
        options.insert(QString::fromUtf8(key), QString::fromUtf8(value));
    QJsonArray controllers;
    for (size_t i = 0; i < controllerKinds_.size(); ++i)
        controllers.append(QJsonObject{{QStringLiteral("connected"), controllerConnected_[i]},
            {QStringLiteral("sixButton"), controllerSixButton_[i]},
            {QStringLiteral("kind"), int(controllerKinds_[i])}});
    QJsonArray cheatsJson;
    for (const auto& entry : cheats())
        cheatsJson.append(QJsonObject{{QStringLiteral("code"),entry.code},
            {QStringLiteral("description"),entry.description},{QStringLiteral("enabled"),entry.enabled}});
    return QJsonObject{{QStringLiteral("version"),2},
        {QStringLiteral("gameIdentity"),QString::fromLatin1(game.toHex())},
        {QStringLiteral("coreIdentity"),QString::fromLatin1(recordingCoreHash_.toHex())},
        {QStringLiteral("coreBuild"),QString::fromLatin1(exactCoreHash_.toHex())},
        {QStringLiteral("coreVersion"),QStringLiteral(KEGA_VERSION)},
        {QStringLiteral("coreCompatibility"),QString::fromUtf8(recordingAbi_)},
        {QStringLiteral("options"),options},
        {QStringLiteral("autoFixChecksum"),autoFixChecksum_},
        {QStringLiteral("discIndex"),discIndex_},
        {QStringLiteral("frameRate"),fps()},
        {QStringLiteral("controllers"),controllers},
        {QStringLiteral("cheats"),cheatsJson},
        {QStringLiteral("bios"),movieBiosFingerprints()}};
}
bool LibretroHost::prepareMovie(const QJsonObject& configuration, QString& error) {
    error.clear();
    movieCompatibilityWarning_.clear();
    if (!loaded_) { error = QStringLiteral("Нет загруженной игры."); return false; }
    const int version = configuration.value(QStringLiteral("version")).toInt(-1);
    if ((version != 1 && version != 2) ||
        !configuration.value(QStringLiteral("gameIdentity")).isString() ||
        !configuration.value(QStringLiteral("coreIdentity")).isString() ||
        !configuration.value(QStringLiteral("options")).isObject() ||
        !configuration.value(QStringLiteral("autoFixChecksum")).isBool() ||
        !configuration.value(QStringLiteral("discIndex")).isDouble() ||
        !configuration.value(QStringLiteral("controllers")).isArray() ||
        !configuration.value(QStringLiteral("cheats")).isArray() ||
        !configuration.value(QStringLiteral("bios")).isObject()) {
        error = QStringLiteral("Неверная конфигурация записи."); return false;
    }
    if (configuration.contains(QStringLiteral("frameRate"))) {
        const auto rate = configuration.value(QStringLiteral("frameRate"));
        if (!rate.isDouble() || !std::isfinite(rate.toDouble()) || rate.toDouble() < 1 || rate.toDouble() > 240) {
            error = QStringLiteral("Неверная частота кадров записи."); return false;
        }
    }
    const auto game = movieGameIdentity();
    if (game.isEmpty() || configuration.value(QStringLiteral("gameIdentity")).toString() != QString::fromLatin1(game.toHex())) {
        error = QStringLiteral("Запись относится к другой игре или изменённому образу диска."); return false;
    }
    const auto identityText = configuration.value(QStringLiteral("coreIdentity")).toString().toLatin1();
    const auto identity = QByteArray::fromHex(identityText);
    if (identity.size() != 32 || identity.toHex() != identityText) {
        error = QStringLiteral("Повреждено описание версии ядра в записи."); return false;
    }
    if (version == 1 ? !acceptsLegacyMovieCore(identity) : identity != recordingCoreHash_) {
        const auto requiredVersion = configuration.value(QStringLiteral("coreVersion")).toString(
            version == 1 ? QStringLiteral("0.10.0") : QStringLiteral("неизвестной версии"));
        const auto requiredBuild = configuration.value(QStringLiteral("coreBuild")).toString(
            QString::fromLatin1(identityText));
        movieCompatibilityWarning_ = QStringLiteral(
            "Запись: %1 / ядро %2\nСейчас: %3 / ядро %4\nВозможна рассинхронизация")
            .arg(requiredVersion.left(40), requiredBuild.left(12),
                 QStringLiteral(KEGA_VERSION), QString::fromLatin1(exactCoreHash_.toHex().left(12)));
        // A compatibility mismatch is advisory. File bounds, game identity and
        // the core's own state decoder still determine whether loading is possible.
    }
    if (configuration.value(QStringLiteral("bios")).toObject() != movieBiosFingerprints()) {
        error = QStringLiteral("BIOS записи отличаются от установленных."); return false;
    }
    const int recordedDisc = configuration.value(QStringLiteral("discIndex")).toInt(-1);
    if (recordedDisc < 0 || (discs_.isEmpty() ? recordedDisc != 0 : recordedDisc >= discs_.size())) {
        error = QStringLiteral("Номер диска записи не совпадает с плейлистом."); return false;
    }
    if (!discs_.isEmpty()) {
        if (!DiscMedia::unchanged(discs_[recordedDisc],error)) return false;
        if (discs_[recordedDisc].needs32x && (!api_->features || !(api_->features() & 2u))) {
            error = QStringLiteral("Для выбранного диска записи требуется ядро CD32X."); return false;
        }
    }
    const bool cd = isCd() || (api_->paprium_cd_active && api_->paprium_cd_active());
    if (cd && !api_->get_bram) {
        error = QStringLiteral("Для записи Sega CD требуется ядро с доступом к BRAM."); return false;
    }
    const auto options = configuration.value(QStringLiteral("options")).toObject();
    auto newOptions = variables_;
    bool changedOptions = options.size() != qsizetype(variables_.size());
    for (auto entry = options.begin(); entry != options.end(); ++entry) {
        if (!entry.value().isString()) {
            error = QStringLiteral("Повреждено описание параметров ядра записи."); return false;
        }
        const auto key = entry.key().toUtf8();
        const auto choices = variableValues_.find(key);
        const auto value = entry.value().toString().toUtf8();
        const auto current = newOptions.find(key);
        if (current == newOptions.end() || choices == variableValues_.end() || !choices->second.contains(value)) {
            changedOptions = true;
            continue;
        }
        current->second = value;
    }
    if (changedOptions) {
        if (!movieCompatibilityWarning_.isEmpty()) movieCompatibilityWarning_ += QLatin1Char('\n');
        movieCompatibilityWarning_ += QStringLiteral(
            "Набор параметров ядра изменился: недоступные параметры записи пропущены. Возможна рассинхронизация.");
    }
    const auto controllers = configuration.value(QStringLiteral("controllers")).toArray();
    if (controllers.size() != 3) { error = QStringLiteral("Неверная конфигурация контроллеров записи."); return false; }
    std::array<bool,3> connected{}, sixButton{};
    std::array<ControllerType,3> kinds{};
    for (int i = 0; i < 3; ++i) {
        const auto value = controllers[i];
        const auto object = value.toObject();
        const auto kindValue = object.value(QStringLiteral("kind"));
        if (!value.isObject() || !object.value(QStringLiteral("connected")).isBool() ||
            !object.value(QStringLiteral("sixButton")).isBool() || !kindValue.isDouble() ||
            kindValue.toInt(-1) < 0 || kindValue.toInt(-1) > int(ControllerType::Justifier) ||
            (i == 0 && kindValue.toInt() >= int(ControllerType::Menacer)) ||
            (i == 2 && kindValue.toInt() != int(ControllerType::Pad) &&
             kindValue.toInt() != int(ControllerType::Justifier))) {
            error = QStringLiteral("Неверная конфигурация контроллеров записи."); return false;
        }
        connected[size_t(i)] = object.value(QStringLiteral("connected")).toBool();
        sixButton[size_t(i)] = object.value(QStringLiteral("sixButton")).toBool();
        kinds[size_t(i)] = ControllerType(kindValue.toInt());
    }
    const auto cheatArray = configuration.value(QStringLiteral("cheats")).toArray();
    if (cheatArray.size() > Cheats::MaxCodes) { error = QStringLiteral("Слишком много читов в записи."); return false; }
    CheatList decoded, normalized;
    for (const auto& value : cheatArray) {
        const auto object = value.toObject();
        if (!value.isObject() || !object.value(QStringLiteral("code")).isString() ||
            !object.value(QStringLiteral("description")).isString() ||
            !object.value(QStringLiteral("enabled")).isBool()) {
            error = QStringLiteral("Неверные читы записи."); return false;
        }
        decoded.append({object.value(QStringLiteral("code")).toString(),
            object.value(QStringLiteral("description")).toString(),
            object.value(QStringLiteral("enabled")).toBool()});
    }
    if (!Cheats::normalize(decoded, cheatPlatform(), normalized, error)) return false;
    if (normalized != decoded || (!cheatsSupported() &&
        std::any_of(normalized.begin(),normalized.end(),[](const CheatEntry& entry) { return entry.enabled; }))) {
        error = QStringLiteral("Читы записи несовместимы с ядром."); return false;
    }
    // Every validation above precedes changes to live emulation. Keep movie
    // cheats in memory so replay never rewrites the user's cheat file.
    if (!movieConfigurationActive_) {
        preMovieVariables_ = variables_;
        preMovieOptionOverrides_ = optionOverrides_;
        preMovieAutoFixChecksum_ = autoFixChecksum_;
        preMovieControllerConnected_ = controllerConnected_;
        preMovieControllerSixButton_ = controllerSixButton_;
        preMovieControllerKinds_ = controllerKinds_;
        movieConfigurationActive_ = true;
    }
    optionOverrides_ = newOptions;
    for (const auto& [key,value] : newOptions) if (variables_[key] != value) {
        variables_[key] = value; variablesUpdated_ = true;
    }
    autoFixChecksum_ = configuration.value(QStringLiteral("autoFixChecksum")).toBool();
    controllerConnected_ = connected; controllerSixButton_ = sixButton; controllerKinds_ = kinds;
    movieCheats_ = std::move(normalized); movieCheatsActive_ = true;
    buttons_.fill(0); gunAim_.fill(GunAim{});
    const int previousDisc = discIndex_;
    if (recordedDisc != discIndex_) { discIndex_ = recordedDisc; setDiscIdentity(); }
    if (hardReset(error)) return true;
    // Preflight failures leave the original core running; undo the pending
    // playlist selection so a caller can restore its previous movie state.
    if (loaded_ && previousDisc != discIndex_) { discIndex_ = previousDisc; setDiscIdentity(); }
    return false;
}
bool LibretroHost::restoreMovieConfiguration(QString& error) {
    error.clear();
    if (!movieConfigurationActive_) return true;
    const bool optionChanged = variables_ != preMovieVariables_;
    variables_ = std::move(preMovieVariables_);
    optionOverrides_ = std::move(preMovieOptionOverrides_);
    variablesUpdated_ |= optionChanged;
    autoFixChecksum_ = preMovieAutoFixChecksum_;
    controllerConnected_ = preMovieControllerConnected_;
    controllerSixButton_ = preMovieControllerSixButton_;
    controllerKinds_ = preMovieControllerKinds_;
    movieCheats_.clear(); movieCheatsActive_ = false;
    movieConfigurationActive_ = false;
    buttons_.fill(0); gunAim_.fill(GunAim{});
    if (loaded_) {
        applyControllerPorts();
        applyCheats();
        if (!discs_.isEmpty()) setDiscIdentity();
    }
    return true;
}
QByteArray LibretroHost::stateCoreIdentity() const {
    // Ordinary MD/CD retain their stable GPX state identity. The attachment
    // has its own versioned state contract, independent of executable hashes.
    if (api_->paprium_cd_active && api_->paprium_cd_active())
        return digest(coreHash_ + QByteArrayLiteral("kegarefusion-gpx-f27655b-paprium-cd-v1"));
    if (api_->paprium_active && api_->paprium_active())
        return digest(coreHash_ + QByteArrayLiteral("kegarefusion-gpx-f27655b-paprium-v1"));
    if (api_->features && (api_->features() & 2u))
        return digest(coreHash_ + QByteArrayLiteral("kegarefusion-32x-state-v1"));
    return coreHash_;
}
bool LibretroHost::saveState(int slot, QString& error) {
    if (!loaded_ || slot < 0 || slot > 9) { error = QStringLiteral("Нет игры или неверный слот сохранения."); return false; }
    if (isCd() && discEjected()) { error = QStringLiteral("Закройте привод перед сохранением состояния."); return false; }
    QByteArray payload = serialize();
    if (payload.isEmpty()) { error = QStringLiteral("Ядро не смогло сохранить состояние."); return false; }
    const QByteArray progress = captureAchievementProgress ? captureAchievementProgress() : QByteArray();
    if (progress.size() > maxAchievementProgressSize ||
        (!progress.isEmpty() && payload.size() > maxStateSize - 16 - progress.size())) {
        error = QStringLiteral("Прогресс достижений слишком велик для сохранения."); return false;
    }
    if (!progress.isEmpty()) {
        QByteArray wrapped;
        wrapped.reserve(16 + payload.size() + progress.size());
        wrapped.append(achievementStateMagic);
        const quint32 nativeSize = qToBigEndian(quint32(payload.size()));
        const quint32 progressSize = qToBigEndian(quint32(progress.size()));
        wrapped.append(reinterpret_cast<const char*>(&nativeSize), sizeof(nativeSize));
        wrapped.append(reinterpret_cast<const char*>(&progressSize), sizeof(progressSize));
        wrapped.append(payload);
        wrapped.append(progress);
        payload = std::move(wrapped);
    }
    QByteArray bytes = stateMagic + romHash_ + stateCoreIdentity() + digest(payload);
    quint32 size = qToBigEndian(quint32(payload.size()));
    bytes.append(reinterpret_cast<const char*>(&size), sizeof(size));
    bytes.append(payload);
    return atomicWrite(statePath(slot), bytes, error);
}
bool LibretroHost::loadState(int slot, QString& error, StateLoadError* failure) {
    if (failure) *failure = StateLoadError::Failed;
    if (!loaded_ || slot < 0 || slot > 9) { error = QStringLiteral("Нет игры или неверный слот сохранения."); return false; }
    if (isCd() && discEjected()) { error = QStringLiteral("Закройте привод перед загрузкой состояния."); return false; }
    QFile file(saveReadPath(QStringLiteral("states"), QStringLiteral(".slot%1.krs").arg(slot)));
    if (!file.open(QIODevice::ReadOnly)) {
        if (!QFileInfo::exists(file.fileName())) {
            if (failure) *failure = StateLoadError::Missing;
            error = QStringLiteral("Сохранение в слоте %1 не найдено").arg(slot);
        } else {
            error = QStringLiteral("Не удалось прочитать сохранение в слоте %1: %2").arg(slot).arg(file.errorString());
        }
        return false;
    }
    if (file.size() < stateHeaderSize || file.size() > stateHeaderSize + maxStateSize) { error = QStringLiteral("Неверный размер сохранения."); return false; }
    const auto bytes = file.readAll();
    if (bytes.size() < stateHeaderSize || bytes.left(8) != stateMagic || bytes.mid(8,32) != romHash_ || bytes.mid(40,32) != stateCoreIdentity()) {
        error = QStringLiteral("Сохранение относится к другой игре или сборке ядра."); return false;
    }
    const quint32 size = qFromBigEndian<quint32>(bytes.constData() + 104);
    const auto payload = bytes.mid(stateHeaderSize);
    if (size != quint32(payload.size()) || bytes.mid(72,32) != digest(payload)) { error = QStringLiteral("Сохранение повреждено."); return false; }
    QByteArray native = payload;
    QByteArray progress;
    if (payload.startsWith(achievementStateMagic)) {
        if (payload.size() < 16) { error = QStringLiteral("Повреждён прогресс достижений в сохранении."); return false; }
        const quint32 nativeSize = qFromBigEndian<quint32>(payload.constData() + 8);
        const quint32 progressSize = qFromBigEndian<quint32>(payload.constData() + 12);
        if (!nativeSize || nativeSize > maxStateSize || progressSize > maxAchievementProgressSize ||
            quint64(nativeSize) + quint64(progressSize) + 16 != quint64(payload.size())) {
            error = QStringLiteral("Повреждён прогресс достижений в сохранении."); return false;
        }
        native = payload.mid(16, nativeSize);
        progress = payload.mid(16 + nativeSize, progressSize);
    }
    if (!unserialize(native)) { error = QStringLiteral("Ядро отклонило сохранение."); return false; }
    if (restoreAchievementProgress) restoreAchievementProgress(progress);
    if (failure) *failure = StateLoadError::None;
    audio_.clear();
    return true;
}
bool LibretroHost::flushSaveRam(QString& error) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return updateSaveRam(now, true, error);
}
bool LibretroHost::pollSaveRam(qint64 nowMs, QString& error) {
    return updateSaveRam(nowMs, false, error);
}
bool LibretroHost::updateSaveRam(qint64 nowMs, bool force, QString& error) {
    error.clear();
    if (!loaded_ || !saveRamWritable_ || moviePlayback_) return true;
    if (isCd() || (api_->paprium_cd_active && api_->paprium_cd_active())) {
        // An unmodified CD core retains its own unload writer. Paprium is a
        // cartridge and may also expose ordinary SRAM below.
        if (api_->get_bram) for (unsigned index = 0; index < 2; ++index) {
            const void* data = nullptr; size_t size = 0; const char* rawPath = nullptr;
            if (!api_->get_bram(index, &data, &size, &rawPath)) continue;
            const QString path = QDir::cleanPath(QDir::fromNativeSeparators(QString::fromUtf8(rawPath)));
            const QString saveRoot = QDir::cleanPath(QDir::fromNativeSeparators(QString::fromUtf8(savePath_))) + '/';
            if (!path.startsWith(saveRoot) || !path.endsWith(QLatin1String(".brm")) ||
                size > 64 * 1024 * 1024) {
                error = QStringLiteral("Ядро вернуло некорректный путь или размер BRAM.");
                return false;
            }
            if (!batterySave_.update(path, data, qsizetype(size), nowMs, force, false, error))
                return false;
        }
        if (isCd()) return true;
    }
    for (unsigned id : {RETRO_MEMORY_SAVE_RAM, RETRO_MEMORY_RTC}) {
        const auto size = saveMemorySizes_[id == RETRO_MEMORY_RTC ? 1 : 0];
        const auto data = api_->get_memory_data(id);
        if (!size || !data) continue;
        const auto path = dataPath_ + "/saves/" + gameKey_ + (id == RETRO_MEMORY_RTC ? ".rtc" : ".srm");
        if (size > size_t(maxStateSize)) {
            error = QStringLiteral("Ядро вернуло неверный размер памяти сохранения."); return false;
        }
        if (!batterySave_.update(path, data, qsizetype(size), nowMs, force,
                                 id == RETRO_MEMORY_SAVE_RAM, error)) return false;
    }
    return true;
}
bool LibretroHost::restoreSaveRam(QString& error) {
    if (isCd()) return true;
    for (unsigned id : {RETRO_MEMORY_SAVE_RAM, RETRO_MEMORY_RTC}) {
        const auto size = saveMemorySizes_[id == RETRO_MEMORY_RTC ? 1 : 0];
        auto data = api_->get_memory_data(id);
        if (!size || !data) continue;
        QFile file(saveReadPath(QStringLiteral("saves"), id == RETRO_MEMORY_RTC ? QStringLiteral(".rtc") : QStringLiteral(".srm")));
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
    case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
        if (!h->contentInfoActive_ || !data) return false;
        *static_cast<const retro_game_info_ext**>(data) = &h->contentInfo_;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *static_cast<const char**>(data) = h->systemPath_.constData(); return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *static_cast<const char**>(data) = h->savePath_.constData(); return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        const auto format = *static_cast<retro_pixel_format*>(data);
        if (format != RETRO_PIXEL_FORMAT_0RGB1555 && format != RETRO_PIXEL_FORMAT_RGB565 && format != RETRO_PIXEL_FORMAT_XRGB8888) return false;
        h->pixelFormat_ = format; return true;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *static_cast<bool*>(data) = true; return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = h->variablesUpdated_; h->variablesUpdated_ = false; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *static_cast<unsigned*>(data) = 0; return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        auto* vars = static_cast<const retro_variable*>(data);
        for (; vars && vars->key; ++vars) {
            const QByteArray value(vars->value);
            const auto start = value.indexOf(';');
            if (start >= 0) {
                const auto choices = value.mid(start + 1).trimmed().split('|');
                h->variableValues_[vars->key] = choices;
                const auto selected = h->optionOverrides_.find(vars->key);
                h->variables_[vars->key] = selected != h->optionOverrides_.end() && choices.contains(selected->second)
                    ? selected->second : choices.first();
            }
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* var = static_cast<retro_variable*>(data);
        const auto found = h->variables_.find(var->key);
        var->value = found == h->variables_.end() ? nullptr : found->second.constData();
        return found != h->variables_.end();
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        if (!data || !validGeometry(*static_cast<retro_game_geometry*>(data))) return false;
        h->av_.geometry = *static_cast<retro_game_geometry*>(data); return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        if (!data || !validAvInfo(*static_cast<retro_system_av_info*>(data))) return false;
        h->av_ = *static_cast<retro_system_av_info*>(data); return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE: *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH; return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const auto* msg = static_cast<retro_message*>(data);
        if (h->message && msg && msg->msg) h->message(QString::fromUtf8(msg->msg));
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE: {
        auto* request = static_cast<retro_vfs_interface_info*>(data);
        if (!request || request->required_interface_version > 3) return false;
        request->required_interface_version = 3;
        request->iface = CoreFileSystem::interface();
        return true;
    }
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        // GPX unregisters after a cartridge load, but registers only at init.
        // Retain the callbacks as a capability; isCd() gates their use.
        if (data) h->disk_ = *static_cast<const retro_disk_control_callback*>(data);
        return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        if (!data) return false;
        *static_cast<int*>(data) = (h->videoEnabled_ ? 1 : 0) | (h->audioEnabled_ ? 2 : 0);
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS: return true;
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
        if (!data) return false;
        const auto* map = static_cast<const retro_memory_map*>(data);
        if (map->num_descriptors > h->achievementDescriptors_.size() ||
            (map->num_descriptors && !map->descriptors)) return false;
        h->resetAchievementMemory();
        h->achievementDescriptorCount_ = unsigned(map->num_descriptors);
        for (unsigned i = 0; i < h->achievementDescriptorCount_; ++i) {
            h->achievementDescriptors_[i] = map->descriptors[i];
            // The core's descriptor array is stack-local. Only pointer values
            // to core-owned memory survive the callback, not descriptor text.
            h->achievementDescriptors_[i].addrspace = nullptr;
        }
        return true;
    }
    default: return false;
    }
}
void LibretroHost::video(const void* data, unsigned width, unsigned height, size_t pitch) {
    auto* h = active_;
    if (!h || !h->videoEnabled_ || !data || data == RETRO_HW_FRAME_BUFFER_VALID || !width || !height || width > 4096 || height > 4096) return;
    const size_t bpp = h->pixelFormat_ == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
    if (pitch < width * bpp) return;
    QImage result;
    if (bpp == 2) {
        // Qt uses its optimized RGB565/555 converter; the wrapped core buffer
        // is read-only and convertToFormat owns the resulting frame pixels.
        const QImage wrapped(static_cast<const uchar*>(data), int(width), int(height), qsizetype(pitch),
            h->pixelFormat_ == RETRO_PIXEL_FORMAT_RGB565 ? QImage::Format_RGB16 : QImage::Format_RGB555);
        result = wrapped.convertToFormat(QImage::Format_RGB32);
    } else {
        result = QImage(int(width), int(height), QImage::Format_RGB32);
        if (!result.isNull()) for (unsigned y = 0; y < height; ++y) {
            const auto* source = static_cast<const uchar*>(data) + pitch*y;
            auto* out = reinterpret_cast<QRgb*>(result.scanLine(int(y)));
            for (unsigned x = 0; x < width; ++x) {
                quint32 pixel; std::memcpy(&pixel,source+x*4,4); out[x] = pixel | 0xff000000u;
            }
        }
    }
    if (result.isNull()) return;
    const auto filter = h->variables_.find("genesis_plus_gx_blargg_ntsc_filter");
    if (filter != h->variables_.end() && filter->second != "disabled") {
        // GPX update_viewport() filters the full viewport before removing the
        // SMS left border. H40 uses 4->8 samples; H32/SMS/GG use 3->7. All
        // native viewport widths (including overscan/GG crop) are multiples of 4.
        const int full = int(h->av_.geometry.base_width);
        const int crop = full - int(width);
        int nativeFull = 0;
        if (full > 0 && full % 8 == 0) {
            const int minimum = full/8*4-1;
            nativeFull = (minimum+3)/4*4;
        } else if (full > 0 && full % 7 == 0) {
            const int minimum = (full/7-1)*3;
            const int candidate = (minimum+3)/4*4;
            if (candidate <= minimum+2) nativeFull = candidate;
        }
        // GPX trims 20/40 filtered samples for the optional 8/16 native-pixel
        // SMS border. Blargg leaves vertical/interlaced geometry unchanged.
        if (nativeFull > 0 && crop >= 0 && crop <= 40 && crop % 20 == 0) {
            const QSize native(nativeFull-crop*2/5,int(height));
            if (VideoFrameGeometry::validSize(native)) VideoFrameGeometry::setNativeSize(result,native);
        }
    }
    h->frame_ = std::move(result);
}
void LibretroHost::audioSample(int16_t left, int16_t right) { const int16_t samples[2]{left,right}; audioBatch(samples,1); }
size_t LibretroHost::audioBatch(const int16_t* data, size_t frames) {
    auto* h = active_;
    if (h && h->audioEnabled_ && data && frames <= 192000 && h->audio_.size() + qsizetype(frames * 4) <= 768000)
        h->audio_.append(reinterpret_cast<const char*>(data), qsizetype(frames * 4));
    return frames;
}
void LibretroHost::inputPoll() {}
int16_t LibretroHost::inputState(unsigned port, unsigned device, unsigned, unsigned id) {
    if (!active_ || port >= active_->callbackPlayers_.size()) return 0;
    const int owner = active_->callbackPlayers_[port];
    const unsigned baseDevice = device & RETRO_DEVICE_MASK;
    if (owner < 0 || !active_->controllerConnected_[owner])
        return baseDevice == RETRO_DEVICE_LIGHTGUN && id == RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN;
    auto mask = active_->buttons_[owner];
    if (baseDevice == RETRO_DEVICE_LIGHTGUN) {
        const auto& aim = active_->gunAim_[owner];
        const bool reload = (mask & (1u << 2)) != 0;
        switch (id) {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return aim.x;
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return aim.y;
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return aim.offscreen || reload;
        case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return reload || (mask & (1u << 1));
        case RETRO_DEVICE_ID_LIGHTGUN_AUX_A: return !!(mask & 1u);
        case RETRO_DEVICE_ID_LIGHTGUN_AUX_B: return !!(mask & (1u << 8));
        case RETRO_DEVICE_ID_LIGHTGUN_START: return !!(mask & (1u << 3));
        default: return 0;
        }
    }
    if (baseDevice != RETRO_DEVICE_JOYPAD) return 0;
    if (port == 0 && active_->contentExtension_ == "sms")
        mask |= (active_->buttons_[0] | active_->buttons_[1]) & (1u << RETRO_DEVICE_ID_JOYPAD_START);
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return int16_t(mask);
    return id < 16 && (mask & (1u << id)) ? 1 : 0;
}
