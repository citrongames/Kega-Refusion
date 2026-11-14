#pragma once

#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QSize>
#include <QString>
#include <QVector>
#include <memory>
#include <vector>

class BlarggPass;
class QOpenGLFramebufferObject;

// RetroArch desktop GLSL presets and their per-pass inputs.
struct ShaderParameter {
    QString id;
    QString label;
    float value = 0, minimum = 0, maximum = 1, step = 1;
};

struct ShaderPass {
    enum class Scale { Source, Viewport, Absolute };
    QString path;
    QByteArray source;
    bool linear = false;
    bool explicitScale = false;
    bool floating = false, srgb = false, mipmap = false;
    QString alias;
    Scale scaleX = Scale::Source, scaleY = Scale::Source;
    double factorX = 1, factorY = 1;
    unsigned wrap = 0x812f; // GL_CLAMP_TO_EDGE
    unsigned frameCountMod = 0;
};

struct ShaderLut {
    QString id, path;
    bool linear = true, mipmap = false;
    unsigned wrap = 0x812d; // GL_CLAMP_TO_BORDER
};

struct ShaderPreset {
    static constexpr int maxPasses = 32;
    QString path;
    QVector<ShaderPass> passes;
    QVector<ShaderParameter> parameters;
    QVector<ShaderLut> textures;
    int feedbackPass = -1;
    int blarggFilter = 0;
    static bool load(const QString& path, ShaderPreset& result, QString& error);
    bool usesSpriteOwnership() const {
        return path == QLatin1String(":/shaders/refusion-dither.glslp") && passes.size() == 1
            && passes[0].path == QLatin1String(":/shaders/refusion-dither.glsl");
    }
};

// All methods, including destruction, require its owning GL context current.
class ShaderPipeline final : protected QOpenGLFunctions {
public:
    ShaderPipeline();
    ~ShaderPipeline();
    bool initialize(const ShaderPreset& preset, QString& error);
    bool render(const QImage& frame, unsigned framebuffer, const QSize& surface,
                const QRect& viewport, quint64 frameCount, int frameDirection, QString& error,
                const QImage& spriteOwnership = {}, double originalAspect = 0,
                const QSize& presentationSize = {});
    void releaseBlarggImages();
    void resetFrameHistory() { resetHistory_ = true; }
    bool setParameter(const QString& id, float value);
    QVector<ShaderParameter> parameters() const { return preset_.parameters; }

private:
    struct Program;
    bool loadTextures(QString& error);
    bool compile(const QByteArray& source, std::unique_ptr<Program>& program,
                 QString& error, int passIndex);
    void draw(Program& program, unsigned texture, const QSize& input,
              const QSize& output, quint64 count, int direction, bool linear, unsigned wrap,
              int hasSpriteOwnership = -1, bool mipmap = false);
    ShaderPreset preset_;
    std::unique_ptr<BlarggPass> blargg_;
    std::vector<std::unique_ptr<Program>> programs_;
    std::unique_ptr<Program> stock_;
    std::vector<std::unique_ptr<QOpenGLFramebufferObject>> buffers_;
    std::unique_ptr<QOpenGLFramebufferObject> feedbackBuffer_;
    unsigned feedbackTexture_ = 0;
    QSize feedbackSize_;
    std::vector<unsigned> lutTextures_, historyTextures_;
    std::vector<QSize> historySizes_;
    qint64 lutBytes_ = 0;
    int maxTexture_ = 0;
    bool canSrgb_ = false, resetHistory_ = true;
    quint64 lastFrame_ = 0;
    QOpenGLBuffer vertices_;
    unsigned texture_ = 0;
    QSize textureSize_, finalViewportSize_;
    float originalAspect_ = 1.f;
    bool lastPassFacesScreen_ = false;
};
