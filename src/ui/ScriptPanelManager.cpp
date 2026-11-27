#include "ui/ScriptPanelManager.h"

#include <QBoxLayout>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QValidator>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace {

QString literalCaption(QString text) {
    return text.replace(QLatin1Char('&'), QStringLiteral("&&"));
}

class TextLimit final : public QValidator {
public:
    using QValidator::QValidator;

    State validate(QString& text, int&) const override {
        return text.toUtf8().size() <= ScriptUiLimits::MaxTextBytes ? Acceptable : Invalid;
    }
};

class PanelNumber final : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;

    bool editing() const { return hasFocus() && lineEdit()->isModified(); }

    void reconcile(double value, bool discardDraft) {
        if (this->value() != value || (discardDraft && lineEdit()->isModified())) {
            setValue(value);
            lineEdit()->setModified(false);
        }
    }
};

int decimalPlaces(double value) {
    const QString decimal = QLocale::c().toString(value, 'g', QLocale::FloatingPointShortest);
    const qsizetype exponent = decimal.indexOf(QLatin1Char('e'));
    const qsizetype point = decimal.indexOf(QLatin1Char('.'));
    const int digits = point < 0 ? 0 : int((exponent < 0 ? decimal.size() : exponent) - point - 1);
    return digits - (exponent < 0 ? 0 : decimal.mid(exponent + 1).toInt());
}

class PanelGraph final : public QWidget {
public:
    explicit PanelGraph(QWidget* parent) : QWidget(parent) {
        setMinimumSize(140, 72);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }

    void setPoints(const QVector<double>& points) {
        if (points_ == points) return;
        points_ = points;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const QRectF border = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(palette().color(QPalette::Mid));
        painter.setBrush(palette().brush(QPalette::Base));
        painter.drawRect(border);
        if (points_.isEmpty()) return;

        const QRectF plot = border.adjusted(4, 4, -4, -4);
        const auto extrema = std::minmax_element(points_.cbegin(), points_.cend());
        const double minimum = *extrema.first;
        const double maximum = *extrema.second;
        // Halve both operands before subtracting when the unscaled span would overflow.
        const double scale = std::isfinite(maximum - minimum) ? 1.0 : 0.5;
        const double lower = minimum * scale;
        const double span = maximum * scale - lower;
        QPolygonF curve;
        curve.reserve(points_.size());
        for (qsizetype i = 0; i < points_.size(); ++i) {
            const double x = points_.size() == 1 ? 0.5 : double(i) / (points_.size() - 1);
            const double y = span > 0 ? (points_[i] * scale - lower) / span : 0.5;
            curve.append(QPointF(plot.left() + x * plot.width(), plot.bottom() - y * plot.height()));
        }
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        if (curve.size() == 1) painter.drawPoint(curve.first());
        else painter.drawPolyline(curve);
    }

private:
    QVector<double> points_;
};

} // namespace

