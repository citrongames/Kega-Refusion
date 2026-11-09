#include "ui/ShaderPipeline.h"
#include "ui/GameView.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLWidget>
#include <QTimer>
#include <QEventLoop>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
void write(const QString& path, const QByteArray& text) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "cannot write shader fixture");
    require(file.write(text) == text.size(), "incomplete shader fixture");
}
QByteArray shader(const QByteArray& fragment) {
    return R"(
#ifdef VERTEX
attribute vec4 VertexCoord;
attribute vec4 TexCoord;
uniform mat4 MVPMatrix;
varying vec2 tc;
void main() { gl_Position = MVPMatrix * VertexCoord; tc = TexCoord.xy; }
#else
uniform sampler2D Texture;
uniform vec2 OutputSize;
varying vec2 tc;
)" + fragment + "\n#endif\n";
}
void parserTests(const QString& directory) {
    QString error;
    ShaderPreset preset;
    require(ShaderPreset::load(QStringLiteral(":/shaders/refusion-dither.glslp"), preset, error), qPrintable(error));
    require(preset.passes.size() == 1 && preset.parameters.size() == 1, "real dither passes/parameters missing");
    require(preset.parameters[0].id == QStringLiteral("DITHER_STRENGTH"), "dither pragma parameter order changed");
    require(!ShaderPreset::load(QStringLiteral("unsupported.slangp"), preset, error) && error.contains("Slang"), "unsupported Slang was accepted");
    const QString bad = directory + QStringLiteral("/bad.glslp");
    write(bad, "shaders=2\nshader0=missing.glsl\nshader1=missing.glsl\n");
    require(!ShaderPreset::load(bad, preset, error), "missing pass was accepted");
    write(bad, "shaders=99\n");
    require(!ShaderPreset::load(bad, preset, error), "unbounded pass count was accepted");
    write(bad, "#reference other.glslp\n");
    require(!ShaderPreset::load(bad, preset, error), "unsupported preset reference was accepted");
    write(directory + QStringLiteral("/a.glsl"), shader("void main() { gl_FragColor=texture2D(Texture,tc); }"));
    write(bad, "shaders=1\nshader0=a.glsl\ntextures=lookup\nlookup=a.png\n");
    require(!ShaderPreset::load(bad, preset, error) && error.contains("Unsupported"), "unsupported LUT was silently ignored");
    write(bad, "shaders=1\nshader0=a.glsl\nfloat_framebuffer0=true\n");
    require(!ShaderPreset::load(bad, preset, error), "unsupported float framebuffer was accepted");
    write(bad, "shaders=1\nshader0=a.glsl\nscale0=nan\n");
    require(!ShaderPreset::load(bad, preset, error), "nonfinite framebuffer scale was accepted");
    write(directory + QStringLiteral("/loop.glsl"), "#include \"loop.glsl\"\n");
    require(!ShaderPreset::load(directory + QStringLiteral("/loop.glsl"), preset, error), "recursive include was accepted");
    write(directory + QStringLiteral("/params.glsl"), "#pragma parameter GAIN \"Gain\" 0.5 0.0 1.0 0.1\n" + shader("void main() { gl_FragColor=texture2D(Texture,tc); }"));
    write(bad, "shaders = 1 # comment\nshader0 = \"params.glsl\" # comment\nparameters = GAIN\nGAIN = 0.7\nscale_type0=source\nscale0=1\n");
    require(ShaderPreset::load(bad, preset, error), qPrintable(error));
    require(qAbs(preset.parameters[0].value - .7f) < .0001f, "preset parameter override was lost");
}

