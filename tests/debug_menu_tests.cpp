#include "core/EmulationSession.h"
#include "i18n/Localization.h"
#include "ui/DebugDialogs.h"
#include "ui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class T> T* child(QObject& parent, const char* name) {
    auto* found = parent.findChild<T*>(QString::fromLatin1(name));
    require(found, name);
    return found;
}

void press(QWidget& target, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
           bool repeat = false) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers, QString(), repeat);
    QApplication::sendEvent(&target, &event);
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    try {
        require(argc > 1, "project root argument is required");
        QTemporaryDir data;
        require(data.isValid(), "temporary data directory unavailable");
        const QString settingsPath = QDir(data.path()).filePath(QStringLiteral("settings.ini"));
        {
            QSettings settings(settingsPath, QSettings::IniFormat);
            settings.setValue(QStringLiteral("ui/language"), QStringLiteral("en"));
            settings.setValue(QStringLiteral("view/blarggFilter"), 1); // Legacy monochrome.
            settings.sync();
        }
        Localization::initialize(QString::fromLocal8Bit(argv[1]), data.path());
        require(Localization::currentLanguage() == QLatin1String("en"),
                "English catalog did not load");

        {
            EmulationSession session(QString(), data.path());
            MainWindow window(session, data.path(), true);
            window.show();
            QApplication::processEvents();
            QMenu* shader = child<QMenu>(window, "shaderMenu");
            require(!window.findChild<QMenu*>(QStringLiteral("blarggMenu")),
                    "Blargg still has a separate submenu");
            QAction* off = child<QAction>(window, "shaderOff");
            QAction* composite = child<QAction>(window, "blargg2");
            QAction* svideo = child<QAction>(window, "blargg3");
            QAction* rgb = child<QAction>(window, "blargg4");
            QAction* gdapt = child<QAction>(window, "shaderGdapt");
            require(off->isChecked() && !window.findChild<QAction*>(QStringLiteral("blargg1")),
                    "legacy monochrome was not normalized to Off");
            const auto shaderActions = shader->actions();
            require(shaderActions.indexOf(composite) < shaderActions.indexOf(svideo) &&
                    shaderActions.indexOf(svideo) < shaderActions.indexOf(rgb) &&
                    shaderActions.indexOf(rgb) < shaderActions.indexOf(gdapt),
                    "Blargg modes are not listed before GDAPT");
            require(composite->text() == QLatin1String("Composite (Blargg)") &&
                    svideo->text() == QLatin1String("S-Video (Blargg)") &&
                    rgb->text() == QLatin1String("RGB (Blargg)"),
                    "Blargg modes are not identified explicitly");
            composite->trigger();
            rgb->trigger();
            require(rgb->isChecked() && !composite->isChecked(),
                    "Blargg modes are not exclusive");
            off->trigger();
            require(off->isChecked() && !rgb->isChecked(),
                    "Off did not clear the Blargg selection");

            QMenu* debug = child<QMenu>(window, "debugMenu");
            require(!debug->menuAction()->isVisible(), "debug menu is visible without a game");
            const quint32 mdVideo = (quint32(1) << 0) | (quint32(1) << 3);
            const quint64 mdAudio = (quint64(1) << 0) | (quint64(1) << 34);
            session.debugControlsChanged(mdVideo, mdAudio);
            require(debug->menuAction()->isVisible() && debug->title() == QLatin1String("&Debug"),
                    "capabilities did not reveal the localized menu");
            QAction* graphicsAction = child<QAction>(window, "debugGraphicsAction");
            QAction* audioAction = child<QAction>(window, "debugAudioAction");
            graphicsAction->trigger();
            audioAction->trigger();
            QApplication::processEvents();
            auto* graphics = child<DebugGraphicsDialog>(window, "debugGraphicsDialog");
            auto* audio = child<DebugAudioDialog>(window, "debugAudioDialog");
            require(graphics->isVisible() && audio->isVisible() &&
                    graphics->windowModality() == Qt::NonModal &&
                    audio->windowModality() == Qt::NonModal,
                    "debug controls did not open as modeless windows");
            auto* planeA = child<QCheckBox>(*graphics, "debugVideo0");
            auto* sprites = child<QCheckBox>(*graphics, "debugVideo3");
            auto* psgTone = child<QCheckBox>(*audio, "debugAudio0");
            auto* pwmLeft = child<QCheckBox>(*audio, "debugAudio34");
            auto* gain0 = child<QSlider>(*audio, "debugAudioGain0");
            auto* percent0 = child<QSpinBox>(*audio, "debugAudioPercent0");
            require(planeA->isVisible() && sprites->isVisible() &&
                    psgTone->isVisible() && pwmLeft->isVisible() &&
                    planeA->isChecked() && sprites->isChecked() &&
                    psgTone->isChecked() && pwmLeft->isChecked() &&
                    gain0->value() == 100 && percent0->value() == 100,
                    "available controls did not start enabled at 100%");
            require(!child<QCheckBox>(*graphics, "debugVideo2")->isVisible() &&
                    !child<QCheckBox>(*audio, "debugAudio4")->isVisible(),
                    "unavailable channels appeared in the windows");
            require(child<QPushButton>(*graphics, "debugBackdropColor")->icon()
                        .pixmap(16, 16).toImage().pixelColor(8, 8) == QColor(255, 0, 255),
                    "backdrop did not default to magenta");
            planeA->click();
            psgTone->click();
            require(!planeA->isChecked() && !psgTone->isChecked(),
                    "layer and channel checkboxes did not toggle");
            child<QPushButton>(*graphics, "debugRestoreVideo")->click();
            child<QPushButton>(*audio, "debugRestoreAudio")->click();
            require(planeA->isChecked() && psgTone->isChecked(),
                    "restore controls did not enable channels");
            gain0->setValue(45);
            require(percent0->value() == 45, "gain slider did not update percentage");
            percent0->setValue(25);
            require(gain0->value() == 25, "percentage did not update gain slider");
            child<QPushButton>(*audio, "debugResetGains")->click();
            require(gain0->value() == 100 && percent0->value() == 100,
                    "reset gains did not restore 100%");

            session.achievementsHardcoreChanged(true);
            require(!graphicsAction->isEnabled() && !planeA->isEnabled() &&
                    audioAction->isEnabled() && psgTone->isEnabled(),
                    "Hardcore did not lock only graphics controls");
            session.achievementsHardcoreChanged(false);
            require(planeA->isEnabled(), "graphics controls stayed locked after Hardcore");

            // The two already open windows must follow every system change.
            session.debugControlsChanged(quint32(1) << 4, quint64(1) << 19);
            QApplication::processEvents();
            require(graphics->isVisible() && audio->isVisible() &&
                    !planeA->isVisible() && !psgTone->isVisible() &&
                    child<QCheckBox>(*graphics, "debugVideo4")->isVisible() &&
                    child<QCheckBox>(*audio, "debugAudio19")->isVisible() &&
                    child<QSlider>(*audio, "debugAudioGain19")->value() == 100,
                    "open dialogs did not refresh for another system");
            session.debugControlsChanged(0, 0);
            QApplication::processEvents();
            require(graphics->isVisible() && audio->isVisible() &&
                    child<QLabel>(*graphics, "debugGraphicsEmpty")->isVisible() &&
                    child<QLabel>(*audio, "debugAudioEmpty")->isVisible() &&
                    !debug->menuAction()->isVisible(),
                    "open dialogs did not show the unloaded state");
            session.debugControlsChanged(mdVideo, mdAudio);
            QApplication::processEvents();
            require(graphics->isVisible() && audio->isVisible() &&
                    planeA->isVisible() && psgTone->isVisible() &&
                    planeA->isChecked() && psgTone->isChecked() && gain0->value() == 100,
                    "new game did not refresh and reset open dialogs");

            graphics->close();
            audio->close();
            graphicsAction->trigger();
            audioAction->trigger();
            require(graphics->isVisible() && audio->isVisible(),
                    "closing a debug window prevented reopening it");
            graphics->close();
            audio->close();
            window.activateWindow();
            QApplication::processEvents();
            require(window.isActiveWindow(), "main window did not regain focus for hotkey test");
            auto* master = child<QSlider>(window, "masterVolumeSlider");
            const int initialVolume = master->value();
            press(window, Qt::Key_Equal, Qt::ShiftModifier);
            require(master->value() == initialVolume + 5,
                    "plus hotkey did not raise master volume");
            press(window, Qt::Key_Minus, Qt::NoModifier, true);
            require(master->value() == initialVolume,
                    "global volume hotkeys did not change the shared slider");
            press(window, Qt::Key_Plus, Qt::KeypadModifier);
            require(master->value() == initialVolume + 5,
                    "keypad plus did not raise master volume");
            press(window, Qt::Key_Minus, Qt::KeypadModifier);
            require(master->value() == initialVolume,
                    "keypad minus did not lower master volume");
        }

        QSettings settings(settingsPath, QSettings::IniFormat);
        const auto keys = settings.allKeys();
        require(std::none_of(keys.begin(), keys.end(), [](const QString& key) {
            return key.contains(QStringLiteral("debug"), Qt::CaseInsensitive);
        }), "debug controls were persisted to settings");
        std::puts("debug UI tests passed: Blargg selection, live modeless dialogs, reset, volume hotkeys");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "debug UI tests failed: %s\n", error.what());
        return 1;
    }
}