class ScriptPanelManager::PanelWindow final : public QWidget {
public:
    PanelWindow(ScriptPanelManager& manager, const ScriptUiWindow& window)
        : QWidget(manager.parentWindow_, Qt::Window | Qt::CustomizeWindowHint |
                  Qt::WindowTitleHint | Qt::WindowCloseButtonHint),
          manager_(manager), owner_(window.owner), run_(window.run), handle_(window.handle),
          scroll_(new QScrollArea(this)) {
        setObjectName(QStringLiteral("luaScriptPanel"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setWindowModality(Qt::NonModal);
        scroll_->setFrameShape(QFrame::NoFrame);
        scroll_->setWidgetResizable(true);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(scroll_);
    }

    bool matches(quint64 owner, quint64 run, quint64 handle) const {
        return owner == owner_ && run == run_ && handle == handle_;
    }

    void open() {
        limitSize(parentWidget()->screen());
        const int scrollWidth = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
        resize((scroll_->widget()->sizeHint() + QSize(scrollWidth, 0))
                   .expandedTo(QSize(240, 80)).boundedTo(maximumSize()));
        show();
        if (auto* handle = windowHandle()) {
            connect(handle, &QWindow::screenChanged, this, [this](QScreen* screen) { limitSize(screen); });
        }
    }

    void apply(const ScriptUiWindow& window, bool newEpoch) {
        const QScopedValueRollback<bool> applying(applying_, true);
        if (windowTitle() != window.title) setWindowTitle(window.title);
        const bool reopen = newEpoch && closingSequence_;
        if (newEpoch) closingSequence_ = 0;
        if (closingSequence_) return;

        const bool rebuild = !scroll_->widget() || !sameStructure(window.controls);
        if (rebuild) {
            controls_.clear();
            controls_.reserve(window.controls.size());
            auto* content = new QWidget;
            auto* layout = new QVBoxLayout(content);
            layout->setAlignment(Qt::AlignTop);
            scroll_->setWidget(content);
            for (const auto& control : window.controls) appendControl(control, layout);
        }
        for (qsizetype i = 0; i < window.controls.size(); ++i) {
            auto& control = controls_[i];
            if (newEpoch || control.pendingSequence <= window.acknowledged) control.pendingSequence = 0;
            updateControl(control, window.controls[i], rebuild, newEpoch);
        }
        if (reopen && isHidden()) show();
    }

    void reject(const ScriptUiEvent& event) {
        if (event.kind == ScriptUiEventKind::Close && closingSequence_ == event.sequence) {
            closingSequence_ = 0;
            if (isHidden()) show();
        }
        if (event.kind != ScriptUiEventKind::Change) return;
        for (auto& control : controls_) {
            if (control.model.id != event.control || control.pendingSequence != event.sequence) continue;
            control.pendingSequence = 0;
            updateControl(control, control.model, false, false);
            return;
        }
    }

    void retire() {
        retired_ = true;
        hide();
        deleteLater();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        if (!retired_ && !closingSequence_) {
            closingSequence_ = ++manager_.sequence_;
            emit manager_.eventRequested(makeEvent(ScriptUiEventKind::Close, closingSequence_, {}, {}));
            // A synchronous queue rejection can cancel this close.
            if (!closingSequence_) {
                event->ignore();
                return;
            }
        }
        event->accept();
    }

private:
    struct ControlWidget {
        QWidget* widget = nullptr;
        QWidget* editor = nullptr;
        QLabel* caption = nullptr;
        QBoxLayout* children = nullptr;
        ScriptUiControl model;
        quint64 pendingSequence = 0;
    };

    void limitSize(const QScreen* screen) {
        if (screen) setMaximumSize(screen->availableGeometry().size() * 0.9);
    }

    bool sameStructure(const QVector<ScriptUiControl>& controls) const {
        if (controls.size() != controls_.size()) return false;
        for (qsizetype i = 0; i < controls.size(); ++i) {
            const auto& current = controls_[i].model;
            const auto& incoming = controls[i];
            if (current.id != incoming.id || current.kind != incoming.kind ||
                current.parent != incoming.parent) return false;
        }
        return true;
    }

    void addCaption(ControlWidget& control, QWidget* parent, Qt::Orientation orientation) {
        control.widget = new QWidget(parent);
        auto* layout = new QBoxLayout(orientation == Qt::Horizontal ? QBoxLayout::LeftToRight :
                                      QBoxLayout::TopToBottom, control.widget);
        layout->setContentsMargins(0, 0, 0, 0);
        control.caption = new QLabel;
        control.caption->setTextFormat(Qt::PlainText);
        control.caption->setWordWrap(true);
        control.caption->setBuddy(control.editor);
        layout->addWidget(control.caption);
        layout->addWidget(control.editor, 1);
    }

    void appendControl(const ScriptUiControl& model, QVBoxLayout* root) {
        const qsizetype index = controls_.size();
        auto* layout = model.parent < 0 ? root : controls_[model.parent].children;
        QWidget* parent = layout->parentWidget();
        ControlWidget control;
        switch (model.kind) {
        case ScriptUiKind::Row:
            control.widget = new QWidget(parent);
            control.children = new QHBoxLayout(control.widget);
            control.children->setContentsMargins(0, 0, 0, 0);
            break;
        case ScriptUiKind::Group:
            control.widget = new QGroupBox(parent);
            control.children = new QVBoxLayout(control.widget);
            break;
        case ScriptUiKind::Label: {
            auto* label = new QLabel(parent);
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(true);
            control.widget = label;
            break;
        }
        case ScriptUiKind::Button: {
            auto* button = new QPushButton(parent);
            control.widget = button;
            connect(button, &QPushButton::clicked, this, [this, index] {
                sendControl(ScriptUiEventKind::Click, index, {});
            });
            break;
        }
        case ScriptUiKind::Checkbox: {
            auto* checkbox = new QCheckBox(parent);
            control.widget = checkbox;
            connect(checkbox, &QCheckBox::toggled, this, [this, index](bool checked) {
                sendControl(ScriptUiEventKind::Change, index, checked);
            });
            break;
        }
        case ScriptUiKind::Text: {
            auto* field = new QLineEdit;
            field->setMaxLength(ScriptUiLimits::MaxTextBytes);
            field->setValidator(new TextLimit(field));
            control.editor = field;
            addCaption(control, parent, Qt::Horizontal);
            connect(field, &QLineEdit::editingFinished, this, [this, index, field] {
                if (!field->isModified()) return;
                field->setModified(false);
                sendControl(ScriptUiEventKind::Change, index, field->text());
            });
            break;
        }
        case ScriptUiKind::Number: {
            auto* number = new PanelNumber;
            number->setKeyboardTracking(false);
            control.editor = number;
            addCaption(control, parent, Qt::Horizontal);
            connect(number, &QDoubleSpinBox::valueChanged, this, [this, index](double value) {
                sendControl(ScriptUiEventKind::Change, index, value);
            });
            break;
        }
        case ScriptUiKind::Select: {
            auto* selector = new QComboBox;
            control.editor = selector;
            addCaption(control, parent, Qt::Horizontal);
            connect(selector, &QComboBox::activated, this, [this, index](int selected) {
                sendControl(ScriptUiEventKind::Change, index, selected + 1);
            });
            break;
        }
        case ScriptUiKind::Graph:
            control.editor = new PanelGraph(nullptr);
            addCaption(control, parent, Qt::Vertical);
            break;
        }
        if (!control.editor) control.editor = control.widget;
        control.editor->setObjectName(model.id);
        layout->addWidget(control.widget);
        controls_.append(std::move(control));
    }

    void updateControl(ControlWidget& control, const ScriptUiControl& model, bool first, bool discardDraft) {
        const auto& previous = control.model;
        const QSignalBlocker blockEditor(control.editor);
        if (first || model.enabled != previous.enabled) control.widget->setEnabled(model.enabled);
        if (control.caption && (first || model.text != previous.text)) {
            control.caption->setText(model.text);
            control.caption->setVisible(!model.text.isEmpty());
            control.editor->setAccessibleName(model.text.isEmpty() ? model.id : model.text);
        }
        switch (model.kind) {
        case ScriptUiKind::Row:
            break;
        case ScriptUiKind::Group:
            if (first || model.text != previous.text)
                static_cast<QGroupBox*>(control.widget)->setTitle(literalCaption(model.text));
            break;
        case ScriptUiKind::Label:
            if (first || model.text != previous.text) static_cast<QLabel*>(control.widget)->setText(model.text);
            break;
        case ScriptUiKind::Button:
            if (first || model.text != previous.text)
                static_cast<QPushButton*>(control.widget)->setText(literalCaption(model.text));
            break;
        case ScriptUiKind::Checkbox: {
            auto* checkbox = static_cast<QCheckBox*>(control.widget);
            if (first || model.text != previous.text) checkbox->setText(literalCaption(model.text));
            if (!control.pendingSequence && checkbox->isChecked() != model.value.toBool())
                checkbox->setChecked(model.value.toBool());
            break;
        }
        case ScriptUiKind::Text: {
            auto* field = static_cast<QLineEdit*>(control.editor);
            if (!control.pendingSequence && (discardDraft || !field->hasFocus() || !field->isModified())) {
                if (field->text() != model.value.toString()) field->setText(model.value.toString());
                if (discardDraft) field->setModified(false);
            }
            break;
        }
        case ScriptUiKind::Number: {
            auto* number = static_cast<PanelNumber*>(control.editor);
            if (control.pendingSequence || (!discardDraft && number->editing())) break;
            const double value = model.value.toDouble();
            if (first || number->minimum() != model.minimum || number->maximum() != model.maximum ||
                number->singleStep() != model.step || number->value() != value) {
                const int decimals = std::max({0, decimalPlaces(model.minimum), decimalPlaces(model.maximum),
                                               decimalPlaces(model.step), decimalPlaces(value)});
                if (first || decimals > number->decimals()) number->setDecimals(decimals);
                if (number->minimum() != model.minimum || number->maximum() != model.maximum)
                    number->setRange(model.minimum, model.maximum);
                if (number->singleStep() != model.step) number->setSingleStep(model.step);
            }
            number->reconcile(value, discardDraft);
            break;
        }
        case ScriptUiKind::Select: {
            auto* selector = static_cast<QComboBox*>(control.editor);
            const int current = selector->currentIndex();
            if (first || model.items != previous.items) {
                selector->clear();
                selector->addItems(model.items);
            }
            const int selected = control.pendingSequence ? current : model.value.toInt() - 1;
            if (selector->currentIndex() != selected) selector->setCurrentIndex(selected);
            break;
        }
        case ScriptUiKind::Graph:
            static_cast<PanelGraph*>(control.editor)->setPoints(model.points);
            break;
        }
        if (&previous != &model) control.model = model;
    }

    ScriptUiEvent makeEvent(ScriptUiEventKind kind, quint64 sequence, const QString& control,
                            const QVariant& value) const {
        return {owner_, run_, handle_, sequence, manager_.epoch_, control, kind, value};
    }

    void sendControl(ScriptUiEventKind kind, qsizetype index, const QVariant& value) {
        if (applying_ || retired_ || closingSequence_) return;
        auto& control = controls_[index];
        const quint64 sequence = ++manager_.sequence_;
        if (kind == ScriptUiEventKind::Change) control.pendingSequence = sequence;
        emit manager_.eventRequested(makeEvent(kind, sequence, control.model.id, value));
    }

    ScriptPanelManager& manager_;
    const quint64 owner_, run_, handle_;
    QScrollArea* const scroll_;
    QVector<ControlWidget> controls_;
    quint64 closingSequence_ = 0;
    bool retired_ = false, applying_ = false;
};

ScriptPanelManager::ScriptPanelManager(QWidget* parent)
    : QObject(parent), parentWindow_(parent) {}

ScriptPanelManager::~ScriptPanelManager() {
    for (auto* window : windows_) window->retire();
}

void ScriptPanelManager::applySnapshot(const ScriptUiSnapshot& snapshot) {
    if (receivedSnapshot_ && (snapshot.revision <= revision_ || snapshot.epoch < epoch_)) return;
    const bool newEpoch = receivedSnapshot_ && snapshot.epoch != epoch_;
    receivedSnapshot_ = true;
    revision_ = snapshot.revision;
    epoch_ = snapshot.epoch;

    QList<PanelWindow*> previous;
    previous.swap(windows_);
    for (const auto& state : snapshot.windows) {
        const auto existing = std::find_if(previous.begin(), previous.end(), [&](const PanelWindow* window) {
            return window->matches(state.owner, state.run, state.handle);
        });
        if (existing == previous.end()) {
            auto* window = new PanelWindow(*this, state);
            windows_.append(window);
            window->apply(state, false);
            window->open();
        } else {
            auto* window = *existing;
            previous.erase(existing);
            windows_.append(window);
            window->apply(state, newEpoch);
        }
    }
    for (auto* window : previous) window->retire();
}

void ScriptPanelManager::rejectEvent(const ScriptUiEvent& event) {
    if (event.epoch != epoch_) return;
    for (auto* window : windows_) {
        if (window->matches(event.owner, event.run, event.window)) {
            window->reject(event);
            return;
        }
    }
}

bool ScriptPanelManager::ownsWindow(const QWidget* widget) const {
    for (auto* current = widget; current; current = current->parentWidget()) {
        for (auto* window : windows_) {
            if (current == window) return true;
        }
    }
    return false;
}
