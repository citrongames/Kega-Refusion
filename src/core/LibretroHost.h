#pragma once
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QLibrary>
#include <QString>
#include <QList>
#include <map>
#include <array>
#include <functional>
#include <memory>
#include <libretro.h>
#include <rc_libretro.h>
#include "DiscMedia.h"
#include "RomArchive.h"
#include "BatterySave.h"
#include "PapriumMusic.h"
#include "Cheats.h"
#include "../input/ControllerType.h"
#include <QStringList>

// Own on exactly one emulation thread. One libretro instance per process.
class LibretroHost final {
public:
    explicit LibretroHost(QString corePath, QString dataPath);
    ~LibretroHost();
    bool load(const QString& path, QString& error, int archiveIndex = -1,
              const RomArchive::Entry* archiveEntry = nullptr);
    void unload();
    bool isCd() const { return loaded_ && !discs_.isEmpty(); }
    QStringList discPaths() const;
    int discIndex() const { return discIndex_; }
    bool discEjected() const;
    bool changeDisc(const QString& path, QString& error);
    bool selectDisc(int index, QString& error);
    bool setDiscEjected(bool ejected, QString& error);
    void runFrame(bool renderVideo = true, bool captureAudio = true);
    bool frameInputPolled() const;
    bool supportsInputPollMetrics() const;
    void setExpansionDevices(bool cd, bool x32);
    void reset();
    bool hardReset(QString& error);
    // Region: 0 Auto, 1 Japan, 2 USA, 3 Europe. Filter: 0 Off, 1 Mono,
    // 2 Composite, 3 S-Video, 4 RGB. Core applies changes on the next frame.
    bool setRegion(int region, QString& error);
    bool setBlarggFilter(int filter, QString& error);
    bool setFastCdLoading(bool enabled, QString& error);
    bool setRemoveSpriteLimit(bool enabled, QString& error);
    // Applies to the core's ROM copy at load/power cycle, never to source bytes.
    bool setAutoFixChecksum(bool enabled, QString& error);
    const CheatList& cheats() const { return movieCheatsActive_ ? movieCheats_ : cheats_.entries(); }
    Cheats::Platform cheatPlatform() const;
    bool cheatsSupported() const;
    QString cheatSupportText() const;
    // activeChanged excludes descriptions and disabled-code edits.
    bool setCheats(const CheatList& entries, QString& error, bool* activeChanged = nullptr);
    bool saveState(int slot, QString& error);
    enum class StateLoadError { None, Missing, Failed };
    bool loadState(int slot, QString& error, StateLoadError* failure = nullptr);
    QByteArray serialize() const;
    QByteArray netplayDigest() const;
    static QByteArray unpackNetworkState(const QByteArray& packed);
    bool setNetplayStorage(const QString& scope, QString& error);
    bool unserialize(const QByteArray& state);
    unsigned occupiedStateSlots() const;
    QByteArray captureRewindState() const;
    bool restoreRewindState(const QByteArray& state);
    // Movie snapshots are independent of quick states and include every battery region.
    QByteArray captureMovieState(bool checksum = true) const;
    // Network snapshots retain the movie payload but use an OS-independent core contract.
    QByteArray captureNetworkState(bool checksum = true) const;
    QByteArray networkCompatibility() const;
    // trusted is only for snapshots retained inside RewindBuffer, never files.
    bool restoreMovieState(const QByteArray& state, QString& error, bool trusted = false);
    QJsonObject movieConfiguration() const;
    bool prepareNetwork(const QJsonObject& configuration, QString& error);
    bool restoreNetworkState(const QByteArray& state, QString& error);
    bool prepareMovie(const QJsonObject& configuration, QString& error);
    bool restoreMovieConfiguration(QString& error);
    QByteArray movieGameIdentity() const;
    QByteArray movieCoreIdentity() const;
    unsigned achievementConsoleId() const;
    QString achievementHash(QString& error) const;
    uint32_t readAchievementMemory(uint32_t address, uint8_t* buffer, uint32_t count);
    QString achievementHardcoreBlockReason() const;
    std::function<QByteArray()> captureAchievementProgress;
    std::function<void(const QByteArray&)> restoreAchievementProgress;
    const QString& movieCompatibilityWarning() const { return movieCompatibilityWarning_; }
    void setMoviePlayback(bool playback) { moviePlayback_ = playback; }
    bool flushSaveRam(QString& error);
    bool pollSaveRam(qint64 nowMs, QString& error);
    void setButtons(unsigned port, unsigned mask);
    // Virtual console ports, independent of physical input device selection.
    // Configuration survives unload/load. Returns true only for a change.
    bool setControllerConnected(unsigned port, bool connected);
    bool controllerConnected(unsigned port) const;
    bool setControllerSixButton(unsigned port, bool sixButton);
    bool controllerSixButton(unsigned port) const;
    bool setControllerKind(unsigned player, ControllerType type);
    ControllerType controllerKind(unsigned player) const { return player < controllerKinds_.size() ? controllerKinds_[player] : ControllerType::Pad; }
    void setGunAim(unsigned player, int x, int y, bool offscreen);
    bool loaded() const { return loaded_; }
    double fps() const { return av_.timing.fps; }
    int sampleRate() const { return int(av_.timing.sample_rate); }
    double aspectRatio() const;
    QSize nativeVideoSize() const;
    const QImage& frame() const { return frame_; }
    const QByteArray& audio() const { return audio_; }
    QString gamePath() const { return gamePath_; }
    QString contentName() const { return contentName_; }
    std::function<void(const QString&)> message;
private:
    QByteArray stateCoreIdentity() const;
    bool acceptsLegacyMovieCore(const QByteArray& identity) const;
    QJsonObject movieBiosFingerprints() const;
    struct Api;
    bool initialize(QString& error);
    void prepareCoreContent();
    bool applyExpansionDevices(QString& error);
    bool hasCdHardware() const;
    void loadCheats();
    void applyCheats();
    void clearCoreCheats();
    void applyControllerPorts(bool force = false);
    bool loadCd(const QString& path, QString& error);
    bool checkDiscBios(const DiscImage& image, QString& error) const;
    bool registerDiscs(QString& error);
    bool refreshDiscFiles(QString& error);
    void setDiscIdentity();
    bool setCoreOption(const QByteArray& key, const QByteArray& value, QString& error);
    QString statePath(int slot) const;
    QString saveReadPath(const QString& directory, const QString& suffix) const;
    bool restoreSaveRam(QString& error);
    bool updateSaveRam(qint64 nowMs, bool force, QString& error);
    void resetAchievementMemory();
    static void achievementCoreMemoryInfo(uint32_t id, rc_libretro_core_memory_info_t* info);
    static void* achievementCdOpenTrack(const char* path, uint32_t track);
    static size_t achievementCdReadSector(void* track, uint32_t sector, void* buffer, size_t count);
    static void achievementCdCloseTrack(void* track);
    static uint32_t achievementCdFirstSector(void* track);
    static bool environment(unsigned command, void* data);
    static void video(const void* data, unsigned width, unsigned height, size_t pitch);
    static void audioSample(int16_t left, int16_t right);
    static size_t audioBatch(const int16_t* data, size_t frames);
    static void inputPoll();
    static int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id);
    static std::uint64_t papriumMusicRead(void* user, unsigned track, std::uint64_t frame,
                                          std::int16_t* interleaved, std::size_t frames);
    static thread_local LibretroHost* active_;
    QLibrary library_;
    std::unique_ptr<Api> api_;
    QString netplaySaveRoot_;
    QString storageRoot() const { return netplaySaveRoot_.isEmpty() ? dataPath_ : netplaySaveRoot_; }
    QString dataPath_, gamePath_, gameKey_, legacyGameKey_, contentName_;
    QString movieCompatibilityWarning_;
    QByteArray systemPath_, savePath_, content_, coreContent_, encodedGamePath_, romHash_, coreHash_, exactCoreHash_, recordingAbi_, recordingCoreHash_;
    QByteArray contentDirectory_, contentBaseName_, contentExtension_, archiveMember_;
    retro_game_info_ext contentInfo_{};
    retro_disk_control_callback disk_{};
    std::array<retro_memory_descriptor,8> achievementDescriptors_{};
    unsigned achievementDescriptorCount_ = 0;
    rc_libretro_memory_regions_t achievementMemory_{};
    unsigned achievementMemoryConsole_ = 0;
    mutable unsigned achievementConsoleCache_ = 0;
    bool achievementMemoryInitialized_ = false;
    mutable QString achievementHashCache_;
    mutable QByteArray achievementCdHeader_;
    QVector<DiscImage> discs_;
    int discIndex_ = 0;
    bool contentInfoActive_ = false;
    std::map<QByteArray,QByteArray> variables_, optionOverrides_;
    std::map<QByteArray,QList<QByteArray>> variableValues_;
    bool variablesUpdated_ = false, autoFixChecksum_ = false;
    bool attachCd_ = false, attach32x_ = false, contentNeeds32x_ = false;
    std::array<unsigned,3> buttons_{};
    std::array<bool,3> controllerConnected_{{true,false,false}};
    std::array<bool,3> controllerSixButton_{{true,true,true}};
    std::array<unsigned,2> controllerDevices_{{~0u,~0u}};
    std::array<ControllerType,3> controllerKinds_{{ControllerType::Pad,ControllerType::Pad,ControllerType::Justifier}};
    // GPX compacts callback indices over connected devices; Justifiers add a third callback.
    std::array<int,3> callbackPlayers_{{0,-1,-1}};
    struct GunAim { int16_t x = 0, y = 0; bool offscreen = true; };
    std::array<GunAim,3> gunAim_{};
    std::array<size_t,2> saveMemorySizes_{};
    BatterySave batterySave_;
    PapriumMusic papriumMusic_;
    Cheats::Store cheats_;
    CheatList movieCheats_;
    bool movieCheatsActive_ = false, moviePlayback_ = false, movieConfigurationActive_ = false;
    std::map<QByteArray,QByteArray> preMovieVariables_, preMovieOptionOverrides_;
    bool preMovieAutoFixChecksum_ = false;
    std::array<bool,3> preMovieControllerConnected_{}, preMovieControllerSixButton_{};
    std::array<ControllerType,3> preMovieControllerKinds_{};
    QList<QByteArray> activeCheatCodes_;
    retro_system_av_info av_{};
    retro_pixel_format pixelFormat_ = RETRO_PIXEL_FORMAT_0RGB1555;
    QImage frame_;
    QByteArray audio_;
    bool videoEnabled_ = true, audioEnabled_ = true;
    bool initialized_ = false, loaded_ = false, saveRamWritable_ = true;
};
