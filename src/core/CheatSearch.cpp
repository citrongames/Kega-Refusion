#include "CheatSearch.h"
#include "i18n/Localization.h"

#include <QtGlobal>
#include <algorithm>
#include <utility>

namespace Cheats {
namespace {
constexpr qsizetype MaxSnapshotBytes = 1024 * 1024;

bool compare(Search::Comparison comparison, quint32 current, quint32 previous,
             quint32 exact) {
    switch (comparison) {
    case Search::Exact: return current == exact;
    case Search::Changed: return current != previous;
    case Search::Unchanged: return current == previous;
    case Search::Increased: return current > previous;
    case Search::Decreased: return current < previous;
    case Search::Unknown: return true;
    }
    return false;
}
} // namespace

quint32 Search::readValue(const QByteArray& memory, quint32 offset,
                          bool bigEndian, int width) {
    const auto* bytes = reinterpret_cast<const uchar*>(memory.constData()) + offset;
    quint32 result = 0;
    if (bigEndian) {
        for (int i = 0; i < width; ++i)
            result = (result << 8) | bytes[i];
    } else {
        for (int i = width - 1; i >= 0; --i)
            result = (result << 8) | bytes[i];
    }
    return result;
}

bool Search::filter(const QByteArray& memory, quint32 base, bool bigEndian, int width,
                    Comparison comparison, quint32 value, bool restart, QString& error) {
    error.clear();
    if (width != 1 && width != 2 && width != 4) {
        error = Localization::text(QStringLiteral("Размер значения должен быть 1, 2 или 4 байта."));
        return false;
    }
    if (comparison < Exact || comparison > Unknown) {
        error = Localization::text(QStringLiteral("Неизвестное условие поиска."));
        return false;
    }
    if (memory.size() < width || memory.size() > MaxSnapshotBytes ||
        quint64(base) + quint64(memory.size()) > (quint64(1) << 32)) {
        error = Localization::text(QStringLiteral("Недопустимый диапазон памяти для поиска."));
        return false;
    }
    if (restart) {
        if (comparison != Exact && comparison != Unknown) {
            error = Localization::text(QStringLiteral("Для сравнения нужен предыдущий снимок памяти."));
            return false;
        }
    } else if (!started_) {
        error = Localization::text(QStringLiteral("Сначала начните поиск."));
        return false;
    } else if (base != base_ || bigEndian != bigEndian_ || width != width_ ||
               memory.size() != latest_.size()) {
        error = Localization::text(QStringLiteral("Память или формат значения изменились. Начните новый поиск."));
        return false;
    }

    if (comparison == Exact && width < 4 && value >= (quint32(1) << (width*8))) {
        error = Localization::text(QStringLiteral("Значение не помещается в выбранный размер."));
        return false;
    }
    // Copy before committing, so callers may reuse or mutate their own buffer.
    QByteArray next(memory.constData(), memory.size());
    QVector<quint32> kept;
    const quint32 exact = width == 1 ? value & 0xffu :
                          width == 2 ? value & 0xffffu : value;
    if (restart) {
        const quint32 stride = bigEndian && width > 1 ? 2u : 1u;
        const quint32 first = stride == 2 ? (base & 1u) : 0u;
        const quint32 last = quint32(next.size() - width);
        if (first <= last) {
            kept.reserve(int((last - first) / stride + 1));
            for (quint32 offset = first; offset <= last; offset += stride) {
                const quint32 current = readValue(next, offset, bigEndian, width);
                if (compare(comparison, current, current, exact)) kept.append(offset);
            }
        }
        QByteArray().swap(prior_);
    } else {
        kept.reserve(candidates_.size());
        for (quint32 offset : std::as_const(candidates_)) {
            const quint32 current = readValue(next, offset, bigEndian, width);
            const quint32 previous = readValue(latest_, offset, bigEndian, width);
            if (compare(comparison, current, previous, exact)) kept.append(offset);
        }
        prior_ = std::move(latest_);
    }
    latest_ = std::move(next);
    candidates_ = std::move(kept);
    base_ = base;
    bigEndian_ = bigEndian;
    width_ = width;
    started_ = true;
    return true;
}

QList<Search::Match> Search::matches(int limit) const {
    QList<Match> result;
    if (!started_ || limit <= 0) return result;
    const qsizetype size = std::min<qsizetype>(candidates_.size(), limit);
    result.reserve(size);
    for (qsizetype index = 0; index < size; ++index) {
        const quint32 offset = candidates_[index];
        const quint32 current = readValue(latest_, offset, bigEndian_, width_);
        const quint32 previous = prior_.isEmpty() ? current :
                                 readValue(prior_, offset, bigEndian_, width_);
        result.append({base_ + offset, previous, current});
    }
    return result;
}

void Search::clear() {
    QByteArray().swap(latest_);
    QByteArray().swap(prior_);
    QVector<quint32>().swap(candidates_);
    base_ = 0;
    bigEndian_ = false;
    width_ = 0;
    started_ = false;
}

} // namespace Cheats
