#include "core/RomPatch.h"
#include "core/RomPatchSet.h"
#include "core/CartridgeLimits.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <cstdlib>
#include <limits>
#include <zlib.h>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << Qt::endl;
        std::exit(1);
    }
}

void checkApplied(const QByteArray& source, const QByteArray& patch, const QByteArray& expected,
                  const char* message) {
    QString error = QStringLiteral("stale error");
    check(RomPatch::apply(source, patch, error) == expected && error.isEmpty(), message);
}

void checkRejected(const QByteArray& source, const QByteArray& patch, const char* expectedError) {
    QString error;
    if (!RomPatch::apply(source, patch, error).isEmpty() || error != QLatin1String(expectedError)) {
        QTextStream(stderr) << "FAIL: expected \"" << expectedError << "\", got \"" << error
                            << '"' << Qt::endl;
        std::exit(1);
    }
}

void appendNumber(QByteArray& bytes, quint64 number) {
    while (number >= 128) {
        bytes.append(char(number % 128));
        number = number / 128 - 1;
    }
    bytes.append(char(128 + number));
}

void appendCommand(QByteArray& bytes, quint64 action, quint64 count) {
    appendNumber(bytes, (count - 1) * 4 + action);
}

void appendCrc(QByteArray& bytes, const QByteArray& contents) {
    const auto crc = crc32(0, reinterpret_cast<const Bytef*>(contents.constData()), uInt(contents.size()));
    for (int byte = 0; byte < 4; ++byte)
        bytes.append(char((crc >> (8 * byte)) & 0xff));
}

QByteArray bpsTrailer(const QByteArray& body, const QByteArray& source, const QByteArray& target) {
    QByteArray patch("BPS1");
    patch.append(body);
    appendCrc(patch, source);
    appendCrc(patch, target);
    appendCrc(patch, patch);
    return patch;
}

QByteArray bpsBody(quint64 sourceSize, quint64 targetSize, quint64 metadataSize) {
    QByteArray body;
    appendNumber(body, sourceSize);
    appendNumber(body, targetSize);
    appendNumber(body, metadataSize);
    return body;
}

QByteArray bpsPatch(const QByteArray& source, const QByteArray& target, const QByteArray& commands,
                    const QByteArray& metadata = {}) {
    auto body = bpsBody(quint64(source.size()), quint64(target.size()), quint64(metadata.size()));
    body.append(metadata);
    body.append(commands);
    return bpsTrailer(body, source, target);
}

void testIps() {
    const QByteArray source("abcd");
    const QByteArray records = QByteArray::fromHex("00000100025859000007000000035a000002000121");
    const QByteArray patch = "PATCH" + records + "EOF";
    checkApplied(source, patch, QByteArray("aX!d\0\0\0ZZZ", 10),
                 "IPS literal, RLE, out-of-order overlap and zero-filled expansion");
    checkApplied(source, "PATCHEOF", source, "IPS with no records");
    checkApplied(source, patch + QByteArray::fromHex("000008"), QByteArray("aX!d\0\0\0Z", 8),
                 "IPS truncation through an RLE record");
    checkApplied(source, "PATCHEOF" + QByteArray::fromHex("000006"), QByteArray("abcd\0\0", 6),
                 "IPS explicit size extension is zero filled");

    const char* malformed = "The IPS patch is malformed or incomplete.";
    checkRejected(source, "PATCH", malformed);
    checkRejected(source, "PATCHEO", malformed);
    checkRejected(source, "PATCH" + QByteArray::fromHex("00000100"), malformed);
    checkRejected(source, "PATCH" + QByteArray::fromHex("000001000258"), malformed);
    checkRejected(source, "PATCH" + QByteArray::fromHex("00000700000003"), malformed);
    checkRejected(source, "PATCH" + QByteArray::fromHex("000007000000005a") + "EOF", malformed);
    checkRejected(source, "PATCHEOFx", malformed);
    checkRejected(source, "PATCHEOF" + QByteArray::fromHex("000000"),
                  "The patched ROM has an invalid size.");
    const auto repeatedRecord = QByteArray::fromHex("0000000000ffff5a");
    checkRejected(source, "PATCH" + repeatedRecord.repeated(8193) + "EOF",
                  "The IPS patch writes too much data.");
}

