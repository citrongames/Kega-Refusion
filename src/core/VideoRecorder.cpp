#include "VideoRecorder.h"
#include "i18n/Localization.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QtEndian>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

namespace {
constexpr size_t QueueFrames = 8;
constexpr quint64 MaximumSegments = 256;
constexpr qint64 MaximumPixels = 1024 * 1024;
constexpr qsizetype MaximumAudioBytes = 256 * 1024;

QString mediaError(const QString& operation, int status) {
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(status, detail, sizeof(detail));
    return Localization::text(operation) + QStringLiteral(": ") + QString::fromUtf8(detail);
}

struct InputFrame {
    QImage image;
    QByteArray pcm;
    int sampleRate = 0;
    AVRational rate{};
};

QString partPath(const QString& original, quint64 part) {
    if (part == 1) return original;
    const QFileInfo info(original);
    return info.dir().filePath(info.completeBaseName() +
        QStringLiteral(".part%1.mov").arg(part, 3, 10, QLatin1Char('0')));
}

// libavformat owns the container layout, packet timestamps, fragment metadata,
// and AAC priming edits. A QFile AVIO device also supports Unicode Windows paths.
class Segment final {
public:
    ~Segment() { release(); }

    bool matches(const InputFrame& input) const {
        return size_ == input.image.size() && sampleRate_ == input.sampleRate &&
               av_cmp_q(rate_, input.rate) == 0;
    }

    bool open(const QString& path, const InputFrame& first, QString& error) {
        path_ = path;
        size_ = first.image.size();
        sampleRate_ = first.sampleRate;
        rate_ = first.rate;
        file_.setFileName(path);
        if (!file_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            error = Localization::text(QStringLiteral("Не удалось создать видеофайл: ")) + file_.errorString();
            return false;
        }
        created_ = true;
        int status = avformat_alloc_output_context2(&format_, nullptr, "mov", nullptr);
        if (status < 0 || !format_) {
            error = mediaError(QStringLiteral("Не удалось создать средство записи MOV"), status < 0 ? status : AVERROR(ENOMEM));
            return false;
        }
        auto* buffer = static_cast<unsigned char*>(av_malloc(64 * 1024));
        if (!buffer) {
            error = Localization::text(QStringLiteral("Не удалось выделить буфер записи видео."));
            return false;
        }
        io_ = avio_alloc_context(buffer, 64 * 1024, 1, &file_, nullptr, write, seek);
        if (!io_) {
            av_free(buffer);
            error = Localization::text(QStringLiteral("Не удалось создать устройство записи видео."));
            return false;
        }
        format_->pb = io_;
        format_->flags |= AVFMT_FLAG_CUSTOM_IO;
        format_->max_interleave_delta = 500000;
        if (!openVideo(error) || !openAudio(error)) return false;
        packet_ = av_packet_alloc();
        if (!packet_) {
            error = Localization::text(QStringLiteral("Не удалось выделить пакет записи видео."));
            return false;
        }
        AVDictionary* options = nullptr;
        // Bound muxer packet/index memory and leave completed fragments readable
        // after an interrupted capture. Delaying moov preserves AAC priming edits.
        av_dict_set(&options, "movflags", "delay_moov+default_base_moof+skip_trailer", 0);
        av_dict_set(&options, "frag_duration", "1000000", 0);
        av_dict_set(&options, "frag_size", "4194304", 0);
        // Console oscillator rates need a precise codec clock, while QuickTime
        // expects a modest MOV track timescale. Rescale absolute timestamps below.
        av_dict_set(&options, "video_track_timescale", "60000", 0);
        av_dict_set(&options, "use_editlist", "1", 0);
        status = avformat_write_header(format_, &options);
        av_dict_free(&options);
        if (status < 0) {
            error = mediaError(QStringLiteral("Не удалось записать заголовок MOV"), status);
            return false;
        }
        headerWritten_ = true;
        return true;
    }

