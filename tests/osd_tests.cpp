#include "ui/GameView.h"
#include "core/VideoFrameGeometry.h"

#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QTemporaryDir>
#include <QTimer>

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}

void prepare(GameView& view, const QSize& size = QSize(920, 620)) {
    view.resize(size);
    QImage frame(8, 8, QImage::Format_RGB32);
    frame.fill(QColor(37, 58, 81));
    view.setFrame(frame, 4.0 / 3.0);
    view.setAspectMode(GameView::AspectMode::Stretch);
}

QImage render(GameView& view) {
    QImage image(view.size(), QImage::Format_RGB32);
    image.fill(Qt::magenta);
    view.render(&image);
    return image;
}

QRect changedBounds(const QImage& baseline, const QImage& actual) {
    require(baseline.size() == actual.size(), "comparison image sizes differ");
    QRect bounds;
    for (int y = 0; y < actual.height(); ++y) {
        const auto* before = reinterpret_cast<const QRgb*>(baseline.constScanLine(y));
        const auto* after = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
        for (int x = 0; x < actual.width(); ++x)
            if (before[x] != after[x]) bounds |= QRect(x, y, 1, 1);
    }
    return bounds;
}

QRect colorBounds(const QImage& image, const QColor& color) {
    QRect bounds;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y) == color) bounds |= QRect(x, y, 1, 1);
    return bounds;
}

void waitMs(int duration) {
    QEventLoop loop;
    QTimer::singleShot(duration, &loop, &QEventLoop::quit);
    loop.exec();
}

void testGameBorders() {
    GameView view;
    view.resize(920, 620);
    QImage frame(320, 224, QImage::Format_RGB32);
    const QColor gameColor(37, 58, 81);
    frame.fill(gameColor);
    view.setFrame(frame, 4.0 / 3.0);
    QImage image = render(view);
    require(image.pixelColor(0, 310) == Qt::black &&
            image.pixelColor(919, 310) == Qt::black,
            "pillarbox fields around loaded game are not black");
    require(image.pixelColor(460, 310) == gameColor, "game frame was overwritten");
    view.resize(640, 720);
    image = render(view);
    require(image.pixelColor(320, 0) == Qt::black &&
            image.pixelColor(320, 719) == Qt::black,
            "letterbox fields around loaded game are not black");
}

void testCleanScreenshot() {
    GameView view;
    view.resize(920, 620);
    require(view.captureGameImage().isNull(), "screenshot captured the empty-game placeholder");
    const QColor colour(37, 58, 81);
    QImage frame(320, 224, QImage::Format_RGB32);
    frame.fill(colour);
    view.setFrame(frame, 4.0 / 3.0);
    for (const auto mode : {GameView::AspectMode::Console, GameView::AspectMode::SquarePixels,
                            GameView::AspectMode::Stretch}) {
        view.setAspectMode(mode);
        const QImage clean = view.captureGameImage();
        require(!clean.isNull() && colorBounds(clean, colour) == clean.rect(),
                "clean screenshot contains display borders or missing game pixels");
        if (mode == GameView::AspectMode::Stretch)
            require(clean.size() == view.size(), "stretched screenshot dimensions differ from game viewport");
        view.setPausedStatus(true);
        view.setMutedStatus(true);
        view.setFastForwardStatus(true);
        view.setRewindStatus(true);
        view.showStateSlots(3, 1U << 3);
        const QImage overlays = view.captureImage();
        require(view.captureGameImage() == clean, "screenshot contains status or slot OSD");
        require(view.captureImage() == overlays, "screenshot changed the visible OSD state");
        view.showOsd(QStringLiteral("Screenshot saved"));
        const QImage notice = view.captureImage();
        require(view.captureGameImage() == clean, "screenshot contains a transient notice");
        require(view.captureImage() == notice, "screenshot discarded a transient notice");
        view.setPausedStatus(false);
        view.setMutedStatus(false);
        view.setFastForwardStatus(false);
        view.setRewindStatus(false);
        view.setOsdEnabled(false);
        view.setOsdEnabled(true);
    }
    QTemporaryDir directory;
    require(directory.isValid(), "screenshot codec scratch unavailable");
    const QImage clean = view.captureGameImage();
    for (const QByteArray format : {QByteArray("png"), QByteArray("jpeg")}) {
        require(QImageWriter::supportedImageFormats().contains(format), "screenshot encoder unavailable");
        require(QImageReader::supportedImageFormats().contains(format), "screenshot decoder unavailable");
        const QString path = directory.filePath(QString::fromLatin1(format) + QStringLiteral("-capture.") +
                                                 (format == "jpeg" ? QStringLiteral("jpg") : QStringLiteral("png")));
        QFile file(path);
        require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "cannot create screenshot fixture");
        QImageWriter writer(&file, format);
        if (format == "jpeg") writer.setQuality(95);
        require(writer.write(clean) && file.flush(), "screenshot encoding failed");
        file.close();
        QImageReader reader(path);
        const QImage decoded = reader.read();
        require(!decoded.isNull() && decoded.size() == clean.size(), "screenshot decoding or dimensions failed");
        const QColor pixel = decoded.pixelColor(decoded.width()/2, decoded.height()/2);
        require(qAbs(pixel.red()-colour.red()) <= 2 && qAbs(pixel.green()-colour.green()) <= 2 &&
                    qAbs(pixel.blue()-colour.blue()) <= 2, "screenshot codec changed game colours");
        require(!file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "screenshot collision overwrote existing file");
    }
}

