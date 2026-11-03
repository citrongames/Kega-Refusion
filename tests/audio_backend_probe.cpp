// Standalone deployment probe. Link against Qt6::Core and Qt6::Multimedia.
// Run once with the deployed media plugins and once without them, preserving
// the same audio device settings. --force-fallback selects a deliberately
// missing media plugin so an installed Qt backend cannot mask missing files.
// Exit 0: all checks passed, 1: audio failed, 2: usage error, 77: no audio device.
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
using State = decltype(std::declval<QAudioSink>().state());
using Error = decltype(std::declval<QAudioSink>().error());
std::atomic<bool> sawFallbackWarning{false};

void logMessage(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (message.startsWith(QStringLiteral("No QtMultimedia backends found.")))
        sawFallbackWarning = true;
    const QByteArray line = qFormatLogMessage(type, context, message).toLocal8Bit();
    std::fprintf(stderr, "%s\n", line.constData());
}

void processFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

QAudioFormat coreFormat(int sampleRate) {
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

QJsonObject describe(const QAudioDevice& device) {
    QJsonObject result;
    result["idHex"] = QString::fromLatin1(device.id().toHex());
    result["description"] = device.description();
    result["isDefault"] = device.isDefault();
    const QAudioFormat preferred = device.preferredFormat();
    QJsonObject preferredDescription;
    preferredDescription["sampleRate"] = preferred.sampleRate();
    preferredDescription["channelCount"] = preferred.channelCount();
    preferredDescription["sampleFormat"] = int(preferred.sampleFormat());
    result["preferredFormat"] = preferredDescription;
    result["stereoInt16_44100"] = device.isFormatSupported(coreFormat(44100));
    result["stereoInt16_48000"] = device.isFormatSupported(coreFormat(48000));
    return result;
}

struct StreamResult {
    qint64 acceptedBytes = 0;
    qint64 processedUsecs = 0;
    bool active = false;
    bool error = false;
};

StreamResult streamSilence(QAudioSink& sink, QIODevice* output, int milliseconds) {
    StreamResult result;
    if (!output) return result;
    const QByteArray silence(4096, '\0');
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        const auto free = sink.bytesFree();
        const qint64 count = std::min<qint64>(free - free % 4, silence.size());
        if (count > 0) {
            const qint64 written = output->write(silence.constData(), count);
            if (written < 0) {
                result.error = true;
                break;
            }
            result.acceptedBytes += written;
        }
        result.active |= sink.state() == State::ActiveState;
        const auto error = sink.error();
        result.error |= error != Error::NoError && error != Error::UnderrunError;
        processFor(5);
    }
    result.processedUsecs = sink.processedUSecs();
    return result;
}

QJsonObject probe(const QAudioDevice& device, int sampleRate) {
    QJsonObject result;
    result["sampleRate"] = sampleRate;
    const QAudioFormat format = coreFormat(sampleRate);
    const bool supported = device.isFormatSupported(format);
    result["supported"] = supported;
    if (!supported) {
        result["passed"] = false;
        return result;
    }

    QAudioSink sink(device, format);
    sink.setBufferSize(qint64(sampleRate) * 4 * 40 / 1000);
    sink.setVolume(0.0); // Only silence is supplied; the probe never emits a tone.
    QIODevice* output = sink.start();
    const bool opened = output && sink.error() == Error::NoError;
    result["opened"] = opened;
    result["bufferBytes"] = double(sink.bufferSize());
    const StreamResult initial = streamSilence(sink, output, 250);
    result["acceptedBytes"] = double(initial.acceptedBytes);
    result["processedUsecs"] = double(initial.processedUsecs);
    result["active"] = initial.active;
    result["streamError"] = initial.error;

    sink.setVolume(0.37);
    const bool volume = std::abs(sink.volume() - 0.37) < 0.0001;
    result["volume"] = volume;
    sink.suspend();
    processFor(15);
    const bool suspended = sink.state() == State::SuspendedState;
    result["suspended"] = suspended;
    sink.resume();
    const StreamResult resumed = streamSilence(sink, output, 100);
    const bool resumedOkay = resumed.active && resumed.acceptedBytes > 0 &&
                             resumed.processedUsecs > initial.processedUsecs && !resumed.error;
    result["resumed"] = resumedOkay;

    sink.reset();
    const bool reset = sink.state() == State::StoppedState && sink.error() == Error::NoError;
    result["reset"] = reset;
    output = sink.start();
    const bool reopened = output && sink.error() == Error::NoError;
    const StreamResult restarted = streamSilence(sink, output, 100);
    const bool restartedOkay = reopened && restarted.active && restarted.acceptedBytes > 0 &&
                               restarted.processedUsecs > 0 && !restarted.error;
    result["reopened"] = restartedOkay;
    sink.reset();
    result["passed"] = opened && initial.active && initial.acceptedBytes > 0 &&
                       initial.processedUsecs > 0 && !initial.error && volume &&
                       suspended && resumedOkay && reset && restartedOkay;
    return result;
}
QJsonArray loadedMediaPlugins() {
    QJsonArray plugins;
#ifdef Q_OS_WIN
    for (const wchar_t* name : {L"ffmpegmediaplugin.dll", L"windowsmediaplugin.dll"})
        if (GetModuleHandleW(name))
            plugins.append(QString::fromWCharArray(name));
#elif defined(Q_OS_LINUX)
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (maps.open(QIODevice::ReadOnly)) {
        QStringList names;
        for (const QByteArray& line : maps.readAll().split('\n')) {
            const qsizetype start = line.indexOf('/');
            if (start >= 0 && line.contains("/multimedia/") && line.contains(".so")) {
                const QString name = QString::fromUtf8(line.mid(start));
                if (!names.contains(name)) names.append(name);
            }
        }
        std::sort(names.begin(), names.end());
        for (const QString& name : names) plugins.append(name);
    }
#endif
    return plugins;
}
} // namespace