void testBps() {
    const QByteArray source("abcdefgh");
    const QByteArray target("abxyfgfxyxyfgfxy");
    QByteArray commands;
    appendCommand(commands, 0, 2); // SourceRead "ab"
    appendCommand(commands, 1, 2); // TargetRead "xy"
    commands.append("xy");
    appendCommand(commands, 2, 2); // SourceCopy from 5
    appendNumber(commands, 10);
    appendCommand(commands, 2, 1); // SourceCopy back from 7 to 5
    appendNumber(commands, 5);
    appendCommand(commands, 3, 2); // TargetCopy from 2
    appendNumber(commands, 4);
    appendCommand(commands, 3, 7); // TargetCopy back from 4 to 2
    appendNumber(commands, 5);
    const auto patch = bpsPatch(source, target, commands, QByteArray(260, 'm'));
    checkApplied(source, patch, target, "all BPS commands and relative offsets with metadata");
    checkRejected("ABCDEFGH", patch, "The BPS patch does not match the source ROM.");
    checkRejected("abcdefghX", patch, "The BPS patch does not match the source ROM.");
    checkRejected(source, bpsPatch(source, QByteArray(target.size(), 'x'), commands),
                  "The patched ROM checksum is invalid.");
    auto corruptPatch = patch;
    corruptPatch[7] = char(uchar(corruptPatch.at(7)) ^ 0x80);
    checkRejected(source, corruptPatch, "The BPS patch checksum is invalid.");

    QByteArray repeatCommands;
    appendCommand(repeatCommands, 1, 3);
    repeatCommands.append("123");
    appendCommand(repeatCommands, 3, 2097);
    appendNumber(repeatCommands, 0);
    const auto repeatTarget = QByteArray("123").repeated(700);
    checkApplied(source, bpsPatch(source, repeatTarget, repeatCommands), repeatTarget,
                 "BPS overlapping TargetCopy and multi-byte command lengths");
}

void testMalformedBps() {
    const QByteArray source("abc");
    const QByteArray target("x");
    const auto normalHeader = bpsBody(3, 1, 0);
    const char* malformed = "The BPS patch is malformed or incomplete.";
    checkRejected(source, "BPS1", malformed);
    checkRejected(source, bpsTrailer(QByteArray(3, '\0'), source, target), malformed);
    checkRejected(source, bpsTrailer(QByteArray(12, '\x7f'), source, target), malformed);
    checkRejected(source, bpsTrailer(bpsBody(3, 1, 100), source, target), malformed);
    checkRejected(source, bpsTrailer(bpsBody(3, 1, std::numeric_limits<quint64>::max()), source, target),
                  malformed);
    checkRejected(source, bpsTrailer(bpsBody(3, 0, 0), source, {}),
                  "The patched ROM has an invalid size.");
    checkRejected(source, bpsTrailer(bpsBody(3, CartridgeLimits::MaxRomSize + 1, 0), source, target),
                  "The patched ROM has an invalid size.");

    checkRejected(source, bpsTrailer(normalHeader, source, target), malformed);
    checkRejected(source, bpsPatch(source, target, QByteArray::fromHex("81")), malformed);
    checkRejected(source, bpsPatch(source, target, QByteArray::fromHex("817880")), malformed);
    checkRejected(source, bpsPatch(source, target, QByteArray::fromHex("84")), malformed);
    checkRejected(source, bpsPatch(source, "xxxx", QByteArray::fromHex("8c")), malformed);
    checkRejected(source, bpsPatch(source, target, QByteArray::fromHex("82")), malformed);
    checkRejected(source, bpsPatch(source, target, QByteArray::fromHex("8283")), malformed);
    checkRejected(source, bpsPatch(source, target, QByteArray::fromHex("8286")), malformed);
    checkRejected(source, bpsPatch(source, target, QByteArray::fromHex("8380")), malformed);
    checkRejected(source, bpsPatch(source, "xx", QByteArray::fromHex("81788383")), malformed);

    QByteArray oversizedCommand;
    appendNumber(oversizedCommand, std::numeric_limits<quint64>::max());
    checkRejected(source, bpsPatch(source, target, oversizedCommand), malformed);
    QByteArray oversizedOffset = QByteArray::fromHex("82");
    appendNumber(oversizedOffset, std::numeric_limits<quint64>::max() - 1);
    checkRejected(source, bpsPatch(source, target, oversizedOffset), malformed);
}

void writePatch(const QDir& directory, const QString& name, const QByteArray& patch) {
    QFile file(directory.filePath(name));
    check(file.open(QIODevice::WriteOnly), "open fixture patch file");
    check(file.write(patch) == patch.size(), "write fixture patch file");
    check(file.flush(), "flush fixture patch file");
}