void testFilteredSquarePixels() {
    const std::array<std::pair<QSize,QSize>,6> sizes{{
        {{256,224},{602,224}}, {{320,224},{640,224}},
        {{248,192},{582,192}}, {{240,192},{562,192}},
        {{320,448},{640,448}}, {{160,144},{378,144}}
    }};
    const QColor colour(37,58,81);
    for (const auto& [native,samples] : sizes) {
        GameView view; view.resize(1100,900); view.setOsdEnabled(false);
        QImage raw(native,QImage::Format_RGB32); raw.fill(colour);
        QImage filtered(samples,QImage::Format_RGB32); filtered.fill(colour);
        VideoFrameGeometry::setNativeSize(filtered,native);
        for (bool integer : {false,true}) {
            view.setIntegerScaling(integer);
            for (const auto mode : {GameView::AspectMode::SquarePixels,GameView::AspectMode::Console,
                                    GameView::AspectMode::Stretch}) {
                view.setAspectMode(mode);
                view.setFrame(raw,4.0/3.0); const QRect expected = colorBounds(render(view),colour);
                view.setFrame(filtered,4.0/3.0); const QRect actual = colorBounds(render(view),colour);
                require(actual == expected, "filtered sample width changed native display rectangle");
            }
        }
        filtered.setText(VideoFrameGeometry::nativeWidthKey,QStringLiteral("999999999999999999999"));
        require(VideoFrameGeometry::nativeSize(filtered) == samples,"invalid geometry metadata was not bounded");
    }
    GameView maximum,regular; prepare(maximum); prepare(regular);
    maximum.setFastForwardFactor(0); regular.setFastForwardFactor(1);
    maximum.setFastForwardStatus(true); regular.setFastForwardStatus(true);
    require(render(maximum) != render(regular),"maximum speed still displayed a numeric factor");
}

