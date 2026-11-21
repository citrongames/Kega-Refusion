#include "DebugCoverageMap.h"
#include "i18n/Localization.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <exception>
#include <thread>
#include <utility>

namespace {
constexpr size_t maxPackedBytes = 64 * 1024 * 1024;

size_t packedSize(quint32 bytes) {
    return size_t((quint64(bytes) + 3) / 4);
}
} // namespace

struct DebugCoverageMap::Export {
    enum class Result { Writing, Complete, FileError, Failed };

    ~Export() {
        if (thread.joinable()) thread.join();
    }

    void start(QString game, QByteArray romHash, QVector<DebugCoverageSource> sources,
               quint64 unknownCode, quint64 unknownData, quint32 instance, std::vector<quint8> packed) {
        thread = std::thread([this, game = std::move(game), romHash = std::move(romHash),
                              sources = std::move(sources), unknownCode, unknownData, instance,
                              packed = std::move(packed)]() mutable {
            Result final = Result::Failed;
            try {
                final = write(game, romHash, sources, unknownCode, unknownData, instance, packed);
            } catch (...) {
                // Formatting or writing the optional export cannot stop the emulator.
            }
            std::vector<quint8>().swap(packed);
            result.store(final, std::memory_order_release);
        });
    }

    Result write(const QString& game, const QByteArray& romHash, const QVector<DebugCoverageSource>& sources,
                 quint64 unknownCode, quint64 unknownData, quint32 instance, const std::vector<quint8>& packed) {
        QJsonArray regions;
        size_t offset = 0;
        for (const auto& source : sources) {
            const auto& d = source.native;
            const size_t length = packedSize(d.size);
            regions.append(QJsonObject{
                {QStringLiteral("id"), qint64(d.id)},
                {QStringLiteral("name"), QString::fromUtf8(d.name)},
                {QStringLiteral("size"), qint64(d.size)},
                {QStringLiteral("flags"), qint64(d.flags)},
                {QStringLiteral("execCpuMask"), qint64(d.exec_cpu_mask)},
                {QStringLiteral("readCpuMask"), qint64(d.read_cpu_mask)},
                {QStringLiteral("inspectDomain"), qint64(d.inspect_domain)},
                {QStringLiteral("inspectOffset"), qint64(d.inspect_offset)},
                {QStringLiteral("code"), qint64(source.code)},
                {QStringLiteral("data"), qint64(source.data)},
                {QStringLiteral("both"), qint64(source.both)},
                {QStringLiteral("packedOffset"), qint64(offset)},
                {QStringLiteral("packedBytes"), qint64(length)},
            });
            offset += length;
        }
        const auto header = QJsonDocument(QJsonObject{
            {QStringLiteral("version"), qint64(KR_COV_VERSION)},
            {QStringLiteral("instance"), qint64(instance)},
            {QStringLiteral("game"), game},
            {QStringLiteral("romSha256"), QString::fromLatin1(romHash.toHex())},
            {QStringLiteral("unknownCode"), qint64(unknownCode)},
            {QStringLiteral("unknownData"), qint64(unknownData)},
            {QStringLiteral("sources"), regions},
        }).toJson(QJsonDocument::Compact);

        QFile file(path);
        const auto fileError = [&] {
            detail = file.errorString();
            return Result::FileError;
        };
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fileError();
        char prefix[] = "KRCDL1\r\n\0\0\0\0";
        qToLittleEndian(quint32(header.size()), prefix + 8);
        if (file.write(prefix, 12) != 12 || file.write(header) != header.size() ||
            file.write(reinterpret_cast<const char*>(packed.data()), qint64(packed.size())) != qint64(packed.size()) ||
            !file.flush()) return fileError();
        file.close();
        return file.error() == QFileDevice::NoError ? Result::Complete : fileError();
    }

    QString path, detail;
    std::atomic<Result> result{Result::Writing};
    std::thread thread;
};

DebugCoverageMap::DebugCoverageMap() = default;
DebugCoverageMap::~DebugCoverageMap() = default;

bool DebugCoverageMap::start(const QVector<kr_cov_source>& sources, QString game, QByteArray romHash, QString& error, quint32 instance) {
    error.clear();
    std::array<Region, KR_COV_MAX_SOURCES> regions{};
    if (sources.isEmpty() || sources.size() >= KR_COV_MAX_SOURCES) {
        error = Localization::text(QStringLiteral("Не найдены подходящие области физической памяти."));
        return false;
    }
    for (auto source : sources) {
        if (!source.id || source.id >= regions.size() || !source.size ||
            regions[source.id].info.native.id) {
            error = Localization::text(QStringLiteral("Ядро вернуло неверное описание области покрытия."));
            return false;
        }
        source.name[sizeof(source.name) - 1] = 0;
        regions[source.id].info.native = source;
    }
    size_t total = 0;
    for (auto& region : regions) {
        const auto bytes = packedSize(region.info.native.size);
        if (bytes > maxPackedBytes - total) {
            error = Localization::text(QStringLiteral("Карта покрытия превышает 64 МиБ."));
            return false;
        }
        region.packedOffset = total;
        total += bytes;
    }

    if (total == packed_.size()) {
        std::fill(packed_.begin(), packed_.end(), 0);
    } else {
        // Only one map allocation is retained. Explicit exports own a separate
        // snapshot and are not affected by starting another capture.
        std::vector<quint8>().swap(packed_);
        try {
            packed_.resize(total);
        } catch (const std::exception&) {
            regions_ = {};
            game_.clear();
            romHash_.clear();
            unknownCode_ = unknownData_ = 0;
            active_ = changed_ = false;
            ++revision_;
            error = Localization::text(QStringLiteral("Не удалось выделить память для карты покрытия."));
            return false;
        }
    }

    regions_ = regions;
    game_ = std::move(game);
    romHash_ = std::move(romHash);
    instance_ = instance;
    unknownCode_ = unknownData_ = 0;
    active_ = true;
    changed_ = false;
    ++revision_;
    return true;
}

