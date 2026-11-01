#pragma once
#include <QString>
#include <memory>

// Read-only virtual 44.1 kHz stereo WAV over an original MP3. PCM is decoded
// on demand into one small block; seeking uses minimp3's compressed-frame index.
class Mp3Stream final {
public:
    Mp3Stream();
    ~Mp3Stream();
    bool open(const QString& path, QString& error);
    qint64 size() const;
    qint64 pos() const;
    bool seek(qint64 position);
    qint64 read(char* destination, qint64 bytes);
private:
    struct State;
    std::unique_ptr<State> state_;
};
