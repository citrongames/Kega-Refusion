#include "core/LibretroHost.h"
#include "ui/ShaderPipeline.h"

#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QSurfaceFormat>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace {
constexpr char mmpxPreset[] = ":/shaders/mmpx-advanced.glslp";
constexpr char ditherPreset[] = ":/shaders/refusion-dither.glslp";
constexpr int sampleCount = 30;
const QSize presentationSize(1280, 720);

struct Frame {
    QImage image;
    QImage ownership;
    double aspect = 0;
};

void require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}

void initialize(ShaderPipeline& pipeline, const QString& path = {}) {
    ShaderPreset preset;
    QString error;
    if ((!path.isEmpty() && !ShaderPreset::load(path, preset, error)) ||
        !pipeline.initialize(preset, error)) {
        throw std::runtime_error(error.toStdString());
    }
}

void paint(ShaderPipeline& pipeline, const Frame& frame, QOpenGLFramebufferObject& target,
           QRect viewport, quint64 frameCount) {
    QString error;
    if (!pipeline.render(frame.image, target.handle(), target.size(), viewport, frameCount, 1,
                         error, frame.ownership, frame.aspect)) {
        throw std::runtime_error(error.toStdString());
    }
}

QImage capture(ShaderPipeline& pipeline, const Frame& frame, QOpenGLFramebufferObject& target,
               QRect viewport) {
    paint(pipeline, frame, target, viewport, 0);
    return target.toImage().convertToFormat(QImage::Format_RGB32);
}