    bool append(const InputFrame& input, QString& error) {
        const QImage rgb = input.image.convertToFormat(QImage::Format_RGB888);
        if (rgb.isNull()) {
            error = Localization::text(QStringLiteral("Не удалось преобразовать исходный кадр игры."));
            return false;
        }
        int status = av_frame_make_writable(videoFrame_);
        if (status < 0) {
            error = mediaError(QStringLiteral("Не удалось подготовить видеокадр"), status);
            return false;
        }
        for (int y = 0; y < rgb.height(); ++y)
            std::memcpy(videoFrame_->data[0] + y * videoFrame_->linesize[0],
                        rgb.constScanLine(y), size_t(rgb.width()) * 3);
        videoFrame_->pts = videoFrames_;
        if (!encode(video_, videoStream_, videoFrame_, error)) return false;
        ++videoFrames_;
        audioPending_.append(input.pcm);
        const qsizetype bytes = audio_->frame_size * 4;
        qsizetype offset = 0;
        while (audioPending_.size() - offset >= bytes) {
            if (!audioFrame(audioPending_.constData() + offset, audio_->frame_size, error))
                return false;
            offset += bytes;
        }
        if (offset) audioPending_.remove(0, offset);
        return true;
    }

    bool close(QString& error) {
        if (!headerWritten_ || closed_) return true;
        closed_ = true;
        bool ok = true;
        if (!audioPending_.isEmpty()) {
            ok = audioFrame(audioPending_.constData(), int(audioPending_.size() / 4), error);
            audioPending_.clear();
        }
        if (ok) ok = encode(video_, videoStream_, nullptr, error);
        if (ok) ok = encode(audio_, audioStream_, nullptr, error);
        const int trailer = av_write_trailer(format_);
        if (ok && trailer < 0) {
            error = mediaError(QStringLiteral("Не удалось завершить файл MOV"), trailer);
            ok = false;
        }
        avio_flush(io_);
        if (ok && io_->error < 0) {
            error = mediaError(QStringLiteral("Не удалось записать видеофайл"), io_->error);
            ok = false;
        }
        if (!file_.flush() && ok) {
            error = Localization::text(QStringLiteral("Не удалось сохранить видеофайл: ")) + file_.errorString();
            ok = false;
        }
        return ok;
    }

private:
    bool openVideo(QString& error) {
        const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
        if (!encoder) {
            error = Localization::text(QStringLiteral("Кодировщик видео PNG без потерь недоступен."));
            return false;
        }
        video_ = avcodec_alloc_context3(encoder);
        videoStream_ = avformat_new_stream(format_, nullptr);
        if (!video_ || !videoStream_) {
            error = Localization::text(QStringLiteral("Не удалось создать кодировщик видео."));
            return false;
        }
        video_->width = size_.width();
        video_->height = size_.height();
        video_->pix_fmt = AV_PIX_FMT_RGB24;
        video_->time_base = av_inv_q(rate_);
        video_->framerate = rate_;
        video_->thread_count = 1;
        video_->compression_level = 6;
        video_->sample_aspect_ratio = AVRational{1, 1};
        if (format_->oformat->flags & AVFMT_GLOBALHEADER)
            video_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        const int status = avcodec_open2(video_, encoder, nullptr);
        if (status < 0) {
            error = mediaError(QStringLiteral("Не удалось открыть кодировщик PNG без потерь"), status);
            return false;
        }
        if (!copyParameters(video_, videoStream_, error)) return false;
        videoStream_->avg_frame_rate = rate_;
        videoStream_->sample_aspect_ratio = AVRational{1, 1};
        videoFrame_ = av_frame_alloc();
        if (!videoFrame_) {
            error = Localization::text(QStringLiteral("Не удалось выделить кадр записи видео."));
            return false;
        }
        videoFrame_->format = video_->pix_fmt;
        videoFrame_->width = video_->width;
        videoFrame_->height = video_->height;
        const int buffer = av_frame_get_buffer(videoFrame_, 32);
        if (buffer < 0) {
            error = mediaError(QStringLiteral("Не удалось выделить исходные пиксели видео"), buffer);
            return false;
        }
        return true;
    }

