#include "DebugDialogs.h"
#include "DebugInspectorWidgets.h"
#include "i18n/Localization.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QFrame>
#include <QGroupBox>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
Qt::WindowFlags debugWindowFlags() {
    return Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
           Qt::WindowCloseButtonHint;
}
}

DebugGraphicsDialog::DebugGraphicsDialog(QWidget* parent)
    : QDialog(parent, debugWindowFlags()) {
    setObjectName(QStringLiteral("debugGraphicsDialog"));
    setWindowTitle(Localization::text(QStringLiteral("Графика")));
    setWindowModality(Qt::NonModal);
    setMinimumWidth(280);
    resize(610, 450);
    auto* outer = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("debugGraphicsTabs"));
    outer->addWidget(tabs_);
    auto* controls = new QWidget(tabs_);
    auto* layout = new QVBoxLayout(controls);
    tabs_->addTab(controls, Localization::text(QStringLiteral("Управление")));
    placeholder_ = new QLabel(Localization::text(QStringLiteral("Игра не загружена")), this);
    placeholder_->setObjectName(QStringLiteral("debugGraphicsEmpty"));
    layout->addWidget(placeholder_);
    const std::array<QString, 6> names{
        Localization::text(QStringLiteral("Плоскость A")),
        Localization::text(QStringLiteral("Плоскость B")),
        Localization::text(QStringLiteral("Окно")),
        Localization::text(QStringLiteral("Спрайты")),
        Localization::text(QStringLiteral("Фон")),
        Localization::text(QStringLiteral("Слой 32X"))
    };
    for (unsigned index = 0; index < layers_.size(); ++index) {
        auto* layer = new QCheckBox(names[index], this);
        layer->setObjectName(QStringLiteral("debugVideo%1").arg(index));
        layer->setVisible(false);
        layers_[index] = layer;
        layout->addWidget(layer);
        connect(layer, &QCheckBox::toggled, this, [this, index](bool visible) {
            const quint32 bit = quint32(1) << index;
            if (visible) disabled_ &= ~bit;
            else disabled_ |= bit;
            restore_->setEnabled(disabled_ != 0);
            emit maskChanged(disabled_);
        });
    }
    restore_ = new QPushButton(Localization::text(QStringLiteral("Включить все слои")), this);
    restore_->setObjectName(QStringLiteral("debugRestoreVideo"));
    layout->addWidget(restore_);
    connect(restore_, &QPushButton::clicked, this, [this] {
        disabled_ = 0;
        for (QCheckBox* layer : layers_) {
            const QSignalBlocker blocker(layer);
            layer->setChecked(true);
        }
        restore_->setEnabled(false);
        emit maskChanged(0);
    });
    auto* colorRow = new QHBoxLayout;
    backdropButton_ = new QPushButton(Localization::text(QStringLiteral("Цвет подложки…")), this);
    backdropButton_->setObjectName(QStringLiteral("debugBackdropColor"));
    colorRow->addWidget(backdropButton_);
    connect(backdropButton_, &QPushButton::clicked, this, &DebugGraphicsDialog::showColorPicker);
    resetBackdrop_ = new QPushButton(Localization::text(QStringLiteral("Сбросить цвет подложки")), this);
    resetBackdrop_->setObjectName(QStringLiteral("debugBackdropReset"));
    colorRow->addWidget(resetBackdrop_);
    connect(resetBackdrop_, &QPushButton::clicked, this, [this] {
        backdrop_ = QColor(255, 0, 255);
        updateBackdropIcon();
        emit backdropChanged(0x00ff00ffu);
    });
    layout->addLayout(colorRow);
    layout->addStretch();
    const std::array<DebugVideoInspector::View, 5> modes{
        DebugVideoInspector::Tiles, DebugVideoInspector::Map, DebugVideoInspector::Sprites,
        DebugVideoInspector::Palette, DebugVideoInspector::Framebuffer
    };
    const std::array<QString, 5> labels{
        Localization::text(QStringLiteral("Тайлы")),
        Localization::text(QStringLiteral("Карта")),
        Localization::text(QStringLiteral("Спрайты")),
        Localization::text(QStringLiteral("Палитра")),
        Localization::text(QStringLiteral("Буфер 32X"))
    };
    for (size_t i = 0; i < modes.size(); ++i) {
        views_[i] = new DebugVideoInspector(modes[i], tabs_);
        tabs_->addTab(views_[i], labels[i]);
        connect(views_[i], &DebugVideoInspector::memoryRequested, this, &DebugGraphicsDialog::memoryRequested);
    }
    registers_ = new DebugStateInspector(KR_INSPECT_VIDEO, tabs_);
    tabs_->insertTab(tabs_->indexOf(views_.back()), registers_,
                     Localization::text(QStringLiteral("Регистры")));
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        for (auto* view : views_)
            if (view != tabs_->currentWidget()) view->setSnapshot({});
        if (registers_ != tabs_->currentWidget()) registers_->setSnapshot({});
        emit inspectionChanged();
    });
    updateBackdropIcon();
    setControls(0, 0, false, false, backdrop_);
}

