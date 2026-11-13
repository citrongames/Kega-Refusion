#include "ShaderPipeline.h"

#include <QImageReader>
#include <QMatrix4x4>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <array>

// Internal, immutable binding plan produced once after linking each GLSL program.
struct ShaderPipeline::Program {
    struct Frame {
        enum class Kind { Original, Pass, Previous, Feedback };
        Kind kind = Kind::Original;
        int index = 0;
        int sampler = -1;
        int inputSize = -1;
        int textureSize = -1;
        int texCoord = -1;
    };
    struct Lut {
        int index = 0;
        int sampler = -1;
    };
    struct Parameter {
        int index = 0;
        int location = -1;
    };

    std::unique_ptr<QOpenGLShaderProgram> shader;
    int passIndex = -1;
    int requiredHistory = 0;
    int texture = -1;
    int inputSize = -1;
    int textureSize = -1;
    int outputSize = -1;
    int finalViewportSize = -1;
    int frameCount = -1;
    int frameDirection = -1;
    int mvp = -1;
    int spriteOwnership = -1;
    int originalAspect = -1;
    int originalAspectRotated = -1;
    int vertexCoord = -1;
    int texCoord = -1;
    int color = -1;
    int colorUpper = -1;
    int lutTexCoord = -1;
    std::vector<Frame> frames;
    std::vector<Lut> luts;
    std::vector<Parameter> parameters;
    std::vector<int> defaultSamplers; // Legacy sampler2D names sample unit zero.
};

namespace {
constexpr qint64 videoMemoryLimit = 128 * 1024 * 1024;
bool fail(QString& error, const QString& message) { error = message; return false; }

const QByteArray stockSource = R"(
#ifdef VERTEX
attribute vec4 VertexCoord;
attribute vec4 TexCoord;
uniform mat4 MVPMatrix;
varying vec2 tex_coord;
void main() { gl_Position = MVPMatrix * VertexCoord; tex_coord = TexCoord.xy; }
#elif defined(FRAGMENT)
uniform sampler2D Texture;
varying vec2 tex_coord;
void main() { gl_FragColor = vec4(texture2D(Texture, tex_coord).rgb, 1.0); }
#endif
)";
qint64 textureBytes(QSize size, int bytesPerPixel, bool mipmap) {
    qint64 bytes = qint64(size.width()) * size.height() * bytesPerPixel;
    while (mipmap && (size.width() > 1 || size.height() > 1)) {
        size = QSize(qMax(1, size.width() / 2), qMax(1, size.height() / 2));
        bytes += qint64(size.width()) * size.height() * bytesPerPixel;
    }
    return bytes;
}
unsigned passFormat(const ShaderPass& pass) {
    if (pass.floating) return GL_RGBA32F;
    return pass.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
}
} // namespace

ShaderPipeline::ShaderPipeline() : vertices_(QOpenGLBuffer::VertexBuffer) {}
ShaderPipeline::~ShaderPipeline() {
    if (texture_) glDeleteTextures(1, &texture_);
    if (!historyTextures_.empty()) glDeleteTextures(int(historyTextures_.size()), historyTextures_.data());
    if (!lutTextures_.empty()) glDeleteTextures(int(lutTextures_.size()), lutTextures_.data());
}

