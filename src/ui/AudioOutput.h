#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QAudioSink;
class QIODevice;

class AudioOutput final : public QObject {
    Q_OBJECT
public:
    explicit AudioOutput(QObject* parent = nullptr);
    void play(const QByteArray& pcm, int sampleRate);
    void setMuted(bool muted);
    void setVolume(int percent);
    void clear();

signals:
    void unavailable(const QString& message);

private:
    void configure(int sampleRate);

    QAudioSink* sink_ = nullptr;
    QIODevice* stream_ = nullptr;
    int sampleRate_ = 0;
    int volume_ = 80;
    bool muted_ = false;
    bool notified_ = false;
};
