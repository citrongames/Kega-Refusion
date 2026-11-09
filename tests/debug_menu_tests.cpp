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
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
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
#include <SDL.h>

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
        const int first = rowFor(gameTable, 0);
        const int second = rowFor(gameTable, 1);
        check(first >= 0 && second >= 0, QStringLiteral("gameplay targets are missing"));
        if (first >= 0 && second >= 0) {
            auto* oldKey = gameTable->cellWidget(first, 1)->findChild<QKeySequenceEdit*>();
            auto* newKey = gameTable->cellWidget(second, 1)->findChild<QKeySequenceEdit*>();
            newKey->setKeySequence(QKeySequence(Qt::Key_X));
            check(oldKey->keySequence().isEmpty() &&
                  newKey->keySequence() == QKeySequence(Qt::Key_X),
                  QStringLiteral("new keyboard binding did not clear the previous target"));
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
            gameTable->cellDoubleClicked(first, 0);
            QApplication::processEvents();
            check(input->capturing() && error->isVisible() && !error->text().isEmpty(),
                  QStringLiteral("double-click did not show capture waiting footer"));
            check(error->styleSheet().contains(QStringLiteral("#18783c")) ||
                  error->styleSheet().contains(QStringLiteral("#70d993")),
                  QStringLiteral("capture waiting footer is not green"));
            if (!preview.isEmpty())
                check(dialog->grab().save(preview + QStringLiteral("_waiting.png")),
                      QStringLiteral("waiting preview could not be saved"));
            pad.button(10, true); input->poll();
            pad.button(10, false); input->poll();
            diagram->targetDoubleClicked(1);
            pad.button(10, true); input->poll();
            pad.button(10, false); input->poll();
            check(gameTable->item(first, 2)->text() == QStringLiteral("—") &&
                  gameTable->item(second, 2)->text() != QStringLiteral("—"),
                  QStringLiteral("diagram double-click or latest-wins pad binding failed"));
            tabs->setCurrentIndex(tabs->count() - 1);
            QApplication::processEvents();
            check(player->isVisible() && player->currentData().toInt() == 1,
                  QStringLiteral("command pad source is unclear"));
            commandTable->cellDoubleClicked(0, 0);
            pad.button(10, true); input->poll();
            pad.button(10, false); input->poll();
            check(commandTable->item(0, 2)->text() != QStringLiteral("—") &&
                  gameTable->item(second, 2)->text() == QStringLiteral("—"),
                  QStringLiteral("command binding did not take the pad control from gameplay"));
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