    bool openAudio(QString& error) {
        const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!encoder) {
            error = Localization::text(QStringLiteral("Кодировщик звука AAC недоступен."));
            return false;
        }
        audio_ = avcodec_alloc_context3(encoder);
        audioStream_ = avformat_new_stream(format_, nullptr);
        if (!audio_ || !audioStream_) {
            error = Localization::text(QStringLiteral("Не удалось создать кодировщик звука."));
            return false;
        }
        audio_->sample_rate = sampleRate_;
        audio_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audio_->bit_rate = 256000;
        audio_->time_base = AVRational{1, sampleRate_};
        av_channel_layout_default(&audio_->ch_layout, 2);
        if (format_->oformat->flags & AVFMT_GLOBALHEADER)
            audio_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        const int status = avcodec_open2(audio_, encoder, nullptr);
        if (status < 0) {
            error = mediaError(QStringLiteral("Не удалось открыть кодировщик AAC"), status);
            return false;
        }
        if (audio_->frame_size <= 0 || audio_->frame_size > 8192) {
            error = Localization::text(QStringLiteral("Кодировщик AAC вернул неверный размер звукового кадра."));
            return false;
        }
        if (!copyParameters(audio_, audioStream_, error)) return false;
        audioFrame_ = av_frame_alloc();
        if (!audioFrame_) {
            error = Localization::text(QStringLiteral("Не удалось выделить звуковой кадр."));
            return false;
        }
        audioFrame_->format = audio_->sample_fmt;
        audioFrame_->sample_rate = sampleRate_;
        audioFrame_->nb_samples = audio_->frame_size;
        const int layout = av_channel_layout_copy(&audioFrame_->ch_layout, &audio_->ch_layout);
        const int buffer = layout < 0 ? layout : av_frame_get_buffer(audioFrame_, 0);
        if (buffer < 0) {
            error = mediaError(QStringLiteral("Не удалось выделить звуковые данные"), buffer);
            return false;
        }
        return true;
    }

    static bool copyParameters(AVCodecContext* codec, AVStream* stream, QString& error) {
        stream->time_base = codec->time_base;
        const int status = avcodec_parameters_from_context(stream->codecpar, codec);
        if (status < 0) {
            error = mediaError(QStringLiteral("Не удалось скопировать параметры записи"), status);
            return false;
        }
        return true;
    }

    bool audioFrame(const char* pcm, int samples, QString& error) {
        // Buffer capacity stays one full AAC frame even after a short last frame.
        audioFrame_->nb_samples = audio_->frame_size;
        const int writable = av_frame_make_writable(audioFrame_);
        if (writable < 0) {
            error = mediaError(QStringLiteral("Не удалось подготовить звуковой кадр"), writable);
            return false;
        }
        auto* left = reinterpret_cast<float*>(audioFrame_->data[0]);
        auto* right = reinterpret_cast<float*>(audioFrame_->data[1]);
        for (int i = 0; i < samples; ++i) {
            left[i] = qFromLittleEndian<qint16>(pcm + i * 4) / 32768.0f;
            right[i] = qFromLittleEndian<qint16>(pcm + i * 4 + 2) / 32768.0f;
        }
        audioFrame_->nb_samples = samples;
        audioFrame_->pts = audioSamples_;
        audioSamples_ += samples;
        return encode(audio_, audioStream_, audioFrame_, error);
    }

