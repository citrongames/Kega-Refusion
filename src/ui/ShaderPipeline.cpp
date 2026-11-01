#include "ShaderPipeline.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMatrix4x4>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QRegularExpression>
#include <QSet>
#include <QVector2D>
#include <QtMath>
#include <cmath>

namespace {
constexpr qsizetype sourceLimit = 512 * 1024;
constexpr int passLimit = 8;
bool fail(QString& error, const QString& message) { error = message; return false; }

QString relativePath(const QString& parent, QString path) {
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.startsWith(QLatin1Char(':')) || QDir::isAbsolutePath(path)) return path;
    return QDir::cleanPath(QFileInfo(parent).path() + QLatin1Char('/') + path);
}

bool readSource(const QString& path, QByteArray& result, QString& error, int depth = 0) {
    if (depth > 12) return fail(error, QStringLiteral("Too many shader #include levels."));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("Cannot read shader: %1").arg(path));
    if (file.size() > sourceLimit)
        return fail(error, QStringLiteral("Shader exceeds the 512 KiB limit: %1").arg(path));
    QByteArray source = file.readAll();
    if (source.startsWith("\xef\xbb\xbf")) source.remove(0, 3);
    const QRegularExpression include(QStringLiteral("^\\s*#include\\s+[\"<]([^\">]+)[\">]\\s*$"));
    for (const QByteArray& line : source.split('\n')) {
        const auto match = include.match(QString::fromUtf8(line));
        if (match.hasMatch()) {
            QByteArray nested;
            if (!readSource(relativePath(path, match.captured(1)), nested, error, depth + 1))
                return false;
            result += nested;
        } else {
            result += line + '\n';
        }
        if (result.size() > sourceLimit)
            return fail(error, QStringLiteral("Expanded shader exceeds the 512 KiB limit."));
    }
    return true;
}

bool boolean(const QString& value, bool& result) {
    if (value == QStringLiteral("true") || value == QStringLiteral("1")) { result = true; return true; }
    if (value == QStringLiteral("false") || value == QStringLiteral("0")) { result = false; return true; }
    return false;
}

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
} // namespace

