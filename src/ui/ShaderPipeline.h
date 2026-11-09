#pragma once

#include <QImage>
#include <QMap>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QSize>
#include <QString>
#include <QVector>
#include <memory>
#include <vector>

class QOpenGLFramebufferObject;
class QOpenGLShaderProgram;

// Deliberately implements the serial GLSL subset, not the Slang/Cg runtime.
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
    Scale scaleX = Scale::Source, scaleY = Scale::Source;
    double factorX = 1, factorY = 1;
    unsigned wrap = 0x812f; // GL_CLAMP_TO_EDGE
    unsigned frameCountMod = 0;
};

struct ShaderPreset {
    QString path;
    QVector<ShaderPass> passes;
    QVector<ShaderParameter> parameters;
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
                const QImage& spriteOwnership = {});
    bool setParameter(const QString& id, float value);
    QVector<ShaderParameter> parameters() const { return preset_.parameters; }

private:
    bool compile(const QByteArray& source, std::unique_ptr<QOpenGLShaderProgram>& program,
                 QString& error);
    void draw(QOpenGLShaderProgram& program, unsigned texture, const QSize& input,
              const QSize& output, quint64 count, int direction, bool linear, unsigned wrap,
              int hasSpriteOwnership = -1);
    ShaderPreset preset_;
    std::vector<std::unique_ptr<QOpenGLShaderProgram>> programs_;
    std::unique_ptr<QOpenGLShaderProgram> stock_;
    std::vector<std::unique_ptr<QOpenGLFramebufferObject>> buffers_;
    QOpenGLBuffer vertices_;
    unsigned texture_ = 0;
    QSize textureSize_;
};