void DebugGraphicsDialog::setControls(quint32 available, quint32 disabled,
                                      bool hardcore, bool ready, QColor backdrop, bool firecore) {
    available_ = available;
    const int framebuffer = tabs_->indexOf(views_.back());
    tabs_->setTabText(framebuffer, Localization::text(firecore ? QStringLiteral("Растр Firecore")
                                                               : QStringLiteral("Буфер 32X")));
    tabs_->setTabVisible(framebuffer, firecore || (available & (1u << 5)));
    disabled_ = disabled & available;
    backdrop_ = backdrop;
    placeholder_->setVisible(available == 0);
    if ((!ready || hardcore || !available) && colorDialog_) colorDialog_->hide();
    for (unsigned index = 0; index < layers_.size(); ++index) {
        QCheckBox* layer = layers_[index];
        const quint32 bit = quint32(1) << index;
        layer->setVisible((available & bit) != 0);
        layer->setEnabled(ready && !hardcore);
        const QSignalBlocker blocker(layer);
        layer->setChecked((disabled_ & bit) == 0);
    }
    restore_->setEnabled(ready && !hardcore && disabled_ != 0);
    backdropButton_->setEnabled(ready && !hardcore && available != 0);
    resetBackdrop_->setEnabled(ready && !hardcore && available != 0 &&
                               backdrop_ != QColor(255, 0, 255));
    updateBackdropIcon();
}

void DebugGraphicsDialog::updateBackdropIcon() {
    QPixmap swatch(16, 16);
    swatch.fill(backdrop_);
    backdropButton_->setIcon(QIcon(swatch));
    resetBackdrop_->setEnabled(backdrop_ != QColor(255, 0, 255) &&
                               backdropButton_->isEnabled());
}

void DebugGraphicsDialog::showColorPicker() {
    if (!colorDialog_) {
        colorDialog_ = new QColorDialog(backdrop_, this);
        colorDialog_->setObjectName(QStringLiteral("debugBackdropPicker"));
        colorDialog_->setOption(QColorDialog::DontUseNativeDialog);
        colorDialog_->setWindowFlags(debugWindowFlags());
        colorDialog_->setWindowTitle(Localization::text(QStringLiteral("Цвет подложки")));
        colorDialog_->setWindowModality(Qt::NonModal);
        connect(colorDialog_, &QColorDialog::colorSelected, this, [this](const QColor& color) {
            if (!backdropButton_->isEnabled()) return;
            backdrop_ = color;
            updateBackdropIcon();
            emit backdropChanged(quint32(color.rgb()) & 0x00ffffffu);
        });
    }
    colorDialog_->setCurrentColor(backdrop_);
    colorDialog_->show();
    colorDialog_->raise();
    colorDialog_->activateWindow();
}

DebugInspectionRequest DebugGraphicsDialog::request() const {
    DebugInspectionRequest request;
    if (isVisible() && tabs_->currentIndex() > 0) request.parts = KR_INSPECT_VIDEO;
    return request;
}

void DebugGraphicsDialog::setSnapshot(const DebugInspectionSnapshot& snapshot) {
    if (!snapshot.available()) {
        for (auto* view : views_) view->setSnapshot(snapshot);
        registers_->setSnapshot(snapshot);
        return;
    }
    if (!(snapshot.parts & KR_INSPECT_VIDEO) || !isVisible()) return;
    if (tabs_->currentWidget() == registers_) {
        registers_->setSnapshot(snapshot);
    } else {
        for (auto* view : views_)
            if (view == tabs_->currentWidget()) view->setSnapshot(snapshot);
    }
}

void DebugGraphicsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    emit inspectionChanged();
}