void testUpperLeftBounds() {
    // Exercise all three OSD font sizes without depending on platform glyph metrics.
    for (const QSize& size : std::array<QSize, 3>{{{400, 280}, {920, 620}, {1920, 1080}}}) {
        GameView view;
        prepare(view, size);
        const QImage baseline = render(view);
        view.setPausedStatus(true);
        view.setFastForwardStatus(true);
        view.showOsd(QStringLiteral("Saved: slot 3"));
        const QImage actual = render(view);
        const QRect bounds = changedBounds(baseline, actual);
        const int margin = qBound(6, size.width() / 65, 16);
        require(!bounds.isEmpty(), "active OSD did not render");
        require(bounds.left() == margin && bounds.top() == margin,
                "OSD does not begin at the upper-left margin");
        require(bounds.right() < size.width() / 2 && bounds.bottom() < size.height() / 2,
                "short status/action OSD escaped the upper-left area");
        require(actual.copy(0, size.height() / 2, size.width(), size.height() / 2) ==
                    baseline.copy(0, size.height() / 2, size.width(), size.height() / 2),
                "OSD changed the bottom of the game image");
        require(actual.copy(size.width() / 2, 0, size.width() / 2, size.height()) ==
                    baseline.copy(size.width() / 2, 0, size.width() / 2, size.height()),
                "OSD changed the right of the game image");
    }
}

void testOnlyLatestAction() {
    GameView actual, expected;
    prepare(actual);
    prepare(expected);
    actual.showOsd(QStringLiteral("Earlier action"), 2600, QStringLiteral("first"));
    actual.showOsd(QStringLiteral("Saved: slot 2"), 2600, QStringLiteral("second"));
    expected.showOsd(QStringLiteral("Saved: slot 2"), 2600, QStringLiteral("second"));
    require(render(actual) == render(expected), "previous action remains visible");
    actual.showStateSlots(3, 1U << 2);
    expected.showStateSlots(3, 1U << 2);
    require(render(actual) == render(expected), "slot selection did not replace prior notice");
    GameView slotOnly;
    prepare(slotOnly);
    slotOnly.showStateSlots(3, 1U << 2);
    require(render(actual) == render(slotOnly), "slot selection left a prior action visible");
    actual.showOsd(QStringLiteral("Saved: slot 3"));
    GameView noticeOnly;
    prepare(noticeOnly);
    noticeOnly.showOsd(QStringLiteral("Saved: slot 3"));
    require(render(actual) == render(noticeOnly), "notice did not replace the slot row");
}

void testStatusRelease() {
    GameView view;
    prepare(view);
    const QImage baseline = render(view);
    view.setPausedStatus(true);
    const QImage paused = render(view);
    require(paused != baseline, "pause status did not render");
    view.setPausedStatus(false);
    require(render(view) == baseline, "pause release left an OSD message");
    view.setFastForwardStatus(true);
    require(render(view) != baseline, "fast-forward status did not render");
    view.setFastForwardStatus(false);
    require(render(view) == baseline, "fast-forward release left an OSD message");
    view.setPausedStatus(true);
    view.setFastForwardStatus(true);
    view.setFastForwardStatus(false);
    require(render(view) == paused, "fast-forward release disturbed active pause status");
    view.setPausedStatus(false);
    require(render(view) == baseline, "combined status release left an OSD message");
}

void testRewindAndSpeedStatus() {
    GameView view, expected;
    prepare(view);
    prepare(expected);
    const QImage baseline = render(view);
    view.setFastForwardFactor(8);
    require(render(view) == baseline, "inactive speed change displayed an OSD");
    view.setFastForwardStatus(true);
    const QImage speed8 = render(view);
    require(speed8 != baseline, "configured fast-forward status did not render");
    expected.setFastForwardStatus(true);
    require(speed8 != render(expected), "configured speed still displays the default factor");
    expected.setFastForwardFactor(8);
    require(speed8 == render(expected), "active speed factor change did not repaint");
    view.setFastForwardStatus(false);
    require(render(view) == baseline, "configured fast-forward release left an OSD");
    view.setRewindStatus(true);
    const QImage rewind = render(view);
    require(rewind != baseline, "rewind status did not render");
    view.showStateSlots(9, 1U << 9);
    require(render(view).copy(0, 0, view.width(), changedBounds(baseline, rewind).bottom() + 1)
                == rewind.copy(0, 0, view.width(), changedBounds(baseline, rewind).bottom() + 1),
            "slot row displaced the persistent rewind status");
    view.showOsd(QStringLiteral("Latest action"));
    expected.setFastForwardStatus(false);
    expected.showOsd(QStringLiteral("Latest action"));
    view.setRewindStatus(false);
    require(render(view) == render(expected), "rewind release left text or removed the action");
    view.setOsdEnabled(false);
    view.setOsdEnabled(true);
    view.setRewindStatus(true);
    view.setRewindStatus(false);
    require(render(view) == baseline, "rewind release displayed a transient notice");
}

