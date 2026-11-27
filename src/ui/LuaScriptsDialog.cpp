#include "LuaScriptsDialog.h"
#include "i18n/Localization.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPaintEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPromise>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSet>
#include <QStandardPaths>
#include <QStyle>
#include <QStringDecoder>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QThreadPool>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

namespace {

constexpr int maxLogRows = 250;
constexpr int maxLogMessageChars = 2048;

QString text(const char* source) {
    return Localization::text(QString::fromUtf8(source));
}

template<typename Result, typename Work, typename Done>
void fileTask(QObject* owner, Work work, Done done) {
    auto promise = std::make_shared<QPromise<Result>>();
    promise->start();
    auto* watcher = new QFutureWatcher<Result>(owner);
    QObject::connect(watcher, &QFutureWatcherBase::finished, owner, [watcher, done = std::move(done)] {
        const Result result = watcher->result();
        watcher->deleteLater();
        done(result);
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, work = std::move(work)] {
        promise->addResult(work());
        promise->finish();
    });
}

struct SourceResult {
    QByteArray source;
    QString error;
};

SourceResult readSource(const QString& path) {
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
        return {{}, text("Не удалось прочитать скрипт.")};
    if (file.size() > ScriptLimits::MaxSourceBytes)
        return {{}, text("Размер скрипта превышает 256 КиБ.")};
    auto source = file.read(ScriptLimits::MaxSourceBytes + 1);
    if (file.error() != QFileDevice::NoError)
        return {{}, text("Не удалось прочитать скрипт.")};
    if (source.size() > ScriptLimits::MaxSourceBytes)
        return {{}, text("Размер скрипта превышает 256 КиБ.")};
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString decoded = decoder(source);
    if (decoder.hasError() || decoded.contains(QChar::Null))
        return {{}, text("Исходник скрипта должен быть текстом в UTF-8.")};
    return {std::move(source), {}};
}

struct ImageResult {
    QHash<QString, QImage> images;
    QString error;
    int line = 0;
};

ImageResult readImages(const QString& sourcePath, const QByteArray& source) {
    constexpr QByteArrayView directive("-- @image");
    QHash<QString, QImage> images;
    qsizetype totalBytes = 0;
    QString directory;
    QByteArrayView remaining(source);
    if (remaining.startsWith("\xef\xbb\xbf")) remaining = remaining.sliced(3);
    int line = 0;
    while (!remaining.isEmpty()) {
        ++line;
        const qsizetype end = remaining.indexOf('\n');
        const auto statement = (end < 0 ? remaining : remaining.first(end)).trimmed();
        remaining = end < 0 ? QByteArrayView{} : remaining.sliced(end + 1);
        if (!statement.startsWith(directive)) continue;
        auto nameBytes = statement.sliced(directive.size());
        if (!nameBytes.isEmpty() && nameBytes.front() != ' ' && nameBytes.front() != '\t') continue;
        nameBytes = nameBytes.trimmed();
        const auto name = QString::fromUtf8(nameBytes);
        static const QRegularExpression invalidName(QStringLiteral(R"([\x00-\x1f<>:"/\\|?*])"));
        if (nameBytes.size() > ScriptLimits::MaxImageNameBytes || !name.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)
            || invalidName.match(name).hasMatch()) {
            return {{}, text("Укажите имя PNG рядом со скриптом в строке -- @image имя.png."), line};
        }
        if (images.contains(name)) continue;
        if (images.size() >= ScriptLimits::MaxImages)
            return {{}, text("Скрипт может загрузить не более 16 изображений."), line};

        if (directory.isEmpty())
            directory = QFileInfo(sourcePath).absoluteDir().canonicalPath();
        const QFileInfo info(QFileInfo(sourcePath).absoluteDir(), name);
        const QString canonical = info.canonicalFilePath();
        const QString imageError = text("Не удалось загрузить PNG рядом со скриптом: %1").arg(name);
        if (directory.isEmpty() || canonical.isEmpty() || !info.isFile()
            || QFileInfo(canonical).absolutePath() != directory)
            return {{}, imageError, line};

        QFile file(canonical);
        if (!file.open(QIODevice::ReadOnly) || file.size() > ScriptLimits::MaxImageBytes)
            return {{}, imageError, line};
        auto encoded = file.read(ScriptLimits::MaxImageBytes + 1);
        if (encoded.size() > ScriptLimits::MaxImageBytes || file.error() != QFileDevice::NoError)
            return {{}, imageError, line};
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "png");
        const QSize size = reader.size();
        if (size.isEmpty()) return {{}, imageError, line};
        if (size.width() > ScriptLimits::MaxImageSide
            || size.height() > ScriptLimits::MaxImageSide
            || totalBytes + qsizetype(size.width()) * size.height() * 4 > ScriptLimits::MaxImageBytes)
            return {{}, text("Размер изображений превышает предел (1024 × 1024, всего 8 МиБ)."), line};

