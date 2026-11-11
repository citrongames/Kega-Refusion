#include "core/EmulationSession.h"
#include "i18n/Localization.h"
#include "ui/DebugDialogs.h"
#include "ui/ControlsPage.h"
#include "ui/ControllerDiagram.h"
#include "ui/MainWindow.h"
#include "ui/GameView.h"
#include "input/InputManager.h"
#include "input/ControllerType.h"
#include "input/ControllerTopology.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QColor>
#include <QCryptographicHash>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTabBar>
#include <QTimer>
#include <QThread>
#include <SDL.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <functional>
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

QString shaderSettingsPrefix(const QString& path) {
    return QStringLiteral("view/shaderParameters/%1/").arg(QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex()));
}

void testShaderMigration() {
    require(QGuiApplication::platformName() != QLatin1String("offscreen") &&
            QGuiApplication::platformName() != QLatin1String("minimal"),
            "--shader-migration requires a native Qt platform");
    QTemporaryDir data;
    require(data.isValid(), "shader settings directory unavailable");
    const QString settingsPath = QDir(data.path()).filePath(QStringLiteral("settings.ini"));
    const QString oldPreset = QStringLiteral(":/shaders/gdapt.glslp");
    const QString newPreset = QStringLiteral(":/shaders/refusion-dither.glslp");
    const QString customPreset = QDir(data.path()).filePath(QStringLiteral("gdapt.glslp"));
    const QString oldPrefix = shaderSettingsPrefix(oldPreset);
    const QString newPrefix = shaderSettingsPrefix(newPreset);
    const QString customPrefix = shaderSettingsPrefix(customPreset);
    ShaderPreset defaults;
    QString error;
    require(ShaderPreset::load(newPreset, defaults, error), qPrintable(error));
    require(defaults.parameters.size() == 1 &&
            defaults.parameters[0].id == QLatin1String("DITHER_STRENGTH"),
            "built-in dither strength parameter is missing");
    {
        QFile source(QDir(data.path()).filePath(QStringLiteral("custom.glsl")));
        require(source.open(QIODevice::WriteOnly), "custom shader fixture unavailable");
        require(source.write(R"(#pragma parameter MODE "Custom control" 0.25 0.0 1.0 0.05
#if defined(VERTEX)
attribute vec4 VertexCoord;
attribute vec4 TexCoord;
uniform mat4 MVPMatrix;
varying vec2 vTexCoord;
void main() { gl_Position = MVPMatrix * VertexCoord; vTexCoord = TexCoord.xy; }
#elif defined(FRAGMENT)
uniform sampler2D Texture;
varying vec2 vTexCoord;
void main() { gl_FragColor = texture2D(Texture, vTexCoord); }
#endif
)") > 0, "custom shader fixture write failed");
        QFile preset(customPreset);
        require(preset.open(QIODevice::WriteOnly) &&
                preset.write("shaders = 1\nshader0 = custom.glsl\n") > 0,
                "custom preset fixture write failed");
    }
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.setValue(QStringLiteral("view/shaderPreset"), oldPreset);
        settings.setValue(QStringLiteral("view/blarggFilter"), 4);
        settings.setValue(QStringLiteral("view/aspect"), 2);
        settings.setValue(QStringLiteral("view/integer"), true);
        settings.setValue(QStringLiteral("view/osd"), false);
        settings.setValue(QStringLiteral("files/shaderDirectory"), data.path());
        settings.setValue(oldPrefix + QStringLiteral("MODE"), 1.0);
        settings.setValue(oldPrefix + QStringLiteral("PWR"), 10.0);
        settings.setValue(oldPrefix + QStringLiteral("DITHER_STRENGTH"), 0.2);
        settings.setValue(customPrefix + QStringLiteral("MODE"), 0.75);
        settings.sync();
    }
    {
        EmulationSession session(QString(), data.path());
        MainWindow window(session, data.path(), true);
        auto* view = window.findChild<GameView*>();
        require(view && view->shaderPreset() == newPreset,
                "retired built-in selection did not migrate");
        QAction* dither = child<QAction>(window, "shaderDither");
        QAction* rgb = child<QAction>(window, "blargg4");
        QAction* off = child<QAction>(window, "shaderOff");
        require(dither->isChecked() && !rgb->isChecked() && !off->isChecked() &&
                dither->text() == QLatin1String("Dither blending"),
                "migrated shader menu selection is incorrect");
        require(view->shaderParameters().size() == defaults.parameters.size() &&
                view->shaderParameters()[0].value == defaults.parameters[0].value,
                "retired shader parameters leaked into the new shader");
        QAction* lcd = child<QAction>(window, "gameGearLcd");
        require(!lcd->isVisible(), "LCD ghosting is visible without a Game Gear game");
        session.gameGearChanged(true);
        lcd->trigger();
        require(lcd->isVisible() && lcd->isChecked() && dither->isChecked(),
                "Game Gear LCD ghosting did not combine with GLSL");
        rgb->trigger();
        require(!lcd->isChecked(), "Blargg did not disable LCD ghosting");
        lcd->trigger();
        require(lcd->isChecked() && !rgb->isChecked() && !off->isChecked(),
                "LCD ghosting did not replace Blargg");
        off->trigger();
        require(off->isChecked() && !lcd->isChecked(), "Off did not clear LCD ghosting");
        session.gameGearChanged(false);
        require(!lcd->isVisible(), "LCD ghosting stayed visible after leaving Game Gear");
        rgb->trigger();
        require(rgb->isChecked() && !dither->isChecked() && view->shaderPreset().isEmpty(),
                "Blargg did not clear the built-in shader");
        dither->trigger();
        require(dither->isChecked() && !rgb->isChecked() && view->shaderPreset() == newPreset,
                "built-in shader did not clear Blargg");
        off->trigger();
        require(off->isChecked() && !dither->isChecked() && view->shaderPreset().isEmpty(),
                "Off did not clear the built-in shader");
        dither->trigger();
        QString parameterFailure;
        bool inspected = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("shaderParametersDialog"));
            if (!dialog) {
                parameterFailure = QStringLiteral("shader parameter dialog did not open");
                if (auto* active = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                    active->reject();
            } else {
                auto* strength = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("DITHER_STRENGTH"));
                if (!strength) parameterFailure = QStringLiteral("dither strength editor is missing");
                else strength->setValue(0.35);
                dialog->accept();
            }
            inspected = true;
        });
        child<QAction>(window, "shaderParameters")->trigger();
        require(inspected && parameterFailure.isEmpty(), qPrintable(parameterFailure));
        require(qAbs(view->shaderParameters()[0].value - 0.35f) < 0.00001f,
                "parameter editor did not update the built-in shader");
    }
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        require(settings.value(QStringLiteral("view/shaderPreset")).toString() == newPreset &&
                settings.value(QStringLiteral("view/blarggFilter")).toInt() == 0,
                "migrated selection was not persisted exclusively");
        require(qAbs(settings.value(newPrefix + QStringLiteral("DITHER_STRENGTH")).toFloat() - 0.35f) < 0.00001f,
                "edited dither strength was not saved under its own preset");
        require(!settings.contains(oldPrefix + QStringLiteral("MODE")) &&
                !settings.contains(oldPrefix + QStringLiteral("PWR")) &&
                !settings.contains(oldPrefix + QStringLiteral("DITHER_STRENGTH")),
                "retired built-in parameter group was not removed");
        require(settings.value(customPrefix + QStringLiteral("MODE")).toDouble() == 0.75 &&
                settings.value(QStringLiteral("view/aspect")).toInt() == 2 &&
                settings.value(QStringLiteral("view/integer")).toBool() &&
                !settings.value(QStringLiteral("view/osd")).toBool() &&
                settings.value(QStringLiteral("files/shaderDirectory")).toString() == data.path(),
                "shader migration changed unrelated settings");
    }
    {
        EmulationSession session(QString(), data.path());
        MainWindow window(session, data.path(), true);
        auto* view = window.findChild<GameView*>();
        require(view && view->shaderPreset() == newPreset &&
                child<QAction>(window, "shaderDither")->isChecked() &&
                qAbs(view->shaderParameters()[0].value - 0.35f) < 0.00001f,
                "new shader selection and parameters did not survive restart");
    }
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.setValue(QStringLiteral("view/shaderPreset"), customPreset);
        settings.setValue(oldPrefix + QStringLiteral("MODE"), 0.5);
        settings.sync();
    }
    {
        EmulationSession session(QString(), data.path());
        MainWindow window(session, data.path(), true);
        auto* view = window.findChild<GameView*>();
        require(view && view->shaderPreset() == customPreset &&
                child<QAction>(window, "shaderCustom")->isChecked() &&
                !child<QAction>(window, "shaderDither")->isChecked() &&
                view->shaderParameters().size() == 1 &&
                view->shaderParameters()[0].id == QLatin1String("MODE") &&
                view->shaderParameters()[0].value == 0.75f,
                "external gdapt.glslp path or parameters were migrated");
    }
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        require(settings.value(QStringLiteral("view/shaderPreset")).toString() == customPreset &&
                settings.value(customPrefix + QStringLiteral("MODE")).toDouble() == 0.75 &&
                settings.value(oldPrefix + QStringLiteral("MODE")).toDouble() == 0.5,
                "custom selection triggered built-in parameter cleanup");
        settings.setValue(QStringLiteral("view/shaderPreset"), QString());
        settings.setValue(QStringLiteral("view/blarggFilter"), 3);
        settings.sync();
    }
    {
        EmulationSession session(QString(), data.path());
        MainWindow window(session, data.path(), true);
        require(child<QAction>(window, "blargg3")->isChecked() &&
                !child<QAction>(window, "shaderDither")->isChecked(),
                "unchanged Blargg preference was not restored");
    }
}