bool ShaderPipeline::compile(const QByteArray& source, std::unique_ptr<Program>& program,
                             QString& error, int passIndex) {
    QString code = QString::fromUtf8(source);
    const QRegularExpression version(QStringLiteral("^\\s*#version[^\\n]*"), QRegularExpression::MultilineOption);
    const auto match = version.match(code);
    const auto format = QOpenGLContext::currentContext()->format();
    const int glVersion = format.majorVersion() * 100 + format.minorVersion() * 10;
    std::vector<QByteArray> directives;
    if (match.hasMatch()) {
        directives.push_back(match.captured().trimmed().toUtf8());
    } else if (glVersion >= 300) {
        // RetroArch glcore starts at 130; newer compatibility GLSL accepts
        // shaders that use texture() or 420pack constructs without a #version.
        directives.push_back("#version 130");
        if (glVersion >= 420) directives.push_back("#version 420 compatibility");
        directives.push_back("#version 120");
    } else {
        directives.push_back("#version 120");
        directives.push_back("#version 110");
    }
    if (match.hasMatch()) code.remove(match.capturedStart(), match.capturedLength());
    code.remove(QRegularExpression(QStringLiteral("^\\s*#pragma\\s+parameter[^\\n]*"),
                                   QRegularExpression::MultilineOption));
    QByteArray definitions = "#define PARAMETER_UNIFORM\n#define _HAS_ORIGINALASPECT_UNIFORMS\n";
    for (const auto& pass : preset_.passes)
        if (!pass.alias.isEmpty()) definitions += "#define " + pass.alias.toUtf8() + "_ALIAS\n";

    auto next = std::make_unique<Program>();
    next->passIndex = passIndex;
    const QByteArray body = code.toUtf8();
    QString compileLog;
    for (const QByteArray& directive : directives) {
        auto shader = std::make_unique<QOpenGLShaderProgram>();
        if (shader->addShaderFromSourceCode(QOpenGLShader::Vertex,
                directive + "\n#define VERTEX\n" + definitions + "#line 1\n" + body) &&
            shader->addShaderFromSourceCode(QOpenGLShader::Fragment,
                directive + "\n#define FRAGMENT\n" + definitions + "#line 1\n" + body) &&
            shader->link()) {
            next->shader = std::move(shader);
            break;
        }
        compileLog = shader->log();
    }
    if (!next->shader) return fail(error, compileLog.left(3000));

    auto& shader = *next->shader;
    auto uniform = [&](const QByteArray& name) {
        int location = shader.uniformLocation(name.constData());
        if (location < 0) location = shader.uniformLocation((QByteArray("ruby") + name).constData());
        return location;
    };
    auto attribute = [&](const QByteArray& name) {
        int location = shader.attributeLocation(name.constData());
        if (location < 0) location = shader.attributeLocation((QByteArray("ruby") + name).constData());
        return location;
    };
    QSet<int> knownSamplers;
    next->texture = uniform("Texture");
    if (next->texture >= 0) knownSamplers.insert(next->texture);
    next->inputSize = uniform("InputSize");
    next->textureSize = uniform("TextureSize");
    next->outputSize = uniform("OutputSize");
    next->finalViewportSize = uniform("FinalViewportSize");
    next->frameCount = uniform("FrameCount");
    next->frameDirection = uniform("FrameDirection");
    next->mvp = uniform("MVPMatrix");
    next->spriteOwnership = uniform("HasSpriteOwnership");
    next->originalAspect = uniform("OriginalAspect");
    next->originalAspectRotated = uniform("OriginalAspectRotated");
    next->vertexCoord = attribute("VertexCoord");
    next->texCoord = attribute("TexCoord");
    next->color = attribute("Color");
    next->colorUpper = attribute("COLOR");
    next->lutTexCoord = attribute("LUTTexCoord");

    auto addFrame = [&](Program::Frame::Kind kind, int index, const QByteArray& prefix) {
        Program::Frame frame;
        frame.kind = kind;
        frame.index = index;
        frame.sampler = uniform(prefix + "Texture");
        frame.inputSize = uniform(prefix + "InputSize");
        frame.textureSize = uniform(prefix + "TextureSize");
        frame.texCoord = attribute(prefix + "TexCoord");
        if (frame.sampler >= 0) knownSamplers.insert(frame.sampler);
        if (frame.sampler >= 0 || frame.inputSize >= 0 ||
            frame.textureSize >= 0 || frame.texCoord >= 0) {
            if (kind == Program::Frame::Kind::Previous)
                next->requiredHistory = qMax(next->requiredHistory, index + 1);
            next->frames.push_back(frame);
        }
    };
    if (passIndex >= 0) {
        // A pass's alias, PassN and PassPrevN all name earlier pass outputs.
        addFrame(Program::Frame::Kind::Original, 0, "Orig");
        addFrame(Program::Frame::Kind::Original, 0,
                 "PassPrev" + QByteArray::number(passIndex + 1));
        addFrame(Program::Frame::Kind::Feedback, 0, "Feedback");
        for (int i = 0; i < passIndex; ++i) {
            addFrame(Program::Frame::Kind::Pass, i, "Pass" + QByteArray::number(i + 1));
            addFrame(Program::Frame::Kind::Pass, i,
                     "PassPrev" + QByteArray::number(passIndex - i));
            if (!preset_.passes[i].alias.isEmpty())
                addFrame(Program::Frame::Kind::Pass, i, preset_.passes[i].alias.toUtf8());
        }
        for (int i = 0; i < 32; ++i)
            addFrame(Program::Frame::Kind::Previous, i,
                     i ? "Prev" + QByteArray::number(i) : QByteArray("Prev"));
        for (int i = 0; i < preset_.textures.size(); ++i) {
            const int location = uniform(preset_.textures[i].id.toUtf8());
            if (location >= 0) {
                knownSamplers.insert(location);
                next->luts.push_back({i, location});
            }
        }
        for (int i = 0; i < preset_.parameters.size(); ++i) {
            const int location = uniform(preset_.parameters[i].id.toUtf8());
            if (location >= 0) next->parameters.push_back({i, location});
        }
    }
    int uniforms = 0, longest = 0;
    glGetProgramiv(shader.programId(), GL_ACTIVE_UNIFORMS, &uniforms);
    glGetProgramiv(shader.programId(), GL_ACTIVE_UNIFORM_MAX_LENGTH, &longest);
    std::vector<char> name(size_t(qMax(1, longest)));
    for (int i = 0; i < uniforms; ++i) {
        int length = 0, size = 0;
        unsigned type = 0;
        glGetActiveUniform(shader.programId(), unsigned(i), int(name.size()),
                           &length, &size, &type, name.data());
        const bool sampler = (type >= 0x8b5d && type <= 0x8b64) ||
                             (type >= 0x8dc0 && type <= 0x8dc5) ||
                             (type >= 0x8dc9 && type <= 0x8dd8) ||
                             (type >= 0x9108 && type <= 0x910d) ||
                             (type >= 0x900c && type <= 0x900f);
        if (!sampler) continue;
        const int location = shader.uniformLocation(name.data());
        if (type != GL_SAMPLER_2D || size != 1)
            return fail(error, QStringLiteral("Unsupported GLSL sampler type or array: %1.")
                                   .arg(QString::fromLatin1(name.data(), length)));
        if (!knownSamplers.contains(location) && location >= 0)
            next->defaultSamplers.push_back(location);
    }
    int maxUnits = 0;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxUnits);
    int usedUnits = 1 + int(next->luts.size());
    for (const auto& frame : next->frames) usedUnits += frame.sampler >= 0;
    if (usedUnits > maxUnits)
        return fail(error, QStringLiteral("Shader needs %1 texture units, but this GPU supports %2.")
                               .arg(usedUnits).arg(maxUnits));
    program = std::move(next);
    return true;
}