        QImage image = reader.read();
        if (image.isNull() || image.size() != size)
            return {{}, imageError, line};
        image.convertTo(QImage::Format_ARGB32_Premultiplied);
        totalBytes += image.sizeInBytes();
        images.insert(name, std::move(image));
    }
    return {std::move(images), {}, 0};
}

struct PrepareResult {
    QByteArray source;
    ImageResult assets;
    QString sourceError;
};

QString saveSource(const QString& path, const QByteArray& source, const QByteArray& expected) {
    const auto current = readSource(path);
    if (!current.error.isEmpty() || current.source != expected)
        return text("Исходник изменился на диске. Перенесите правки перед повторным открытием.");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(source) != source.size() || !file.commit())
        return text("Не удалось сохранить исходник скрипта.");
    return {};
}

bool launchEditor(const QString& path) {
    for (const char* variable : {"VISUAL", "EDITOR"}) {
        auto args = QProcess::splitCommand(qEnvironmentVariable(variable));
        if (args.isEmpty()) continue;
        const QString executable = args.takeFirst();
        args.append(path);
        if (QProcess::startDetached(executable, args)) return true;
    }
#ifdef Q_OS_WIN
    return QProcess::startDetached(QStringLiteral("notepad.exe"), {path});
#elif defined(Q_OS_MACOS)
    return QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-e"), path});
#else
    for (const char* editor : {"kate", "gedit", "gnome-text-editor", "mousepad",
                               "kwrite", "xed", "pluma", "geany", "code"}) {
        const auto executable = QStandardPaths::findExecutable(QString::fromLatin1(editor));
        if (!executable.isEmpty() && QProcess::startDetached(executable, {path})) return true;
    }
    return false;
#endif
}

const char* const apiSignatures[] = {
    "emu.apiVersion",
    "emu.screen() → {width, height}",
    "emu.instances() → 1 | 2",
    "emu.on('beforeframe' | 'frame' | 'reset' | 'loadstate' | 'rewind' | 'draw', function(event) … end)",
    "emu.framecount()",
    "emu.fps()",
    "emu.lagged()",
    "emu.pause([paused])",
    "emu.reset([hard])",
    "emu.frameadvance()",
    "input.devices()",
    "input.get(player)",
    "input.set(player, controls)",
    "input.clear([player])",
    "savestate.create()",
    "savestate.save(handle)",
    "savestate.load(handle)",
    "savestate.free(handle)",
    "memory.domains([instance])",
    "memory.cpus([instance])",
    "memory.read(domain, offset[, width[, endian[, signed[, instance]]]])",
    "memory.range(domain, offset, length[, instance])",
    "memory.peek(cpu, address[, width[, endian[, signed[, instance]]]])",
    "memory.registers(cpu[, instance])",
    "draw.text(x, y, text[, color[, space]])",
    "draw.line(x, y, x2, y2[, color[, space]])",
    "draw.rect(x, y, width, height[, color[, fill[, space]]])",
    "draw.circle(x, y, radius[, color[, fill[, space]]])",
    "draw.image(name, x, y[, space])",
    "print(…)"
};

struct LongBracket {
    int equals = 0;
    int length = 0;
};

LongBracket longBracket(const QString& source, int from) {
    if (from >= source.size() || source[from] != QLatin1Char('[')) return {};
    int end = from + 1;
    while (end < source.size() && source[end] == QLatin1Char('=')) ++end;
    if (end >= source.size() || source[end] != QLatin1Char('[')) return {};
    return {end - from - 1, end - from + 1};
}