int main(int argc, char** argv) {
    bool forceFallback = false;
    QByteArray requestedId;
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        if (argument == "--fallback" || argument == "--force-fallback") forceFallback = true;
        else if (argument == "--device-id-hex" && i + 1 < argc) {
            const QByteArray hex(argv[++i]);
            requestedId = QByteArray::fromHex(hex);
            if (requestedId.toHex() != hex.toLower() || requestedId.isEmpty()) {
                std::fprintf(stderr, "Invalid --device-id-hex value\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "Usage: audio_backend_probe [--fallback] [--device-id-hex HEX]\n");
            return 2;
        }
    }
    if (forceFallback)
        qputenv("QT_MEDIA_BACKEND", "kega_native_audio_probe_unavailable");
    qInstallMessageHandler(logMessage);
    QCoreApplication app(argc, argv);
    // QCoreApplication normally adds the Qt installation's plugin directory.
    // Restrict lookup before first multimedia use to test this deployment only.
    QCoreApplication::setLibraryPaths({QCoreApplication::applicationDirPath() +
                                      QStringLiteral("/plugins")});

    auto devices = QMediaDevices::audioOutputs();
    std::sort(devices.begin(), devices.end(), [](const QAudioDevice& left,
                                               const QAudioDevice& right) {
        return left.id() < right.id();
    });
    QJsonArray inventory;
    for (const auto& device : devices) inventory.append(describe(device));
    QAudioDevice selected = QMediaDevices::defaultAudioOutput();
    if (!requestedId.isEmpty()) {
        selected = {};
        for (const auto& device : devices)
            if (device.id() == requestedId) selected = device;
    }

    QJsonObject report;
    report["qtVersion"] = QString::fromLatin1(qVersion());
    report["mediaBackendEnvironment"] = QString::fromLatin1(qgetenv("QT_MEDIA_BACKEND"));
    report["fallbackObserved"] = sawFallbackWarning.load();
    report["devices"] = inventory;
    report["defaultDeviceIdHex"] =
        QString::fromLatin1(QMediaDevices::defaultAudioOutput().id().toHex());
    report["selectedDeviceIdHex"] = QString::fromLatin1(selected.id().toHex());

    int exitCode = 0;
    if (selected.isNull()) {
        report["status"] = requestedId.isEmpty() ? QStringLiteral("no-output-device")
                                                : QStringLiteral("requested-device-missing");
        exitCode = requestedId.isEmpty() ? 77 : 1;
    } else {
        QJsonArray checks;
        bool passed = !forceFallback || sawFallbackWarning.load();
        for (const int rate : {44100, 48000}) {
            const QJsonObject check = probe(selected, rate);
            passed &= check["passed"].toBool();
            checks.append(check);
        }
        report["checks"] = checks;
        report["status"] = passed ? QStringLiteral("passed") : QStringLiteral("failed");
        exitCode = passed ? 0 : 1;
    }
    const QJsonArray loadedPlugins = loadedMediaPlugins();
    report["loadedMediaPlugins"] = loadedPlugins;
    if (forceFallback && !loadedPlugins.isEmpty()) {
        report["status"] = QStringLiteral("failed-media-plugin-loaded");
        exitCode = 1;
    }
    const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Indented);
    std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
    return exitCode;
}

