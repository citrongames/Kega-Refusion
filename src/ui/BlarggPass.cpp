#include "BlarggPass.h"
#include "BlarggKernel.h"

#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QtEndian>

struct BlarggPass::Program {
    std::unique_ptr<QOpenGLShaderProgram> shader;
    int source = -1, lookup = -1, inputSize = -1;
};

namespace {
constexpr int lookupWidth = 2048;

QByteArray fragmentBody(bool h40) {
    QByteArray code = R"(
uniform sampler2D Source;
uniform sampler2D Lookup;
uniform vec2 InputSize;

// Each texel is one exact uint32 Blargg contribution, least significant
// byte first. Byte sums and carries stay integral in highp float, including
// on GLES 2 without integer textures or bitwise GLSL operators.
vec4 contribution(float sourceX, float sourceY, float phase) {
    // The common RGB frame no longer has a core palette backdrop. Repeat
    // each row's edge samples instead of adding a synthetic black border.
    sourceX = clamp(sourceX, 0.0, InputSize.x - 1.0);
    vec3 color = SAMPLE(Source, vec2((sourceX + 0.5) / InputSize.x,
                                   (sourceY + 0.5) / InputSize.y)).rgb;
    vec3 rgb = floor(color * vec3(31.0, 63.0, 31.0) + 0.5);
    float index = (rgb.r * 2048.0 + rgb.g * 32.0 + rgb.b) * ENTRY_SIZE + phase;
    vec2 uv = vec2((mod(index, 2048.0) + 0.5) / 2048.0,
                   (floor(index / 2048.0) + 0.5) / LOOKUP_HEIGHT);
    return floor(SAMPLE(Lookup, uv) * 255.0 + 0.5);
}
void main() {
    float x = floor(gl_FragCoord.x);
    float y = floor(gl_FragCoord.y);
)";
    if (h40) {
        code += R"(
    float phase = mod(x, 8.0);
    float base = floor(x / 8.0) * 4.0;
    float p0 = base + 3.0;
    float p1 = base + mix(0.0, 4.0, step(2.0, phase));
    float p2 = base + mix(1.0, 5.0, step(4.0, phase));
    float p3 = base + mix(2.0, 6.0, step(6.0, phase));
    vec4 raw = contribution(p0, y, phase)
        + contribution(p1, y, mod(phase+6.0, 8.0)+16.0)
        + contribution(p2, y, mod(phase+4.0, 8.0))
        + contribution(p3, y, mod(phase+2.0, 8.0)+16.0)
        + contribution(p0-4.0, y, phase+8.0)
        + contribution(p1-4.0, y, mod(phase+6.0, 8.0)+24.0)
        + contribution(p2-4.0, y, mod(phase+4.0, 8.0)+8.0)
        + contribution(p3-4.0, y, mod(phase+2.0, 8.0)+24.0);
)";
    } else {
        code += R"(
    float phase = mod(x, 7.0);
    float base = floor(x / 7.0) * 3.0 + mod(InputSize.x, 3.0);
    float p0 = base;
    float p1 = base + mix(-2.0, 1.0, step(2.0, phase));
    float p2 = base + mix(-1.0, 2.0, step(4.0, phase));
    vec4 raw = contribution(p0, y, phase)
        + contribution(p1, y, mod(phase+12.0, 7.0)+14.0)
        + contribution(p2, y, mod(phase+10.0, 7.0)+28.0)
        + contribution(p0-3.0, y, mod(phase+7.0, 14.0))
        + contribution(p1-3.0, y, mod(phase+5.0, 7.0)+21.0)
        + contribution(p2-3.0, y, mod(phase+3.0, 7.0)+35.0);
)";
    }
    code += R"(
    raw.g += floor(raw.r / 256.0); raw.r = mod(raw.r, 256.0);
    raw.b += floor(raw.g / 256.0); raw.g = mod(raw.g, 256.0);
    raw.a += floor(raw.b / 256.0); raw.b = mod(raw.b, 256.0);
    raw.a = mod(raw.a, 256.0);
    // Error correction leaves each channel biased by 512. Decode the
    // ten-bit signed ranges before saturation, exactly as NTSC_CLAMP_.
    vec3 rgb = vec3(mod(floor(raw.b/32.0) + raw.a*8.0, 1024.0),
                    mod(floor(raw.g/8.0) + raw.b*32.0, 1024.0),
                    mod(floor(raw.r/2.0) + raw.g*128.0, 1024.0)) - 512.0;
    rgb = floor(clamp(rgb, 0.0, 255.0) / vec3(8.0, 4.0, 8.0));
    // Bit replication matches the native RGB565 -> RGB32 conversion.
    rgb = rgb * vec3(8.0, 4.0, 8.0) + floor(rgb / vec3(4.0, 16.0, 4.0));
    OUTPUT = vec4(rgb / 255.0, 1.0);
}
)";
    code.prepend(h40 ? "#define ENTRY_SIZE 32.0\n#define LOOKUP_HEIGHT 1024.0\n"
                     : "#define ENTRY_SIZE 42.0\n#define LOOKUP_HEIGHT 1344.0\n");
    return code;
}
}

BlarggPass::BlarggPass() : vertices_(QOpenGLBuffer::VertexBuffer) {}
BlarggPass::~BlarggPass() { releaseImages(); }

void BlarggPass::releaseImages() {
    for (unsigned& texture : lookup_) {
        if (texture) glDeleteTextures(1, &texture);
        texture = 0;
    }
    lookupPreset_.fill(0);
    output_.reset();
}