class LuaHighlighter final : public QSyntaxHighlighter {
public:
    LuaHighlighter(QTextDocument* document, const QPalette& palette)
        : QSyntaxHighlighter(document) {
        const bool dark = palette.color(QPalette::Base).lightness() < 128;
        keyword_.setForeground(dark ? QColor(198, 159, 241) : QColor(114, 45, 140));
        keyword_.setFontWeight(QFont::Bold);
        comment_.setForeground(dark ? QColor(157, 169, 158) : QColor(83, 110, 83));
        string_.setForeground(dark ? QColor(161, 212, 150) : QColor(39, 113, 69));
        number_.setForeground(dark ? QColor(245, 191, 128) : QColor(149, 76, 14));
        api_.setForeground(dark ? QColor(124, 191, 239) : QColor(20, 96, 157));
    }

protected:
    void highlightBlock(const QString& source) override {
        static const QSet<QString> keywords = {
            QStringLiteral("and"), QStringLiteral("break"), QStringLiteral("do"),
            QStringLiteral("else"), QStringLiteral("elseif"), QStringLiteral("end"),
            QStringLiteral("false"), QStringLiteral("for"), QStringLiteral("function"),
            QStringLiteral("goto"), QStringLiteral("if"), QStringLiteral("in"),
            QStringLiteral("local"), QStringLiteral("nil"), QStringLiteral("not"),
            QStringLiteral("or"), QStringLiteral("repeat"), QStringLiteral("return"),
            QStringLiteral("then"), QStringLiteral("true"), QStringLiteral("until"),
            QStringLiteral("while")
        };
        static const QSet<QString> libraries = {
            QStringLiteral("emu"), QStringLiteral("memory"), QStringLiteral("draw"),
            QStringLiteral("input"), QStringLiteral("savestate"), QStringLiteral("print")
        };
        static const QRegularExpression numeric(
            QStringLiteral(R"((?:0[xX][0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?(?:[pP][+-]?\d+)?|\d+(?:\.\d*)?(?:[eE][+-]?\d+)?))"));
        const int size = int(source.size());
        int index = 0;
        setCurrentBlockState(0);
        if (previousBlockState() > 0) {
            index = highlightLong(source, 0, 0, previousBlockState());
        }
        if (index == 0 && currentBlock().blockNumber() == 0 &&
            (source.startsWith(QLatin1Char('#')) || source.startsWith(QChar::ByteOrderMark + QStringLiteral("#")))) {
            setFormat(0, size, comment_);
            return;
        }
        while (index < size) {
            const int start = index;
            const QChar c = source[index++];
            if (c == QLatin1Char('-') && index < size && source[index] == QLatin1Char('-')) {
                ++index;
                const auto open = longBracket(source, index);
                if (!open.length) {
                    setFormat(start, size - start, comment_);
                    break;
                }
                index = highlightLong(source, start, index + open.length, (open.equals + 1) * 2 + 1);
            } else if (c == QLatin1Char('[')) {
                const auto open = longBracket(source, start);
                if (open.length)
                    index = highlightLong(source, start, start + open.length, (open.equals + 1) * 2);
            } else if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
                while (index < size) {
                    const QChar quoted = source[index++];
                    if (quoted == c) break;
                    if (quoted == QLatin1Char('\\') && index < size) ++index;
                }
                setFormat(start, index - start, string_);
            } else if (c.isLetter() || c == QLatin1Char('_')) {
                while (index < size && (source[index].isLetterOrNumber() || source[index] == QLatin1Char('_')))
                    ++index;
                if (index - start <= 9) {
                    const auto word = source.mid(start, index - start);
                    if (keywords.contains(word)) setFormat(start, index - start, keyword_);
                    else if (libraries.contains(word)) setFormat(start, index - start, api_);
                }
            } else if (c.isDigit()) {
                const auto match = numeric.match(source, start, QRegularExpression::NormalMatch,
                                                 QRegularExpression::AnchorAtOffsetMatchOption |
                                                 QRegularExpression::DontCheckSubjectStringMatchOption);
                if (match.hasMatch()) {
                    index = int(match.capturedEnd());
                    setFormat(start, index - start, number_);
                }
            }
        }
    }

private:
    int highlightLong(const QString& source, int start, int from, int state) {
        const QString close = QLatin1Char(']') + QString(state / 2 - 1, QLatin1Char('=')) + QLatin1Char(']');
        const int match = int(source.indexOf(close, from));
        const int end = match < 0 ? int(source.size()) : match + int(close.size());
        setFormat(start, end - start, state % 2 ? comment_ : string_);
        if (match < 0) setCurrentBlockState(state);
        return end;
    }

    QTextCharFormat keyword_, comment_, string_, number_, api_;
};

class LineNumbers final : public QWidget {
public:
    using Paint = std::function<void(QPainter&, const QRect&)>;
    LineNumbers(QWidget* parent, Paint paint) : QWidget(parent), paint_(std::move(paint)) {}
protected:
    void paintEvent(QPaintEvent* event) override {
        QPainter painter(this);
        paint_(painter, event->rect());
    }
private:
    Paint paint_;
};

QPushButton* button(const char* title, const char* name, QWidget* parent) {
    auto* result = new QPushButton(text(title), parent);
    result->setObjectName(QString::fromLatin1(name));
    result->setAutoDefault(false);
    return result;
}

constexpr int idRole = Qt::UserRole;
constexpr int lineRole = Qt::UserRole + 1;

} // namespace

