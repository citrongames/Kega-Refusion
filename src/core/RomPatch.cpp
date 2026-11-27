#include "RomPatch.h"
#include "CartridgeLimits.h"
#include "i18n/Localization.h"
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <limits>
#include <zlib.h>

namespace {

QByteArray fail(QString& error, const char* message) {
    error = Localization::text(QString::fromLatin1(message));
    return {};
}

qsizetype readBigEndian24(const char* bytes) {
    return (qsizetype(uchar(bytes[0])) << 16) | (qsizetype(uchar(bytes[1])) << 8) |
           uchar(bytes[2]);
}

void resizeWithZeros(QByteArray& bytes, qsizetype size) {
    const auto oldSize = bytes.size();
    bytes.resize(size);
    if (size > oldSize)
        std::memset(bytes.data() + oldSize, 0, size_t(size - oldSize));
}

QByteArray applyIps(const QByteArray& source, const QByteArray& patch, QString& error) {
    // Repeated overlapping RLE records can expand the amount of work far
    // beyond either file size. Bound their cumulative writes.
    constexpr qsizetype MaxRecordBytes = 4 * CartridgeLimits::MaxRomSize;
    qsizetype recordBytes = 0;
    QByteArray target = source;
    const char* cursor = patch.constData() + 5;
    const char* end = patch.constData() + patch.size();
    while (end - cursor >= 3) {
        const auto offset = readBigEndian24(cursor);
        cursor += 3;
        if (offset == 0x454f46) { // EOF
            if (cursor == end)
                return target;
            if (end - cursor != 3)
                break;
            const auto size = readBigEndian24(cursor);
            if (size == 0 || size > CartridgeLimits::MaxRomSize)
                return fail(error, "The patched ROM has an invalid size.");
            resizeWithZeros(target, size);
            return target;
        }
        if (end - cursor < 2)
            break;
        qsizetype length = qFromBigEndian<quint16>(cursor);
        cursor += 2;
        const bool rle = length == 0;
        if (rle) {
            if (end - cursor < 3)
                break;
            length = qFromBigEndian<quint16>(cursor);
            cursor += 2;
            if (length == 0)
                break;
        }
        const qsizetype payloadSize = rle ? 1 : length;
        if (end - cursor < payloadSize)
            break;
        const auto recordEnd = offset + length;
        if (recordEnd > CartridgeLimits::MaxRomSize)
            return fail(error, "The patched ROM has an invalid size.");
        if (length > MaxRecordBytes - recordBytes)
            return fail(error, "The IPS patch writes too much data.");
        recordBytes += length;
        if (recordEnd > target.size())
            resizeWithZeros(target, recordEnd);
        char* destination = target.data() + offset;
        if (rle)
            std::memset(destination, uchar(*cursor), size_t(length));
        else
            std::memcpy(destination, cursor, size_t(length));
        cursor += payloadSize;
    }
    return fail(error, "The IPS patch is malformed or incomplete.");
}

quint32 checksum(const char* bytes, qsizetype length) {
    static_assert(CartridgeLimits::MaxRomSize <= std::numeric_limits<uInt>::max());
    static_assert(RomPatch::MaxPatchSize <= std::numeric_limits<uInt>::max());
    return quint32(crc32(0, reinterpret_cast<const Bytef*>(bytes), uInt(length)));
}

// BPS uses the variable-length encoding specified by byuu. A clear high bit
// indicates continuation and adds one unit of the next place value.
// https://github.com/Alcaro/Flips/blob/master/bps_spec.md
bool readBpsNumber(const char*& cursor, const char* end, quint64& value) {
    constexpr auto MaxValue = std::numeric_limits<quint64>::max();
    value = 0;
    quint64 place = 1;
    while (cursor != end) {
        const auto byte = uchar(*cursor++);
        const quint64 digit = byte & 0x7f;
        if (digit > (MaxValue - value) / place)
            return false;
        value += digit * place;
        if (byte & 0x80)
            return true;
        if (place > (MaxValue - value) / 128)
            return false;
        place *= 128;
        value += place;
    }
    return false;
}

bool seekRelative(const char*& cursor, const char* end, qsizetype& offset, qsizetype limit) {
    quint64 delta;
    if (!readBpsNumber(cursor, end, delta))
        return false;
    const auto distance = delta >> 1;
    if (delta & 1) {
        if (distance > quint64(offset))
            return false;
        offset -= qsizetype(distance);
    } else {
        if (distance > quint64(limit - offset))
            return false;
        offset += qsizetype(distance);
    }
    return true;
}

void copyTarget(char* target, qsizetype offset, qsizetype written, qsizetype length) {
    // If the copy reaches its own output, the bytes already written by this
    // command repeat. Copy the available prefix, then double it as needed.
    qsizetype copied = std::min(length, written - offset);
    char* destination = target + written;
    std::memcpy(destination, target + offset, size_t(copied));
    while (copied < length) {
        const qsizetype count = std::min(copied, length - copied);
        std::memcpy(destination + copied, destination, size_t(count));
        copied += count;
    }
}

QByteArray applyBps(const QByteArray& source, const QByteArray& patch, QString& error) {
    constexpr auto malformed = "The BPS patch is malformed or incomplete.";
    constexpr qsizetype FooterSize = 12;
    if (patch.size() < 4 + 3 + FooterSize)
        return fail(error, malformed);
    const char* cursor = patch.constData() + 4;
    const char* footer = patch.constData() + patch.size() - FooterSize;
    const auto sourceCrc = qFromLittleEndian<quint32>(footer);
    const auto targetCrc = qFromLittleEndian<quint32>(footer + 4);
    const auto patchCrc = qFromLittleEndian<quint32>(footer + 8);
    if (checksum(patch.constData(), patch.size() - 4) != patchCrc)
        return fail(error, "The BPS patch checksum is invalid.");

    quint64 sourceSize, targetSize, metadataSize;
    if (!readBpsNumber(cursor, footer, sourceSize) ||
        !readBpsNumber(cursor, footer, targetSize) ||
        !readBpsNumber(cursor, footer, metadataSize) ||
        metadataSize > quint64(footer - cursor))
        return fail(error, malformed);
    if (targetSize == 0 || targetSize > quint64(CartridgeLimits::MaxRomSize))
        return fail(error, "The patched ROM has an invalid size.");
    if (sourceSize != quint64(source.size()) || checksum(source.constData(), source.size()) != sourceCrc)
        return fail(error, "The BPS patch does not match the source ROM.");
    cursor += qsizetype(metadataSize);

    QByteArray target(qsizetype(targetSize), Qt::Uninitialized);
    char* output = target.data();
    qsizetype written = 0;
    qsizetype sourceOffset = 0;
    qsizetype targetOffset = 0;
    while (cursor != footer) {
        quint64 command;
        if (!readBpsNumber(cursor, footer, command))
            return fail(error, malformed);
        const auto count = (command >> 2) + 1;
        if (count > quint64(target.size() - written))
            return fail(error, malformed);
        const auto length = qsizetype(count);
        switch (command & 3) {
        case 0: // SourceRead
            if (written > source.size() || length > source.size() - written)
                return fail(error, malformed);
            std::memcpy(output + written, source.constData() + written, size_t(length));
            break;
        case 1: // TargetRead
            if (length > footer - cursor)
                return fail(error, malformed);
            std::memcpy(output + written, cursor, size_t(length));
            cursor += length;
            break;
        case 2: // SourceCopy
            if (!seekRelative(cursor, footer, sourceOffset, source.size()) ||
                length > source.size() - sourceOffset)
                return fail(error, malformed);
            std::memcpy(output + written, source.constData() + sourceOffset, size_t(length));
            sourceOffset += length;
            break;
        case 3: // TargetCopy
            if (!seekRelative(cursor, footer, targetOffset, written) || targetOffset == written)
                return fail(error, malformed);
            copyTarget(output, targetOffset, written, length);
            targetOffset += length;
            break;
        }
        written += length;
    }
    if (written != target.size())
        return fail(error, malformed);
    if (checksum(output, target.size()) != targetCrc)
        return fail(error, "The patched ROM checksum is invalid.");
    return target;
}

} // namespace

QByteArray RomPatch::apply(const QByteArray& source, const QByteArray& patch, QString& error) {
    error.clear();
    if (source.isEmpty() || source.size() > CartridgeLimits::MaxRomSize)
        return fail(error, "The source ROM has an invalid size.");
    if (patch.size() > MaxPatchSize)
        return fail(error, "The ROM patch is too large.");
    if (patch.startsWith("PATCH"))
        return applyIps(source, patch, error);
    if (patch.startsWith("BPS1"))
        return applyBps(source, patch, error);
    return fail(error, "Unrecognized ROM patch format.");
}