bool ShaderPipeline::initialize(const ShaderPreset& preset, QString& error) {
    initializeOpenGLFunctions();
    // Disabling GLSL keeps the stock display pass, also used for Blargg output.
    if ((!preset.path.isEmpty() && preset.passes.isEmpty()) || preset.passes.size() > ShaderPreset::maxPasses ||
        preset.feedbackPass < -1 || preset.feedbackPass >= preset.passes.size())
        return fail(error, QStringLiteral("Invalid GLSL pass count or feedback pass."));
    preset_ = preset;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture_);
    auto* context = QOpenGLContext::currentContext();
    canSrgb_ = context->format().majorVersion() >= 3 || context->hasExtension("GL_EXT_framebuffer_sRGB")
        || context->hasExtension("GL_ARB_framebuffer_sRGB");
    int history = 0;
    for (const ShaderPass& pass : preset.passes) {
        if (pass.srgb && !pass.floating && !canSrgb_)
            return fail(error, QStringLiteral("This GPU does not support sRGB framebuffer writes."));
        std::unique_ptr<Program> program;
        if (!compile(pass.source, program, error, int(programs_.size()))) {
            error = QStringLiteral("%1\n%2").arg(pass.path, error);
            return false;
        }
        history = qMax(history, program->requiredHistory);
        programs_.push_back(std::move(program));
        buffers_.push_back(nullptr);
    }
    if (!compile(stockSource, stock_, error, -1) || !loadTextures(error)) return false;
    // RetroArch presents an unscaled final shader in screen coordinates. Use
    // the same orientation in our final pass buffer; its blit then stays upright.
    // Explicit FBOs (including feedback) retain the intermediate texture layout.
    const int last = preset_.passes.size() - 1;
    lastPassFacesScreen_ = last >= 0 && !preset_.passes[last].explicitScale
        && preset_.feedbackPass != last && !preset_.passes[last].floating && !preset_.passes[last].srgb;
    // Texture-space position / display-space position. Texture coordinates
    // always use the first pair, also when a shader computes its own scene.
    constexpr float quad[] = {0,0, 0,1, 1,0, 1,1, 0,1, 0,0, 1,1, 1,0};
    if (!vertices_.create() || !vertices_.bind())
        return fail(error, QStringLiteral("Cannot create shader vertex buffer."));
    vertices_.allocate(quad, int(sizeof(quad)));
    vertices_.release();
    glGenTextures(1, &texture_);
    historyTextures_.resize(history);
    historySizes_.resize(history);
    if (history) glGenTextures(history, historyTextures_.data());
    return texture_ || fail(error, QStringLiteral("Cannot allocate the video input texture."));
}

