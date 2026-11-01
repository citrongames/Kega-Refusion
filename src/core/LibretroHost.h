#pragma once
#include <QByteArray>
#include <QImage>
#include <QLibrary>
#include <QString>
#include <QList>
#include <map>
#include <array>
#include <functional>
#include <memory>
#include <libretro.h>
#include "DiscMedia.h"
#include <QStringList>

// Own on exactly one emulation thread. One libretro instance per process.
class LibretroHost final {
public:
    explicit LibretroHost(QString corePath, QString dataPath);
    ~LibretroHost();
    bool load(const QString& path, QString& error, int archiveIndex = -1);
    void unload();
    bool isCd() const { return loaded_ && !discs_.isEmpty(); }
    QStringList discPaths() const;
    int discIndex() const { return discIndex_; }
    bool discEjected() const;
    bool changeDisc(const QString& path, QString& error);
    bool selectDisc(int index, QString& error);
    bool setDiscEjected(bool ejected, QString& error);
    void runFrame(bool renderVideo = true, bool captureAudio = true);
    void reset();
    bool hardReset(QString& error);
    // Region: 0 Auto, 1 Japan, 2 USA, 3 Europe. Filter: 0 Off, 1 Mono,
    // 2 Composite, 3 S-Video, 4 RGB. Core applies changes on the next frame.
    bool setRegion(int region, QString& error);
    bool setBlarggFilter(int filter, QString& error);
    // Applies to the core's ROM copy at load/power cycle, never to source bytes.
    bool setAutoFixChecksum(bool enabled, QString& error);
    bool saveState(int slot, QString& error);
    enum class StateLoadError { None, Missing, Failed };
    bool loadState(int slot, QString& error, StateLoadError* failure = nullptr);
    QByteArray serialize() const;
    bool unserialize(const QByteArray& state);
    unsigned occupiedStateSlots() const;
    QByteArray captureRewindState() const;
    bool restoreRewindState(const QByteArray& state);
    bool flushSaveRam(QString& error);
    void setButtons(unsigned port, unsigned mask);
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
    struct Api;
    bool initialize(QString& error);
    void prepareCoreContent();
    bool loadCd(const QString& path, QString& error);
    bool checkDiscBios(const DiscImage& image, QString& error) const;
    bool registerDiscs(QString& error);
    bool refreshDiscFiles(QString& error);
    void setDiscIdentity();
    bool setCoreOption(const QByteArray& key, const QByteArray& value, QString& error);
    QString statePath(int slot) const;
    bool restoreSaveRam(QString& error);
    static bool environment(unsigned command, void* data);
    static void video(const void* data, unsigned width, unsigned height, size_t pitch);
    static void audioSample(int16_t left, int16_t right);
    static size_t audioBatch(const int16_t* data, size_t frames);
    static void inputPoll();
    static int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id);
    static thread_local LibretroHost* active_;
    QLibrary library_;
    std::unique_ptr<Api> api_;
    QString dataPath_, gamePath_, gameKey_, contentName_;
    QByteArray systemPath_, savePath_, content_, coreContent_, encodedGamePath_, romHash_, coreHash_;
    QByteArray contentDirectory_, contentBaseName_, contentExtension_, archiveMember_;
    retro_game_info_ext contentInfo_{};
    retro_disk_control_callback disk_{};
    QVector<DiscImage> discs_;
    int discIndex_ = 0;
    bool contentInfoActive_ = false;
    std::map<QByteArray,QByteArray> variables_, optionOverrides_;
    std::map<QByteArray,QList<QByteArray>> variableValues_;
    bool variablesUpdated_ = false, autoFixChecksum_ = false;
    std::array<unsigned,2> buttons_{};
    std::array<size_t,2> saveMemorySizes_{};
    retro_system_av_info av_{};
    retro_pixel_format pixelFormat_ = RETRO_PIXEL_FORMAT_0RGB1555;
    QImage frame_;
    QByteArray audio_;
    bool videoEnabled_ = true, audioEnabled_ = true;
    bool initialized_ = false, loaded_ = false, saveRamWritable_ = true;
};
