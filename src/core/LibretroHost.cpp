#include "LibretroHost.h"
#include "RomArchive.h"
#include "VideoFrameGeometry.h"
#include "SegaCdBios.h"
#include "CoreFileSystem.h"
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
#include <chrono>

thread_local LibretroHost* LibretroHost::active_ = nullptr;
struct LibretroHost::Api {
#define API(name) decltype(&retro_##name) name = nullptr
    API(init); API(deinit); API(api_version); API(get_system_info); API(get_system_av_info);
    API(set_environment); API(set_video_refresh); API(set_audio_sample); API(set_audio_sample_batch);
    API(set_input_poll); API(set_input_state); API(set_controller_port_device);
    API(load_game); API(unload_game); API(run); API(reset); API(serialize_size); API(serialize);
    API(unserialize); API(get_memory_data); API(get_memory_size);
#undef API
    void (*manage_bram)(bool) = nullptr;
    bool (*get_bram)(unsigned, const void**, size_t*, const char**) = nullptr;
    const char* (*state_abi)() = nullptr;
};
namespace {
std::atomic<LibretroHost*> coreOwner{nullptr};
constexpr qsizetype maxStateSize = 64 * 1024 * 1024;
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
    api_->manage_bram = reinterpret_cast<decltype(api_->manage_bram)>(library_.resolve("kegarefusion_manage_bram"));
    api_->get_bram = reinterpret_cast<decltype(api_->get_bram)>(library_.resolve("kegarefusion_get_bram"));
    api_->state_abi = reinterpret_cast<decltype(api_->state_abi)>(library_.resolve("kegarefusion_state_abi"));
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
    error = QStringLiteral("Не найден BIOS Sega CD. Импортируйте BIOS через Файл → Sega CD → BIOS…");
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
    QByteArray identity = discs_[discIndex_].hash;
    const auto region = optionOverrides_.find("genesis_plus_gx_region_detect");
    identity += region == optionOverrides_.end() ? QByteArray("auto") : region->second;
    for (const auto* code : {"J","U","E"}) {
        QFile bios(dataPath_ + "/bios/bios_CD_" + code + ".bin");
        identity += bios.open(QIODevice::ReadOnly) && bios.size() == 131072 ? digest(bios.readAll()) : QByteArray(32,'\0');
    }
    romHash_ = digest(identity);
    gameKey_ = QString::fromLatin1(romHash_.toHex());
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
    for (unsigned port = 0; port < 2; ++port)
        api_->set_controller_port_device(port,RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD,1));
    // The battery extension exposes formatted BRAM and its regional path.
    // GPX still owns initialization/formatting; the host owns atomic writes.
    saveMemorySizes_.fill(0);
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
    return setDiscEjected(false,error);
}
bool LibretroHost::changeDisc(const QString& path, QString& error) {
    error.clear();
    if (!isCd()) { error = QStringLiteral("Игра Sega CD не запущена."); return false; }
    DiscImage image;
    if (!DiscMedia::inspect(path,image,error) || !DiscMedia::prepare(image,dataPath_,error)) return false;
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
    if (!refreshDiscFiles(error)) return false;
    setDiscIdentity();
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
    const QStringList supported{"md", "gen", "bin", "smd", "sms", "gg", "sg", "68k"};
    if (!supported.contains(ext) || content.size() < 512 || content.size() > 32 * 1024 * 1024) {
        error = QStringLiteral("Неподдерживаемый картридж или неверный размер ROM."); return false;
    }
    const auto encodedPath = QDir::toNativeSeparators(info.absoluteFilePath()).toUtf8();
    if (!initialize(error)) return false;
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
    contentName_ = name;
    romHash_ = digest(content_);
    gameKey_ = QString::fromLatin1(romHash_.toHex());
    api_->get_system_av_info(&av_);
    if (!validAvInfo(av_)) {
        error = QStringLiteral("Ядро вернуло некорректные параметры видео/звука."); unload(); return false;
    }
    // Genesis 6-button pad; Master System/Game Gear/SG-1000 keep plain joypads.
    const bool md = ext != "sms" && ext != "gg" && ext != "sg";
    for (unsigned port = 0; port < 2; ++port)
        api_->set_controller_port_device(port, md ? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1) : RETRO_DEVICE_JOYPAD);
    saveMemorySizes_[0] = isCd() ? 0 : api_->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    saveMemorySizes_[1] = isCd() ? 0 : api_->get_memory_size(RETRO_MEMORY_RTC);
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
    CoreFileSystem::clearVirtualFiles();
    discs_.clear(); discIndex_ = 0;
    contentInfo_ = {}; contentInfoActive_ = false;
    content_.clear(); coreContent_.clear(); audio_.clear(); frame_ = QImage(); gamePath_.clear(); gameKey_.clear(); contentName_.clear(); romHash_.clear();
    buttons_.fill(0);
    saveRamWritable_ = true;
    saveMemorySizes_.fill(0);
    batterySave_.clear();
}
void LibretroHost::runFrame(bool renderVideo, bool captureAudio) {
    if (!loaded_) return;
    videoEnabled_ = renderVideo; audioEnabled_ = captureAudio;
    audio_.clear(); api_->run();
    // Serialization and normal calls retain accurate audio state: never set
    // libretro's fast-savestate or hard-disable-audio flags.
    videoEnabled_ = audioEnabled_ = true;
}
void LibretroHost::reset() { if (loaded_) { api_->reset(); audio_.clear(); } }
bool LibretroHost::hardReset(QString& error) {
    error.clear();
    if (!loaded_) { error = QStringLiteral("Нет загруженной игры."); return false; }
    if (isCd() && !checkDiscBios(discs_[discIndex_],error)) return false;
    // Reuse the selected content already in memory: archives need no second
    // selection and moved/deleted ROM files do not affect a power cycle.
    if (!flushSaveRam(error)) return false;
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
    const auto ext = contentExtension_;
    const bool md = ext != "sms" && ext != "gg" && ext != "sg";
    for (unsigned port = 0; port < 2; ++port)
        api_->set_controller_port_device(port, md ? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1) : RETRO_DEVICE_JOYPAD);
    saveMemorySizes_[0] = isCd() ? 0 : api_->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    saveMemorySizes_[1] = isCd() ? 0 : api_->get_memory_size(RETRO_MEMORY_RTC);
    QString saveError;
    saveRamWritable_ = restoreSaveRam(saveError);
    if (!saveRamWritable_ && message) message(saveError + QStringLiteral(" Автозапись отключена, чтобы не затереть существующий файл."));
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
bool LibretroHost::setBlarggFilter(int filter, QString& error) {
    static const std::array<QByteArray,5> values{"disabled", "monochrome", "composite", "svideo", "rgb"};
    if (filter < 0 || filter >= int(values.size())) {
        error = QStringLiteral("Неверный фильтр Blargg."); return false;
    }
    return setCoreOption("genesis_plus_gx_blargg_ntsc_filter", values[size_t(filter)], error);
}
void LibretroHost::setButtons(unsigned port, unsigned mask) { if (port < buttons_.size()) buttons_[port] = mask; }
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
    return loaded_ && size_t(state.size()) == api_->serialize_size() && api_->unserialize(state.constData(), size_t(state.size()));
}
QString LibretroHost::statePath(int slot) const { return dataPath_ + "/states/" + gameKey_ + QStringLiteral(".slot%1.krs").arg(slot); }
unsigned LibretroHost::occupiedStateSlots() const {
    unsigned mask = 0;
    if (loaded_) for (int slot = 0; slot < 10; ++slot)
        if (QFileInfo(statePath(slot)).isFile()) mask |= 1u << slot;
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
bool LibretroHost::saveState(int slot, QString& error) {
    if (!loaded_ || slot < 0 || slot > 9) { error = QStringLiteral("Нет игры или неверный слот сохранения."); return false; }
    if (isCd() && discEjected()) { error = QStringLiteral("Закройте привод перед сохранением состояния."); return false; }
    const auto payload = serialize();
    if (payload.isEmpty()) { error = QStringLiteral("Ядро не смогло сохранить состояние."); return false; }
    QByteArray bytes = stateMagic + romHash_ + coreHash_ + digest(payload);
    quint32 size = qToBigEndian(quint32(payload.size()));
    bytes.append(reinterpret_cast<const char*>(&size), sizeof(size));
    bytes.append(payload);
    return atomicWrite(statePath(slot), bytes, error);
}
bool LibretroHost::loadState(int slot, QString& error, StateLoadError* failure) {
    if (failure) *failure = StateLoadError::Failed;
    if (!loaded_ || slot < 0 || slot > 9) { error = QStringLiteral("Нет игры или неверный слот сохранения."); return false; }
    if (isCd() && discEjected()) { error = QStringLiteral("Закройте привод перед загрузкой состояния."); return false; }
    QFile file(statePath(slot));
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
    if (bytes.size() < stateHeaderSize || bytes.left(8) != stateMagic || bytes.mid(8,32) != romHash_ || bytes.mid(40,32) != coreHash_) {
        error = QStringLiteral("Сохранение относится к другой игре или сборке ядра."); return false;
    }
    const quint32 size = qFromBigEndian<quint32>(bytes.constData() + 104);
    const auto payload = bytes.mid(stateHeaderSize);
    if (size != quint32(payload.size()) || bytes.mid(72,32) != digest(payload)) { error = QStringLiteral("Сохранение повреждено."); return false; }
    if (!unserialize(payload)) { error = QStringLiteral("Ядро отклонило сохранение."); return false; }
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
    if (!loaded_ || !saveRamWritable_) return true;
    if (isCd()) {
        if (!api_->get_bram) return true; // Unmodified cores retain their unload writer.
        for (unsigned index = 0; index < 2; ++index) {
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
        return true;
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
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS: return true;
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
    if (!active_ || port >= 2 || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD) return 0;
    const auto mask = active_->buttons_[port];
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return int16_t(mask);
    return id < 16 && (mask & (1u << id)) ? 1 : 0;
}