bool BlarggPass::initialize(QString& error) {
    initializeOpenGLFunctions();
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture_);
    if (maxTexture_ < lookupWidth) {
        error = QStringLiteral("Blargg needs 2048-pixel lookup textures.");
        return false;
    }
    const auto* context = QOpenGLContext::currentContext();
    const auto format = context->format();
    const bool es = context->isOpenGLES();
    const bool modern = es ? format.majorVersion() >= 3
                           : format.profile() == QSurfaceFormat::CoreProfile;
    QByteArray version = es ? (modern ? "#version 300 es\n" : "#version 100\n")
                            : (modern ? "#version 150\n" : "#version 120\n");
    QByteArray precision = es ? "precision highp float;\nprecision highp int;\n" : "";
    const QByteArray vertex = version + precision
        + (modern ? "in vec2 Position;\n" : "attribute vec2 Position;\n")
        + "void main() { gl_Position = vec4(Position * 2.0 - 1.0, 0.0, 1.0); }\n";
    for (int mode = 0; mode < 2; ++mode) {
        auto program = std::make_unique<Program>();
        program->shader = std::make_unique<QOpenGLShaderProgram>();
        const QByteArray fragment = version + precision
            + (modern ? "out vec4 FragmentColor;\n#define OUTPUT FragmentColor\n#define SAMPLE texture\n"
                      : "#define OUTPUT gl_FragColor\n#define SAMPLE texture2D\n")
            + fragmentBody(mode == 1);
        auto& shader = *program->shader;
        shader.bindAttributeLocation("Position", 0);
        if (!shader.addShaderFromSourceCode(QOpenGLShader::Vertex, vertex)
            || !shader.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment)
            || !shader.link()) {
            error = QStringLiteral("Blargg GPU shader: %1").arg(shader.log().left(3000));
            return false;
        }
        program->source = shader.uniformLocation("Source");
        program->lookup = shader.uniformLocation("Lookup");
        program->inputSize = shader.uniformLocation("InputSize");
        programs_[mode] = std::move(program);
    }
    if (!es && modern && !vertexArray_.create()) {
        error = QStringLiteral("Cannot create Blargg vertex array.");
        return false;
    }
    constexpr float quad[] = {0,0, 1,0, 0,1, 1,1};
    if (!vertices_.create() || !vertices_.bind()) {
        error = QStringLiteral("Cannot create Blargg vertex buffer.");
        return false;
    }
    vertices_.allocate(quad, int(sizeof(quad)));
    vertices_.release();
    return true;
}

bool BlarggPass::prepareKernel(int preset, bool h40, QString& error) {
    const int mode = h40 ? 1 : 0;
    if (lookup_[mode] && lookupPreset_[mode] == preset) return true;
    auto table = BlarggKernel::generate(preset, h40);
    if (table.empty()) {
        error = QStringLiteral("Invalid Blargg preset.");
        return false;
    }
    for (auto& word : table) word = qToLittleEndian(word);
    if (!lookup_[mode]) glGenTextures(1, &lookup_[mode]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, lookup_[mode]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, lookupWidth,
                 int(table.size() / lookupWidth), 0, GL_RGBA, GL_UNSIGNED_BYTE, table.data());
    const unsigned status = glGetError();
    glActiveTexture(GL_TEXTURE0);
    if (status != GL_NO_ERROR) {
        error = QStringLiteral("Cannot upload Blargg lookup texture (OpenGL %1).").arg(status);
        return false;
    }
    lookupPreset_[mode] = preset;
    return true;
}

bool BlarggPass::render(unsigned inputTexture, const QSize& inputSize, int preset,
                       bool h40, QString& error) {
    const int mode = h40 ? 1 : 0;
    const QSize outputSize(BlarggKernel::outputWidth(inputSize.width(), h40), inputSize.height());
    if (!programs_[mode] || !inputTexture || inputSize.isEmpty()
        || outputSize.width() > maxTexture_ || outputSize.height() > maxTexture_) {
        error = QStringLiteral("Invalid Blargg input size or uninitialized GPU filter.");
        return false;
    }
    if (!prepareKernel(preset, h40, error)) return false;
    if (!output_ || output_->size() != outputSize) {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        format.setInternalTextureFormat(QOpenGLContext::currentContext()->isOpenGLES()
                                            ? GL_RGBA : GL_RGBA8);
        output_ = std::make_unique<QOpenGLFramebufferObject>(outputSize, format);
        if (!output_->isValid()) {
            error = QStringLiteral("Cannot create Blargg framebuffer.");
            output_.reset();
            return false;
        }
    }
    output_->bind();
    glViewport(0, 0, outputSize.width(), outputSize.height());
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    auto& program = *programs_[mode];
    program.shader->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, lookup_[mode]);
    glUniform1i(program.source, 0);
    glUniform1i(program.lookup, 1);
    glUniform2f(program.inputSize, float(inputSize.width()), float(inputSize.height()));
    if (vertexArray_.isCreated()) vertexArray_.bind();
    vertices_.bind();
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * int(sizeof(float)), nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    vertices_.release();
    if (vertexArray_.isCreated()) vertexArray_.release();
    program.shader->release();
    glActiveTexture(GL_TEXTURE0);
    return true;
}

unsigned BlarggPass::texture() const { return output_ ? output_->texture() : 0; }
QSize BlarggPass::size() const { return output_ ? output_->size() : QSize(); }