bool ShaderPipeline::setParameter(const QString& id, float value) {
    if (!qIsFinite(value)) return false;
    for (auto& parameter : preset_.parameters) {
        if (parameter.id != id) continue;
        parameter.value = qBound(parameter.minimum, value, parameter.maximum);
        return true;
    }
    return false;
}

void ShaderPipeline::draw(Program& program, unsigned texture, const QSize& input,
                          const QSize& output, quint64 count, int direction, bool linear,
                          unsigned wrap, int hasSpriteOwnership, bool mipmap) {
    auto& shader = *program.shader;
    shader.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mipmap ? (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST)
                           : (linear ? GL_LINEAR : GL_NEAREST));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, int(wrap));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, int(wrap));
    if (program.texture >= 0) glUniform1i(program.texture, 0);
    for (int location : program.defaultSamplers) glUniform1i(location, 0);
    if (program.inputSize >= 0) glUniform2f(program.inputSize, float(input.width()), float(input.height()));
    if (program.textureSize >= 0) glUniform2f(program.textureSize, float(input.width()), float(input.height()));
    if (program.outputSize >= 0) glUniform2f(program.outputSize, float(output.width()), float(output.height()));
    if (program.finalViewportSize >= 0)
        glUniform2f(program.finalViewportSize, float(finalViewportSize_.width()), float(finalViewportSize_.height()));
    if (program.frameCount >= 0) glUniform1i(program.frameCount, int(count & 0x7fffffff));
    if (program.frameDirection >= 0) glUniform1i(program.frameDirection, direction);
    if (program.spriteOwnership >= 0 && hasSpriteOwnership >= 0)
        glUniform1i(program.spriteOwnership, hasSpriteOwnership);
    if (program.originalAspect >= 0 || program.originalAspectRotated >= 0) {
        const float aspect = originalAspect_;
        if (program.originalAspect >= 0) glUniform1f(program.originalAspect, aspect);
        if (program.originalAspectRotated >= 0) glUniform1f(program.originalAspectRotated, aspect);
    }
    if (program.mvp >= 0) {
        QMatrix4x4 matrix;
        matrix.ortho(0, 1, 0, 1, -1, 1);
        glUniformMatrix4fv(program.mvp, 1, GL_FALSE, matrix.constData());
    }
    for (const auto& parameter : program.parameters)
        glUniform1f(parameter.location, preset_.parameters[parameter.index].value);

    // LUT filtering/wrapping is established when the LUT is uploaded.
    int unit = 1;
    for (const auto& lut : program.luts) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, lutTextures_[size_t(lut.index)]);
        glUniform1i(lut.sampler, unit++);
    }
    for (const auto& frame : program.frames) {
        unsigned frameTexture = 0;
        QSize frameSize;
        switch (frame.kind) {
        case Program::Frame::Kind::Original:
            frameTexture = texture_;
            frameSize = textureSize_;
            break;
        case Program::Frame::Kind::Pass:
            if (size_t(frame.index) < buffers_.size() && buffers_[size_t(frame.index)]) {
                frameTexture = buffers_[size_t(frame.index)]->texture();
                frameSize = buffers_[size_t(frame.index)]->size();
            }
            break;
        case Program::Frame::Kind::Previous:
            if (size_t(frame.index) < historyTextures_.size() &&
                size_t(frame.index) < historySizes_.size()) {
                frameTexture = historyTextures_[size_t(frame.index)];
                frameSize = historySizes_[size_t(frame.index)];
            }
            break;
        case Program::Frame::Kind::Feedback:
            frameTexture = feedbackTexture_;
            frameSize = feedbackSize_;
            break;
        }
        if (frame.inputSize >= 0)
            glUniform2f(frame.inputSize, float(frameSize.width()), float(frameSize.height()));
        if (frame.textureSize >= 0)
            glUniform2f(frame.textureSize, float(frameSize.width()), float(frameSize.height()));
        if (frame.sampler >= 0) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, frameTexture);
            glUniform1i(frame.sampler, unit++);
        }
    }
    glActiveTexture(GL_TEXTURE0);

    vertices_.bind();
    auto bindCoord = [&](int location, int offset) {
        if (location < 0) return;
        shader.enableAttributeArray(location);
        shader.setAttributeBuffer(location, GL_FLOAT, offset * int(sizeof(float)),
                                  2, 4 * int(sizeof(float)));
    };
    const bool displayVertices = program.passIndex < 0 ? !lastPassFacesScreen_
        : lastPassFacesScreen_ && program.passIndex == preset_.passes.size() - 1;
    bindCoord(program.vertexCoord, displayVertices ? 2 : 0);
    bindCoord(program.texCoord, 0);
    bindCoord(program.lutTexCoord, 0);
    for (const auto& frame : program.frames) bindCoord(frame.texCoord, 0);
    if (program.color >= 0) shader.setAttributeValue(program.color, 1.f, 1.f, 1.f, 1.f);
    if (program.colorUpper >= 0) shader.setAttributeValue(program.colorUpper, 1.f, 1.f, 1.f, 1.f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (program.vertexCoord >= 0) shader.disableAttributeArray(program.vertexCoord);
    if (program.texCoord >= 0) shader.disableAttributeArray(program.texCoord);
    if (program.lutTexCoord >= 0) shader.disableAttributeArray(program.lutTexCoord);
    for (const auto& frame : program.frames)
        if (frame.texCoord >= 0) shader.disableAttributeArray(frame.texCoord);
    vertices_.release();
    shader.release();
}

