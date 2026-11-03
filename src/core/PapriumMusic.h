#pragma once

#include <QString>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

class Mp3Stream;

// Optional, external Paprium soundtrack. Track IDs are the cartridge's music
// commands; positions and lengths are native-rate stereo PCM frames.
class PapriumMusic final {
public:
    PapriumMusic();
    ~PapriumMusic();

    static std::array<QString, 0x3f> discoverTracks(const QString& romPath);
    static bool isTrackFileName(const QString& name);
    void configure(const QString& romPath);
    void clear();

    bool available(unsigned track) const;
    std::uint64_t frames(unsigned track);
    int sampleRate(unsigned track);
    std::size_t read(unsigned track, std::uint64_t frame,
                     std::int16_t* interleaved, std::size_t frames);
    const QString& error() const { return error_; }

private:
    bool activate(unsigned track);

    std::array<QString, 0x3f> paths_{};
    std::array<std::uint64_t, 0x3f> lengths_{};
    std::array<int, 0x3f> rates_{};
    std::array<bool, 0x3f> lengthKnown_{};
    std::array<bool, 0x3f> failed_{};
    std::unique_ptr<Mp3Stream> stream_;
    unsigned active_ = 0;
    QString error_;
};
