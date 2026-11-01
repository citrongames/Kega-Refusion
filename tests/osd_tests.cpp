#include "ui/GameView.h"

#include <QApplication>
#include <QEventLoop>
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

void testGlobalToggle() {
    GameView view;
    prepare(view);
    const QImage baseline = render(view);
    view.setPausedStatus(true);
    view.setFastForwardStatus(true);
    const QImage activeStatuses = render(view);
    view.showOsd(QStringLiteral("Saved before disable"));
    require(render(view) != activeStatuses, "transient action did not render");
    view.setOsdEnabled(false);
    require(render(view) == baseline, "disabling OSD did not immediately hide all overlays");
    view.showOsd(QStringLiteral("Action while disabled"));
    view.setOsdEnabled(true);
    require(render(view) == activeStatuses,
            "re-enabling did not restore active statuses alone");
    view.setOsdEnabled(false);
    view.setPausedStatus(false);
    view.setFastForwardStatus(false);
    view.showOsd(QStringLiteral("Another disabled action"));
    view.setOsdEnabled(true);
    require(render(view) == baseline,
            "disabled status changes or actions left a stale overlay");
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
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        testGameBorders();
        testUpperLeftBounds();
        testOnlyLatestAction();
        testStatusRelease();
        testGlobalToggle();
        testNoticeExpiry();
        std::puts("OSD tests passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "OSD test failed: %s\n", e.what());
        return 1;
    }
}