class LuaSourceEditor final : public QPlainTextEdit {
public:
    explicit LuaSourceEditor(QWidget* parent) : QPlainTextEdit(parent) {
        setObjectName(QStringLiteral("luaSource"));
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
        numbers_ = new LineNumbers(this, [this](QPainter& painter, const QRect& rect) {
            paintLineNumbers(painter, rect);
        });
        numbers_->setAttribute(Qt::WA_TransparentForMouseEvents);
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { placeLineNumbers(); });
        connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
            if (dy) numbers_->scroll(0, dy);
            else numbers_->update(0, rect.y(), numbers_->width(), rect.height());
        });
        new LuaHighlighter(document(), palette());
        placeLineNumbers();
    }

    void goToLine(int line) {
        const auto block = document()->findBlockByNumber(line - 1);
        if (!block.isValid()) return;
        setTextCursor(QTextCursor(block));
        centerCursor();
        setFocus();
        QTextEdit::ExtraSelection selected;
        selected.cursor = textCursor();
        selected.format.setProperty(QTextFormat::FullWidthSelection, true);
        QColor color = palette().highlight().color();
        color.setAlpha(64);
        selected.format.setBackground(color);
        setExtraSelections({selected});
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QPlainTextEdit::resizeEvent(event);
        placeLineNumbers();
    }

    void keyPressEvent(QKeyEvent* event) override {
        constexpr auto shortcuts = Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
        if (!(event->modifiers() & shortcuts) && !event->text().isEmpty()
            && !fits(event->text().size(), selectedChars())) return;
        QPlainTextEdit::keyPressEvent(event);
    }

    void inputMethodEvent(QInputMethodEvent* event) override {
        if (!fits(event->commitString().size(), selectedChars() + event->replacementLength())) return;
        QPlainTextEdit::inputMethodEvent(event);
    }

    void insertFromMimeData(const QMimeData* data) override {
        if (!data->hasText()) return;
        const auto paste = data->text();
        if (fits(paste.size(), selectedChars())) insertPlainText(paste);
    }

private:
    int selectedChars() const {
        const auto cursor = textCursor();
        return cursor.selectionEnd() - cursor.selectionStart();
    }

    bool fits(qsizetype length, int replaced) const {
        return document()->characterCount() - 1 - replaced + length <= ScriptLimits::MaxSourceBytes;
    }

    void placeLineNumbers() {
        const int digits = QString::number(blockCount()).size();
        const int width = 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
        setViewportMargins(width, 0, 0, 0);
        const auto rect = contentsRect();
        numbers_->setGeometry(rect.left(), rect.top(), width, rect.height());
    }

    void paintLineNumbers(QPainter& painter, const QRect& rect) {
        painter.fillRect(rect, palette().alternateBase());
        painter.setPen(palette().color(QPalette::PlaceholderText));
        QTextBlock block = firstVisibleBlock();
        while (block.isValid()) {
            const auto bounds = blockBoundingGeometry(block).translated(contentOffset());
            if (bounds.top() > rect.bottom()) break;
            if (block.isVisible() && bounds.bottom() >= rect.top())
                painter.drawText(0, qRound(bounds.top()), numbers_->width() - 6, fontMetrics().height(),
                                 Qt::AlignRight | Qt::AlignVCenter, QString::number(block.blockNumber() + 1));
            block = block.next();
        }
    }

    LineNumbers* numbers_ = nullptr;
};

