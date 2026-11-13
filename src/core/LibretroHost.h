#pragma once
#include "InputDisplay.h"
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QLibrary>
#include <QString>
#include <QList>
#include <QMap>
#include <map>
#include <array>
#include "input/ControllerTopology.h"
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

// One logical session on the emulation worker; optional isolated Link Cable peer.
class LibretroHost final {
public:
    explicit LibretroHost(QString corePath, QString dataPath);
    ~LibretroHost();
    bool load(const QString& path, QString& error, int archiveIndex = -1,
              const RomArchive::Entry* archiveEntry = nullptr);
    void unload();
    bool isCd() const { return loaded_ && !discs_.isEmpty(); }
    // A loaded game keeps one storage snapshot through resets and settings edits.
    QString dataFolder(const QString& category) const;
    QStringList discPaths() const;
    int discIndex() const { return discIndex_; }
    bool discEjected() const;
    // Reports only operations capable of disturbing the drive, even if they fail.
    bool changeDisc(const QString& path, QString& error, bool* timelineChanged = nullptr);
    bool selectDisc(int index, QString& error, bool* timelineChanged = nullptr);
    bool setDiscEjected(bool ejected, QString& error, bool* timelineChanged = nullptr);
    bool setLinkCableEnabled(bool enabled, QString& error);
    bool linkCableEnabled() const { return linkRequested_; }
    void setLinkPresentationRole(int role) { linkPresentationRole_ = role == 2 ? 2 : 1; }
    const QImage& frameForLinkRole(int role) const;
    double aspectRatioForLinkRole(int role) const;
    void runFrame(bool renderVideo = true, bool captureAudio = true);
    // Woven output needs two consecutive input frames from T-2; other modes need T-1.
    // The scope preserves transient fields through canonical restore and bypasses
    // recursive LCD ghosting. A warmup frame may render without publication.
    bool rewindPreviewNeedsPair() const;
    void beginRewindPreview();
    void endRewindPreview();
    void runRewindPreviewFrame(bool renderVideo = true, bool singleFieldFallback = false, bool publishVideo = true);
    bool frameInputPolled() const;
    bool supportsInputPollMetrics() const;
    void setExpansionDevices(bool cd, bool x32);
    void reset();
    bool hardReset(QString& error, bool applyPending = true);
    void setNukedYm2612(bool enabled) { nukedYm2612_ = enabled; }
    void setSmsFmEnabled(bool enabled) { smsFmEnabled_ = enabled; }
    bool isMasterSystem() const;
    bool isGameGear() const;
    bool gameGearLcdEnabled() const;
    bool smsPauseStateSupported() const;
    struct DebugCapabilities { quint32 video = 0; quint64 audio = 0; };
    DebugCapabilities debugCapabilities() const;
    void setDebugMasks(quint32 video, quint64 audio);
    void setDebugBackdrop(quint32 rgb);
    void setDebugAudioGain(unsigned channel, unsigned percent);
    quint32 debugVideoMask() const { return debugVideoMask_; }
    quint64 debugAudioMask() const { return debugAudioMask_; }
    // Region: 0 Auto, 1 Japan, 2 USA, 3 Europe. Filter: 0 Off, 1 Mono,
    // 2 Composite, 3 S-Video, 4 RGB. Core applies changes on the next frame.
    bool setRegion(int region, QString& error, bool* changed = nullptr);
    bool setBlarggFilter(int filter, QString& error, bool* changed = nullptr);
    bool setGameGearLcd(bool enabled, QString& error, bool* changed = nullptr);
    bool setFastCdLoading(bool enabled, QString& error, bool* changed = nullptr);
    bool setRemoveSpriteLimit(bool enabled, QString& error, bool* changed = nullptr);
    // Applies to the core's ROM copy at load/power cycle, never to source bytes.
    bool setAutoFixChecksum(bool enabled, QString& error);
    const CheatList& cheats() const { return movieCheatsActive_ ? movieCheats_ : cheats_.entries(); }
    Cheats::Platform cheatPlatform() const;
    bool cheatsSupported() const;
    bool cheatMemory(QByteArray& memory, quint32& base, bool& bigEndian, QString& error) const;
    QString cheatSupportText() const;
    // activeChanged excludes descriptions and disabled-code edits.
    bool setCheats(const CheatList& entries, QString& error, bool* activeChanged = nullptr);
    bool saveState(int slot, QString& error);
    bool saveStateToFile(const QString& path, QString& error);
    enum class StateLoadError { None, Missing, Failed };
    bool loadState(int slot, QString& error, StateLoadError* failure = nullptr);
    bool loadStateFromFile(const QString& path, QString& error, StateLoadError* failure = nullptr);
    QByteArray serialize() const;
    QByteArray netplayDigest() const;
    static QByteArray unpackNetworkState(const QByteArray& packed);
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
    bool restoreMovieState(const QByteArray& state, QString& error, bool trusted = false, bool* restoredClock = nullptr);
    QJsonObject movieConfiguration(bool forPowerReset = false) const;
    bool prepareNetwork(const QJsonObject& configuration, QString& error);
    bool restoreNetworkState(const QByteArray& state, QString& error);
    bool prepareMovie(const QJsonObject& configuration, QString& error);
    bool restoreMovieConfiguration(QString& error, bool keepRunningFm = false);
    QByteArray movieGameIdentity() const;
    QByteArray movieCoreIdentity() const;
    unsigned achievementConsoleId() const;
    QString achievementHash(QString& error) const;
    uint32_t readAchievementMemory(uint32_t address, uint8_t* buffer, uint32_t count);
    bool captureAchievementMemory(QByteArray& output);
    static uint32_t readAchievementSnapshot(const QByteArray& snapshot, uint32_t address,
                                            uint8_t* buffer, uint32_t count);
    QString achievementHardcoreBlockReason() const;
    std::function<QByteArray()> captureAchievementProgress;
    std::function<void(const QByteArray&)> restoreAchievementProgress;
    const QString& movieCompatibilityWarning() const { return movieCompatibilityWarning_; }
    void setMoviePlayback(bool playback) { moviePlayback_ = playback; }
    bool flushSaveRam(QString& error);
    void setPerGameCdBackup(bool enabled) { perGameCdBackup_ = enabled; }
    void setNetworkReadOnly(bool readOnly) { networkReadOnly_ = readOnly; }
    bool pollSaveRam(qint64 nowMs, QString& error);
    ConsoleProfile controllerProfile() const;
    InputDisplayState inputDisplay(bool withButtons = true) const;
    void setButtons(unsigned port, unsigned mask);
    // Virtual console ports, independent of physical input device selection.
    // Configuration survives unload/load. Returns true only for a change.
    bool setControllerAdapter(ControllerAdapter adapter, QString& error);
    ControllerAdapter controllerAdapter() const;
    ControllerAdapter requestedControllerAdapter() const { return adapter_; }
    unsigned controllerPlayerCount() const;
    bool hasJCart() const;
    unsigned jcartPlayerBase() const;
    bool setControllerConnected(unsigned port, bool connected);
    bool controllerConnected(unsigned port) const;
    bool setControllerSixButton(unsigned port, bool sixButton);
    bool controllerSixButton(unsigned port) const;
    bool setControllerKind(unsigned player, ControllerType type);
    ControllerType controllerKind(unsigned player) const;
    void setGunAim(unsigned player, int x, int y, bool offscreen);
    void setPointer(unsigned player, int x, int y);
    bool loaded() const { return loaded_; }
    struct RunClock {
        quint64 frames = 0;
        quint64 ticks = 0; // 2^32 ticks per second, independent of video mode.
        quint64 microseconds() const {
            return (ticks >> 32) * 1000000 + (((ticks & 0xffffffffULL) * 1000000) >> 32);
        }
    };
    RunClock runClock() const { return runClock_; }
    void setRunClock(RunClock clock) { runClock_ = clock; }
    double fps() const { return av_.timing.fps; }
    int sampleRate() const { return int(av_.timing.sample_rate); }
    double aspectRatio() const;
    QSize nativeVideoSize() const;
    const QImage& frame() const;
    void restoreFramePreview(const QImage& frame, double aspect);
    // Presentation-only sidecar; pixels remain separate from clean captures.
    void setSpriteOwnershipEnabled(bool enabled);
    const QImage& spriteOwnership() const;
    // Capture pixels before core output filters. Enable before the first recorded
    // runFrame; the returned image owns its pixels and follows the presented peer.
    bool setRecordingVideoEnabled(bool enabled);
    QImage recordingFrame() const;
    const QByteArray& audio() const;
    QString gamePath() const { return gamePath_; }
    QString contentName() const { return contentName_; }
    std::function<void(const QString&)> message;
private:
    struct Activation { LibretroHost* previous; explicit Activation(LibretroHost* h) : previous(active_) { active_=h; } ~Activation() { active_=previous; } };
    struct LinkPair;
    std::unique_ptr<LibretroHost> linkPeer_;
    struct LinkPairDeleter { void operator()(LinkPair*) const; };
    std::unique_ptr<LinkPair,LinkPairDeleter> linkPair_;
    LibretroHost* linkParent_ = nullptr;
    bool linkRequested_ = false;
    int linkPresentationRole_ = 1;
    bool supportsLinkCable() const;
    bool startLinkedPair(QString& error);
    void stopLinkedPair();
    void runLinkedFrame(bool video, bool audio);
    void resetLinkedPair();
    qsizetype serializedSize() const;
    QByteArray captureLinkedState() const;
    bool restoreLinkedState(const QByteArray&, bool transactional = true);
    QByteArray stateCoreIdentity() const;
    bool acceptsLegacyMovieCore(const QByteArray& identity) const;
    QJsonObject movieBiosFingerprints() const;
    void snapshot32xBios();
    QJsonObject active32xBiosFingerprints_;
    quint32 debugVideoMask_ = 0, debugBackdrop_ = 0xff00ff;
    quint64 debugAudioMask_ = 0;
    std::array<quint8,38> debugAudioGains_ = [] { std::array<quint8,38> gains{}; gains.fill(100); return gains; }();
    RunClock runClock_;
    double runRate_ = 0;
    quint64 ticksPerFrame_ = 0;
    QByteArray serializeCore(bool includeClock, qsizetype prefix = 0, qsizetype suffix = 0) const;
    QByteArray captureMoviePayload(bool checksum, bool includeClock) const;
    bool restoreCore(const char* data, qsizetype size, bool transactional = true);
    struct Api;
    bool initialize(QString& error);
    bool selectDataFolders(QString& error);
    void prepareCoreContent();
    bool applyPowerOnOptions(QString& error, bool applyPending = true);
    bool hasCdHardware() const;
    void loadCheats();
    void applyCheats();
    void clearCoreCheats();
    void applyControllerPorts(bool force = false);
    bool loadCd(const QString& path, QString& error);
    bool checkDiscBios(const DiscImage& image, QString& error, const QString& biosDirectory = {}) const;
    bool registerDiscs(QString& error);
    bool refreshDiscFiles(QString& error);
    void setDiscIdentity();
    bool setCoreOption(const QByteArray& key, const QByteArray& value, QString& error, bool* changed = nullptr);
    bool applyGameGearLcd(QString& error, bool* changed = nullptr);
    bool saveStateFile(const QString& path, QString& error, bool keepPrevious);
    QString statePath(int slot) const;
    QString saveReadPath(const QString& directory, const QString& suffix) const;
    bool restoreSaveRam(QString& error);
    bool updateSaveRam(qint64 nowMs, bool force, QString& error);
    void resetAchievementMemory();
    bool initializeAchievementMemory();
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
    QString dataPath_, gamePath_, gameKey_, legacyGameKey_, contentName_;
    QMap<QString,QString> dataFolders_;
    QString movieCompatibilityWarning_;
    void refreshRecordingIdentity();
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
    QByteArray localBlarggFilter_ = "disabled";
    bool localGameGearLcd_ = false;
    std::map<QByteArray,QList<QByteArray>> variableValues_;
    bool variablesUpdated_ = false, autoFixChecksum_ = false;
    bool nukedYm2612_ = false;
    bool smsFmEnabled_ = true;
    bool attachCd_ = false, attach32x_ = false, contentNeeds32x_ = false;
    std::array<unsigned,MaxControllerPlayers> buttons_{};
    std::array<bool,MaxControllerPlayers> controllerConnected_{{true,false,false}};
    std::array<bool,MaxControllerPlayers> controllerSixButton_{{true,true,true,true,true,true,true,true}};
    ControllerAdapter adapter_ = ControllerAdapter::None, preMovieAdapter_ = ControllerAdapter::None;
    unsigned appliedKinds_ = ~0u;
    bool appliedExtended_ = false;
    ControllerAdapter appliedAdapter_ = ControllerAdapter::None;
    unsigned appliedConnected_ = 0, appliedSixButton_ = 0;
    std::array<unsigned,2> controllerDevices_{{~0u,~0u}};
    std::array<ControllerType,MaxControllerPlayers> controllerKinds_{{ControllerType::Pad,ControllerType::Pad,ControllerType::Justifier}};
    // GPX compacts callback indices over connected devices; Justifiers add a third callback.
    std::array<int,MaxControllerPlayers> callbackPlayers_{{0,-1,-1,-1,-1,-1,-1,-1}};
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
    std::array<bool,MaxControllerPlayers> preMovieControllerConnected_{}, preMovieControllerSixButton_{};
    std::array<ControllerType,MaxControllerPlayers> preMovieControllerKinds_{};
    QList<QByteArray> activeCheatCodes_;
    retro_system_av_info av_{};
    retro_pixel_format pixelFormat_ = RETRO_PIXEL_FORMAT_0RGB1555;
    QImage frame_, spriteOwnership_;
    bool spriteOwnershipEnabled_ = false;
    QByteArray audio_;
    bool videoEnabled_ = true, audioEnabled_ = true;
    bool rewindPreview_ = false;
    unsigned rewindPreviewDepth_ = 0;
    bool rewindPreviewBeforeScope_ = false;
    bool recordingVideoEnabled_ = false;
    bool initialized_ = false, loaded_ = false, saveRamWritable_ = true;
    bool networkReadOnly_ = false;
    bool perGameCdBackup_ = true;
    QByteArray cdSavePath_;
    const QByteArray& activeSavePath() const { return cdSavePath_.isEmpty() ? savePath_ : cdSavePath_; }
    void rememberEmptyCdBackup();
};