void DebugGraphicsDialog::hideEvent(QHideEvent* event) {
    if (colorDialog_) colorDialog_->hide();
    QDialog::hideEvent(event);
    setSnapshot({});
    emit inspectionChanged();
}

DebugAudioDialog::DebugAudioDialog(QWidget* parent)
    : QDialog(parent, debugWindowFlags()) {
    setObjectName(QStringLiteral("debugAudioDialog"));
    setWindowTitle(Localization::text(QStringLiteral("Звуковые каналы")));
    setWindowModality(Qt::NonModal);
    resize(540, 560);
    gains_.fill(100);
    auto* outer = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("debugAudioTabs"));
    outer->addWidget(tabs_);
    auto* controls = new QWidget(tabs_);
    tabs_->addTab(controls, Localization::text(QStringLiteral("Управление")));
    auto* layout = new QVBoxLayout(controls);
    placeholder_ = new QLabel(Localization::text(QStringLiteral("Игра не загружена")), this);
    placeholder_->setObjectName(QStringLiteral("debugAudioEmpty"));
    layout->addWidget(placeholder_);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(content);
    layout->addWidget(scroll);

    std::array<QString, 38> names;
    for (unsigned i = 0; i < 3; ++i)
        names[i] = Localization::text(QStringLiteral("Тон %1")).arg(i + 1);
    names[3] = Localization::text(QStringLiteral("Шум"));
    for (unsigned i = 0; i < 6; ++i) names[i + 4] = QStringLiteral("FM %1").arg(i + 1);
    for (unsigned i = 0; i < 9; ++i)
        names[i + 10] = Localization::text(QStringLiteral("Мелодия %1")).arg(i + 1);
    names[19] = Localization::text(QStringLiteral("Бас-барабан"));
    names[20] = Localization::text(QStringLiteral("Хай-хэт"));
    names[21] = Localization::text(QStringLiteral("Малый барабан"));
    names[22] = Localization::text(QStringLiteral("Том"));
    names[23] = Localization::text(QStringLiteral("Тарелка"));
    for (unsigned i = 0; i < 8; ++i)
        names[i + 24] = Localization::text(QStringLiteral("Канал %1")).arg(i + 1);
    names[32] = names[34] = Localization::text(QStringLiteral("Левый"));
    names[33] = names[35] = Localization::text(QStringLiteral("Правый"));
    names[36] = QStringLiteral("DAC");
    names[37] = QStringLiteral("Paprium");

    const std::array<QString, 8> groupNames{
        QStringLiteral("PSG"), QStringLiteral("YM2612"), QStringLiteral("YM2413"),
        QStringLiteral("Sega CD PCM"), QStringLiteral("CDDA"),
        QStringLiteral("32X PWM"), QStringLiteral("DAC"), QStringLiteral("Paprium")
    };
    const std::array<unsigned, 9> bounds{0, 4, 10, 24, 32, 34, 36, 37, 38};
    for (unsigned groupIndex = 0; groupIndex < groups_.size(); ++groupIndex) {
        auto* group = new QGroupBox(groupNames[groupIndex], content);
        group->setVisible(false);
        groups_[groupIndex] = group;
        auto* groupLayout = new QVBoxLayout(group);
        groupLayout->setSpacing(3);
        contentLayout->addWidget(group);
        for (unsigned channel = bounds[groupIndex]; channel < bounds[groupIndex + 1]; ++channel) {
            auto* row = new QWidget(group);
            row->setVisible(false);
            rows_[channel] = row;
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(4, 0, 4, 0);
            auto* check = new QCheckBox(names[channel], row);
            check->setObjectName(QStringLiteral("debugAudio%1").arg(channel));
            channels_[channel] = check;
            rowLayout->addWidget(check, 1);
            auto* slider = new QSlider(Qt::Horizontal, row);
            slider->setObjectName(QStringLiteral("debugAudioGain%1").arg(channel));
            slider->setRange(0, 100);
            slider->setMinimumWidth(120);
            sliders_[channel] = slider;
            rowLayout->addWidget(slider, 2);
            auto* percent = new QSpinBox(row);
            percent->setObjectName(QStringLiteral("debugAudioPercent%1").arg(channel));
            percent->setRange(0, 100);
            percent->setSuffix(QStringLiteral(" %"));
            percents_[channel] = percent;
            rowLayout->addWidget(percent);
            groupLayout->addWidget(row);
            connect(check, &QCheckBox::toggled, this, [this, channel](bool audible) {
                const quint64 bit = quint64(1) << channel;
                if (audible) disabled_ &= ~bit;
                else disabled_ |= bit;
                restore_->setEnabled(disabled_ != 0);
                emit maskChanged(disabled_);
            });
            connect(slider, &QSlider::valueChanged, percent, &QSpinBox::setValue);
            connect(percent, qOverload<int>(&QSpinBox::valueChanged), slider, &QSlider::setValue);
            connect(slider, &QSlider::valueChanged, this, [this, channel](int value) {
                gains_[channel] = unsigned(value);
                resetGains_->setEnabled(true);
                emit gainChanged(channel, unsigned(value));
            });
        }
    }
    contentLayout->addStretch();
    auto* buttonRow = new QHBoxLayout;
    restore_ = new QPushButton(Localization::text(QStringLiteral("Включить все каналы")), this);
    restore_->setObjectName(QStringLiteral("debugRestoreAudio"));
    buttonRow->addWidget(restore_);
    connect(restore_, &QPushButton::clicked, this, [this] {
        disabled_ = 0;
        for (QCheckBox* channel : channels_) {
            const QSignalBlocker blocker(channel);
            channel->setChecked(true);
        }
        restore_->setEnabled(false);
        emit maskChanged(0);
    });
    resetGains_ = new QPushButton(Localization::text(QStringLiteral("Громкость 100% для всех")), this);
    resetGains_->setObjectName(QStringLiteral("debugResetGains"));
    buttonRow->addWidget(resetGains_);
    connect(resetGains_, &QPushButton::clicked, this, [this] {
        for (unsigned channel = 0; channel < sliders_.size(); ++channel)
            if (available_ & (quint64(1) << channel)) sliders_[channel]->setValue(100);
        resetGains_->setEnabled(false);
    });
    layout->addLayout(buttonRow);
    state_ = new DebugStateInspector(KR_INSPECT_AUDIO, tabs_);
    tabs_->addTab(state_, Localization::text(QStringLiteral("Состояние")));
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        if (tabs_->currentWidget() != state_) state_->setSnapshot({});
        emit inspectionChanged();
    });
    setControls(0, 0, false, gains_);
}