LuaScriptsDialog::LuaScriptsDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint) {
    setObjectName(QStringLiteral("luaScriptsDialog"));
    setWindowTitle(text("Lua-скрипты"));
    setWindowModality(Qt::NonModal);
    resize(690, 430);
    auto* layout = new QVBoxLayout(this);

    auto* commands = new QHBoxLayout;
    open_ = button("Открыть скрипт…", "luaOpen", this);
    run_ = button("Запустить скрипт", "luaRun", this);
    stop_ = button("Остановить", "luaStop", this);
    restart_ = button("Перезапустить скрипт", "luaRestart", this);
    remove_ = button("Убрать", "luaRemove", this);
    commands->addWidget(open_);
    commands->addWidget(run_);
    commands->addWidget(stop_);
    commands->addWidget(restart_);
    commands->addStretch();
    commands->addWidget(remove_);
    layout->addLayout(commands);

    scripts_ = new QTreeWidget(this);
    scripts_->setObjectName(QStringLiteral("luaScripts"));
    scripts_->setHeaderLabels({text("Скрипт"), text("Состояние")});
    scripts_->setRootIsDecorated(false);
    scripts_->setItemsExpandable(false);
    scripts_->setUniformRowHeights(true);
    scripts_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    scripts_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    scripts_->setMinimumHeight(72);
    scripts_->setMaximumHeight(152);
    layout->addWidget(scripts_);
    availability_ = new QLabel(text("Запуск Lua сейчас недоступен."), this);
    layout->addWidget(availability_);

    auto* logHeader = new QHBoxLayout;
    auto* logLabel = new QLabel(text("Журнал скриптов"), this);
    logHeader->addWidget(logLabel);
    logHeader->addStretch();
    auto* clear = button("Очистить журнал", "luaClearLog", this);
    logHeader->addWidget(clear);
    layout->addLayout(logHeader);
    log_ = new QListWidget(this);
    log_->setObjectName(QStringLiteral("luaLog"));
    log_->setUniformItemSizes(true);
    log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log_->setMinimumHeight(80);
    logLabel->setBuddy(log_);
    layout->addWidget(log_, 1);

    editorToggle_ = new QToolButton(this);
    editorToggle_->setObjectName(QStringLiteral("luaEditorToggle"));
    editorToggle_->setText(text("Редактор Lua"));
    editorToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    editorToggle_->setArrowType(Qt::RightArrow);
    editorToggle_->setCheckable(true);
    layout->addWidget(editorToggle_, 0, Qt::AlignLeft);

    editorPanel_ = new QWidget(this);
    auto* editorLayout = new QVBoxLayout(editorPanel_);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editor_ = new LuaSourceEditor(editorPanel_);
    editor_->setMinimumHeight(150);
    editorLayout->addWidget(editor_, 1);
    auto* editorCommands = new QHBoxLayout;
    save_ = button("Сохранить скрипт", "luaSave", editorPanel_);
    external_ = button("Внешний редактор", "luaExternalEditor", editorPanel_);
    auto* api = new QToolButton(editorPanel_);
    api->setObjectName(QStringLiteral("luaApiHints"));
    api->setText(text("Подсказки API"));
    api->setPopupMode(QToolButton::InstantPopup);
    auto* apiMenu = new QMenu(api);
    auto* apiHint = new QLabel(editorPanel_);
    apiHint->setTextFormat(Qt::PlainText);
    apiHint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    apiHint->setWordWrap(true);
    apiHint->hide();
    for (const char* signature : apiSignatures) {
        const auto usage = QString::fromUtf8(signature);
        const auto name = usage.section(QLatin1Char('('), 0, 0);
        auto* action = apiMenu->addAction(name);
        connect(action, &QAction::triggered, this, [apiHint, usage] {
            apiHint->setText(usage);
            apiHint->show();
        });
    }
    api->setMenu(apiMenu);
    editorCommands->addWidget(save_);
    editorCommands->addWidget(external_);
    editorCommands->addStretch();
    editorCommands->addWidget(api);
    editorLayout->addLayout(editorCommands);
    editorLayout->addWidget(apiHint);
    layout->addWidget(editorPanel_, 2);
    editorPanel_->hide();

    connect(open_, &QPushButton::clicked, this, &LuaScriptsDialog::openScript);
    connect(run_, &QPushButton::clicked, this, &LuaScriptsDialog::startScript);
    connect(stop_, &QPushButton::clicked, this, &LuaScriptsDialog::stopScript);
    connect(restart_, &QPushButton::clicked, this, &LuaScriptsDialog::startScript);
    connect(remove_, &QPushButton::clicked, this, &LuaScriptsDialog::removeScript);
    connect(save_, &QPushButton::clicked, this, [this] { saveScript(); });
    connect(external_, &QPushButton::clicked, this, [this] { saveScript(true); });
    connect(clear, &QPushButton::clicked, log_, &QListWidget::clear);
    connect(editorToggle_, &QToolButton::toggled, this, &LuaScriptsDialog::setEditorExpanded);
    connect(scripts_, &QTreeWidget::currentItemChanged, this, &LuaScriptsDialog::selectScript);
    connect(log_, &QListWidget::itemActivated, this, &LuaScriptsDialog::jumpToError);
    connect(editor_->document(), &QTextDocument::modificationChanged, this, [this](bool dirty) {
        if (settingSource_) return;
        if (auto* script = entry(editingId_)) {
            script->dirty = dirty;
            setRowStatus(*script);
            updateControls();
        }
    });
    updateControls();
}

LuaScriptsDialog::Entry* LuaScriptsDialog::entry(quint64 id) {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [id](const Entry& script) {
        return script.id == id;
    });
    return found == entries_.end() ? nullptr : &*found;
}

LuaScriptsDialog::Entry* LuaScriptsDialog::selected() {
    auto* item = scripts_->currentItem();
    return item ? entry(item->data(0, idRole).toULongLong()) : nullptr;
}