    bool encode(AVCodecContext* codec, AVStream* stream, AVFrame* frame, QString& error) {
        int status = avcodec_send_frame(codec, frame);
        if (status < 0) {
            error = mediaError(QStringLiteral("Не удалось передать кадр на запись"), status);
            return false;
        }
        for (;;) {
            status = avcodec_receive_packet(codec, packet_);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return true;
            if (status < 0) {
                error = mediaError(QStringLiteral("Не удалось сжать пакет записи"), status);
                return false;
            }
            if (codec == video_ && packet_->duration <= 0) packet_->duration = 1;
            const qint64 originalPts = packet_->pts;
            const qint64 originalDuration = packet_->duration;
            av_packet_rescale_ts(packet_, codec->time_base, stream->time_base);
            if (codec == video_ && originalPts != AV_NOPTS_VALUE) {
                // Derive each duration from the next absolute frame boundary,
                // so fractional ticks alternate instead of accumulating drift.
                packet_->duration = av_rescale_q(originalPts + originalDuration,
                    codec->time_base, stream->time_base) - packet_->pts;
            }
            packet_->stream_index = stream->index;
            status = av_interleaved_write_frame(format_, packet_);
            av_packet_unref(packet_);
            if (status < 0) {
                error = mediaError(QStringLiteral("Не удалось записать пакет видео"), status);
                return false;
            }
        }
    }

#if LIBAVFORMAT_VERSION_MAJOR >= 61
    static int write(void* opaque, const uint8_t* data, int bytes) {
#else
    static int write(void* opaque, uint8_t* data, int bytes) {
#endif
        auto* file = static_cast<QFile*>(opaque);
        qint64 total = 0;
        while (total < bytes) {
            const qint64 result = file->write(reinterpret_cast<const char*>(data) + total, bytes - total);
            if (result <= 0) return AVERROR(EIO);
            total += result;
        }
        return bytes;
    }

    static int64_t seek(void* opaque, int64_t offset, int whence) {
        auto* file = static_cast<QFile*>(opaque);
        if (whence & AVSEEK_SIZE) return file->size();
        whence &= ~AVSEEK_FORCE;
        qint64 target = offset;
        if (whence == SEEK_CUR) target += file->pos();
        else if (whence == SEEK_END) target += file->size();
        else if (whence != SEEK_SET) return AVERROR(EINVAL);
        return target >= 0 && file->seek(target) ? file->pos() : AVERROR(EIO);
    }

    void release() {
        av_packet_free(&packet_);
        av_frame_free(&videoFrame_);
        av_frame_free(&audioFrame_);
        avcodec_free_context(&video_);
        avcodec_free_context(&audio_);
        avformat_free_context(format_);
        format_ = nullptr;
        if (io_) {
            av_freep(&io_->buffer);
            avio_context_free(&io_);
        }
        file_.close();
        // Only remove an empty/incomplete file that this writer created itself.
        if (created_ && !videoFrames_) QFile::remove(path_);
    }

    QString path_;
    QFile file_;
    QSize size_;
    int sampleRate_ = 0;
    AVRational rate_{};
    AVFormatContext* format_ = nullptr;
    AVIOContext* io_ = nullptr;
    AVCodecContext* video_ = nullptr;
    AVCodecContext* audio_ = nullptr;
    AVStream* videoStream_ = nullptr;
    AVStream* audioStream_ = nullptr;
    AVFrame* videoFrame_ = nullptr;
    AVFrame* audioFrame_ = nullptr;
    AVPacket* packet_ = nullptr;
    QByteArray audioPending_;
    qint64 videoFrames_ = 0, audioSamples_ = 0;
    bool created_ = false, headerWritten_ = false, closed_ = false;
};
} // namespace

struct VideoRecorder::Impl final : QThread {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<InputFrame> queue;
    Result output;
    bool running = false, accepting = false, finished = false;

    bool shuttingDown = false;