struct VirtualControlsPad {
    int index = -1;
    SDL_Joystick* joystick = nullptr;
    VirtualControlsPad() {
        index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_UNKNOWN, 2, 12, 1);
        require(index >= 0, "virtual controls pad could not be attached");
        joystick = SDL_JoystickOpen(index);
        require(joystick != nullptr, "virtual controls pad could not be opened");
    }
    ~VirtualControlsPad() {
        if (joystick) SDL_JoystickClose(joystick);
        if (index >= 0) SDL_JoystickDetachVirtual(index);
    }
    void button(int button, bool pressed) {
        require(SDL_JoystickSetVirtualButton(joystick, button, pressed ? 1 : 0) == 0,
                "virtual controls button update failed");
    }
};

void press(QWidget& target, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
           bool repeat = false) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers, QString(), repeat);
    QApplication::sendEvent(&target, &event);
}

void testControlsSystem(const QString& corePath, const QString& dataPath) {
    QSettings settings(QDir(dataPath).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    settings.setValue(QStringLiteral("achievements/enabled"), false);
    settings.sync();

    QByteArray z80(32768, char(0));
    const auto program = QByteArray::fromHex("f331f0df2100c03418fd");
    z80.replace(0, program.size(), program);
    z80.replace(0x7ff0, 8, "TMR SEGA");
    z80[0x7fff] = char(0x4c);
    QByteArray gg = z80;
    gg[0x7fff] = char(0x6c);
    QByteArray sg = z80;
    sg.replace(0x7ff0, 8, QByteArray(8, char(0)));
    const auto writeRom = [&](const QString& name, const QByteArray& bytes) {
        const QString path = QDir(dataPath).filePath(name);
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
                "controls system ROM fixture write failed");
        return path;
    };
    const QString smsPath = writeRom(QStringLiteral("master.sms"), z80);
    const QString ggPath = writeRom(QStringLiteral("handheld.gg"), gg);
    const QString sgPath = writeRom(QStringLiteral("early.sg"), sg);

    EmulationSession session(corePath, dataPath);
    MainWindow window(session, dataPath, true);
    window.show();
    QApplication::processEvents();
    auto* input = window.findChild<InputManager*>();
    require(input, "controls system input manager unavailable");
    int loaded = 0, closed = 0, errors = 0;
    QObject::connect(&session, &EmulationSession::gameLoaded, &window,
                     [&](const QString&, double, const QString&, ConsoleProfile) { ++loaded; });
    QObject::connect(&session, &EmulationSession::gameClosed, &window, [&] { ++closed; });
    QObject::connect(&session, &EmulationSession::errorOccurred, &window,
                     [&](const QString& message) {
        ++errors;
        std::fprintf(stderr, "%s\n", qPrintable(message));
    });
    const auto waitFor = [&](const std::function<bool()>& predicate, const char* message) {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 5000) {
            QApplication::processEvents();
            QThread::msleep(2);
        }
        require(predicate(), message);
    };
    const auto inspect = [&](ConsoleProfile expected, int expectedPlayer,
                             int selectPlayer, int selectProfile) {
        bool inspected = false;
        QString failure;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("emulatorSettingsDialog"));
            auto* navigation = dialog ? dialog->findChild<QListWidget*>(QStringLiteral("settingsNavigation")) : nullptr;
            if (!navigation) {
                failure = QStringLiteral("controls system settings dialog missing");
            } else {
                navigation->setCurrentRow(7);
                QApplication::processEvents();
                auto* profile = dialog->findChild<QComboBox*>(QStringLiteral("profileBox"));
                auto* player = dialog->findChild<QComboBox*>(QStringLiteral("playerBox"));
                if (!profile || !player) {
                    failure = QStringLiteral("controls system selector missing");
                } else if (profile->currentData().toInt() != int(expected) ||
                           player->currentData().toInt() != expectedPlayer) {
                    failure = QStringLiteral("controls opened on wrong system or port");
                } else {
                    if (selectPlayer >= 0) {
                        const int index = player->findData(selectPlayer);
                        if (index < 0) failure = QStringLiteral("remembered port unavailable");
                        else player->setCurrentIndex(index);
                    }
                    if (selectProfile >= 0) {
                        const int index = profile->findData(selectProfile);
                        if (index < 0) failure = QStringLiteral("manual system unavailable");
                        else profile->setCurrentIndex(index);
                    }
                }
            }
            inspected = true;
            if (dialog) dialog->reject();
            else if (auto* active = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                active->reject();
        });
        child<QAction>(window, "emulatorSettingsAction")->trigger();
        require(inspected && failure.isEmpty(), qPrintable(failure));
    };
    const auto open = [&](const QString& path, int expectedCount, ConsoleProfile expected) {
        window.openRom(path);
        waitFor([&] { return loaded == expectedCount || errors; },
                "controls system ROM load timed out");
        require(errors == 0 && input->profile() == expected,
                "loaded ROM selected wrong input system");
    };

    inspect(ConsoleProfile::MegaDrive, 0, -1, -1);
    open(smsPath, 1, ConsoleProfile::MasterSystem);
    {
        QMap<int, int> keys;
        for (int command = PauseCommand; command <= ScreenshotCommand; ++command)
            keys[command] = input->commandKey(command);
        keys[FastForwardCommand] = int(Qt::ControlModifier) | int(Qt::ShiftModifier) | Qt::Key_K;
        input->setCommandKeys(keys);
        auto* view = window.findChild<GameView*>();
        require(view, "running game view is missing");
        window.activateWindow();
        view->setFocus();
        QApplication::processEvents();
        require(window.isActiveWindow() && view->hasFocus(),
                "running game view did not receive keyboard focus");
        const auto release = [&](int key, Qt::KeyboardModifiers modifiers) {
            QKeyEvent event(QEvent::KeyRelease, key, modifiers);
            QApplication::sendEvent(view, &event);
        };
        press(*view, Qt::Key_Return, Qt::AltModifier);
        require(window.isFullScreen(), "Alt+Enter did not enter fullscreen");
        release(Qt::Key_Return, Qt::AltModifier);
        press(*view, Qt::Key_Return, Qt::AltModifier);
        require(!window.isFullScreen(), "Alt+Enter did not leave fullscreen");
        release(Qt::Key_Return, Qt::AltModifier);
        window.activateWindow();
        view->setFocus();
        QApplication::processEvents();
        const auto pressChord = [&] {
            press(*view, Qt::Key_Control, Qt::ControlModifier);
            press(*view, Qt::Key_Shift, Qt::ControlModifier | Qt::ShiftModifier);
            require(!view->fastForwardActive(), "modifier alone triggered held command");
            press(*view, Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier);
            require(view->fastForwardActive(), "Ctrl+Shift+K did not hold fast forward");
        };
        pressChord();
        release(Qt::Key_Shift, Qt::ControlModifier);
        require(!view->fastForwardActive(),
                "held command stayed active after its modifier was released first");
        release(Qt::Key_K, Qt::ControlModifier);
        release(Qt::Key_Control, Qt::NoModifier);
        pressChord();
        release(Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier);
        require(!view->fastForwardActive(),
                "held command stayed active after its primary key was released");
        release(Qt::Key_Shift, Qt::ControlModifier);
        release(Qt::Key_Control, Qt::NoModifier);
    }
    for (int key : {Qt::Key_F5, Qt::Key_F8}) {
        bool correctPicker = false;
        QTimer dismiss;
        dismiss.setSingleShot(true);
        QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
            if (auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                correctPicker = picker->acceptMode() == (key == Qt::Key_F5
                    ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
                picker->reject();
            }
        });
        window.activateWindow();
        QApplication::processEvents();
        dismiss.start(0);
        press(window, key, Qt::ShiftModifier);
        require(correctPicker, "shifted quick-state key did not open the matching file picker");
    }

    inspect(ConsoleProfile::MasterSystem, 0, 1, -1);
    open(ggPath, 2, ConsoleProfile::GameGear);
    inspect(ConsoleProfile::GameGear, 0, -1, int(ConsoleProfile::MegaDrive));
    inspect(ConsoleProfile::GameGear, 0, -1, -1);
    open(sgPath, 3, ConsoleProfile::SG1000);
    inspect(ConsoleProfile::SG1000, 0, -1, -1);
    open(smsPath, 4, ConsoleProfile::MasterSystem);
    inspect(ConsoleProfile::MasterSystem, 1, -1, -1);
    session.closeGame();
    waitFor([&] { return closed == 1 || errors; }, "controls system close timed out");
    require(errors == 0, "controls system close failed");
    inspect(ConsoleProfile::MegaDrive, 0, -1, -1);
    session.shutdown();
}