void testPatchSet() {
    QTemporaryDir directory;
    check(directory.isValid(), "create patch fixture directory");
    QDir local(directory.path());
    check(local.mkdir("fallback"), "create fixture fallback directory");
    const QDir fallback(local.filePath("fallback"));
    const QString romPath = local.filePath("Collection.zip");
    const QString memberName = "subdir/gAME.v1.md";
    const QByteArray source("abc");
    QByteArray content = source;
    QStringList applied{"stale patch"};
    QString error = "stale error";
    writePatch(local, "Game.v1.translation.ips", "ignored");
    writePatch(local, "Collection.ips", "ignored");
    check(RomPatchSet::apply(content, romPath, memberName, fallback.path(), applied, error) &&
          content == source && applied.isEmpty() && error.isEmpty(),
          "ignore unrelated patches and clear stale diagnostics");

    const QByteArray literal = QByteArray::fromHex("8178");
    writePatch(fallback, "Game.v1.IPS", "PATCH" + QByteArray::fromHex("000000000178") + "EOF");
    check(RomPatchSet::apply(content, romPath, memberName, fallback.path(), applied, error) &&
          content == "xbc" && applied == QStringList{"Game.v1.IPS"} && error.isEmpty(),
          "use member basename for case-insensitive lookup in the fallback folder");

    writePatch(local, "Game.v1.IpS", "PATCH" + QByteArray::fromHex("000000000178") + "EOF");
    writePatch(local, "game.v1.01.BPS", bpsPatch("xbc", "x", literal));
    writePatch(local, "Game.v1.1.ips", "lower-priority IPS");
    writePatch(local, "Game.v1.02.ips", "PATCH" + QByteArray::fromHex("000001000179") + "EOF");
    writePatch(fallback, "Game.v1.3.ips", "ignored");
    content = source;
    check(RomPatchSet::apply(content, romPath, memberName, fallback.path(), applied, error) &&
          content == "xy" && applied == QStringList{"Game.v1.IpS", "game.v1.01.BPS", "Game.v1.02.ips"} &&
          error.isEmpty(), "apply the local chain in order, prefer BPS at the same step, and ignore fallback");

    for (int step = 1; step <= 10; ++step)
        writePatch(local, QString("Numeric.%1.ips").arg(step),
                   "PATCH" + QByteArray::fromHex("0000000001") + char('a' + step) + "EOF");
    content = source;
    check(RomPatchSet::apply(content, local.filePath("Numeric.md"), "Numeric.md", {}, applied, error) &&
          content == "kbc" && applied.size() == 10 && applied[1] == "Numeric.2.ips" &&
          applied.back() == "Numeric.10.ips", "sort numbered plain-ROM patches numerically");
}

void testPatchSetFailures() {
    QTemporaryDir directory;
    check(directory.isValid(), "create patch failure fixture directory");
    QDir local(directory.path());
    check(local.mkdir("fallback"), "create failure fixture fallback directory");
    const QDir fallback(local.filePath("fallback"));
    const QString romPath = local.filePath("Collection.zip");
    const QByteArray source("abc");
    const auto rejected = [&](const QString& memberName, const QString& detail, const char* message) {
        QByteArray content = source;
        QStringList applied{"stale patch"};
        QString error;
        check(!RomPatchSet::apply(content, romPath, memberName, fallback.path(), applied, error) &&
              content == source && applied.isEmpty() && error.contains(detail), message);
    };

    writePatch(local, "Gap.2.ips", "PATCHEOF");
    writePatch(fallback, "Gap.1.ips", "PATCHEOF");
    rejected("dir/Gap.32x", "missing step 1", "reject a gap without looking in the fallback folder");

    writePatch(local, "Duplicate.01.ips", "PATCHEOF");
    writePatch(local, "Duplicate.1.ips", "PATCHEOF");
    rejected("Duplicate.md", "match the same step", "reject equal numerical steps with different spelling");

    writePatch(local, "Zero.00.ips", "PATCHEOF");
    rejected("Zero.md", "Invalid ROM patch number", "reject step zero");
    writePatch(local, "TooHigh.33.ips", "PATCHEOF");
    rejected("TooHigh.md", "Invalid ROM patch number", "reject numbers outside the supported range");

    writePatch(local, "Atomic.1.ips", "PATCH" + QByteArray::fromHex("000000000178") + "EOF");
    writePatch(local, "Atomic.2.bps", bpsPatch("abc", "x", QByteArray::fromHex("8178")));
    rejected("Atomic.md", "Atomic.2.bps. The BPS patch does not match the source ROM.",
             "failed patch names the file and leaves the original ROM intact");

    for (int step = 1; step <= 32; ++step)
        writePatch(local, QString("Full.%1.ips").arg(step), "PATCHEOF");
    auto content = source;
    QStringList applied;
    QString error;
    check(RomPatchSet::apply(content, romPath, "Full.md", fallback.path(), applied, error) &&
          content == source && applied.size() == 32, "support exactly 32 patches");
    writePatch(local, "Full.ips", "PATCHEOF");
    rejected("Full.md", "more than 32 patches", "count the unnumbered patch toward the total limit");
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    checkRejected("abc", "IPS32", "Unrecognized ROM patch format.");
    checkRejected({}, "PATCHEOF", "The source ROM has an invalid size.");
    testIps();
    testBps();
    testMalformedBps();
    testPatchSet();
    testPatchSetFailures();
    return 0;
}