void testStateSlots() {
    // Include a narrow, tall window: slot font must fit independently of game height.
    for (const QSize& size : std::array<QSize, 4>{{{400, 280}, {400, 1080},
                                                 {920, 620}, {1920, 1080}}}) {
        GameView view;
        prepare(view, size);
        const QImage baseline = render(view);
        view.showStateSlots(0, 0);
        const QImage empty = render(view);
        const QRect panel = changedBounds(baseline, empty);
        const int margin = qBound(6, size.width() / 65, 16);
        require(panel.top() == margin && panel.left() == margin,
                "slot OSD did not begin at upper-left margin");
        require(panel.right() < size.width() - margin && panel.bottom() < size.height() / 2,
                "slot OSD did not fit the compact upper-left area");
        require(empty.copy(0, size.height() / 2, size.width(), size.height() / 2) ==
                    baseline.copy(0, size.height() / 2, size.width(), size.height() / 2),
                "slot OSD changed the bottom of the game image");

        QRect previousSelection;
        QRect previousOccupied;
        for (int slot = 0; slot < 10; ++slot) {
            view.showStateSlots(slot, 0);
            const QImage selected = render(view);
            const QRect highlight = colorBounds(selected, QColor(126, 224, 233));
            require(highlight.width() >= 12 && highlight.height() >= 12,
                    "selected slot has no visible cell outline");
            require(panel.contains(highlight), "selected cell escaped the slot panel");
            if (slot) {
                require(highlight.left() > previousSelection.right() &&
                            highlight.top() == previousSelection.top() &&
                            highlight.size() == previousSelection.size(),
                        "slot selection did not advance through ten distinct cells");
            }
            previousSelection = highlight;

            // Select another slot so changing this bit cannot change the selected label.
            view.showStateSlots((slot + 1) % 10, 0);
            const QImage unoccupied = render(view);
            view.showStateSlots((slot + 1) % 10, 1U << slot);
            const QImage occupied = render(view);
            const QRect changed = changedBounds(unoccupied, occupied);
            require(changed.width() >= 10 && changed.height() >= 10 &&
                        highlight.contains(changed),
                    "occupied and empty slots do not have distinct cell fills");
            require(!colorBounds(occupied, QColor(72, 84, 96)).isEmpty(),
                    "occupied slot fill was not rendered");
            if (slot)
                require(changed.left() > previousOccupied.right(),
                        "occupied bitmask did not address separate numbered cells");
            previousOccupied = changed;
        }
        view.showStateSlots(0, 1);
        const QImage selectedOccupied = render(view);
        const QRect highlight = colorBounds(selectedOccupied, QColor(126, 224, 233));
        require(selectedOccupied.copy(0, 0, size.width(), highlight.top()) !=
                    empty.copy(0, 0, size.width(), highlight.top()),
                "selected occupied/empty label did not change");
        view.showStateSlots(9, 0x3ffU);
        const QImage allOccupied = render(view);
        view.showStateSlots(9, 0xffffffffU);
        require(render(view) == allOccupied, "bits outside ten slots changed the row");
    }
}

void testMuteStatus() {
    GameView view;
    prepare(view);
    const QImage baseline = render(view);
    view.setMutedStatus(true);
    const QImage muted = render(view);
    require(muted != baseline, "mute status did not render");
    const QRect bounds = changedBounds(baseline, muted);
    require(bounds.left() < 30 && bounds.top() < 30 && bounds.bottom() < 100,
            "mute status is not at the upper left");
    view.setOsdEnabled(false);
    require(render(view) == baseline, "disabled OSD still shows mute");
    view.setOsdEnabled(true);
    require(render(view) == muted, "active mute was not restored");
    view.setMutedStatus(false);
    require(render(view) == baseline, "unmuting left a notification");
}

