#pragma once
#include <QString>
#include <memory>

// Read-only virtual stereo WAV over an original MP3. PCM is decoded
// on demand into one small block; seeking uses minimp3's compressed-frame index.
class Mp3Stream final {
public:
    Mp3Stream();
    ~Mp3Stream();
    // expectedRate=0 accepts 44.1 or 48 kHz; the default preserves CD audio.
    bool open(const QString& path, QString& error, int expectedRate = 44100);
    int sampleRate() const;
    qint64 size() const;
    qint64 pos() const;
    bool seek(qint64 position);
    qint64 read(char* destination, qint64 bytes);
private:
    struct State;
    std::unique_ptr<State> state_;
};