void DebugAudioDialog::setControls(quint64 available, quint64 disabled, bool ready,
                                   const std::array<unsigned, 38>& gains) {
    available_ = available;
    disabled_ = disabled & available;
    gains_ = gains;
    placeholder_->setVisible(available == 0);
    for (unsigned channel = 0; channel < channels_.size(); ++channel) {
        const quint64 bit = quint64(1) << channel;
        rows_[channel]->setVisible((available & bit) != 0);
        rows_[channel]->setEnabled(ready);
        const QSignalBlocker checkBlock(channels_[channel]);
        channels_[channel]->setChecked((disabled_ & bit) == 0);
        const QSignalBlocker sliderBlock(sliders_[channel]);
        const QSignalBlocker percentBlock(percents_[channel]);
        sliders_[channel]->setValue(int(gains[channel]));
        percents_[channel]->setValue(int(gains[channel]));
    }
    const std::array<unsigned, 9> bounds{0, 4, 10, 24, 32, 34, 36, 37, 38};
    for (unsigned group = 0; group < groups_.size(); ++group) {
        bool hasChannel = false;
        for (unsigned channel = bounds[group]; channel < bounds[group + 1]; ++channel)
            hasChannel |= (available & (quint64(1) << channel)) != 0;
        groups_[group]->setVisible(hasChannel);
    }
    restore_->setEnabled(ready && disabled_ != 0);
    bool anyGain = false;
    for (unsigned channel = 0; channel < gains.size(); ++channel)
        anyGain |= (available & (quint64(1) << channel)) && gains[channel] != 100;
    resetGains_->setEnabled(ready && anyGain);
}


DebugInspectionRequest DebugAudioDialog::request() const {
    DebugInspectionRequest request;
    if (isVisible() && tabs_->currentWidget() == state_) request.parts = KR_INSPECT_AUDIO;
    return request;
}

void DebugAudioDialog::setSnapshot(const DebugInspectionSnapshot& snapshot) {
    if (!snapshot.available() || (isVisible() && tabs_->currentWidget() == state_))
        state_->setSnapshot(snapshot);
}

void DebugAudioDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    emit inspectionChanged();
}

void DebugAudioDialog::hideEvent(QHideEvent* event) {
    QDialog::hideEvent(event);
    setSnapshot({});
    emit inspectionChanged();
}
