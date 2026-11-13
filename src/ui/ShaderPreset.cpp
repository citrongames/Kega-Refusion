#include "ShaderPipeline.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QtMath>
#include <cmath>
#include <limits>

namespace {
constexpr qsizetype sourceLimit = 512 * 1024;
constexpr int lutLimit = 32;
constexpr qsizetype presetReferenceByteLimit = 2 * 1024 * 1024;
constexpr int presetReferenceLimit = 64;
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

struct PresetValue {
    QString text;
    QString origin;
};
using PresetValues = QMap<QString, PresetValue>;
struct PresetReadBudget {
    qsizetype bytes = 0;
    int files = 0;
};

bool readPresetValues(const QString& path, PresetValues& values, QSet<QString>& active,
                      PresetReadBudget& budget, QString& error, int depth = 0) {
    if (depth > 16)
        return fail(error, QStringLiteral("Too many preset #reference levels."));
    if (++budget.files > presetReferenceLimit)
        return fail(error, QStringLiteral("Too many preset #references."));
    const QString identity = QFileInfo(path).canonicalFilePath();
    if (active.contains(identity))
        return fail(error, QStringLiteral("Recursive preset #reference: %1").arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > sourceLimit)
        return fail(error, QStringLiteral("Cannot read GLSL preset: %1").arg(path));
    if (file.size() > presetReferenceByteLimit - budget.bytes)
        return fail(error, QStringLiteral("Expanded preset exceeds the 2 MiB limit."));
    budget.bytes += file.size();
    active.insert(identity);
    const QString source = QString::fromUtf8(file.readAll());
    const QRegularExpression assignment(QStringLiteral(
        "^\\s*([A-Za-z_][A-Za-z_0-9]*)\\s*=\\s*(?:\"([^\"]*)(?:\"|$)|([^\\s#]*))"));
    const QRegularExpression reference(QStringLiteral(
        "^\\s*#reference\\s+(?:\"([^\"]+)\"|([^\\s#]+))\\s*(?:#.*)?$"));
    PresetValues local;
    QStringList references;
    for (const QString& line : source.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;
        if (trimmed.startsWith(QStringLiteral("#reference"))) {
            const auto match = reference.match(line);
            if (!match.hasMatch())
                return fail(error, QStringLiteral("Invalid preset #reference: %1").arg(line.left(100)));
            references.append(match.captured(1).isNull() ? match.captured(2) : match.captured(1));
            continue;
        }
        if (trimmed.startsWith(QStringLiteral("#include")))
            return fail(error, QStringLiteral("Preset #include is not supported: %1").arg(path));
        if (trimmed.startsWith(QLatin1Char('#'))) continue;
        const auto match = assignment.match(line);
        if (!match.hasMatch())
            return fail(error, QStringLiteral("Invalid preset line: %1").arg(line.left(100)));
        QString item = match.captured(2).isNull() ? match.captured(3).trimmed() : match.captured(2);
        if (match.captured(2).isNull() && item.endsWith(QLatin1Char(';'))) {
            item.chop(1);
            item = item.trimmed();
        }
        local.insert(match.captured(1), {item, path});
    }
    for (const QString& referenced : references) {
        const QString referencePath = relativePath(path, referenced);
        if (QFileInfo(referencePath).suffix().compare(QStringLiteral("glslp"), Qt::CaseInsensitive) != 0)
            return fail(error, QStringLiteral("Preset #reference must name a .glslp file: %1").arg(referenced));
        if (!readPresetValues(referencePath, values, active, budget, error, depth + 1)) return false;
    }
    for (auto it = local.cbegin(); it != local.cend(); ++it) values.insert(it.key(), it.value());
    active.remove(identity);
    return true;
}

bool wrapMode(const QString& value, unsigned& result) {
    if (value == QStringLiteral("repeat")) result = GL_REPEAT;
    else if (value == QStringLiteral("mirrored_repeat")) result = GL_MIRRORED_REPEAT;
    else if (value == QStringLiteral("clamp_to_border")) result = GL_CLAMP_TO_BORDER;
    else if (value == QStringLiteral("clamp_to_edge") || value == QStringLiteral("edge"))
        result = GL_CLAMP_TO_EDGE;
    else return false;
    return true;
}

} // namespace