void LuaScriptsDialog::selectScript(QTreeWidgetItem* item) {
    storeEditor();
    showSource(item ? entry(item->data(0, idRole).toULongLong()) : nullptr);
    updateControls();
}

void LuaScriptsDialog::storeEditor() {
    if (auto* script = entry(editingId_); script && script->dirty) {
        auto source = editor_->toPlainText().toUtf8();
        if (script->diskSource.contains("\r\n")) source.replace("\n", "\r\n");
        if (script->diskSource.startsWith("\xef\xbb\xbf") && !source.startsWith("\xef\xbb\xbf"))
            source.prepend("\xef\xbb\xbf");
        script->source = std::move(source);
    }
}

void LuaScriptsDialog::showSource(const Entry* script) {
    editingId_ = script ? script->id : 0;
    settingSource_ = true;
    editor_->setExtraSelections({});
    editor_->setPlainText(script ? QString::fromUtf8(script->source) : QString{});
    editor_->document()->setModified(script && script->dirty);
    settingSource_ = false;
}

void LuaScriptsDialog::openScript() {
    if (choosingFile_ || entries_.size() >= ScriptLimits::MaxScripts) return;
    auto* dialog = new QFileDialog(this, text("Открыть скрипт…"), lastDirectory_,
                                  text("Скрипты Lua (*.lua);;Все файлы (*)"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setSupportedSchemes({QStringLiteral("file")});
    choosingFile_ = true;
    updateControls();
    connect(dialog, &QFileDialog::fileSelected, this, &LuaScriptsDialog::addScript);
    connect(dialog, &QFileDialog::finished, this, [this] {
        choosingFile_ = false;
        updateControls();
    });
    dialog->show();
}

void LuaScriptsDialog::addScript(const QString& path) {
    if (path.isEmpty() || entries_.size() >= ScriptLimits::MaxScripts) return;
    const auto absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    lastDirectory_ = QFileInfo(absolute).absolutePath();
#ifdef Q_OS_WIN
    constexpr auto pathCase = Qt::CaseInsensitive;
#else
    constexpr auto pathCase = Qt::CaseSensitive;
#endif
    for (const auto& script : entries_) {
        if (script.path.compare(absolute, pathCase) == 0) {
            scripts_->setCurrentItem(script.item);
            return;
        }
    }

    const quint64 id = nextId_++;
    auto* item = new QTreeWidgetItem(scripts_);
    item->setData(0, idRole, id);
    item->setToolTip(0, absolute);
    entries_.append({id, absolute, item, {}, {}});
    setRowStatus(entries_.last());
    scripts_->setCurrentItem(item);
    fileTask<SourceResult>(this, [absolute] { return readSource(absolute); },
        [this, id](const SourceResult& result) {
            auto* script = entry(id);
            if (!script) return;
            script->activity = Activity::Idle;
            if (!result.error.isEmpty()) {
                script->status = ScriptStatus::Error;
                addLog({id, script->path, ScriptStatus::Error, result.error});
            } else {
                script->source = result.source;
                script->diskSource = result.source;
                script->loaded = true;
                if (editingId_ == id) showSource(script);
            }
            setRowStatus(*script);
            updateControls();
        });
}

void LuaScriptsDialog::startScript() {
    auto* script = selected();
    if (!script || !executionAvailable_ || script->activity != Activity::Idle) return;
    storeEditor();
    const bool reload = !script->dirty;
    const QByteArray source = script->source;
    const QString path = script->path;
    const quint64 id = script->id;
    const quint64 generation = startGeneration_;
    const quint64 machineRevision = machineRevision_ ? machineRevision_() : 0;
    script->activity = Activity::Preparing;
    setRowStatus(*script);
    updateControls();
    fileTask<PrepareResult>(this,
        [path, source, reload] {
            auto input = reload ? readSource(path) : SourceResult{source, {}};
            if (!input.error.isEmpty()) return PrepareResult{{}, {}, input.error};
            if (input.source.size() > ScriptLimits::MaxSourceBytes)
                return PrepareResult{{}, {}, text("Размер скрипта превышает 256 КиБ.")};
            auto assets = readImages(path, input.source);
            return PrepareResult{std::move(input.source), std::move(assets), {}};
        },
        [this, id, generation, machineRevision, reload](const PrepareResult& result) {
            auto* script = entry(id);
            if (!script) return;
            script->activity = Activity::Idle;
            if (generation == startGeneration_ && executionAvailable_) {
                if (!result.sourceError.isEmpty()) {
                    if (script->status != ScriptStatus::Running) script->status = ScriptStatus::Error;
                    addLog({id, script->path, ScriptStatus::Error, result.sourceError});
                } else {
                    const bool changed = result.source != script->source;
                    script->source = result.source;
                    if (reload) script->diskSource = result.source;
                    script->loaded = true;
                    if (changed && editingId_ == id) showSource(script);
                    if (!result.assets.error.isEmpty()) {
                        if (script->status != ScriptStatus::Running) script->status = ScriptStatus::Error;
                        addLog({id, script->path, ScriptStatus::Error, result.assets.error, result.assets.line});
                    } else {
                        script->activity = Activity::Starting;
                    }
                }
            } else {
                addLog({id, script->path, ScriptStatus::Error,
                        text("Запуск скрипта отменён.")});
            }
            setRowStatus(*script);
            updateControls();
            if (script->activity == Activity::Starting)
                emit commandRequested({ScriptAction::Run, id, script->path, script->source,
                                       result.assets.images, machineRevision});
        });
}

void LuaScriptsDialog::stopScript() {
    auto* script = selected();
    if (!script) return;
    const bool starting = script->activity == Activity::Starting;
    const bool running = script->activity == Activity::Idle && script->status == ScriptStatus::Running;
    if (!starting && !running) return;
    script->stoppingPendingStart = starting;
    script->activity = Activity::Stopping;
    setRowStatus(*script);
    updateControls();
    emit commandRequested({ScriptAction::Stop, script->id, script->path, {}, {}});
}

void LuaScriptsDialog::removeScript() {
    const auto* script = selected();
    if (!script || script->activity != Activity::Idle || script->status == ScriptStatus::Running) return;
    const quint64 id = script->id;
    auto* item = script->item;
    if (script->dirty && QMessageBox::question(this, windowTitle(),
            text("Убрать скрипт %1 и потерять несохранённые правки?").arg(item->text(0)),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Discard)
        return;
    editingId_ = 0;
    delete item;
    entries_.removeIf([id](const Entry& script) { return script.id == id; });
    selectScript(scripts_->currentItem());
}

void LuaScriptsDialog::saveScript(bool openEditor) {
    auto* script = selected();
    if (!script || !script->loaded || script->activity != Activity::Idle) return;
    const auto path = script->path;
    const quint64 id = script->id;
    const auto launch = [this, id, path] {
        if (!launchEditor(path))
            addLog({id, path, ScriptStatus::Error, text("Не удалось открыть внешний редактор.")});
    };
    if (!script->dirty) {
        if (openEditor) launch();
        return;
    }
    if (openEditor && QMessageBox::question(this, windowTitle(),
            text("Сохранить правки скрипта перед открытием во внешнем редакторе?"),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Save) != QMessageBox::Save)
        return;
    storeEditor();
    const auto source = script->source;
    if (source.size() > ScriptLimits::MaxSourceBytes) {
        addLog({id, path, ScriptStatus::Error, text("Размер скрипта превышает 256 КиБ.")});
        return;
    }
    script->activity = Activity::Saving;
    setRowStatus(*script);
    updateControls();
    const auto expected = script->diskSource;
    fileTask<QString>(this, [path, source, expected] { return saveSource(path, source, expected); },
        [this, id, launch, openEditor](const QString& error) {
            auto* script = entry(id);
            if (!script) return;
            script->activity = Activity::Idle;
            if (!error.isEmpty()) {
                addLog({id, script->path, ScriptStatus::Error, error});
            } else {
                script->dirty = false;
                script->diskSource = script->source;
                if (editingId_ == id) editor_->document()->setModified(false);
                if (openEditor) launch();
            }
            setRowStatus(*script);
            updateControls();
        });
}

void LuaScriptsDialog::setEditorExpanded(bool expanded) {
    editorToggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    editorPanel_->setVisible(expanded);
}

void LuaScriptsDialog::jumpToError(QListWidgetItem* item) {
    const int line = item->data(lineRole).toInt();
    if (auto* script = entry(item->data(idRole).toULongLong()); line > 0 && script && script->loaded) {
        scripts_->setCurrentItem(script->item);
        editorToggle_->setChecked(true);
        editor_->goToLine(line);
    }
}

void LuaScriptsDialog::applyUpdates(const QList<ScriptUpdate>& updates) {
    if (updates.isEmpty()) return;
    bool statusChanged = false;
    for (const auto& update : updates) {
        if (update.status != ScriptStatus::Log) {
            if (auto* script = entry(update.id)) {
                if (script->activity == Activity::Starting ||
                    (script->activity == Activity::Stopping && update.status == ScriptStatus::Stopped)) {
                    script->activity = Activity::Idle;
                    script->stoppingPendingStart = false;
                }
                script->status = update.status;
                setRowStatus(*script);
                statusChanged = true;
            }
        }
        if (!update.text.isEmpty()) addLog(update);
    }
    if (statusChanged) updateControls();
}

void LuaScriptsDialog::rejectCommand(const ScriptCommand& command, const QString& reason) {
    if (auto* script = entry(command.id)) {
        if (script->activity == Activity::Stopping && script->stoppingPendingStart)
            script->activity = Activity::Starting;
        else if (script->activity == Activity::Starting || script->activity == Activity::Stopping)
            script->activity = Activity::Idle;
        script->stoppingPendingStart = false;
        setRowStatus(*script);
    }
    addLog({command.id, command.path, ScriptStatus::Error, reason});
    updateControls();
}

void LuaScriptsDialog::addLog(const ScriptUpdate& update) {
    const auto content = (update.status == ScriptStatus::Log ? update.text : Localization::text(update.text))
        .left(maxLogMessageChars);
    const auto name = QFileInfo(update.path).fileName();
    const auto location = update.line > 0
        ? QStringLiteral("%1:%2").arg(name).arg(update.line) : name;
    const auto message = location.isEmpty() ? content : QStringLiteral("%1 · %2").arg(location, content);
    const auto* scrollbar = log_->verticalScrollBar();
    const bool atBottom = scrollbar->value() == scrollbar->maximum();
    while (log_->count() >= maxLogRows)
        delete log_->takeItem(0);
    auto* item = new QListWidgetItem(message.simplified(), log_);
    item->setToolTip(QStringLiteral("<qt>%1</qt>")
        .arg(message.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
    item->setData(idRole, update.id);
    item->setData(lineRole, update.line);
    if (update.status == ScriptStatus::Error)
        item->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
    if (atBottom) log_->scrollToBottom();
}

void LuaScriptsDialog::setExecutionAvailable(bool available) {
    if (executionAvailable_ == available) return;
    executionAvailable_ = available;
    if (!available) discardPendingStarts();
    updateControls();
}

void LuaScriptsDialog::setMachineRevisionProvider(std::function<quint64()> provider) {
    machineRevision_ = std::move(provider);
}

void LuaScriptsDialog::discardPendingStarts() {
    ++startGeneration_;
}

bool LuaScriptsDialog::confirmDiscardEdits() {
    QStringList unsaved;
    for (const auto& script : entries_) {
        if (script.dirty) unsaved.append(QFileInfo(script.path).fileName());
    }
    if (unsaved.isEmpty()) return true;
    QMessageBox question(QMessageBox::Question, windowTitle(),
                         text("Закрыть эмулятор и потерять несохранённые правки Lua?"),
                         QMessageBox::Discard | QMessageBox::Cancel, parentWidget());
    question.setTextFormat(Qt::PlainText);
    question.setInformativeText(unsaved.join(QLatin1Char('\n')));
    question.setDefaultButton(QMessageBox::Cancel);
    return question.exec() == QMessageBox::Discard;
}

void LuaScriptsDialog::setRowStatus(Entry& script) {
    auto name = QFileInfo(script.path).fileName();
    if (script.dirty) name += QStringLiteral(" *");
    script.item->setText(0, name);

    QString state;
    switch (script.activity) {
    case Activity::Opening: state = text("Чтение скрипта…"); break;
    case Activity::Preparing: state = text("Подготовка скрипта…"); break;
    case Activity::Saving: state = text("Сохранение скрипта…"); break;
    case Activity::Starting: state = text("Запуск скрипта…"); break;
    case Activity::Stopping: state = text("Остановка скрипта…"); break;
    case Activity::Idle:
        switch (script.status) {
        case ScriptStatus::Running: state = text("Работает"); break;
        case ScriptStatus::Stopped: state = text("Остановлен"); break;
        case ScriptStatus::Error: state = text("Ошибка"); break;
        case ScriptStatus::Log: break;
        }
        break;
    }
    script.item->setText(1, state);
}

void LuaScriptsDialog::updateControls() {
    const auto* script = selected();
    const bool idle = script && script->activity == Activity::Idle;
    const bool running = script && script->status == ScriptStatus::Running;
    const bool canStart = idle && executionAvailable_;
    const bool editable = idle && script->loaded;
    open_->setEnabled(entries_.size() < ScriptLimits::MaxScripts && !choosingFile_);
    run_->setEnabled(canStart && !running);
    stop_->setEnabled((script && script->activity == Activity::Starting) || (idle && running));
    restart_->setEnabled(canStart && running);
    remove_->setEnabled(idle && !running);
    editor_->setReadOnly(!editable);
    save_->setEnabled(editable && script->dirty);
    external_->setEnabled(editable);
    availability_->setVisible(!executionAvailable_);
}