void syntheticTests() {
    std::unique_ptr<ShaderPipeline> active;
    const auto select = [&active](const QString& path) {
        auto next = std::make_unique<ShaderPipeline>();
        initialize(*next, path);
        active = std::move(next);
    };
    select(QLatin1String(mmpxPreset));
    // Keep the pipeline through resolution changes so texture and pass targets resize
    // in place. The colored interior is far from the preset's clamp-to-border taps.
    for (const QSize size : {QSize(160, 144), QSize(256, 192), QSize(320, 224),
                             QSize(320, 240), QSize(320, 448), QSize(160, 144)}) {
        const QRgb color = qRgb(37, 115, 201);
        QImage solid(size, QImage::Format_RGB32);
        solid.fill(color);
        const QRect viewport(QPoint(7, 11), size * 2);
        QOpenGLFramebufferObject target(viewport.size() + QSize(20, 30));
        require(target.isValid(), "cannot create the resize test framebuffer");
        const QImage actual = capture(*active, {solid, {}, 0}, target, viewport);
        require(actual.size() == target.size(), "MMPX could not render at the requested dimensions");
        const QRect interior = viewport.adjusted(8, 8, -8, -8);
        for (int y = interior.top(); y <= interior.bottom(); ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
            require(std::all_of(row + interior.left(), row + interior.right() + 1,
                                [color](QRgb pixel) { return pixel == color; }),
                    "MMPX changed a flat color or rendered outside the expected viewport");
        }
        const QRgb black = qRgb(0, 0, 0);
        require(actual.pixel(viewport.left() - 1, viewport.center().y()) == black &&
                actual.pixel(viewport.right() + 1, viewport.center().y()) == black &&
                actual.pixel(viewport.center().x(), viewport.top() - 1) == black &&
                actual.pixel(viewport.center().x(), viewport.bottom() + 1) == black,
                "MMPX colored the letterbox");
    }

    QImage pattern(48, 36, QImage::Format_RGB32);
    // A moderate contrast gives the advanced shader room to blend the slope.
    const QRgb background = qRgb(36, 36, 36);
    const QRgb edge = qRgb(144, 144, 144);
    const QRgb ink = qRgb(29, 156, 239);
    pattern.fill(background);
    for (int y = 5; y <= 17; ++y)
        for (int x = 6; x <= 10 + (y - 5) * 2; ++x)
            pattern.setPixel(x, y, edge);
    // A one-pixel-wide F exercises the horizontal and vertical strokes.
    for (int y = 24; y <= 31; ++y) pattern.setPixel(8, y, ink);
    for (int x = 8; x <= 15; ++x) pattern.setPixel(x, 24, ink);
    for (int x = 8; x <= 13; ++x) pattern.setPixel(x, 27, ink);

    QOpenGLFramebufferObject target(pattern.size() * 2);
    require(target.isValid(), "cannot create the edge test framebuffer");
    const QRect viewport(QPoint(), target.size());
    const QImage actual = capture(*active, {pattern, {}, 0}, target, viewport);
    require(!actual.isNull(), "MMPX edge test produced no pixels");
    require(actual.pixel(20, 28) == edge && actual.pixel(82, 16) == background,
            "MMPX lost the orientation or flat interiors of the test pattern");
    require(actual.pixel(16, 60) == ink && actual.pixel(26, 48) == ink &&
            actual.pixel(22, 54) == ink && actual.pixel(24, 51) == background,
            "MMPX lost one-pixel text strokes or filled their gap");

    const QImage nearest = pattern.scaled(target.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
    int changed = 0;
    int mixed = 0;
    for (int y = 15; y < 32; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
        for (int x = 27; x < 63; ++x) {
            const QRgb pixel = row[x];
            changed += pixel != nearest.pixel(x, y);
            mixed += pixel != background && pixel != edge;
            require(qRed(pixel) == qGreen(pixel) && qGreen(pixel) == qBlue(pixel) &&
                    qRed(pixel) >= qRed(background) && qRed(pixel) <= qRed(edge),
                    "MMPX introduced an unrelated color in the two-color slope");
        }
    }
    require(changed > 0, "MMPX rendered a sloped edge as nearest-neighbor");
    require(mixed > 0, "MMPX did not blend intermediate values on a moderate-contrast slope");

    QImage stripes(pattern.size(), QImage::Format_RGB32);
    for (int y = 0; y < stripes.height(); ++y)
        for (int x = 0; x < stripes.width(); ++x)
            stripes.setPixel(x, y, x % 2 ? qRgb(20, 40, 220) : qRgb(220, 40, 20));
    select(QLatin1String(ditherPreset));
    const QImage dithered = capture(*active, {stripes, {}, 0}, target, viewport);
    require(!dithered.isNull() && dithered.pixel(viewport.center()) == qRgb(120, 40, 120),
            "dither did not blend the test stripes after replacing MMPX");
    select({});
    require(capture(*active, {stripes, {}, 0}, target, viewport) ==
            stripes.scaled(target.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation),
            "switching off the shader did not restore stock nearest-neighbor scaling");
    std::printf("Synthetic MMPX tests passed (%d changed slope samples, %d mixed).\n", changed, mixed);
}

std::array<Frame, sampleCount> sampleFrames(const QString& core, const QString& rom,
                                           const QString& state) {
    QTemporaryDir dir;
    require(dir.isValid(), "cannot isolate game data in a test directory");
    LibretroHost host(core, dir.path());
    host.setNetworkReadOnly(true);
    host.setSpriteOwnershipEnabled(true);
    QString error;
    if (!host.load(rom, error, -1, nullptr, false))
        throw std::runtime_error(QStringLiteral("Unable to load %1: %2").arg(rom, error).toStdString());
    if (state == QLatin1String("-")) {
        for (int i = 0; i < 360; ++i) host.runFrame(false, false);
    } else if (!host.loadStateFromFile(state, error)) {
        throw std::runtime_error(QStringLiteral("Unable to load %1: %2").arg(state, error).toStdString());
    }

    std::array<Frame, sampleCount> frames;
    int ownershipFrames = 0;
    for (Frame& frame : frames) {
        host.runFrame(true, false);
        frame = {host.frame(), host.spriteOwnership(), host.aspectRatio()};
        require(!frame.image.isNull(), "core returned an empty video frame");
        require(frame.image.size() == frames.front().image.size(),
                "core changed video size during the benchmark sample");
        require(frame.ownership.isNull() || frame.ownership.size() == frame.image.size(),
                "sprite ownership dimensions differ from the captured frame");
        ownershipFrames += !frame.ownership.isNull();
    }
    std::printf("ROM: %s\nScene: %s\nSource: %dx%d at %.3f Hz; sprite ownership on %d/%d frames\n",
                qPrintable(rom), state == QLatin1String("-") ? "boot + 360 frames" : qPrintable(state),
                frames.front().image.width(), frames.front().image.height(), host.fps(),
                ownershipFrames, sampleCount);
    return frames;
}

void save(const QImage& image, const QString& path) {
    if (!image.save(path, "PNG")) {
        throw std::runtime_error(QStringLiteral("Unable to save %1").arg(path).toStdString());
    }
}

void compare(const std::array<Frame, sampleCount>& frames, const QString& output,
             QOpenGLFunctions& gl) {
    QDir folder(output);
    require(QDir().mkpath(folder.absolutePath()), "cannot create the capture directory");

    constexpr int filterCount = 3;
    const std::array<QString, filterCount> paths = {{QString(), QLatin1String(ditherPreset),
                                                   QLatin1String(mmpxPreset)}};
    const std::array<const char*, filterCount> names = {{"stock", "dither", "mmpx"}};
    std::array<ShaderPipeline, filterCount> pipelines;
    QOpenGLFramebufferObject target(presentationSize);
    require(target.isValid(), "cannot create the benchmark framebuffer");
    const double aspect = frames.front().aspect > 0.0 ? frames.front().aspect :
        double(frames.front().image.width()) / frames.front().image.height();
    QSize viewportSize(qRound(presentationSize.height() * aspect), presentationSize.height());
    if (viewportSize.width() > presentationSize.width())
        viewportSize = QSize(presentationSize.width(), qRound(presentationSize.width() / aspect));
    const QRect viewport(QPoint((presentationSize.width() - viewportSize.width()) / 2,
                                (presentationSize.height() - viewportSize.height()) / 2), viewportSize);

    std::printf("Viewport: %dx%d, aspect %.6f\n", viewport.width(), viewport.height(), aspect);
    save(frames.front().image, folder.filePath(QStringLiteral("source.png")));
    constexpr int warmupFrames = 120;
    std::array<quint64, filterCount> frameNumbers = {};
    for (int filter = 0; filter < filterCount; ++filter) {
        ShaderPipeline& pipeline = pipelines[filter];
        initialize(pipeline, paths[filter]);
        save(capture(pipeline, frames.front(), target, viewport),
             folder.filePath(QString::fromLatin1(names[filter]) + QStringLiteral(".png")));
        for (int i = 0; i < warmupFrames; ++i)
            paint(pipeline, frames[i % sampleCount], target, viewport, ++frameNumbers[filter]);
        gl.glFinish();
    }

    constexpr int batches = 3;
    constexpr int repeats = 10;
    std::array<std::array<double, batches>, filterCount> times = {};
    for (int batch = 0; batch < batches; ++batch) {
        for (int offset = 0; offset < filterCount; ++offset) {
            // Rotate order to reduce bias from clock and temperature changes.
            const int filter = (offset + batch) % filterCount;
            gl.glFinish();
            QElapsedTimer timer;
            timer.start();
            for (int repeat = 0; repeat < repeats; ++repeat)
                for (const Frame& frame : frames)
                    paint(pipelines[filter], frame, target, viewport, ++frameNumbers[filter]);
            gl.glFinish();
            times[filter][batch] = double(timer.nsecsElapsed()) / (1e6 * sampleCount * repeats);
        }
    }
    std::printf("1280x720 synchronized render and upload time (ms/frame, median of %d batches of %d;"
                " excludes emulation, PNG readback, and swap/vsync)\n", batches, sampleCount * repeats);
    for (int filter = 0; filter < filterCount; ++filter) {
        const auto samples = times[filter];
        auto sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        std::printf("  %-6s %.3f (%.3f, %.3f, %.3f)\n", names[filter], sorted[batches / 2],
                    samples[0], samples[1], samples[2]);
    }
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 1 && args.size() != 5) {
        std::fprintf(stderr, "Usage: mmpx_tests [CORE ROM STATE|- OUTPUT_DIRECTORY]\n");
        return 2;
    }

    try {
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(2, 1);
        QOpenGLContext context;
        context.setFormat(format);
        require(context.create() && !context.isOpenGLES(), "desktop OpenGL context unavailable");
        QOffscreenSurface surface;
        surface.setFormat(context.format());
        surface.create();
        require(surface.isValid() && context.makeCurrent(&surface), "cannot make offscreen context current");
        auto& gl = *context.functions();
        std::printf("Renderer: %s; OpenGL: %s\n",
                    reinterpret_cast<const char*>(gl.glGetString(GL_RENDERER)),
                    reinterpret_cast<const char*>(gl.glGetString(GL_VERSION)));

        syntheticTests();
        if (args.size() == 5)
            compare(sampleFrames(args[1], args[2], args[3]), args[4], gl);
        context.doneCurrent();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MMPX test failed: %s\n", error.what());
        return 1;
    }
}