void testGlobalToggle() {
    GameView view;
    prepare(view);
    const QImage baseline = render(view);
    view.setPausedStatus(true);
    view.setFastForwardStatus(true);
    view.setRewindStatus(true);
    const QImage activeStatuses = render(view);
    view.showOsd(QStringLiteral("Saved before disable"));
    require(render(view) != activeStatuses, "transient action did not render");
    view.setOsdEnabled(false);
    require(render(view) == baseline, "disabling OSD did not immediately hide all overlays");
    view.showOsd(QStringLiteral("Action while disabled"));
    view.showStateSlots(8, 0x3ffU);
    view.setOsdEnabled(true);
    require(render(view) == activeStatuses,
            "re-enabling did not restore active statuses alone");
    view.setOsdEnabled(false);
    view.setPausedStatus(false);
    view.setFastForwardStatus(false);
    view.setRewindStatus(false);
    view.showOsd(QStringLiteral("Another disabled action"));
    view.setOsdEnabled(true);
    require(render(view) == baseline,
            "disabled status changes or actions left a stale overlay");
    view.showStateSlots(4, 1U << 4);
    require(render(view) != baseline, "slot selection did not render before disable");
    view.setOsdEnabled(false);
    view.setOsdEnabled(true);
    require(render(view) == baseline, "re-enabling restored a stale slot row");
    view.setOsdEnabled(false);
    view.setPausedStatus(true);
    require(render(view) == baseline, "status activated while disabled became visible");
    view.setOsdEnabled(true);
    require(render(view) != baseline, "status activated while disabled was not restored");
}

void testNoticeExpiry() {
    GameView view;
    prepare(view);
    view.setPausedStatus(true);
    const QImage paused = render(view);
    view.showOsd(QStringLiteral("Temporary action"), 500);
    require(render(view) != paused, "timed notice did not render");
    QEventLoop loop;
    QTimer::singleShot(800, &loop, &QEventLoop::quit);
    loop.exec();
    require(render(view) == paused, "expired notice remained or removed active status");
    view.setPausedStatus(false);
    GameView clean;
    prepare(clean);
    require(render(view) == render(clean), "notice expiry left pixels after status release");
}
void testStateSlotExpiry() {
    GameView view;
    prepare(view);
    view.setRewindStatus(true);
    const QImage rewind = render(view);
    view.showOsd(QStringLiteral("Earlier short notice"), 500);
    waitMs(350);
    view.showStateSlots(2, 1U << 2);
    const QImage slotRow = render(view);
    require(slotRow != rewind, "timed slot row did not render");
    waitMs(1500);
    require(render(view) == slotRow, "slot row expired before its two-second duration");
    waitMs(750);
    require(render(view) == rewind, "slot row remained after expiry or removed rewind");
    view.setRewindStatus(false);
    GameView clean;
    prepare(clean);
    require(render(view) == render(clean), "slot/status expiry left pixels behind");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        if (app.arguments().contains(QStringLiteral("--screenshot-only"))) {
            testCleanScreenshot();
            std::puts("clean screenshots passed"); return 0;
        }
        if (app.arguments().contains(QStringLiteral("--mute-only"))) {
            testMuteStatus();
            std::puts("mute OSD passed"); return 0;
        }
        testMuteStatus();
        if (app.arguments().contains(QStringLiteral("--aspect-only"))) {
            testFilteredSquarePixels();
            std::puts("filtered square-pixel geometry passed"); return 0;
        }
        testGameBorders();
        testCleanScreenshot();
        testFilteredSquarePixels();
        testUpperLeftBounds();
        testOnlyLatestAction();
        testStatusRelease();
        testRewindAndSpeedStatus();
        testStateSlots();
        testGlobalToggle();
        testNoticeExpiry();
        testStateSlotExpiry();
        std::puts("OSD tests passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "OSD test failed: %s\n", e.what());
        return 1;
    }
}
