#pragma once
#include <QByteArray>
#include <QtEndian>
#include <limits>
#include <utility>

namespace ScriptStateEnvelope {
inline constexpr qsizetype MaximumScriptBytes = 256 * 1024;
inline constexpr qsizetype HeaderBytes = 16;
inline constexpr qsizetype MaximumOverhead = HeaderBytes + MaximumScriptBytes;

inline QByteArray wrap(const QByteArray& native, const QByteArray& script) {
    if (script.isEmpty()) return native;
    if (native.isEmpty() || script.size() > MaximumScriptBytes ||
        quint64(native.size()) > std::numeric_limits<quint32>::max() ||
        native.size() > std::numeric_limits<qsizetype>::max() - HeaderBytes - script.size())
        return {};
    QByteArray packed = QByteArrayLiteral("KRSCPT01");
    packed.reserve(HeaderBytes + native.size() + script.size());
    const auto nativeSize = qToBigEndian(quint32(native.size()));
    const auto scriptSize = qToBigEndian(quint32(script.size()));
    packed.append(reinterpret_cast<const char*>(&nativeSize), sizeof(nativeSize));
    packed.append(reinterpret_cast<const char*>(&scriptSize), sizeof(scriptSize));
    packed.append(native);
    packed.append(script);
    return packed;
}

inline bool split(const QByteArray& packed, QByteArray& native, QByteArray& script) {
    if (packed.isEmpty()) return false;
    if (!packed.startsWith(QByteArrayLiteral("KRSCPT01"))) {
        native = packed;
        script.clear();
        return true;
    }
    if (packed.size() < HeaderBytes) return false;
    const auto nativeSize = qFromBigEndian<quint32>(packed.constData() + 8);
    const auto scriptSize = qFromBigEndian<quint32>(packed.constData() + 12);
    if (!nativeSize || !scriptSize || scriptSize > MaximumScriptBytes ||
        HeaderBytes + quint64(nativeSize) + scriptSize != quint64(packed.size()))
        return false;
    auto nativeBytes = packed.mid(HeaderBytes, nativeSize);
    auto scriptBytes = packed.mid(HeaderBytes + qsizetype(nativeSize), scriptSize);
    native = std::move(nativeBytes);
    script = std::move(scriptBytes);
    return true;
}
}