bool ShaderPreset::load(const QString& path, ShaderPreset& result, QString& error) {
    error.clear();
    ShaderPreset preset;
    preset.path = path;
    const QString suffix = QFileInfo(path).suffix().toLower();
    QMap<QString, QString> values;
    QSet<QString> consumed;
    if (suffix == QStringLiteral("glslp")) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > sourceLimit)
            return fail(error, QStringLiteral("Cannot read GLSL preset: %1").arg(path));
        const QString text = QString::fromUtf8(file.readAll());
        const QRegularExpression assignment(QStringLiteral("^\\s*([A-Za-z_][A-Za-z_0-9]*)\\s*=\\s*(?:\"([^\"]*)\"|([^#]*?))\\s*(?:#.*)?$"));
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            if (line.trimmed().isEmpty()) continue;
            if (line.trimmed().startsWith(QStringLiteral("#reference")) ||
                line.trimmed().startsWith(QStringLiteral("#include")))
                return fail(error, QStringLiteral("Preset references/includes are not supported. Load a complete .glslp preset."));
            if (line.trimmed().startsWith(QLatin1Char('#'))) continue;
            const auto match = assignment.match(line);
            if (!match.hasMatch())
                return fail(error, QStringLiteral("Invalid preset line: %1").arg(line.left(100)));
            values.insert(match.captured(1), match.captured(2).isNull()
                                            ? match.captured(3).trimmed() : match.captured(2));
        }
    } else if (suffix != QStringLiteral("glsl")) {
        return fail(error, QStringLiteral("Only GLSL .glsl and .glslp files are supported. Slang and Cg require a different renderer."));
    }
    int count = 1;
    if (suffix == QStringLiteral("glslp")) {
        bool okay = false;
        count = values.value(QStringLiteral("shaders")).toInt(&okay);
        if (!okay || count < 1 || count > passLimit)
            return fail(error, QStringLiteral("The GLSL preset must contain 1–8 shader passes."));
        consumed.insert(QStringLiteral("shaders"));
    }
    for (int i = 0; i < count; ++i) {
        ShaderPass pass;
        const QString index = QString::number(i);
        const QString shaderKey = QStringLiteral("shader") + index;
        pass.path = suffix == QStringLiteral("glsl") ? path : relativePath(path, values.value(shaderKey));
        consumed.insert(shaderKey);
        if (QFileInfo(pass.path).suffix().compare(QStringLiteral("glsl"), Qt::CaseInsensitive) != 0)
            return fail(error, QStringLiteral("Pass %1 must name a .glsl shader.").arg(i + 1));
        if (!readSource(pass.path, pass.source, error)) return false;
        // Temporal history, LUTs and cross-pass samplers cannot silently become Texture.
        const QRegularExpression advanced(QStringLiteral("\\b(?:Prev[0-9]*Texture|PREV[0-9]*|OrigTexture|Pass[0-9]+Texture|PassPrev[0-9]+Texture)\\b"));
        if (advanced.match(QString::fromUtf8(pass.source)).hasMatch())
            return fail(error, QStringLiteral("Pass %1 uses history/original/cross-pass inputs, which this GLSL renderer does not support.").arg(i + 1));
        auto take = [&](const QString& prefix) {
            const QString key = prefix + index;
            consumed.insert(key);
            return values.value(key);
        };
        const QString filter = take(QStringLiteral("filter_linear"));
        if (!filter.isEmpty() && !boolean(filter, pass.linear))
            return fail(error, QStringLiteral("Invalid filter_linear%1.").arg(i));
        for (const QString& feature : {QStringLiteral("float_framebuffer"), QStringLiteral("srgb_framebuffer"),
                                      QStringLiteral("mipmap_input")}) {
            const QString value = take(feature);
            bool enabled = false;
            if (!value.isEmpty() && (!boolean(value, enabled) || enabled))
                return fail(error, QStringLiteral("%1%2 is not supported.").arg(feature, index));
        }
        const QString alias = take(QStringLiteral("alias"));
        if (!alias.isEmpty())
            return fail(error, QStringLiteral("Shader pass aliases are not supported."));
        const QString wrap = take(QStringLiteral("wrap_mode"));
        if (wrap == QStringLiteral("repeat")) pass.wrap = GL_REPEAT;
        else if (wrap == QStringLiteral("mirrored_repeat")) pass.wrap = GL_MIRRORED_REPEAT;
        else if (wrap == QStringLiteral("clamp_to_border")) pass.wrap = GL_CLAMP_TO_BORDER;
        else if (!wrap.isEmpty() && wrap != QStringLiteral("clamp_to_edge"))
            return fail(error, QStringLiteral("Unsupported texture wrap mode: %1").arg(wrap));
        const QString mod = take(QStringLiteral("frame_count_mod"));
        if (!mod.isEmpty()) {
            bool okay = false;
            pass.frameCountMod = mod.toUInt(&okay);
            if (!okay || !pass.frameCountMod)
                return fail(error, QStringLiteral("Invalid frame_count_mod%1.").arg(i));
        }
        const QString commonType = take(QStringLiteral("scale_type"));
        const QString commonScale = take(QStringLiteral("scale"));
        const QString xType = take(QStringLiteral("scale_type_x"));
        const QString yType = take(QStringLiteral("scale_type_y"));
        const QString xScale = take(QStringLiteral("scale_x"));
        const QString yScale = take(QStringLiteral("scale_y"));
        const bool scaled = !commonType.isEmpty() || !commonScale.isEmpty() ||
                            !xType.isEmpty() || !yType.isEmpty() || !xScale.isEmpty() || !yScale.isEmpty();
        auto scale = [&](QString type, QString value, ShaderPass::Scale& mode, double& factor) {
            if (type.isEmpty()) type = (i == count - 1 && !scaled)
                                       ? QStringLiteral("viewport") : QStringLiteral("source");
            if (type == QStringLiteral("source")) mode = ShaderPass::Scale::Source;
            else if (type == QStringLiteral("viewport")) mode = ShaderPass::Scale::Viewport;
            else if (type == QStringLiteral("absolute")) mode = ShaderPass::Scale::Absolute;
            else return false;
            if (!value.isEmpty()) {
                bool okay = false;
                factor = value.toDouble(&okay);
                if (!okay || !std::isfinite(factor) || factor <= 0 || factor > 16384) return false;
            }
            return true;
        };
        if (!scale(commonType.isEmpty() ? xType : commonType,
                   commonScale.isEmpty() ? xScale : commonScale, pass.scaleX, pass.factorX) ||
            !scale(commonType.isEmpty() ? yType : commonType,
                   commonScale.isEmpty() ? yScale : commonScale, pass.scaleY, pass.factorY))
            return fail(error, QStringLiteral("Invalid scaling in shader pass %1.").arg(i + 1));
        const QRegularExpression param(QStringLiteral(
            "^\\s*#pragma\\s+parameter\\s+(\\w+)\\s+\"([^\"]+)\"\\s+([-+0-9.eE]+)\\s+([-+0-9.eE]+)\\s+([-+0-9.eE]+)(?:\\s+([-+0-9.eE]+))?"),
            QRegularExpression::MultilineOption);
        auto matches = param.globalMatch(QString::fromUtf8(pass.source));
        while (matches.hasNext()) {
            const auto match = matches.next();
            const QString id = match.captured(1);
            bool duplicate = false;
            for (const auto& existing : preset.parameters) duplicate |= existing.id == id;
            if (duplicate) continue;
            ShaderParameter p;
            p.id = id; p.label = match.captured(2);
            bool okay = false;
            p.minimum = match.captured(4).toFloat(&okay);
            if (!okay || !qIsFinite(p.minimum)) return fail(error, QStringLiteral("Invalid shader parameter range."));
            p.maximum = match.captured(5).toFloat(&okay);
            if (!okay || !qIsFinite(p.maximum) || p.maximum < p.minimum) return fail(error, QStringLiteral("Invalid shader parameter range."));
            p.step = match.captured(6).isEmpty() ? 0.01f : match.captured(6).toFloat(&okay);
            if (!okay || !qIsFinite(p.step) || p.step <= 0) return fail(error, QStringLiteral("Invalid shader parameter step."));
            p.value = values.value(id, match.captured(3)).toFloat(&okay);
            if (!okay || !qIsFinite(p.value)) return fail(error, QStringLiteral("Invalid shader parameter: %1").arg(id));
            p.value = qBound(p.minimum, p.value, p.maximum);
            consumed.insert(id);
            preset.parameters.append(p);
        }
        preset.passes.append(pass);
    }
    consumed.insert(QStringLiteral("parameters"));
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        if (!consumed.contains(it.key()))
            return fail(error, QStringLiteral("Unsupported GLSL preset option: %1").arg(it.key()));
    result = std::move(preset);
    return true;
}

