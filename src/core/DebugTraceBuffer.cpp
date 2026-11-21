#include "DebugTraceBuffer.h"
#include "i18n/Localization.h"

#include <QFile>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <exception>
#include <thread>
#include <utility>

namespace {
constexpr size_t recordsPerBatch = 512;
constexpr size_t queueBatches = 64;
constexpr size_t mebibyte = 1024 * 1024;

QString writerBusy() {
    return Localization::text(QStringLiteral("Дождитесь окончания записи файла трассы."));
}

const char* eventName(uint32_t kind) {
    switch (kind) {
    case KR_TRACE_EXEC: return "EXEC";
    case KR_TRACE_VDP_WRITE: return "VDP_WRITE";
    case KR_TRACE_DMA_START: return "DMA_START";
    case KR_TRACE_DMA_END: return "DMA_END";
    case KR_TRACE_IRQ_ACCEPT: return "IRQ_ACCEPT";
    case KR_TRACE_DISC_COMMAND: return "DISC_COMMAND";
    case KR_TRACE_LINK_EDGE: return "LINK_EDGE";
    case KR_TRACE_BANK_CHANGE: return "BANK_CHANGE";
    case KR_TRACE_DISCONTINUITY: return "DISCONTINUITY";
    default: return "UNKNOWN";
    }
}

template<typename Number>
void number(QByteArray& text, Number value, int base = 10) {
    char digits[24];
    const auto end = std::to_chars(digits, digits + sizeof(digits), value, base).ptr;
    text.append(digits, end - digits);
    text.append('\t');
}

void formatRecord(QByteArray& text, const DebugTraceRecord& record) {
    const auto& event = record.native;
    number(text, record.sequence);
    number(text, record.frame);
    number(text, record.epoch);
    number(text, record.instance);
    number(text, event.mclk);
    number(text, event.local_ticks);
    number(text, event.cpu);
    text.append(eventName(event.kind)).append('\t');
    number(text, event.device);
    number(text, event.address, 16);
    number(text, event.value, 16);
    number(text, event.aux, 16);
    number(text, event.flags, 16);
    number(text, unsigned(event.width));
    constexpr char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < std::min<size_t>(event.valid_bytes, sizeof(event.bytes)); ++i) {
        text.append(hex[event.bytes[i] >> 4]);
        text.append(hex[event.bytes[i] & 15]);
    }
    text.append('\n');
}
} // namespace

struct DebugTraceBuffer::Writer {
    enum class End { Producing, Stopped, Overflow };
    enum class Result { Running, Complete, Overflow, FileError, Failed };
    struct Batch {
        std::array<DebugTraceRecord, recordsPerBatch> records;
        size_t size = 0;
    };
    static_assert(sizeof(Batch) * queueBatches <= 8 * mebibyte);
    static_assert(std::atomic<size_t>::is_always_lock_free &&
                  std::atomic<End>::is_always_lock_free &&
                  std::atomic<Result>::is_always_lock_free);

    explicit Writer(bool live) : live(live), queue(live ? queueBatches : 0) {}

    ~Writer() {
        if (thread.joinable()) thread.join();
    }

    bool push(const DebugTraceRecord& record) noexcept {
        if (batchSize == 0 && head - consumed.load(std::memory_order_acquire) == queueBatches)
            return false;
        auto& batch = queue[head % queueBatches];
        batch.records[batchSize++] = record;
        if (batchSize == recordsPerBatch) publish();
        return true;
    }

    void finish(End final) noexcept {
        publish();
        end.store(final, std::memory_order_release);
    }

    void publish() noexcept {
        if (!batchSize) return;
        queue[head % queueBatches].size = batchSize;
        batchSize = 0;
        published.store(++head, std::memory_order_release);
    }