bool ShaderPreset::load(const QString& path, ShaderPreset& result, QString& error) {
    error.clear();
    ShaderPreset preset;
    preset.path = path;
    const QString suffix = QFileInfo(path).suffix().toLower();
    PresetValues values;
    QSet<QString> consumed;
    if (suffix == QStringLiteral("glslp")) {
        QSet<QString> active;
        PresetReadBudget budget;
        if (!readPresetValues(path, values, active, budget, error)) return false;
    } else if (suffix != QStringLiteral("glsl")) {
        return fail(error, QStringLiteral("Only GLSL .glsl and .glslp files are supported. Slang and Cg require a different renderer."));
    }
    auto value = [&](const QString& key) { return values.value(key).text; };
    int count = 1;
    if (suffix == QStringLiteral("glslp")) {
        bool okay = false;
        count = value(QStringLiteral("shaders")).toInt(&okay);
        if (!okay || count < 1 || count > maxPasses)
            return fail(error, QStringLiteral("The GLSL preset must contain 1–32 shader passes."));
        consumed.insert(QStringLiteral("shaders"));
    }
    const QString feedback = value(QStringLiteral("feedback_pass"));
    consumed.insert(QStringLiteral("feedback_pass"));
    if (!feedback.isEmpty()) {
        bool okay = false;
        preset.feedbackPass = feedback.toInt(&okay);
        if (!okay || preset.feedbackPass < 0 || preset.feedbackPass >= count)
            return fail(error, QStringLiteral("Invalid feedback_pass."));
    }
    for (int i = 0; i < count; ++i) {
        ShaderPass pass;
        const QString index = QString::number(i);
        const QString shaderKey = QStringLiteral("shader") + index;
        const PresetValue shaderValue = values.value(shaderKey);
        if (suffix == QStringLiteral("glslp") && shaderValue.text.isEmpty())
            return fail(error, QStringLiteral("Missing shader%1 in GLSL preset.").arg(i));
        pass.path = suffix == QStringLiteral("glsl") ? path : relativePath(shaderValue.origin, shaderValue.text);
        consumed.insert(shaderKey);
        if (QFileInfo(pass.path).suffix().compare(QStringLiteral("glsl"), Qt::CaseInsensitive) != 0)
            return fail(error, QStringLiteral("Pass %1 must name a .glsl shader.").arg(i + 1));
        if (!readSource(pass.path, pass.source, error)) return false;
        auto take = [&](const QString& prefix) {
            const QString key = prefix + index;
            consumed.insert(key);
            return value(key);
        };
        const QString filter = take(QStringLiteral("filter_linear"));
        if (!filter.isEmpty() && !boolean(filter, pass.linear))
            return fail(error, QStringLiteral("Invalid filter_linear%1.").arg(i));
        const QString floating = take(QStringLiteral("float_framebuffer"));
        if (!floating.isEmpty() && !boolean(floating, pass.floating))
            return fail(error, QStringLiteral("Invalid float_framebuffer%1.").arg(i));
        const QString srgb = take(QStringLiteral("srgb_framebuffer"));
        if (!srgb.isEmpty() && !boolean(srgb, pass.srgb))
            return fail(error, QStringLiteral("Invalid srgb_framebuffer%1.").arg(i));
        const QString mipmap = take(QStringLiteral("mipmap_input"));
        if (!mipmap.isEmpty() && !boolean(mipmap, pass.mipmap))
            return fail(error, QStringLiteral("Invalid mipmap_input%1.").arg(i));
        pass.alias = take(QStringLiteral("alias"));
        if (!pass.alias.isEmpty()) {
            const QRegularExpression identifier(QStringLiteral("^[A-Za-z_][A-Za-z_0-9]*$"));
            if (!identifier.match(pass.alias).hasMatch())
                return fail(error, QStringLiteral("Invalid shader pass alias: %1").arg(pass.alias));
            for (const ShaderPass& existing : preset.passes)
                if (existing.alias == pass.alias)
                    return fail(error, QStringLiteral("Duplicate shader pass alias: %1").arg(pass.alias));
        }
        const QString wrap = take(QStringLiteral("wrap_mode"));
        const QString alternateWrap = take(QStringLiteral("texture_wrap_mode"));
        const QString effectiveWrap = wrap.isEmpty() ? alternateWrap : wrap;
        if (!effectiveWrap.isEmpty() && !wrapMode(effectiveWrap, pass.wrap))
            return fail(error, QStringLiteral("Unsupported texture wrap mode: %1").arg(effectiveWrap));
        const QString mod = take(QStringLiteral("frame_count_mod"));
        if (!mod.isEmpty()) {
            bool okay = false;
            const double parsed = mod.toDouble(&okay);
            if (!okay || !std::isfinite(parsed) || parsed < 1 ||
                parsed > std::numeric_limits<unsigned>::max() || std::floor(parsed) != parsed)
                return fail(error, QStringLiteral("Invalid frame_count_mod%1.").arg(i));
            pass.frameCountMod = unsigned(parsed);
        }
        const QString commonType = take(QStringLiteral("scale_type"));
        const QString alternateType = take(QStringLiteral("scale_type_"));
        const QString commonScale = take(QStringLiteral("scale"));
        const QString xType = take(QStringLiteral("scale_type_x"));
        const QString yType = take(QStringLiteral("scale_type_y"));
        const QString xScale = take(QStringLiteral("scale_x"));
        const QString yScale = take(QStringLiteral("scale_y"));
        pass.explicitScale = !commonType.isEmpty() || !alternateType.isEmpty() || !commonScale.isEmpty() ||
                            !xType.isEmpty() || !yType.isEmpty() || !xScale.isEmpty() || !yScale.isEmpty();
        auto scale = [&](QString type, QString value, ShaderPass::Scale& mode, double& factor) {
            if (type.isEmpty()) type = (i == count - 1 && !pass.explicitScale)
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
        if (!scale(xType.isEmpty() ? (commonType.isEmpty() ? alternateType : commonType) : xType,
                   xScale.isEmpty() ? commonScale : xScale, pass.scaleX, pass.factorX) ||
            !scale(yType.isEmpty() ? (commonType.isEmpty() ? alternateType : commonType) : yType,
                   yScale.isEmpty() ? commonScale : yScale, pass.scaleY, pass.factorY))
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
            if (!okay || !qIsFinite(p.step) || p.step < 0) return fail(error, QStringLiteral("Invalid shader parameter step."));
            if (p.step == 0) p.step = 0.01f;
            p.value = value(id).isEmpty() ? match.captured(3).toFloat(&okay) : value(id).toFloat(&okay);
            if (!okay || !qIsFinite(p.value)) return fail(error, QStringLiteral("Invalid shader parameter: %1").arg(id));
            p.value = qBound(p.minimum, p.value, p.maximum);
            consumed.insert(id);
            preset.parameters.append(p);
        }
        preset.passes.append(pass);
    }
    consumed.insert(QStringLiteral("textures"));
    const QString textureList = value(QStringLiteral("textures"));
    const QRegularExpression identifier(QStringLiteral("^[A-Za-z_][A-Za-z_0-9]*$"));
    for (const QString& item : textureList.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        if (preset.textures.size() >= lutLimit)
            return fail(error, QStringLiteral("The GLSL preset exceeds the 32 LUT limit."));
        ShaderLut texture;
        texture.id = item.trimmed();
        if (!identifier.match(texture.id).hasMatch())
            return fail(error, QStringLiteral("Invalid LUT identifier: %1").arg(texture.id));
        for (const ShaderLut& existing : preset.textures)
            if (existing.id == texture.id)
                return fail(error, QStringLiteral("Duplicate LUT identifier: %1").arg(texture.id));
        const PresetValue textureValue = values.value(texture.id);
        consumed.insert(texture.id);
        if (textureValue.text.isEmpty())
            return fail(error, QStringLiteral("Missing LUT path: %1").arg(texture.id));
        texture.path = relativePath(textureValue.origin, textureValue.text);
        const QString linearKey = texture.id + QStringLiteral("_linear");
        const QString alternateLinearKey = QStringLiteral("filter_") + texture.id;
        const QString mipmapKey = texture.id + QStringLiteral("_mipmap");
        const QString wrapKey = texture.id + QStringLiteral("_wrap_mode");
        const QString repeatKey = texture.id + QStringLiteral("_repeat");
        consumed.insert(linearKey);
        consumed.insert(alternateLinearKey);
        consumed.insert(mipmapKey);
        consumed.insert(wrapKey);
        consumed.insert(repeatKey);
        const QString linear = value(linearKey);
        if (!linear.isEmpty() && !boolean(linear, texture.linear))
            return fail(error, QStringLiteral("Invalid LUT filter: %1").arg(linearKey));
        const QString alternateLinear = value(alternateLinearKey);
        if (!alternateLinear.isEmpty()) {
            bool alternate = false;
            if (alternateLinear == QStringLiteral("linear")) alternate = true;
            else if (alternateLinear != QStringLiteral("nearest"))
                return fail(error, QStringLiteral("Invalid LUT filter: %1").arg(alternateLinearKey));
            if (linear.isEmpty()) texture.linear = alternate;
        }
        const QString mipmap = value(mipmapKey);
        if (!mipmap.isEmpty() && !boolean(mipmap, texture.mipmap))
            return fail(error, QStringLiteral("Invalid LUT mipmap option: %1").arg(mipmapKey));
        const QString wrap = value(wrapKey);
        if (!wrap.isEmpty() && !wrapMode(wrap, texture.wrap))
            return fail(error, QStringLiteral("Unsupported LUT wrap mode: %1").arg(wrap));
        const QString repeat = value(repeatKey);
        if (!repeat.isEmpty()) {
            bool enabled = false;
            if (!boolean(repeat, enabled))
                return fail(error, QStringLiteral("Invalid LUT repeat option: %1").arg(repeatKey));
            if (wrap.isEmpty() && enabled) texture.wrap = GL_REPEAT;
        }
        preset.textures.append(texture);
    }
    consumed.insert(QStringLiteral("parameters"));
    for (const QString& item : value(QStringLiteral("parameters")).split(QLatin1Char(';'), Qt::SkipEmptyParts))
        consumed.insert(item.trimmed());
    const QRegularExpression unusedPassOption(QStringLiteral(
        "^(?:shader|filter_linear|float_framebuffer|srgb_framebuffer|mipmap_input|alias|"
        "wrap_mode|texture_wrap_mode|frame_count_mod|scale_type|scale_type_|scale|"
        "scale_type_x|scale_type_y|scale_x|scale_y)[0-9]+$"));
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (consumed.contains(it.key()) || unusedPassOption.match(it.key()).hasMatch())
            continue;
        // RetroArch presets often retain overrides for parameters removed from a shader.
        bool numeric = false;
        const double unusedParameter = it.value().text.toDouble(&numeric);
        if (numeric && std::isfinite(unusedParameter)) continue;
        return fail(error, QStringLiteral("Unsupported GLSL preset option: %1").arg(it.key()));
    }
    result = std::move(preset);
    return true;
}