void DebugCoverageMap::mark(const kr_cov_event& event) noexcept {
    if (!active_ || !event.size || (event.kind != KR_COV_EXEC && event.kind != KR_COV_READ)) return;
    const bool exec = event.kind == KR_COV_EXEC;
    auto& region = regions_[event.source < regions_.size() ? event.source : 0];
    const auto& source = region.info.native;
    const auto cpus = exec ? source.exec_cpu_mask : source.read_cpu_mask;
    if (!(cpus & event.cpu) || event.offset >= source.size || event.size > source.size - event.offset) {
        (exec ? unknownCode_ : unknownData_) += event.size;
        changed_ = true;
        ++revision_;
        return;
    }

    auto* flags = packed_.data() + region.packedOffset;
    quint32 added = 0, both = 0;
    const quint32 end = event.offset + event.size;
    for (quint32 i = event.offset; i < end; ++i) {
        const auto shift = 2 * (i & 3);
        const auto bit = quint8(event.kind << shift);
        auto& packedByte = flags[i >> 2];
        if (packedByte & bit) continue;
        both += ((packedByte >> shift) & 3) != 0;
        packedByte |= bit;
        ++added;
    }
    if (!added) return;
    (exec ? region.info.code : region.info.data) += added;
    region.info.both += both;
    changed_ = true;
    ++revision_;
}

void DebugCoverageMap::clear() {
    if (!changed_) return;
    std::fill(packed_.begin(), packed_.end(), 0);
    for (auto& region : regions_)
        region.info.code = region.info.data = region.info.both = 0;
    unknownCode_ = unknownData_ = 0;
    changed_ = false;
    ++revision_;
}

QVector<DebugCoverageSource> DebugCoverageMap::sources() const {
    QVector<DebugCoverageSource> result;
    for (const auto& region : regions_)
        if (region.info.native.id) result.append(region.info);
    return result;
}

QByteArray DebugCoverageMap::page(quint32 source, quint32 offset, unsigned length) const {
    if (!source || source >= regions_.size()) return {};
    const auto& region = regions_[source];
    if (offset >= region.info.native.size) return {};
    length = std::min({length, 256u, region.info.native.size - offset});
    QByteArray result(qsizetype(length), Qt::Uninitialized);
    const auto* flags = packed_.data() + region.packedOffset;
    for (unsigned i = 0; i < length; ++i) {
        const auto at = offset + i;
        result[i] = char((flags[at >> 2] >> (2 * (at & 3))) & 3);
    }
    return result;
}

bool DebugCoverageMap::exportFile(QString path, QString& error) {
    error.clear();
    if (writing()) {
        error = Localization::text(QStringLiteral("Дождитесь окончания экспорта покрытия."));
        return false;
    }
    if (path.isEmpty() || packed_.empty()) {
        error = Localization::text(QStringLiteral("Для экспорта выберите файл и создайте карту покрытия."));
        return false;
    }
    export_.reset();
    try {
        auto job = std::make_unique<Export>();
        job->path = std::move(path);
        job->start(game_, romHash_, sources(), unknownCode_, unknownData_, instance_, packed_);
        export_ = std::move(job);
    } catch (const std::exception&) {
        error = Localization::text(QStringLiteral("Не удалось начать экспорт покрытия."));
        return false;
    }
    return true;
}

bool DebugCoverageMap::writing() const noexcept {
    return export_ && export_->result.load(std::memory_order_acquire) == Export::Result::Writing;
}

QString DebugCoverageMap::status() const {
    if (!export_) return {};
    switch (export_->result.load(std::memory_order_acquire)) {
    case Export::Result::Writing: return {};
    case Export::Result::Complete:
        return Localization::text(QStringLiteral("Покрытие сохранено в «%1».")).arg(export_->path);
    case Export::Result::FileError:
        return Localization::text(QStringLiteral("Не удалось сохранить покрытие в «%1» (%2)."))
            .arg(export_->path, export_->detail);
    case Export::Result::Failed:
        return Localization::text(QStringLiteral("Экспорт покрытия прерван."));
    }
    return {};
}