    void start(QString path, QString game, std::vector<DebugTraceRecord> recent = {}) {
        thread = std::thread([this, path = std::move(path), game = std::move(game),
                              recent = std::move(recent)]() mutable {
            Result final = Result::Failed;
            try {
                final = writeTrace(path, game, recent);
            } catch (...) {
                // An exception must never terminate the emulation process.
            }
            std::vector<DebugTraceRecord>().swap(recent);
            result.store(final, std::memory_order_release);
        });
    }

    Result writeTrace(const QString& path, QString game, const std::vector<DebugTraceRecord>& recent) {
        QFile file(path);
        const auto fileError = [&] {
            detail = file.errorString();
            return Result::FileError;
        };
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fileError();

        for (const QChar ch : {QChar('\t'), QChar('\r'), QChar('\n')})
            game.replace(ch, QLatin1Char(' '));
        const QByteArray header = "# Kega Refusion trace v1\n# game\t" + game.toUtf8() +
            "\nsequence\tframe\tepoch\tinstance\tmclk\tlocal_ticks\tcpu\tkind\tdevice\t"
            "address_hex\tvalue_hex\taux_hex\tflags_hex\twidth\tinstruction_bytes_hex\n";
        if (file.write(header) != header.size()) return fileError();
        QByteArray text;
        text.reserve(256 * recordsPerBatch);
        const auto writeBatch = [&](const DebugTraceRecord* source, size_t count) {
            text.resize(0);
            for (size_t i = 0; i < count; ++i) formatRecord(text, source[i]);
            return file.write(text) == text.size();
        };

        End final = End::Stopped;
        if (live) {
            size_t tail = 0;
            for (;;) {
                // Acquire the final marker before reading the published index.
                // The producer publishes its last partial batch before ending.
                final = end.load(std::memory_order_acquire);
                if (tail != published.load(std::memory_order_acquire)) {
                    const auto& batch = queue[tail % queueBatches];
                    if (!writeBatch(batch.records.data(), batch.size)) return fileError();
                    consumed.store(++tail, std::memory_order_release);
                } else if (final != End::Producing) {
                    break;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        } else {
            for (size_t i = 0; i < recent.size(); i += recordsPerBatch) {
                if (!writeBatch(recent.data() + i, std::min(recordsPerBatch, recent.size() - i)))
                    return fileError();
            }
        }

        const QByteArray footer = final == End::Overflow
            ? "# incomplete: writer queue full\n"
            : "# complete\n";
        if (file.write(footer) != footer.size() || !file.flush()) return fileError();
        file.close();
        if (file.error() != QFileDevice::NoError) return fileError();
        return final == End::Overflow ? Result::Overflow : Result::Complete;
    }

    const bool live;
    std::vector<Batch> queue;
    std::atomic<size_t> published{0}, consumed{0};
    std::atomic<End> end{End::Producing};
    std::atomic<Result> result{Result::Running};
    size_t head = 0, batchSize = 0; // Producer only.
    QString detail;                // Immutable once result is published.
    std::thread thread;
};

DebugTraceBuffer::DebugTraceBuffer() = default;

DebugTraceBuffer::~DebugTraceBuffer() {
    stop();
}

bool DebugTraceBuffer::start(const DebugTraceConfig& config, QString game, QString& error) {
    error.clear();
    if (config.memoryMiB < 1 || config.memoryMiB > 64) {
        error = Localization::text(QStringLiteral("Выберите размер истории от 1 до 64 МиБ."));
        return false;
    }
    if (writing()) {
        error = writerBusy();
        return false;
    }
    stop();
    writer_.reset();
    const size_t count = config.memoryMiB * mebibyte / sizeof(DebugTraceRecord);
    try {
        if (ring_.size() != count) {
            std::vector<DebugTraceRecord>().swap(ring_);
            total_ = nextSlot_ = 0;
            ring_.resize(count);
        }
        if (!config.filePath.isEmpty()) {
            auto writer = std::make_unique<Writer>(true);
            writer->start(config.filePath, game);
            writer_ = std::move(writer);
        }
    } catch (const std::exception&) {
        error = Localization::text(QStringLiteral("Не удалось начать трассировку."));
        return false;
    }
    total_ = nextSlot_ = 0;
    filePath_ = config.filePath;
    game_ = std::move(game);
    capturing_ = true;
    return true;
}

void DebugTraceBuffer::append(const DebugTraceRecord& record) noexcept {
    if (!active()) return;
    ring_[nextSlot_] = record;
    if (++nextSlot_ == ring_.size()) nextSlot_ = 0;
    ++total_;
    if (writer_ && writer_->live && !writer_->push(record)) {
        capturing_ = false;
        writer_->finish(Writer::End::Overflow);
    }
}

void DebugTraceBuffer::stop() noexcept {
    if (!capturing_) return;
    capturing_ = false;
    if (writer_ && writer_->live)
        writer_->finish(Writer::End::Stopped);
}

bool DebugTraceBuffer::clear(QString& error) {
    error.clear();
    nextSlot_ = 0;
    total_ = 0;
    return true;
}

bool DebugTraceBuffer::exportRecent(QString path, QString& error) {
    error.clear();
    if (writing()) {
        error = writerBusy();
        return false;
    }
    const size_t count = retained();
    if (path.isEmpty() || !count) {
        error = Localization::text(QStringLiteral("Для экспорта выберите файл и запишите события трассы."));
        return false;
    }
    if (writer_ && writer_->live) stop(); // A failed live job must stay stopped.
    writer_.reset();
    try {
        std::vector<DebugTraceRecord> recent;
        recent.reserve(count);
        const size_t first = firstSlot(count);
        const size_t tail = std::min(count, ring_.size() - first);
        recent.insert(recent.end(), ring_.begin() + first, ring_.begin() + first + tail);
        recent.insert(recent.end(), ring_.begin(), ring_.begin() + count - tail);
        auto writer = std::make_unique<Writer>(false);
        writer->start(path, game_, std::move(recent));
        writer_ = std::move(writer);
    } catch (const std::exception&) {
        error = Localization::text(QStringLiteral("Не удалось начать экспорт трассы."));
        return false;
    }
    filePath_ = std::move(path);
    return true;
}

QVector<DebugTraceRecord> DebugTraceBuffer::latest(unsigned limit) const {
    const size_t count = std::min<size_t>({limit, 256, retained()});
    QVector<DebugTraceRecord> recent;
    recent.reserve(qsizetype(count));
    size_t slot = firstSlot(count);
    for (size_t i = 0; i < count; ++i) {
        recent.append(ring_[slot]);
        if (++slot == ring_.size()) slot = 0;
    }
    return recent;
}

size_t DebugTraceBuffer::retained() const noexcept {
    return size_t(std::min<quint64>(total_, ring_.size()));
}

size_t DebugTraceBuffer::firstSlot(size_t count) const noexcept {
    return count <= nextSlot_ ? nextSlot_ - count : ring_.size() - (count - nextSlot_);
}

bool DebugTraceBuffer::active() const noexcept {
    return capturing_ && (!writer_ || !writer_->live ||
        writer_->result.load(std::memory_order_acquire) == Writer::Result::Running);
}

bool DebugTraceBuffer::writing() const noexcept {
    return writer_ && writer_->result.load(std::memory_order_acquire) == Writer::Result::Running;
}

QString DebugTraceBuffer::status() const {
    if (!writer_) return {};
    switch (writer_->result.load(std::memory_order_acquire)) {
    case Writer::Result::FileError:
        return Localization::text(QStringLiteral("Не удалось записать трассу в файл «%1» (%2)."))
            .arg(filePath_, writer_->detail);
    case Writer::Result::Failed:
        return Localization::text(QStringLiteral("Запись файла трассы прервана."));
    case Writer::Result::Overflow:
        break;
    default:
        if (writer_->end.load(std::memory_order_acquire) != Writer::End::Overflow) return {};
        break;
    }
    return Localization::text(QStringLiteral("Очередь записи переполнена. Файл трассы содержит только начальные записи."));
}