QImage render(ShaderPipeline& pipeline, const QImage& image, QSize size, QRect viewport = {}) {
    if (viewport.isNull()) viewport = QRect(QPoint(), size);
    QOpenGLFramebufferObject target(size);
    require(target.isValid(), "test output FBO unavailable");
    QString error;
    require(pipeline.render(image, target.handle(), size, viewport, 17, 1, error), qPrintable(error));
    return target.toImage();
}
void memoryBudgetTests(const QString& directory) {
    QString error;
    ShaderPreset preset;
    write(directory + QStringLiteral("/budget.glsl"), shader("void main() { gl_FragColor=texture2D(Texture,tc); }"));
    QImage source(16, 8, QImage::Format_RGB32);
    source.fill(QColor(37, 58, 81));
    QByteArray memoryPreset("shaders=8\n");
    for (int i = 0; i < 8; ++i) {
        const QByteArray index = QByteArray::number(i);
        memoryPreset += "shader" + index + "=budget.glsl\nscale_type" + index + "=absolute\nscale" + index + "=2048\n";
    }
    write(directory + QStringLiteral("/memory.glslp"), memoryPreset);
    require(ShaderPreset::load(directory + QStringLiteral("/memory.glslp"), preset, error), qPrintable(error));
    ShaderPipeline bounded;
    require(bounded.initialize(preset, error), qPrintable(error));
    QOpenGLFramebufferObject smallTarget(QSize(16, 8));
    require(smallTarget.isValid(), "memory budget test FBO unavailable");
    require(!bounded.render(source, smallTarget.handle(), QSize(16, 8), QRect(0, 0, 16, 8), 0, 1, error) &&
            error.contains(QStringLiteral("128 MiB")), "aggregate shader memory budget was not enforced before allocation");
}

void gpuTests(const QString& directory) {
    QString error;
    ShaderPreset preset;
    // Two noncommuting passes prove real GPU programs execute in preset order.
    write(directory + QStringLiteral("/a.glsl"), shader("void main() { vec4 c=texture2D(Texture,tc); gl_FragColor=vec4(c.b,c.g,c.r,0.25); }"));
    write(directory + QStringLiteral("/b.glsl"), shader("void main() { vec4 c=texture2D(Texture,tc); gl_FragColor=vec4(c.r,c.a,c.b,1.0); }"));
    write(directory + QStringLiteral("/two.glslp"), "shaders=2\nshader0=a.glsl\nscale_type0=source\nscale0=1\nshader1=b.glsl\nscale_type1=viewport\nscale1=1\n");
    require(ShaderPreset::load(directory + QStringLiteral("/two.glslp"), preset, error), qPrintable(error));
    ShaderPipeline pipeline;
    require(pipeline.initialize(preset, error), qPrintable(error));
    QImage source(16, 8, QImage::Format_RGB32);
    source.fill(QColor(29, 79, 137));
    source.setPixelColor(0, 0, QColor(240, 10, 20));
    const QRect viewport(8, 4, 64, 32);
    const QImage result = render(pipeline, source, QSize(80, 40), viewport);
    require(result.pixelColor(0, 0) == QColor(Qt::black) &&
            result.pixelColor(79, 20) == QColor(Qt::black), "GPU game borders are not black");
    const QColor center = result.pixelColor(40, 20);
    require(qAbs(center.red() - 137) <= 1 && qAbs(center.green() - 64) <= 1 &&
            qAbs(center.blue() - 29) <= 1, "multipass order or alpha-tag preservation failed");
    const QColor corner = result.pixelColor(9, 5);
    require(qAbs(corner.red() - 20) <= 1 && qAbs(corner.blue() - 240) <= 1,
            "GPU frame was vertically flipped or channel-swapped");

    require(ShaderPreset::load(QStringLiteral(":/shaders/refusion-dither.glslp"), preset, error), qPrintable(error));
    ShaderPipeline dither;
    require(dither.initialize(preset, error), qPrintable(error));
    require(dither.setParameter(QStringLiteral("DITHER_STRENGTH"), 1.0f), "dither parameter cannot be changed");
    require(!dither.setParameter(QStringLiteral("unknown"), 1.0f), "unknown parameter was accepted");
    QImage stripes(32, 16, QImage::Format_RGB32);
    for (int y = 0; y < stripes.height(); ++y)
        for (int x = 0; x < stripes.width(); ++x)
            stripes.setPixelColor(x, y, x % 2 ? QColor(220, 40, 20) : QColor(20, 40, 220));
    const QImage blended = render(dither, stripes, stripes.size());
    const QColor blend = blended.pixelColor(16, 8);
    require(qAbs(blend.red() - 120) <= 2 && qAbs(blend.green() - 40) <= 1 &&
            qAbs(blend.blue() - 120) <= 2, "dither did not merge detected alternating pixels");
    stripes.fill(QColor(37, 58, 81));
    const QImage flat = render(dither, stripes, QSize(64, 32));
    require(flat.pixelColor(32, 16) == QColor(37, 58, 81), "dither changed a flat, nondithered area");
    // No title-specific fixtures: the same geometry must work in either phase.
    const QColor dark(20, 40, 220), light(220, 40, 20);
    for (int pattern = 0; pattern < 4; ++pattern) {
        QImage tile(16, 16, QImage::Format_RGB32);
        for (int y = 0; y < tile.height(); ++y) {
            for (int x = 0; x < tile.width(); ++x) {
                const bool bright = pattern == 0 ? (x + y) % 2 :
                                    pattern == 1 ? y % 2 :
                                    pattern == 2 ? (x % 2 && y % 2) : (x % 2 || y % 2);
                tile.setPixelColor(x, y, bright ? light : dark);
            }
        }
        const QImage mixed = render(dither, tile, tile.size());
        const QColor expected = pattern == 2 ? QColor(70, 40, 170) :
                                pattern == 3 ? QColor(170, 40, 70) : QColor(120, 40, 120);
        for (int y = 0; y < tile.height(); ++y)
            for (int x = 0; x < tile.width(); ++x)
                require(mixed.pixelColor(x, y) == expected, "solid pattern did not blend up to its edge");
    }
    QImage details(35, 19, QImage::Format_RGB32);
    details.fill(dark);
    constexpr const char* digits[] = {"1110111", "0010101", "0110111", "0010101", "1110111"};
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 7; ++x)
            if (digits[y][x] == '1') details.setPixelColor(3+x, 5+y, light);
    for (int x = 0; x < details.width(); x += 2)
        details.setPixelColor(x, 14, light);
    const QImage kept = render(dither, details, details.size());
    require(kept.convertToFormat(details.format()) == details, "thin digits or isolated stippling changed");

    memoryBudgetTests(directory);
    write(directory + QStringLiteral("/lut.glsl"), shader("uniform sampler2D LUT; void main() { gl_FragColor=texture2D(LUT,tc); }"));
    require(ShaderPreset::load(directory + QStringLiteral("/lut.glsl"), preset, error), qPrintable(error));
    ShaderPipeline unsupported;
    require(!unsupported.initialize(preset, error) && error.contains("sampler"), "unsupported custom sampler silently sampled the frame");
    write(directory + QStringLiteral("/broken.glsl"), shader("void main() { deliberately invalid GLSL; }"));
    require(ShaderPreset::load(directory + QStringLiteral("/broken.glsl"), preset, error), qPrintable(error));
    ShaderPipeline invalid;
    require(!invalid.initialize(preset, error) && !error.isEmpty(), "GLSL compiler failure was hidden");
}

