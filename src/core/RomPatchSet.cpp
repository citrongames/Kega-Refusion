#include "RomPatchSet.h"
#include "RomPatch.h"
#include "i18n/Localization.h"
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringView>
#include <array>
#include <utility>

namespace {

constexpr int MaxPatches = 32;
constexpr qint64 MaxChainSize = 512LL * 1024 * 1024;
constexpr int NoStep = -1;
constexpr int InvalidStep = -2;

struct PatchStep {
    QFileInfo ips;
    QFileInfo bps;

    const QFileInfo& preferred() const {
        return bps.filePath().isEmpty() ? ips : bps;
    }
};

bool fail(QString& error, QString message) {
    error = std::move(message);
    return false;
}

int stepNumber(QStringView name, QStringView base) {
    if (!name.startsWith(base, Qt::CaseInsensitive))
        return NoStep;
    const auto suffix = name.sliced(base.size());
    if (suffix.isEmpty())
        return 0;
    if (suffix.size() < 2 || suffix.front() != u'.')
        return NoStep;
    const auto digits = suffix.sliced(1);
    for (const QChar digit : digits) {
        if (digit < u'0' || digit > u'9')
            return NoStep;
    }
    bool valid = false;
    const int step = digits.toInt(&valid);
    return valid && step >= 1 && step <= MaxPatches ? step : InvalidStep;
}

bool discover(const QString& directory, const QString& base, QList<QFileInfo>& patches, QString& error) {
    if (directory.isEmpty() || base.isEmpty())
        return true;

    std::array<PatchStep, MaxPatches + 1> steps;
    int lastStep = 0;
    QDirIterator entries(directory, {QStringLiteral("*.ips"), QStringLiteral("*.bps")},
                         QDir::Files | QDir::Hidden);
    while (entries.hasNext()) {
        entries.next();
        const QFileInfo info = entries.fileInfo();
        const int step = stepNumber(info.completeBaseName(), base);
        if (step == NoStep)
            continue;
        if (step == InvalidStep)
            return fail(error, Localization::text("Invalid ROM patch number in %1.").arg(info.fileName()));
        const bool bps = info.suffix().compare(QStringLiteral("bps"), Qt::CaseInsensitive) == 0;
        auto& match = bps ? steps[step].bps : steps[step].ips;
        if (!match.filePath().isEmpty())
            return fail(error, Localization::text("ROM patches %1 and %2 match the same step.")
                                   .arg(match.fileName(), info.fileName()));
        match = info;
        lastStep = qMax(lastStep, step);
    }
    if (!steps[0].preferred().filePath().isEmpty())
        patches.append(steps[0].preferred());
    if (patches.size() + lastStep > MaxPatches)
        return fail(error, Localization::text("The ROM patch chain contains more than %1 patches.").arg(MaxPatches));
    for (int step = 1; step <= lastStep; ++step) {
        const auto& patch = steps[step].preferred();
        if (patch.filePath().isEmpty())
            return fail(error, Localization::text("The ROM patch chain is missing step %1.").arg(step));
        patches.append(patch);
    }
    return true;
}

bool readPatch(const QFileInfo& info, qint64 remaining, QByteArray& patch, QString& error) {
    const auto readFailure = Localization::text("Could not read ROM patch %1. %2");
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, readFailure.arg(info.fileName(), file.errorString()));
    const qint64 size = file.size();
    if (size < 0)
        return fail(error, readFailure.arg(info.fileName(), file.errorString()));
    if (size > RomPatch::MaxPatchSize)
        return fail(error, readFailure.arg(info.fileName(), Localization::text("The ROM patch is too large.")));
    if (size > remaining)
        return fail(error, readFailure.arg(info.fileName(), Localization::text("The ROM patch chain is too large.")));
    patch = file.read(size + 1);
    if (file.error() != QFileDevice::NoError)
        return fail(error, readFailure.arg(info.fileName(), file.errorString()));
    if (patch.size() != size)
        return fail(error, Localization::text("ROM patch %1 changed while it was being read.").arg(info.fileName()));
    return true;
}

} // namespace

bool RomPatchSet::apply(QByteArray& content, const QString& romPath, const QString& memberName,
                        const QString& fallbackDirectory, QStringList& applied, QString& error) {
    applied.clear();
    error.clear();
    QList<QFileInfo> patches;
    const QString base = QFileInfo(memberName).completeBaseName();
    if (!discover(QFileInfo(romPath).absolutePath(), base, patches, error))
        return false;
    if (patches.isEmpty() && !discover(fallbackDirectory, base, patches, error))
        return false;

    auto working = content;
    qint64 remaining = MaxChainSize;
    QStringList names;
    for (const auto& info : patches) {
        QByteArray patch;
        if (!readPatch(info, remaining, patch, error))
            return false;
        remaining -= patch.size();
        QString detail;
        auto result = RomPatch::apply(working, patch, detail);
        if (result.isEmpty())
            return fail(error, Localization::text("Could not apply ROM patch %1. %2")
                                   .arg(info.fileName(), detail));
        working = std::move(result);
        names.append(info.fileName());
    }
    content = std::move(working);
    applied = std::move(names);
    return true;
}
