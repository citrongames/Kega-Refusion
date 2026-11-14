#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLVertexArrayObject>
#include <QSize>
#include <QString>
#include <array>
#include <memory>

class QOpenGLFramebufferObject;
class QOpenGLShaderProgram;

// Native-resolution Blargg preprocessing. All methods/destruction require the
// owning GL context current. Input and returned textures use first row at v=0.
// The caller scales the returned texture and restores its framebuffer/viewport.
class BlarggPass final : protected QOpenGLFunctions {
public:
    BlarggPass();
    ~BlarggPass();
    bool initialize(QString& error);
    bool render(unsigned inputTexture, const QSize& inputSize, int preset,
                bool h40, QString& error);
    void releaseImages();
    unsigned texture() const;
    QSize size() const;

private:
    struct Program;
    bool prepareKernel(int preset, bool h40, QString& error);
    std::array<std::unique_ptr<Program>, 2> programs_;
    std::array<unsigned, 2> lookup_{};
    std::array<int, 2> lookupPreset_{};
    std::unique_ptr<QOpenGLFramebufferObject> output_;
    QOpenGLBuffer vertices_;
    QOpenGLVertexArrayObject vertexArray_;
    int maxTexture_ = 0;
};