ShaderPipeline::ShaderPipeline() : vertices_(QOpenGLBuffer::VertexBuffer) {}
ShaderPipeline::~ShaderPipeline() {
    if (texture_) glDeleteTextures(1, &texture_);
}

bool ShaderPipeline::compile(const QByteArray& source, std::unique_ptr<QOpenGLShaderProgram>& program,
                             QString& error) {
    QString code = QString::fromUtf8(source);
    const QRegularExpression version(QStringLiteral("^\\s*#version[^\\n]*"), QRegularExpression::MultilineOption);
    const auto match = version.match(code);
    const QByteArray directive = match.hasMatch() ? match.captured().trimmed().toUtf8() : QByteArray("#version 120");
    if (match.hasMatch()) code.remove(match.capturedStart(), match.capturedLength());
    code.remove(QRegularExpression(QStringLiteral("^\\s*#pragma\\s+parameter[^\\n]*"),
                                   QRegularExpression::MultilineOption));
    auto next = std::make_unique<QOpenGLShaderProgram>();
    const QByteArray body = code.toUtf8();
    if (!next->addShaderFromSourceCode(QOpenGLShader::Vertex,
            directive + "\n#define VERTEX\n#define PARAMETER_UNIFORM\n#line 1\n" + body) ||
        !next->addShaderFromSourceCode(QOpenGLShader::Fragment,
            directive + "\n#define FRAGMENT\n#define PARAMETER_UNIFORM\n#line 1\n" + body) ||
        !next->link())
        return fail(error, next->log().left(3000));
    int uniforms = 0;
    glGetProgramiv(next->programId(), GL_ACTIVE_UNIFORMS, &uniforms);
    for (int i = 0; i < uniforms; ++i) {
        char name[512] = {};
        int length = 0, size = 0;
        unsigned type = 0;
        glGetActiveUniform(next->programId(), unsigned(i), int(sizeof(name)), &length, &size, &type, name);
        // GLSL sampler type ranges, including shadow/array/integer samplers.
        const bool sampler = (type >= 0x8b5d && type <= 0x8b64) ||
                             (type >= 0x8dc0 && type <= 0x8dc5) ||
                             (type >= 0x8dc9 && type <= 0x8dd8) ||
                             (type >= 0x9108 && type <= 0x910d) ||
                             type == 0x900c || type == 0x900d || type == 0x900e || type == 0x900f;
        if (sampler && (QByteArray(name) != "Texture" || size != 1 || type != GL_SAMPLER_2D))
            return fail(error, QStringLiteral("Unsupported GLSL sampler %1. Only the current pass Texture (sampler2D) is supported.")
                                   .arg(QString::fromLatin1(name, length)));
    }
    program = std::move(next);
    return true;
}