bool ShaderPipeline::render(const QImage& frame, unsigned framebuffer, const QSize& surface,
                            const QRect& viewport, quint64 frameCount, int direction, QString& error,
                            const QImage& spriteOwnership, double originalAspect, const QSize& presentationSize) {
    if (frame.isNull() || viewport.isEmpty()) return true;
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
    if (frame.width() > maxTexture_ || frame.height() > maxTexture_)
        return fail(error, QStringLiteral("The video source exceeds the GPU texture size limit."));
    // A screenshot changes only the final blit. Keep viewport-dependent passes
    // and feedback buffers at their presentation sizes.
    finalViewportSize_ = presentationSize.isEmpty() ? viewport.size() : presentationSize;
    originalAspect_ = originalAspect > 0 ? float(originalAspect) : float(frame.width()) / frame.height();
    const bool reset = resetHistory_ || frameCount < lastFrame_;
    const bool advance = !reset && frameCount != lastFrame_;
    glActiveTexture(GL_TEXTURE0);
    if (reset) {
        // A yet-unwritten temporal input samples black, including immediately after changing games.
        const unsigned black = 0;
        for (size_t i = 0; i < historyTextures_.size(); ++i) {
            glBindTexture(GL_TEXTURE_2D, historyTextures_[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &black);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            historySizes_[i] = QSize(1, 1);
        }
        feedbackBuffer_.reset();
    } else if (advance && !historyTextures_.empty()) {
        // Rotate ownership: last frame becomes Prev, the oldest texture receives the new upload.
        std::rotate(historyTextures_.rbegin(), historyTextures_.rbegin() + 1, historyTextures_.rend());
        std::rotate(historySizes_.rbegin(), historySizes_.rbegin() + 1, historySizes_.rend());
        std::swap(texture_, historyTextures_.front());
        std::swap(textureSize_, historySizes_.front());
    }
    const bool inputMips = !preset_.passes.isEmpty() && preset_.passes.front().mipmap;
    std::array<QSize, ShaderPreset::maxPasses> outputs;
    QSize input = frame.size();
    qint64 videoBytes = lutBytes_ + textureBytes(input, 4, inputMips);
    for (QSize size : historySizes_) videoBytes += textureBytes(size, 4, inputMips);
    for (qsizetype i = 0; i < preset_.passes.size(); ++i) {
        const auto& pass = preset_.passes[i];
        auto dimension = [&](ShaderPass::Scale mode, double factor, int in, int out) {
            const double result = factor * (mode == ShaderPass::Scale::Absolute ? 1 :
                                           mode == ShaderPass::Scale::Viewport ? out : in);
            return result > maxTexture_ ? 0 : qMax(1, qRound(result));
        };
        const QSize output(dimension(pass.scaleX, pass.factorX, input.width(), finalViewportSize_.width()),
                           dimension(pass.scaleY, pass.factorY, input.height(), finalViewportSize_.height()));
        if (output.isEmpty())
            return fail(error, QStringLiteral("Shader pass %1 exceeds the GPU texture size limit.").arg(i + 1));
        const bool mipmap = i + 1 < preset_.passes.size() && preset_.passes[i + 1].mipmap;
        const qint64 targetBytes = textureBytes(output, pass.floating ? 16 : 4, mipmap);
        videoBytes += targetBytes * (i == preset_.feedbackPass ? 2 : 1);
        if (videoBytes > videoMemoryLimit)
            return fail(error, QStringLiteral("The video source and shader passes exceed the 128 MiB video memory budget."));
        outputs[size_t(i)] = output;
        input = output;
    }
    if (videoBytes > videoMemoryLimit)
        return fail(error, QStringLiteral("The video source exceeds the 128 MiB video memory budget."));
    // Release obsolete targets together so a resize cannot temporarily retain
    // large old targets alongside the new chain.
    for (qsizetype i = 0; i < preset_.passes.size(); ++i)
        if (buffers_[size_t(i)] && buffers_[size_t(i)]->size() != outputs[size_t(i)])
            buffers_[size_t(i)].reset();
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    // RGBA rows are naturally four-byte aligned. Restore the default explicitly:
    // Qt's grayscale glyph atlas uploads rely on it when the OSD is painted next.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const bool usesSpriteOwnership = preset_.usesSpriteOwnership();
    const bool hasSpriteOwnership = usesSpriteOwnership && spriteOwnership.size() == frame.size()
        && spriteOwnership.format() == QImage::Format_Grayscale8;
    // Like RetroArch, keep the top row at v=0 for source images, LUTs and intermediate
    // targets. The display pass selects screen-facing vertices. RGB32 normally uploads without a copy.
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    const bool uploadBgra = frame.format() == QImage::Format_RGB32;
#else
    const bool uploadBgra = false;
#endif
    QImage pixels = uploadBgra ? frame : frame.convertToFormat(QImage::Format_RGBA8888);
    const unsigned uploadFormat = uploadBgra ? GL_BGRA : GL_RGBA;
    if (hasSpriteOwnership) {
        auto* rgba = pixels.bits();
        const qsizetype stride = pixels.bytesPerLine();
        for (int y = 0; y < frame.height(); ++y) {
            auto* out = rgba + y * stride + 3;
            const auto* hint = spriteOwnership.constScanLine(y);
            for (int x = 0; x < frame.width(); ++x) out[x*4] = hint[x] ? 255 : 0;
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, int(pixels.bytesPerLine() / 4));
    if (inputMips) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
    if (textureSize_ != frame.size()) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame.width(), frame.height(), 0,
                     uploadFormat, GL_UNSIGNED_BYTE, pixels.constBits());
        textureSize_ = frame.size();
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width(), frame.height(),
                        uploadFormat, GL_UNSIGNED_BYTE, pixels.constBits());
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (inputMips) glGenerateMipmap(GL_TEXTURE_2D);
    const auto allocate = [](const ShaderPass& pass, QSize size) {
        QOpenGLFramebufferObjectFormat format;
        format.setInternalTextureFormat(passFormat(pass));
        return std::make_unique<QOpenGLFramebufferObject>(size, format);
    };
    if (preset_.feedbackPass >= 0) {
        const size_t index = size_t(preset_.feedbackPass);
        feedbackSize_ = outputs[index];
        if (!feedbackBuffer_ || feedbackBuffer_->size() != feedbackSize_) {
            feedbackBuffer_.reset();
            feedbackBuffer_ = allocate(preset_.passes[preset_.feedbackPass], feedbackSize_);
            if (!feedbackBuffer_->isValid() || !feedbackBuffer_->bind())
                return fail(error, QStringLiteral("Cannot allocate shader feedback buffer."));
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
        } else if (advance && buffers_[index]) {
            feedbackBuffer_.swap(buffers_[index]);
        }
        feedbackTexture_ = feedbackBuffer_->texture();
    }
    unsigned source = texture_;
    input = frame.size();
    glClearColor(0, 0, 0, 0);
    for (qsizetype i = 0; i < preset_.passes.size(); ++i) {
        const auto& pass = preset_.passes[i];
        const QSize output = outputs[size_t(i)];
        auto& buffer = buffers_[size_t(i)];
        if (!buffer) {
            buffer = allocate(pass, output);
            if (!buffer->isValid())
                return fail(error, QStringLiteral("GPU cannot create the requested framebuffer for pass %1.").arg(i + 1));
        }
        buffer->bind();
        // Passes may discard fragments or draw a smaller quad. Never expose a previous frame there.
        glClear(GL_COLOR_BUFFER_BIT);
        if (pass.srgb && !pass.floating) glEnable(GL_FRAMEBUFFER_SRGB);
        glViewport(0, 0, output.width(), output.height());
        draw(*programs_[size_t(i)], source, input, output,
             pass.frameCountMod ? frameCount % pass.frameCountMod : frameCount, direction, pass.linear, pass.wrap,
             usesSpriteOwnership && i == 0 ? int(hasSpriteOwnership) : -1, pass.mipmap);
        if (pass.srgb && !pass.floating) glDisable(GL_FRAMEBUFFER_SRGB);
        source = buffer->texture();
        if (i + 1 < preset_.passes.size() && preset_.passes[i + 1].mipmap) {
            glBindTexture(GL_TEXTURE_2D, source);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        input = output;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, surface.width(), surface.height());
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(viewport.x(), surface.height() - viewport.y() - viewport.height(), viewport.width(), viewport.height());
    draw(*stock_, source, input, viewport.size(), frameCount, direction, false, GL_CLAMP_TO_EDGE);
    const unsigned glError = glGetError();
    if (glError != GL_NO_ERROR)
        return fail(error, QStringLiteral("OpenGL shader rendering failed (0x%1).").arg(glError, 0, 16));
    lastFrame_ = frameCount;
    resetHistory_ = false;
    return true;
}

bool ShaderPipeline::loadTextures(QString& error) {
    lutTextures_.resize(preset_.textures.size());
    if (!lutTextures_.empty()) glGenTextures(int(lutTextures_.size()), lutTextures_.data());
    for (qsizetype i = 0; i < preset_.textures.size(); ++i) {
        const auto& lut = preset_.textures[i];
        QImageReader reader(lut.path);
        const QSize size = reader.size();
        if (size.isEmpty() || size.width() > maxTexture_ || size.height() > maxTexture_)
            return fail(error, QStringLiteral("Invalid or oversized shader texture: %1").arg(lut.path));
        lutBytes_ += textureBytes(size, 4, lut.mipmap);
        if (lutBytes_ > videoMemoryLimit)
            return fail(error, QStringLiteral("Shader textures exceed the 128 MiB video memory budget."));
        const QImage pixels = reader.read().convertToFormat(QImage::Format_RGBA8888);
        if (pixels.size() != size)
            return fail(error, QStringLiteral("Cannot decode shader texture %1: %2").arg(lut.path, reader.errorString()));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, lutTextures_[size_t(i)]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width(), size.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels.constBits());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, int(lut.wrap));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, int(lut.wrap));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, lut.linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, lut.mipmap
            ? (lut.linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST)
            : (lut.linear ? GL_LINEAR : GL_NEAREST));
        if (lut.mipmap) glGenerateMipmap(GL_TEXTURE_2D);
        if (glGetError() != GL_NO_ERROR)
            return fail(error, QStringLiteral("Cannot upload shader texture: %1").arg(lut.path));
    }
    return true;
}