void settle() {
    QEventLoop loop;
    QTimer::singleShot(60, &loop, &QEventLoop::quit);
    loop.exec();
}
QImage capture(GameView& view) {
    view.show();
    settle();
    auto* gpu = view.findChild<QOpenGLWidget*>(QStringLiteral("gpuVideo"));
    require(gpu && gpu->isValid(), "GameView native OpenGL presentation unavailable");
    require(view.findChildren<QOpenGLWidget*>().size() == 1, "native context creation reentered and duplicated the GPU view");
    const QImage image = view.captureImage();
    require(!image.isNull(), "GameView GPU framebuffer is empty");
    return image;
}
void screenshotWidgetTests() {
    GameView view;
    view.resize(920, 620);
    QImage source(32, 16, QImage::Format_RGB32);
    for (int y = 0; y < source.height(); ++y)
        for (int x = 0; x < source.width(); ++x)
            source.setPixelColor(x, y, x % 2 ? QColor(220, 40, 20) : QColor(20, 40, 220));
    view.setFrame(source, 4.0 / 3.0);
    QString error;
    require(view.setShaderPreset(QStringLiteral(":/shaders/refusion-dither.glslp"), &error), qPrintable(error));
    require(view.setShaderParameter(QStringLiteral("DITHER_STRENGTH"), 1), "screenshot shader parameter unavailable");
    const QImage baseline = capture(view);
    view.setPausedStatus(true);
    view.showOsd(QStringLiteral("Screenshot saved"));
    const QImage overlays = capture(view);
    const QImage clean = view.captureGameImage();
    require(!clean.isNull() && clean.width() < baseline.width() && clean.height() == baseline.height(),
            "clean native screenshot retained display borders");
    const int left = qRound((baseline.width() - clean.width()) / 2.0);
    require(clean == baseline.copy(left, 0, clean.width(), clean.height()),
            "clean native screenshot retained OSD or changed filtered output");
    const QColor centre = clean.pixelColor(clean.width()/2, clean.height()/2);
    require(qAbs(centre.red()-120) <= 2 && qAbs(centre.blue()-120) <= 2,
            "clean native screenshot omitted the dither filter shader");
    require(view.captureImage() == overlays, "clean native screenshot changed visible OSD");
}