bool ShaderPipeline::initialize(const ShaderPreset& preset, QString& error) {
    initializeOpenGLFunctions();
    preset_ = preset;
    for (const ShaderPass& pass : preset.passes) {
        std::unique_ptr<QOpenGLShaderProgram> program;
        if (!compile(pass.source, program, error)) {
            error = QStringLiteral("%1\n%2").arg(pass.path, error);
            return false;
        }
        programs_.push_back(std::move(program));
        buffers_.push_back(nullptr);
    }
    if (!compile(stockSource, stock_, error)) return false;
    // Each vertex contains position and texture coordinate; RGBA and alpha survive every FBO pass.
    constexpr float quad[] = {0,0, 0,0, 1,0, 1,0, 0,1, 0,1, 1,1, 1,1};
    if (!vertices_.create() || !vertices_.bind())
        return fail(error, QStringLiteral("Cannot create shader vertex buffer."));
    vertices_.allocate(quad, int(sizeof(quad)));
    vertices_.release();
    glGenTextures(1, &texture_);
    if (!texture_) return fail(error, QStringLiteral("Cannot allocate the video input texture."));
    return true;
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

void ShaderPipeline::draw(QOpenGLShaderProgram& program, unsigned texture, const QSize& input,
                          const QSize& output, quint64 count, int direction, bool linear, unsigned wrap) {
    program.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, int(wrap));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, int(wrap));
    program.setUniformValue("Texture", 0);
    program.setUniformValue("InputSize", QVector2D(input.width(), input.height()));
    program.setUniformValue("TextureSize", QVector2D(input.width(), input.height()));
    program.setUniformValue("OutputSize", QVector2D(output.width(), output.height()));
    program.setUniformValue("FrameCount", int(count & 0x7fffffff));
    program.setUniformValue("FrameDirection", direction);
    QMatrix4x4 matrix;
    matrix.ortho(0, 1, 0, 1, -1, 1);
    program.setUniformValue("MVPMatrix", matrix);
    for (const auto& parameter : preset_.parameters)
        program.setUniformValue(parameter.id.toUtf8().constData(), parameter.value);
    vertices_.bind();
    for (const auto& attribute : {std::pair<const char*,int>{"VertexCoord",0}, {"TexCoord",2}}) {
        const int location = program.attributeLocation(attribute.first);
        if (location >= 0) {
            program.enableAttributeArray(location);
            program.setAttributeBuffer(location, GL_FLOAT, attribute.second * int(sizeof(float)), 2, 4 * int(sizeof(float)));
        }
    }
    const int color = program.attributeLocation("COLOR");
    if (color >= 0) program.setAttributeValue(color, 1.f, 1.f, 1.f, 1.f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    for (const char* name : {"VertexCoord", "TexCoord"}) {
        const int location = program.attributeLocation(name);
        if (location >= 0) program.disableAttributeArray(location);
    }
    vertices_.release();
    program.release();
}

bool ShaderPipeline::render(const QImage& frame, unsigned framebuffer, const QSize& surface,
                            const QRect& viewport, quint64 frameCount, int direction, QString& error) {
    if (frame.isNull() || viewport.isEmpty()) return true;
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    // RGBA rows are naturally four-byte aligned. Restore the default explicitly:
    // Qt's grayscale glyph atlas uploads rely on it when the OSD is painted next.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    // QImage starts at the top; FBOs and every GLSL pass use OpenGL's bottom-left origin.
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    const QImage pixels = frame.convertToFormat(QImage::Format_RGBA8888).flipped(Qt::Vertical);
#else
    const QImage pixels = frame.convertToFormat(QImage::Format_RGBA8888).mirrored();
#endif
    if (textureSize_ != frame.size()) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame.width(), frame.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels.constBits());
        textureSize_ = frame.size();
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width(), frame.height(),
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels.constBits());
    }
    int maxTexture = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
    unsigned source = texture_;
    QSize input = frame.size();
    for (qsizetype i = 0; i < preset_.passes.size(); ++i) {
        const auto& pass = preset_.passes[i];
        auto dimension = [&](ShaderPass::Scale mode, double factor, int in, int out) {
            const double result = factor * (mode == ShaderPass::Scale::Absolute ? 1 :
                                           mode == ShaderPass::Scale::Viewport ? out : in);
            return result > maxTexture ? 0 : qMax(1, qRound(result));
        };
        const QSize output(dimension(pass.scaleX, pass.factorX, input.width(), viewport.width()),
                           dimension(pass.scaleY, pass.factorY, input.height(), viewport.height()));
        // Bound resource use even if a user preset requests many viewport-sized passes.
        if (output.isEmpty() || qint64(output.width()) * output.height() > 32 * 1024 * 1024)
            return fail(error, QStringLiteral("Shader pass %1 exceeds the GPU texture size limit.").arg(i + 1));
        auto& buffer = buffers_[size_t(i)];
        if (!buffer || buffer->size() != output) {
            QOpenGLFramebufferObjectFormat format;
            format.setInternalTextureFormat(GL_RGBA8);
            buffer = std::make_unique<QOpenGLFramebufferObject>(output, format);
            if (!buffer->isValid())
                return fail(error, QStringLiteral("Cannot allocate framebuffer for shader pass %1.").arg(i + 1));
        }
        buffer->bind();
        glViewport(0, 0, output.width(), output.height());
        draw(*programs_[size_t(i)], source, input, output,
             pass.frameCountMod ? frameCount % pass.frameCountMod : frameCount, direction, pass.linear, pass.wrap);
        source = buffer->texture();
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
    return true;
}
