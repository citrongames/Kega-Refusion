#include "core/EmulationSession.h"
#include "i18n/Localization.h"
#include "input/InputManager.h"
#include "ui/MainWindow.h"
#include "ui/AudioOutput.h"
#include <QComboBox>
#include <QDialog>
#include <QTimer>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <cstdio>
#include <functional>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void waitFor(const std::function<bool()>& predicate, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    require(predicate(), message);
}
QByteArray originalRom() {
    // Original 68000 spin loop with a valid MD header, no game data.
    QByteArray rom(32768, '\0');
    qToBigEndian<quint32>(0xffff00, rom.data());
    qToBigEndian<quint32>(0x200, rom.data() + 4);
    rom.replace(0x100, 16, QByteArray("SEGA MEGA DRIVE ", 16));
    rom.replace(0x1f0, 3, "USA");
    rom[0x200] = char(0x60);
    rom[0x201] = char(0xfe);
    return rom;
}
bool recordedConnection(const QByteArray& snapshot, int player) {
    const auto controllers = QJsonDocument::fromJson(snapshot).object()
                                 .value(QStringLiteral("controllers")).toArray();
    require(controllers.size() == int(MaxControllerPlayers), "invalid controller snapshot");
    return controllers.at(player).toObject().value(QStringLiteral("connected")).toBool();
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    try {
        require(argc == 3 || (argc == 4 && app.arguments().last() == "--audio-profiles"), "project root and core library arguments are required");
        QTemporaryDir data;
        require(data.isValid(), "temporary directory unavailable");
        {
            QSettings settings(data.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
            settings.setValue(QStringLiteral("ui/language"), QStringLiteral("en"));
            settings.setValue(QStringLiteral("achievements/enabled"), false);
            if (app.arguments().contains(QStringLiteral("--audio-profiles")))
                settings.setValue(QStringLiteral("emulation/pauseOnFocusLoss"), false);
            settings.sync();
        }
        Localization::initialize(QString::fromLocal8Bit(argv[1]), data.path());
        const QString romPath = data.filePath(QStringLiteral("original.md"));
        {
            const QByteArray rom = originalRom();
            QFile file(romPath);
            require(file.open(QIODevice::WriteOnly) && file.write(rom) == rom.size(),
                    "cannot write original ROM fixture");
        }
        EmulationSession session(QString::fromLocal8Bit(argv[2]), data.path());
        MainWindow window(session, data.path(), true);
        auto* input = window.findChild<InputManager*>();
        require(input, "input manager unavailable");
        auto* videoMenu = window.findChild<QMenu*>(QStringLiteral("videoMenu"));
        auto* movieMenu = window.findChild<QMenu*>(QStringLiteral("movieMenu"));
        auto* videoStart = window.findChild<QAction*>(QStringLiteral("videoRecord"));
        auto* videoExport = window.findChild<QAction*>(QStringLiteral("videoExport"));
        auto* videoStop = window.findChild<QAction*>(QStringLiteral("videoStop"));
        require(videoMenu && movieMenu && videoMenu != movieMenu && videoStart && videoExport && videoStop,
                "separate input recording and video controls unavailable");
        require(!videoMenu->isEnabled(), "video controls available without a game");
        int loaded = 0, errors = 0, notices = 0, mode = 0, slotUpdates = 0;
        QByteArray snapshot;
        QObject::connect(&session, &EmulationSession::gameLoaded, &app,
                         [&](const QString&, double, const QString&) { ++loaded; });
        QObject::connect(&session, &EmulationSession::errorOccurred, &app,
                         [&](const QString& error) {
            ++errors;
            std::fprintf(stderr, "%s\n", qPrintable(error));
        });
        const QString protectedNotice = Localization::text(QStringLiteral(
            "Сначала завершите запись: параметры консоли зафиксированы в записи."));
        QObject::connect(&session, &EmulationSession::actionNotice, &app,
                         [&](const QString& text) { if (text == protectedNotice) ++notices; });
        QObject::connect(&session, &EmulationSession::movieStatusChanged, &app,
                         [&](int next, const QString&, quint64, quint64, double) { mode = next; });
        QObject::connect(&session, &EmulationSession::movieHardwareChanged, &app,
                         [&](const QByteArray& next) { if (!next.isEmpty()) snapshot = next; });
        QObject::connect(&session, &EmulationSession::stateSlotsChanged, &app,
                         [&](int selected, unsigned) { if (selected == 9) ++slotUpdates; });
        const auto barrier = [&] {
            const int target = slotUpdates + 1;
            session.requestSlotStatus(9);
            waitFor([&] { return slotUpdates >= target; }, "session command barrier timed out");
        };
        window.openRom(romPath);
        waitFor([&] { return loaded == 1 || errors; }, "fixture did not load");
        require(!errors && videoStart->isEnabled() && videoExport->isEnabled() &&
                !videoStop->isEnabled(), "video controls did not follow game loading");

        if (app.arguments().contains(QStringLiteral("--audio-profiles"))) {
            auto* audio = window.findChild<AudioOutput*>();
            auto* settingsAction = window.findChild<QAction*>(QStringLiteral("emulatorSettingsAction"));
            require(audio && settingsAction && audio->latencyProfile() == AudioQueue::LatencyProfile::Normal,
                    "normal default/settings unavailable");
            int frames = 0;
            qint64 activeTarget = 0;
            QObject::connect(&session, &EmulationSession::videoFrame, &app,
                             [&](const QImage&, double) { ++frames; });
            QObject::connect(audio, &AudioOutput::clockFeedback, &app,
                             [&](qint64, qint64 target, bool available) { activeTarget = available ? target : 0; });
            for (const int value : {1, 2, 0}) {
                bool changed = false, restored = false;
                const int previous = int(audio->latencyProfile());
                QTimer::singleShot(0, &window, [&] {
                    auto* dialog = window.findChild<QDialog*>(QStringLiteral("emulatorSettingsDialog"));
                    auto* combo = dialog ? dialog->findChild<QComboBox*>(QStringLiteral("audioLatencyProfile")) : nullptr;
                    if (combo) {
                        restored = combo->currentData().toInt() == previous;
                        combo->setCurrentIndex(combo->findData(value));
                        changed = int(audio->latencyProfile()) == value;
                    }
                    if (dialog) dialog->reject(); // Immediate preference survives closing.
                });
                settingsAction->trigger();
                require(changed && restored, "settings did not apply/restore latency immediately");
                QSettings stored(data.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
                require(stored.value(QStringLiteral("audio/latencyProfile")).toInt() == value,
                        "latency preference not saved");
                const int before = frames;
                session.setBackgroundPaused(false);
                waitFor([&] { return frames > before + 8 &&
                    activeTarget == AudioQueue::timing(AudioQueue::normalizeProfile(value)).target; },
                    "real core/audio did not resume with selected latency");
                require(loaded == 1 && !errors, "profile reopened/failed the game");
            }
            session.shutdown();
            std::puts("Audio profiles: real core/native audio continue; settings apply immediately and restore on reopen");
            return 0;
        }

        // The core stores every slot, while InputManager gates disconnected
        // adapter slots. Echoing this snapshot used to issue a forbidden edit.
        const unsigned inactive = MaxControllerPlayers - 1;
        require(!input->isConnected(inactive), "fixture needs an unreachable local adapter slot");
        session.setControllerConnected(inactive, true);
        session.startMovieRecording(data.filePath(QStringLiteral("first.krm")));
        waitFor([&] { return mode == 1 || errors; }, "input recording did not start");
        barrier();
        require(!errors && recordedConnection(snapshot, int(inactive)),
                "raw connected slot was not preserved in the authoritative snapshot");
        require(notices == 0, "starting recording echoed a forbidden controller edit");
        require(videoStart->isEnabled() && !videoExport->isEnabled(),
                "live video and offline export have incorrect input-recording availability");

        // Genuine user changes still go to the core and retain its protection.
        session.setControllerConnected(0, !input->isConnected(0));
        waitFor([&] { return notices == 1 || errors; }, "genuine protected edit did not warn");
        session.stopMovie();
        waitFor([&] { return mode == 0 || errors; }, "input recording did not finish");
        barrier();
        snapshot.clear();
        session.startMovieRecording(data.filePath(QStringLiteral("second.krm")));
        waitFor([&] { return mode == 1 || errors; }, "second input recording did not start");
        barrier();
        require(!errors && !recordedConnection(snapshot, int(inactive)),
                "empty controller snapshot did not reapply the local profile");
        require(notices == 1, "authoritative synchronization generated another warning");
        session.stopMovie();
        waitFor([&] { return mode == 0 || errors; }, "second recording did not finish");
        require(!errors, "recording synchronization caused a session error");
        session.shutdown();
        std::puts("Video menu availability and authoritative controller synchronization passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