void testControlsLayout(MainWindow& window) {
    VirtualControlsPad pad;
    auto* input = window.findChild<InputManager*>();
    require(input != nullptr, "window input manager is missing");
    input->poll();
    QString failure;
    bool inspected = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("emulatorSettingsDialog"));
        if (!dialog) {
            failure = QStringLiteral("settings dialog did not open");
            if (auto* active = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                active->reject();
            inspected = true;
            return;
        }
        const auto check = [&](bool okay, const QString& message) {
            if (!okay && failure.isEmpty()) failure = message;
        };
        check(!(dialog->windowFlags() & Qt::WindowMinMaxButtonsHint) &&
              dialog->windowFlags().testFlag(Qt::WindowCloseButtonHint),
              QStringLiteral("settings title-bar actions are incorrect"));
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == QLatin1String("windows")) {
            const auto style = GetWindowLongPtrW(reinterpret_cast<HWND>(dialog->winId()), GWL_STYLE);
            check(!(style & (WS_MAXIMIZEBOX | WS_MINIMIZEBOX)) && (style & WS_THICKFRAME),
                  QStringLiteral("native settings title-bar buttons or resize border are incorrect"));
        }
#endif
        const auto finish = [&] { inspected = true; dialog->reject(); };
        auto* navigation = dialog->findChild<QListWidget*>(QStringLiteral("settingsNavigation"));
        if (!navigation) { check(false, QStringLiteral("settings navigation missing")); finish(); return; }
        navigation->setCurrentRow(7);
        QApplication::processEvents();
        auto* viewport = dialog->findChild<QScrollArea*>(QStringLiteral("controlsPageViewport"));
        auto* profile = dialog->findChild<QComboBox*>(QStringLiteral("profileBox"));
        auto* player = dialog->findChild<QComboBox*>(QStringLiteral("playerBox"));
        auto* type = dialog->findChild<QComboBox*>(QStringLiteral("deviceTypeBox"));
        auto* tabs = dialog->findChild<QTabBar*>(QStringLiteral("mappingTabs"));
        auto* adapter = dialog->findChild<QComboBox*>(QStringLiteral("adapterBox"));
        auto* controls = dialog->findChild<ControlsPage*>();
        auto* diagram = dialog->findChild<ControllerDiagram*>();
        auto* gameTable = dialog->findChild<QTableWidget*>(QStringLiteral("gameMappingTable"));
        auto* commandTable = dialog->findChild<QTableWidget*>(QStringLiteral("commandMappingTable"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("settingsButtons"));
        auto* error = dialog->findChild<QLabel*>(QStringLiteral("settingsValidationError"));
        if (!viewport || !profile || !player || !type || !tabs || !adapter || !controls ||
            !diagram || !gameTable || !commandTable || !buttons || !error) {
            check(false, QStringLiteral("Controls page or settings footer is missing"));
            finish();
            return;
        }
        // Offscreen's virtual screen may be smaller than the normal desktop.
        // Measure the real page at the dialog's intended default 1100×720.
        dialog->resize(1100, 720);
        QApplication::processEvents();
        check(dialog->size() == QSize(1100, 720),
              QStringLiteral("standard settings geometry was not applied"));
        const auto outerFits = [&](const QString& kind) {
            QApplication::processEvents();
            check(viewport->verticalScrollBar()->maximum() == 0 &&
                  viewport->horizontalScrollBar()->maximum() == 0,
                  QStringLiteral("Controls page scrolls for %1").arg(kind));
        };
        const auto choose = [&](QComboBox* box, int value, const QString& label) {
            const int index = box->findData(value);
            check(index >= 0, QStringLiteral("%1 option is missing").arg(label));
            if (index >= 0) box->setCurrentIndex(index);
            QApplication::processEvents();
        };
        outerFits(QStringLiteral("default six-button pad"));
        check(tabs->count() == 3 && tabs->currentIndex() == 0,
              QStringLiteral("port and command tabs are missing"));
        check(gameTable && gameTable->verticalScrollBar()->maximum() == 0,
              QStringLiteral("default six-button pad button table scrolls"));

        const auto rowFor = [](QTableWidget* table, int target) {
            for (int row = 0; row < table->rowCount(); ++row)
                if (table->item(row, 0)->data(Qt::UserRole).toInt() == target) return row;
            return -1;
        };
        const auto clickCell = [&](QTableWidget* table, int row, int column,
                                   Qt::MouseButton button, bool doubleClick = false) {
            auto* item = table->item(row, column);
            check(item != nullptr, QStringLiteral("mapping cell is missing"));
            if (!item) return;
            table->scrollToItem(item);
            QApplication::processEvents();
            const QRect rect = table->visualItemRect(item);
            auto* view = table->viewport();
            const QPoint point = rect.center();
            check(rect.isValid() && view->rect().contains(point),
                  QStringLiteral("mapping cell is outside its viewport"));
            if (!rect.isValid() || !view->rect().contains(point)) return;
            const auto sendMouse = [&](QEvent::Type type, Qt::MouseButtons held) {
                QMouseEvent event(type, point, view->mapToGlobal(point),
                                  button, held, Qt::NoModifier);
                QApplication::sendEvent(view, &event);
            };
            sendMouse(QEvent::MouseButtonPress, button);
            sendMouse(QEvent::MouseButtonRelease, Qt::NoButton);
            if (doubleClick) {
                sendMouse(QEvent::MouseButtonDblClick, button);
                sendMouse(QEvent::MouseButtonRelease, Qt::NoButton);
            }
            QApplication::processEvents();
        };
        const auto sendKey = [&](int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            press(*controls, key, modifiers);
            QKeyEvent release(QEvent::KeyRelease, key, modifiers);
            QApplication::sendEvent(controls, &release);
        };
        const int first = rowFor(gameTable, 0);
        const int second = rowFor(gameTable, 1);
        check(first >= 0 && second >= 0, QStringLiteral("gameplay targets are missing"));
        if (first >= 0 && second >= 0) {
            auto* oldKey = gameTable->item(first, 1);
            auto* newKey = gameTable->item(second, 1);
            const QString previous = newKey->text();
            clickCell(gameTable, second, 1, Qt::LeftButton);
            sendKey(Qt::Key_X);
            check(newKey->text() == previous && oldKey->text() == QStringLiteral("X"),
                  QStringLiteral("one left click armed keyboard capture"));
            clickCell(gameTable, second, 1, Qt::RightButton, true);
            sendKey(Qt::Key_X);
            check(newKey->text() == previous && !input->capturing(),
                  QStringLiteral("right double-click armed keyboard capture"));
            clickCell(gameTable, second, 1, Qt::LeftButton, true);
            const QString oldPad = gameTable->item(second, 2)->text();
            pad.button(10, true); input->poll();
            pad.button(10, false); input->poll();
            check(newKey->text() == previous && gameTable->item(second, 2)->text() == oldPad,
                  QStringLiteral("keyboard-only capture accepted gamepad input"));
            sendKey(Qt::Key_X);
            check(oldKey->text() == QStringLiteral("—") && newKey->text() == QStringLiteral("X"),
                  QStringLiteral("new keyboard binding did not clear the previous target"));
        }

        tabs->setCurrentIndex(tabs->count() - 1);
        QApplication::processEvents();
        const int pauseRow = rowFor(commandTable, PauseCommand);
        const int fastRow = rowFor(commandTable, FastForwardCommand);
        const int fullscreenRow = rowFor(commandTable, FullscreenCommand);
        const int hardResetRow = rowFor(commandTable, HardResetCommand);
        check(pauseRow >= 0 && fastRow >= 0 && fullscreenRow >= 0 && hardResetRow >= 0,
              QStringLiteral("keyboard command rows are missing"));
        if (first >= 0 && second >= 0 && pauseRow >= 0 && fastRow >= 0 &&
            fullscreenRow >= 0 && hardResetRow >= 0) {
            auto* pauseKey = commandTable->item(pauseRow, 1);
            const QString originalPause = pauseKey->text();
            auto* commandView = commandTable->viewport();
            clickCell(commandTable, pauseRow, 1, Qt::LeftButton);
            sendKey(Qt::Key_X);
            check(!input->capturing() && pauseKey->text() == originalPause,
                  QStringLiteral("one command click armed keyboard capture"));
            clickCell(commandTable, pauseRow, 1, Qt::RightButton, true);
            sendKey(Qt::Key_X);
            check(!input->capturing() && pauseKey->text() == originalPause,
                  QStringLiteral("right command double-click armed capture"));

            const auto waitingColor = [&](int row, int column) {
                commandTable->scrollToItem(commandTable->item(row, column));
                QApplication::processEvents();
                const QRect cell = commandTable->visualItemRect(commandTable->item(row, column));
                const QImage sample = commandView->grab(
                    QRect(cell.right() - 8, cell.center().y(), 2, 2)).toImage();
                const QColor color = sample.pixelColor(0, 0);
                return color.red() > color.green() + 60 && color.red() > color.blue() + 60;
            };
            clickCell(commandTable, pauseRow, 2, Qt::LeftButton, true);
            check(waitingColor(pauseRow, 2) && !waitingColor(pauseRow, 1),
                  QStringLiteral("waiting gamepad cell not visibly distinguished from keyboard"));
            sendKey(Qt::Key_Z);
            check(waitingColor(pauseRow, 2) && pauseKey->text() == originalPause,
                  QStringLiteral("gamepad-only capture accepted a keyboard key"));
            pad.button(11, true); input->poll();
            pad.button(11, false); input->poll();
            check(commandTable->item(pauseRow, 2)->text() != QStringLiteral("—"),
                  QStringLiteral("gamepad-only capture rejected gamepad input"));
            clickCell(commandTable, pauseRow, 2, Qt::MiddleButton);
            check(commandTable->item(pauseRow, 2)->text() == QStringLiteral("—") &&
                  !input->capturing(),
                  QStringLiteral("middle click did not clear command gamepad binding"));

            clickCell(commandTable, pauseRow, 1, Qt::LeftButton);
            child<QPushButton>(*controls, "assignSequence")->click();
            check(waitingColor(pauseRow, 1) && !waitingColor(pauseRow, 2),
                  QStringLiteral("sequence ignored selected keyboard source"));
            pad.button(11, true); input->poll();
            pad.button(11, false); input->poll();
            check(waitingColor(pauseRow, 1),
                  QStringLiteral("keyboard sequence accepted gamepad input"));
            sendKey(Qt::Key_P);
            QApplication::processEvents();
            check(!waitingColor(pauseRow, 1) && waitingColor(fastRow, 1),
                  QStringLiteral("keyboard sequence did not advance to its next row"));
            controls->cancelCapture();

            clickCell(commandTable, pauseRow, 2, Qt::LeftButton);
            child<QPushButton>(*controls, "assignSequence")->click();
            check(waitingColor(pauseRow, 2) && !waitingColor(pauseRow, 1),
                  QStringLiteral("sequence ignored selected gamepad source"));
            sendKey(Qt::Key_Z);
            check(waitingColor(pauseRow, 2),
                  QStringLiteral("gamepad sequence accepted keyboard input"));
            pad.button(11, true); input->poll();
            pad.button(11, false); input->poll();
            QApplication::processEvents();
            check(!waitingColor(pauseRow, 2) && waitingColor(fastRow, 2),
                  QStringLiteral("gamepad sequence did not advance to its next row"));
            controls->cancelCapture();
            clickCell(commandTable, pauseRow, 2, Qt::MiddleButton);

            const auto captureCommand = [&](int row, int key, Qt::KeyboardModifiers modifiers) {
                clickCell(commandTable, row, 1, Qt::LeftButton, true);
                sendKey(key, modifiers);
            };
            captureCommand(pauseRow, Qt::Key_X, Qt::NoModifier);
            check(pauseKey->text() == QStringLiteral("X") &&
                  gameTable->item(second, 1)->text() == QStringLiteral("—"),
                  QStringLiteral("command keyboard assignment did not take gameplay X"));
            captureCommand(fastRow, Qt::Key_X, Qt::NoModifier);
            check(pauseKey->text() == QStringLiteral("—") &&
                  commandTable->item(fastRow, 1)->text() == QStringLiteral("X"),
                  QStringLiteral("new command keyboard assignment did not clear previous hotkey"));

            tabs->setCurrentIndex(0);
            QApplication::processEvents();
            clickCell(gameTable, second, 1, Qt::LeftButton, true);
            sendKey(Qt::Key_X);
            check(gameTable->item(second, 1)->text() == QStringLiteral("X") &&
                  commandTable->item(fastRow, 1)->text() == QStringLiteral("—"),
                  QStringLiteral("gameplay keyboard assignment did not clear command hotkey"));
            clickCell(gameTable, second, 1, Qt::MiddleButton);
            check(gameTable->item(second, 1)->text() == QStringLiteral("—"),
                  QStringLiteral("middle click did not clear gameplay keyboard binding"));

            tabs->setCurrentIndex(tabs->count() - 1);
            QApplication::processEvents();
            clickCell(commandTable, fastRow, 1, Qt::MiddleButton, true);
            sendKey(Qt::Key_Q);
            check(!input->capturing() &&
                  commandTable->item(fastRow, 1)->text() == QStringLiteral("—"),
                  QStringLiteral("middle double-click armed command capture"));
            captureCommand(fullscreenRow, Qt::Key_Return, Qt::AltModifier);
            check(commandTable->item(fullscreenRow, 1)->text() ==
                  QKeySequence(int(Qt::AltModifier) | Qt::Key_Return).toString(QKeySequence::NativeText),
                  QStringLiteral("Alt+Enter command chord was not captured"));
            captureCommand(hardResetRow, Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier);
            check(commandTable->item(hardResetRow, 1)->text() ==
                  QKeySequence(int(Qt::ControlModifier) | int(Qt::ShiftModifier) |
                               Qt::Key_K).toString(QKeySequence::NativeText),
                  QStringLiteral("Ctrl+Shift+key command chord was not captured"));
            clickCell(commandTable, hardResetRow, 1, Qt::MiddleButton);
            check(commandTable->item(hardResetRow, 1)->text() == QStringLiteral("—"),
                  QStringLiteral("middle click did not clear command keyboard binding"));
            clickCell(commandTable, hardResetRow, 0, Qt::LeftButton, true);
            sendKey(Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier);
            check(commandTable->item(hardResetRow, 1)->text() ==
                  QKeySequence(int(Qt::ControlModifier) | int(Qt::ShiftModifier) |
                               Qt::Key_K).toString(QKeySequence::NativeText),
                  QStringLiteral("generic command name cell rejected keyboard input"));
        }

        tabs->setCurrentIndex(1);
        QApplication::processEvents();
        check(player->currentData().toInt() == 1, QStringLiteral("second port tab did not select player"));
        choose(type, int(ControllerType::Pad), QStringLiteral("MD port 2 pad"));
        const QString preview = qEnvironmentVariable("KEGA_CONTROLS_PREVIEW");
        if (!preview.isEmpty()) {
            QDir().mkpath(QFileInfo(preview).absolutePath());
            check(dialog->grab().save(preview + QStringLiteral("_port2.png")),
                  QStringLiteral("port 2 preview could not be saved"));
        }
        if (first >= 0 && second >= 0) {
            clickCell(gameTable, first, 0, Qt::LeftButton, true);
            QApplication::processEvents();
            check(input->capturing() && error->isVisible() && !error->text().isEmpty(),
                  QStringLiteral("left double-click did not show capture waiting footer"));
            check(error->styleSheet().contains(QStringLiteral("#18783c")) ||
                  error->styleSheet().contains(QStringLiteral("#70d993")),
                  QStringLiteral("capture waiting footer is not green"));
            if (!preview.isEmpty())
                check(dialog->grab().save(preview + QStringLiteral("_waiting.png")),
                      QStringLiteral("waiting preview could not be saved"));
            pad.button(10, true); input->poll();
            pad.button(10, false); input->poll();
            clickCell(gameTable, second, 2, Qt::LeftButton, true);
            const QString oldSecondKey = gameTable->item(second, 1)->text();
            sendKey(Qt::Key_J);
            check(gameTable->item(second, 1)->text() == oldSecondKey &&
                  input->capturing(),
                  QStringLiteral("gamepad-only gameplay capture accepted a keyboard key"));
            pad.button(10, true); input->poll();
            pad.button(10, false); input->poll();
            check(gameTable->item(first, 2)->text() == QStringLiteral("—") &&
                  gameTable->item(second, 2)->text() != QStringLiteral("—"),
                  QStringLiteral("gameplay gamepad source or latest-wins binding failed"));
            diagram->targetDoubleClicked(0);
            sendKey(Qt::Key_J);
            check(gameTable->item(first, 1)->text() == QStringLiteral("J"),
                  QStringLiteral("generic diagram capture rejected keyboard input"));
            diagram->targetDoubleClicked(1);
            check(input->capturing(), QStringLiteral("diagram double-click did not arm gamepad capture"));
            pad.button(10, true); input->poll();
            pad.button(10, false); input->poll();
            check(gameTable->item(second, 2)->text() != QStringLiteral("—"),
                  QStringLiteral("generic diagram capture rejected gamepad input"));
            tabs->setCurrentIndex(tabs->count() - 1);
            QApplication::processEvents();
            check(player->isVisible() && player->currentData().toInt() == 1,
                  QStringLiteral("command pad source is unclear"));
            clickCell(commandTable, 0, 0, Qt::LeftButton, true);
            pad.button(10, true); input->poll();
            pad.button(10, false); input->poll();
            check(commandTable->item(0, 2)->text() != QStringLiteral("—") &&
                  gameTable->item(second, 2)->text() == QStringLiteral("—"),
                  QStringLiteral("generic command name capture did not take gameplay pad control"));
            clickCell(commandTable, 0, 2, Qt::MiddleButton);
            check(commandTable->item(0, 2)->text() == QStringLiteral("—"),
                  QStringLiteral("middle click did not clear command gamepad binding"));
            tabs->setCurrentIndex(0);
            QApplication::processEvents();
        }

        choose(type, int(ControllerType::Mouse), QStringLiteral("MD Mouse"));
        outerFits(QStringLiteral("MD Mouse"));
        if (first >= 0 && second >= 0) {
            auto* previous = qobject_cast<QComboBox*>(gameTable->cellWidget(first, 3));
            auto* moved = qobject_cast<QComboBox*>(gameTable->cellWidget(second, 3));
            if (previous && moved) {
                moved->setCurrentIndex(moved->findData(int(Qt::RightButton)));
                check(previous->currentData().toInt() == 0,
                      QStringLiteral("new mouse binding did not clear the previous target"));
                const QPoint point = moved->rect().center();
                QMouseEvent down(QEvent::MouseButtonPress, point, moved->mapToGlobal(point),
                                 Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
                QApplication::sendEvent(moved, &down);
                QMouseEvent up(QEvent::MouseButtonRelease, point, moved->mapToGlobal(point),
                               Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(moved, &up);
                check(moved->currentData().toInt() == 0,
                      QStringLiteral("middle click did not clear mouse binding"));
            }
        }
        tabs->setCurrentIndex(1);
        QApplication::processEvents();
        check(player->currentData().toInt() == 1, QStringLiteral("second port tab did not select player"));
        choose(type, int(ControllerType::Menacer), QStringLiteral("Menacer"));
        outerFits(QStringLiteral("Menacer"));
        choose(type, int(ControllerType::Justifier), QStringLiteral("Justifier"));
        outerFits(QStringLiteral("Justifier"));
        choose(type, int(ControllerType::Menacer), QStringLiteral("Menacer"));
        choose(adapter, int(ControllerAdapter::TeamPlayerPort1), QStringLiteral("Team Player port 1"));
        check(tabs->count() == int(adapterPlayerCount(ControllerAdapter::TeamPlayerPort1)) + 1,
              QStringLiteral("adapter did not expand port tabs"));
        choose(adapter, int(ControllerAdapter::None), QStringLiteral("no adapter"));
        choose(profile, 1, QStringLiteral("Master System"));
        tabs->setCurrentIndex(0);
        QApplication::processEvents();
        choose(type, int(ControllerType::Paddle), QStringLiteral("SMS Paddle"));
        outerFits(QStringLiteral("SMS Paddle"));
        choose(type, int(ControllerType::LightPhaser), QStringLiteral("Light Phaser"));
        outerFits(QStringLiteral("Light Phaser"));

        buttons->button(QDialogButtonBox::Ok)->click();
        QApplication::processEvents();
        check(dialog->isVisible() && error->isVisible() && !error->text().isEmpty(),
              QStringLiteral("invalid controls did not show the sticky validation footer"));
        check(profile->currentData().toInt() == 0 && player->currentData().toInt() == 1,
              QStringLiteral("validation did not select the conflicting MD port"));
        tabs->setCurrentIndex(tabs->count() - 1);
        QApplication::processEvents();
        finish();
    });
    child<QAction>(window, "emulatorSettingsAction")->trigger();
    require(inspected, "settings layout inspection did not run");
    bool reopened = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("emulatorSettingsDialog"));
        auto* controls = dialog ? dialog->findChild<ControlsPage*>() : nullptr;
        if (!dialog || !controls) {
            if (failure.isEmpty()) failure = QStringLiteral("settings did not reopen on controls");
        } else {
            const auto state = controls->viewState();
            if (state.profile != 0 || state.player != 1 || !state.commands)
                if (failure.isEmpty()) failure = QStringLiteral("controls view selection was not restored");
            if (input->mapping(ConsoleProfile::MegaDrive, 1).controllerType != ControllerType::Pad)
                if (failure.isEmpty()) failure = QStringLiteral("cancel committed a controller draft");
            dialog->reject();
        }
        reopened = true;
    });
    child<QAction>(window, "emulatorSettingsAction")->trigger();
    require(reopened, "settings reopen inspection did not run");
    require(failure.isEmpty(), qPrintable(failure));
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

        if (app.arguments().contains(QStringLiteral("--shader-migration"))) {
            testShaderMigration();
            std::puts("Dither menu, exact built-in migration, parameters, custom presets and Blargg persistence passed");
            return 0;
        }

        if (app.arguments().contains(QStringLiteral("--folders"))) {
            EmulationSession session(QString(), data.path());
            MainWindow window(session, data.path(), true);
            bool portableDefault = false;
            QTimer::singleShot(0, &window, [&] {
                auto* dialog = window.findChild<QDialog*>("emulatorSettingsDialog");
                if (!dialog) return;
                auto* nav = dialog->findChild<QListWidget*>("settingsNavigation");
                auto* field = dialog->findChild<QLineEdit*>("folderPath_screenshots");
                auto* buttons = dialog->findChild<QDialogButtonBox*>("settingsButtons");
                if (!nav || !field || !buttons) { dialog->reject(); return; }
                nav->setCurrentRow(nav->count() - 1);
                portableDefault = field->isVisible() && field->text() == QLatin1String("screenshots");
                field->setText(QStringLiteral("images/test"));
                buttons->button(QDialogButtonBox::Ok)->click();
                if (dialog->isVisible()) dialog->reject();
            });
            child<QAction>(window, "emulatorSettingsAction")->trigger();
            require(portableDefault && QFileInfo(data.filePath("images/test")).isDir(),
                    "folders page did not apply the portable screenshot path");
            return 0;
        }

        if (app.arguments().contains(QStringLiteral("--controls-system"))) {
            require(argc == 4, "controls system test requires a core library");
            testControlsSystem(QString::fromLocal8Bit(argv[2]), data.path());
            std::puts("Controls system selection, manual override, remembered port and unload passed");
            return 0;
        }

        if (app.arguments().contains(QStringLiteral("--controls-layout"))) {
            EmulationSession session(QString(), data.path());
            MainWindow window(session, data.path(), true);
            window.show();
            QApplication::processEvents();
            testControlsLayout(window);
            std::puts("Controls layout and sticky validation footer passed");
            return 0;
        }

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
            QAction* dither = child<QAction>(window, "shaderDither");
            require(off->isChecked() && !window.findChild<QAction*>(QStringLiteral("blargg1")),
                    "legacy monochrome was not normalized to Off");
            const auto shaderActions = shader->actions();
            require(shaderActions.indexOf(composite) < shaderActions.indexOf(svideo) &&
                    shaderActions.indexOf(svideo) < shaderActions.indexOf(rgb) &&
                    shaderActions.indexOf(rgb) < shaderActions.indexOf(dither),
                    "Blargg modes are not listed before Dither blending");
            require(dither->text() == QLatin1String("Dither blending") &&
                    !window.findChild<QAction*>(QStringLiteral("shaderGdapt")),
                    "Dither blending menu entry is not localized or the retired entry remains");
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