void widgetTests(const QString& directory) {
    GameView view;
    view.resize(920, 620);
    QImage stripes(32, 16, QImage::Format_RGB32);
    for (int y = 0; y < stripes.height(); ++y)
        for (int x = 0; x < stripes.width(); ++x)
            stripes.setPixelColor(x, y, x % 2 ? QColor(220, 40, 20) : QColor(20, 40, 220));
    view.setFrame(stripes, 4.0 / 3.0);
    const QImage stock = capture(view);
    require(stock.pixelColor(0, stock.height() / 2) == QColor(Qt::black), "native GameView pillarbox is not black");
    QString error;
    require(view.setShaderPreset(QStringLiteral(":/shaders/refusion-dither.glslp"), &error), qPrintable(error));
    require(view.setShaderParameter(QStringLiteral("DITHER_STRENGTH"), 1), "native dither filter parameter cannot be set");
    const QImage filtered = capture(view);
    const QColor center = filtered.pixelColor(filtered.width() / 2, filtered.height() / 2);
    require(qAbs(center.red() - 120) <= 2 && qAbs(center.blue() - 120) <= 2, "GameView did not present dither filter output");
    require(filtered.pixelColor(0, filtered.height() / 2) == QColor(Qt::black), "shader colored GameView borders");

    require(!view.setShaderPreset(directory + QStringLiteral("/broken.glsl"), &error) && !error.isEmpty(),
            "GameView accepted a broken shader without an error");
    require(view.shaderPreset() == QStringLiteral(":/shaders/refusion-dither.glslp"), "failed shader replaced the working preset");
    require(capture(view) == filtered, "failed shader damaged the active pipeline");

    view.setPausedStatus(true);
    view.showOsd(QStringLiteral("Saved: slot 3"));
    const QImage shaderOsd = capture(view);
    const QImage cleanShader = view.captureGameImage();
    require(!cleanShader.isNull() && cleanShader.width() < shaderOsd.width() &&
            cleanShader.height() == shaderOsd.height(), "clean shader screenshot did not crop pillarboxes");
    const int left = qRound((shaderOsd.width() - cleanShader.width()) / 2.0);
    require(cleanShader == filtered.copy(left, 0, cleanShader.width(), cleanShader.height()),
            "clean screenshot contains OSD or lost shader rendering");
    require(view.captureImage() == shaderOsd, "clean GPU screenshot changed the visible OSD");
    require(view.setShaderPreset({}, &error), qPrintable(error));
    const QImage stockOsd = capture(view);
    GameView rasterView;
    rasterView.resize(view.size());
    rasterView.setFrame(stripes, 4.0/3.0);
    rasterView.setPausedStatus(true);
    rasterView.showOsd(QStringLiteral("Saved: slot 3"));
    QImage rasterOsd(stockOsd.size(), QImage::Format_RGB32);
    rasterOsd.setDevicePixelRatio(view.devicePixelRatioF());
    rasterOsd.fill(Qt::magenta);
    rasterView.render(&rasterOsd);
    require(rasterView.findChildren<QOpenGLWidget*>().isEmpty(), "raster OSD reference unexpectedly used OpenGL");
    const QString captureDirectory = qEnvironmentVariable("KEGA_SHADER_CAPTURE_DIR");
    if (!captureDirectory.isEmpty()) {
        QDir().mkpath(captureDirectory);
        require(stock.save(captureDirectory + QStringLiteral("/gpu-stock.png")) &&
                filtered.save(captureDirectory + QStringLiteral("/gpu-dither.png")) &&
                shaderOsd.save(captureDirectory + QStringLiteral("/gpu-dither-osd.png")) &&
                rasterOsd.save(captureDirectory + QStringLiteral("/raster-osd.png")),
                "cannot save requested native GPU captures");
    }
    QRect textBounds;
    for (int y = 0; y < stockOsd.height(); ++y) {
        for (int x = 0; x < stockOsd.width(); ++x) {
            const QColor before = stockOsd.pixelColor(x, y);
            const QColor after = shaderOsd.pixelColor(x, y);
            for (const QColor& marker : {QColor(244,249,250), QColor(126,224,233)}) {
                require((before == marker) == (after == marker), "shader filtered native OSD text or outlines");
                if (marker == QColor(244,249,250))
                    require((before == marker) == (rasterOsd.pixelColor(x,y) == marker),
                            "native OSD glyphs differ from the unfiltered raster OSD");
                if (before == marker) textBounds |= QRect(x,y,1,1);
            }
        }
    }
    require(!textBounds.isEmpty() && textBounds.left() < 30 && textBounds.top() < 30 &&
            textBounds.right() < stockOsd.width() / 2 && textBounds.bottom() < stockOsd.height() / 2,
            "native OSD escaped the upper-left corner");

    view.setPausedStatus(false);
    view.setOsdEnabled(false);
    view.setIntegerScaling(true);
    const QImage integer = capture(view);
    // 16 source lines at integer 38x = 608 lines, with six black lines above/below.
    const int expectedTop = qRound(6 * view.devicePixelRatioF());
    require(integer.pixelColor(integer.width()/2, expectedTop - 1) == QColor(Qt::black) &&
            integer.pixelColor(integer.width()/2, expectedTop) != QColor(Qt::black),
            "GPU integer scaling no longer uses the original source height");
    view.hide();
    view.show();
    require(!capture(view).isNull(), "GPU presentation did not survive hide/show");
    view.close();

    GameView restored;
    restored.resize(920,620);
    restored.setFrame(stripes,4.0/3.0);
    require(restored.setShaderPreset(QStringLiteral(":/shaders/refusion-dither.glslp"), &error), qPrintable(error));
    require(restored.setShaderParameter(QStringLiteral("DITHER_STRENGTH"),1), "startup shader parameter restore failed");
    require(capture(restored) == filtered, "shader preset/parameters set before showing were lost");
    restored.close();
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "shader test scratch unavailable");
        const bool resourceOnly = app.arguments().contains(QStringLiteral("--resource-only"));
        const int externalIndex = app.arguments().indexOf(QStringLiteral("--external-preset"));
        if (!resourceOnly && externalIndex < 0) parserTests(directory.path());
        if (app.arguments().contains(QStringLiteral("--parser-only"))) {
            std::puts("Shader preset/parser tests passed");
            return 0;
        }
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(2, 1);
        format.setDepthBufferSize(0);
        QOpenGLContext context;
        context.setFormat(format);
        require(context.create(), "native OpenGL context unavailable (use --parser-only on headless platforms)");
        QOffscreenSurface surface;
        surface.setFormat(context.format());
        surface.create();
        require(surface.isValid() && context.makeCurrent(&surface), "cannot make GPU test context current");
        std::printf("GPU: %s; OpenGL: %s\n",
                    context.functions()->glGetString(GL_RENDERER),
                    context.functions()->glGetString(GL_VERSION));
        if (externalIndex >= 0) {
            require(externalIndex + 1 < app.arguments().size(), "external preset path missing");
            ShaderPreset preset; QString error;
            require(ShaderPreset::load(app.arguments()[externalIndex+1],preset,error),qPrintable(error));
            ShaderPipeline pipeline;
            require(pipeline.initialize(preset,error),qPrintable(error));
            QImage image(32,32,QImage::Format_RGB32); image.fill(QColor(120,80,40));
            const auto result=render(pipeline,image,QSize(64,64));
            require(!result.isNull() && result.pixelColor(32,32).red()>0,"external preset produced no output");
            std::puts("External shader preset compiled and rendered successfully");
            return 0;
        }
        if (resourceOnly) {
            memoryBudgetTests(directory.path());
            context.doneCurrent();
            screenshotWidgetTests();
            std::puts("Aggregate shader memory budget and clean native screenshot passed");
            return 0;
        }
        gpuTests(directory.path());
        context.doneCurrent();
        widgetTests(directory.path());
        std::puts("Native GameView, unfiltered OSD, integer scaling and GLSL parser, GPU multipass, alpha, orientation, borders and native dither blending tests passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Shader test failed: %s\n", e.what());
        return 1;
    }
}