    void run() override {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return running || shuttingDown; });
                if (!running) return;
            }
            try {
                work();
            } catch (const std::exception&) {
                std::lock_guard<std::mutex> lock(mutex);
                if (output.error.isEmpty())
                    output.error = Localization::text(QStringLiteral("Не удалось завершить запись видео."));
                queue.clear();
                accepting = running = false;
                finished = true;
            }
        }
    }

    void work() {
        std::unique_ptr<Segment> segment;
        quint64 part = 0;
        QString failure;
        for (;;) {
            InputFrame input;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return !queue.empty() || !accepting; });
                if (queue.empty()) break;
                input = std::move(queue.front());
                queue.pop_front();
            }
            if (!segment || !segment->matches(input)) {
                if (segment && !segment->close(failure)) break;
                segment.reset();
                QString original;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    original = output.path;
                }
                if (part >= MaximumSegments) {
                    failure = Localization::text(QStringLiteral("Слишком много изменений видеоформата. Начните новую видеозапись."));
                    break;
                }
                const QString path = partPath(original, ++part);
                segment = std::make_unique<Segment>();
                if (!segment->open(path, input, failure)) break;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    output.paths.append(path);
                }
            }
            if (!segment->append(input, failure)) break;
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++output.frames;
            }
        }
        if (segment) {
            QString finishingError;
            if (!segment->close(finishingError) && failure.isEmpty()) failure = finishingError;
            segment.reset();
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (output.error.isEmpty()) output.error = failure;
        if (!output.frames && output.error.isEmpty())
            output.error = Localization::text(QStringLiteral("Видеозапись не содержит кадров."));
        queue.clear();
        accepting = false;
        running = false;
        finished = true;
    }
};

VideoRecorder::VideoRecorder() : impl_(std::make_unique<Impl>()) {}
VideoRecorder::~VideoRecorder() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->shuttingDown = true;
        impl_->accepting = false;
        impl_->wake.notify_one();
    }
    impl_->wait();
}

bool VideoRecorder::start(const QString& path, QString& error) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->running) {
            error = Localization::text(QStringLiteral("Предыдущая видеозапись ещё завершается."));
            return false;
        }
    }
    if (path.isEmpty() || QFileInfo::exists(path)) {
        error = path.isEmpty() ? Localization::text(QStringLiteral("Выберите файл для записи видео."))
                              : Localization::text(QStringLiteral("Файл для записи видео уже существует."));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->output = Result{};
        impl_->output.path = QFileInfo(path).absoluteFilePath();
        impl_->queue.clear();
        impl_->running = impl_->accepting = true;
        impl_->finished = false;
        // Keep the Qt thread asleep between captures. Stopping a live MOV
        // closes all media resources without triggering native thread teardown.
        if (!impl_->isRunning()) {
            impl_->start();
            if (!impl_->isRunning()) {
                impl_->running = impl_->accepting = false;
                error = Localization::text(QStringLiteral("Не удалось запустить запись видео."));
                return false;
            }
        }
        impl_->wake.notify_one();
    }
    error.clear();
    return true;
}

bool VideoRecorder::capacityAvailable() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->accepting && impl_->queue.size() < QueueFrames;
}

bool VideoRecorder::submit(const QImage& frame, const QByteArray& pcm, int sampleRate, double fps) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->accepting || impl_->queue.size() >= QueueFrames) return false;
    if (frame.isNull() || qint64(frame.width()) * frame.height() > MaximumPixels ||
        frame.width() > 2048 || frame.height() > 2048 ||
        frame.sizeInBytes() > MaximumPixels * 4 ||
        pcm.size() > MaximumAudioBytes || pcm.size() % 4 ||
        sampleRate < 8000 || sampleRate > 192000 ||
        !std::isfinite(fps) || fps < 1 || fps > 1000) {
        impl_->output.error = Localization::text(QStringLiteral("Неверный исходный кадр видео или стереозвука."));
        impl_->accepting = false;
        impl_->wake.notify_one();
        return false;
    }
    impl_->queue.push_back({frame, pcm, sampleRate, av_d2q(fps, 1000000)});
    impl_->wake.notify_one();
    return true;
}

void VideoRecorder::finish() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->accepting = false;
    impl_->wake.notify_one();
}

void VideoRecorder::abort(const QString& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->output.error.isEmpty()) impl_->output.error = error;
    impl_->accepting = false;
    impl_->wake.notify_one();
}

bool VideoRecorder::busy() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->running;
}
bool VideoRecorder::accepting() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->accepting;
}
bool VideoRecorder::completed() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->finished;
}
VideoRecorder::Result VideoRecorder::result() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->output;
}
