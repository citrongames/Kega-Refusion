#include "core/MovieTape.h"
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QtEndian>
#include <QTemporaryDir>
#include <cstdlib>

namespace {
void check(bool value, const char* message) {
    if (!value) qFatal("movie_tape_tests: %s", message);
}
MovieInput input(quint16 buttons, quint8 commands = 0) {
    MovieInput value;
    value.buttons[0] = buttons;
    value.aim[1] = 0x10000'ffffULL; // offscreen, X=-1, Y=0
    value.commands = commands;
    return value;
}
void add16(QByteArray& bytes, quint16 value) { const auto le = qToLittleEndian(value); bytes.append(reinterpret_cast<const char*>(&le), 2); }
void add32(QByteArray& bytes, quint32 value) { const auto le = qToLittleEndian(value); bytes.append(reinterpret_cast<const char*>(&le), 4); }
void add64(QByteArray& bytes, quint64 value) { const auto le = qToLittleEndian(value); bytes.append(reinterpret_cast<const char*>(&le), 8); }
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
void seal(QByteArray& bytes) { bytes.append(digest(bytes)); }
void legacyRun(QByteArray& bytes, quint32 repeat, const MovieInput& input) {
    add32(bytes, repeat);
    for (unsigned player = 0; player < 3; ++player) add16(bytes, input.buttons[player]);
    for (quint64 aim : input.aim) add64(bytes, aim);
    bytes.append(char(input.commands));
}
QByteArray legacyId() { return QByteArray::fromHex("00112233445566778899aabbccddeeff"); }
QByteArray legacyMetadata() {
    return QJsonDocument(QJsonObject{{QStringLiteral("game"), QStringLiteral("legacy fixture")}})
        .toJson(QJsonDocument::Compact);
}
QByteArray legacyMovie(const QByteArray& initial, const QByteArray& resume,
                       const MovieInput& first, const MovieInput& second) {
    const QByteArray metadata = legacyMetadata();
    QByteArray bytes("KRMOVIE1", 8);
    add32(bytes, 1); add32(bytes, quint32(metadata.size())); add32(bytes, quint32(initial.size()));
    add32(bytes, 2); add64(bytes, 3); bytes.append(legacyId());
    add32(bytes, quint32(resume.size())); add64(bytes, 3);
    bytes.append(metadata); bytes.append(initial); bytes.append(resume);
    legacyRun(bytes, 2, first); legacyRun(bytes, 1, second);
    seal(bytes);
    return bytes;
}
QByteArray legacyCheckpoint(const QByteArray& initial, const MovieInput& first) {
    QByteArray identity = legacyMetadata(); identity.append(initial);
    QByteArray bytes("KRMCHK01", 8);
    add32(bytes, 1); add64(bytes, 2); bytes.append(legacyId());
    bytes.append(digest(identity)); add32(bytes, 1);
    legacyRun(bytes, 2, first); seal(bytes);
    return bytes;
}
void writeFixture(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}
void checkLegacyCompatibility(const QTemporaryDir& directory) {
    QString error;
    const QString path = directory.filePath(QStringLiteral("legacy.krm"));
    const QByteArray initial("old-initial", 11), resume("old-resume", 10);
    MovieInput first, second, read;
    first.buttons[0] = 0x0101; first.buttons[1] = 0x0202; first.buttons[2] = 0x0404;
    first.aim[2] = 0x10002'ffffULL;
    second.buttons[0] = 0x0808; second.buttons[2] = 0x1010;
    second.commands = MovieTape::SoftReset;
    const QByteArray original = legacyMovie(initial, resume, first, second);
    const QByteArray checkpoint = legacyCheckpoint(initial, first);
    writeFixture(path, original);
    MovieTape tape;
    check(tape.load(path, error) && tape.size() == 3 && tape.id() == legacyId(),
          "load original v1 binary");
    check(tape.resumeState() == resume && tape.resumeFrame() == 3, "v1 resume state");
    check(tape.inputAt(0, read) && read == first && tape.inputAt(2, read) && read == second,
          "v1 three-player buttons and aim");
    for (unsigned player = 3; player < read.buttons.size(); ++player)
        check(read.buttons[player] == 0, "v1 extra players zero-filled");
    quint64 cursor = 99;
    check(tape.restoreCheckpoint(checkpoint, cursor, true, error) && cursor == 2 && tape.size() == 3,
          "v1 checkpoint playback");
    QByteArray oversizedCheckpoint = checkpoint;
    qToLittleEndian<quint32>(0xffffffffu, oversizedCheckpoint.data()+68);
    oversizedCheckpoint.chop(32); seal(oversizedCheckpoint);
    check(!tape.restoreCheckpoint(oversizedCheckpoint, cursor, false, error) && tape.size() == 3,
          "oversized v1 checkpoint rejected without changing the tape");
    check(tape.save(path, error), "promote v1 recording to v2");
    QFile promoted(path); check(promoted.open(QIODevice::ReadOnly), "read promoted recording");
    const QByteArray promotedBytes = promoted.readAll(); promoted.close();
    check(qFromLittleEndian<quint32>(promotedBytes.constData()+8) == 2,
          "promoted recording uses v2");
    QFile backup(path + QStringLiteral(".bak"));
    check(backup.open(QIODevice::ReadOnly) && backup.readAll() == original,
          "promotion preserves original v1 backup");
    backup.close();
    MovieTape promotedTape;
    check(promotedTape.load(path, error) &&
          promotedTape.restoreCheckpoint(checkpoint, cursor, false, error) && cursor == 2 &&
          promotedTape.size() == 2, "v1 checkpoint restores after v2 promotion");
    MovieInput branch = first; branch.buttons[7] = 0x7777;
    check(promotedTape.append(branch, error) && promotedTape.size() == 3 &&
          promotedTape.save(path, error), "branch v1 checkpoint with player eight");
    MovieTape reloaded;
    check(reloaded.load(path, error) && reloaded.inputAt(2, read) && read == branch,
          "branch from v1 checkpoint retained player eight");
    check(reloaded.forkAt(2, error) && reloaded.id() != legacyId() &&
          !reloaded.restoreCheckpoint(checkpoint, cursor, false, error),
          "v1 checkpoint rejected after fork identity changed");
    QByteArray mislabeled = original;
    qToLittleEndian<quint32>(2, mislabeled.data()+8);
    mislabeled.chop(32); seal(mislabeled);
    writeFixture(directory.filePath(QStringLiteral("mislabeled.krm")), mislabeled);
    check(!tape.load(directory.filePath(QStringLiteral("mislabeled.krm")), error),
          "v1 payload with v2 header rejected");
}
void checkEightPlayerRoundTrip(const QTemporaryDir& directory) {
    QString error;
    MovieTape tape;
    check(tape.create(QJsonObject{}, QByteArray("state", 5), error), "create eight-player tape");
    MovieInput p3, p4, p8, read;
    p3.buttons[2] = 0x0303; p4.buttons[3] = 0x0404; p8.buttons[7] = 0x0808;
    p8.aim[2] = 0x10000'ffffULL;
    for (int i = 0; i < 4; ++i) check(tape.append(p3, error), "append player three");
    check(tape.append(p4, error) && tape.append(p4, error), "append player four");
    check(tape.append(p8, error), "append player eight");
    const QByteArray checkpoint = tape.checkpoint(6, error);
    const QString path = directory.filePath(QStringLiteral("eight.krm"));
    check(tape.save(path, error), "save eight-player tape");
    MovieTape loaded;
    check(loaded.load(path, error) && loaded.size() == 7, "load eight-player tape");
    check(loaded.inputAt(3, read) && read == p3 && loaded.inputAt(4, read) && read == p4 &&
          loaded.inputAt(6, read) && read == p8, "all eight button slots round trip");
    quint64 cursor = 0;
    check(loaded.restoreCheckpoint(checkpoint, cursor, false, error) && cursor == 6,
          "v2 checkpoint branch");
    MovieInput branch = p8; branch.buttons[3] = 0x4444;
    check(loaded.append(branch, error) && loaded.save(path, error), "v2 branch save");
    MovieTape branchTape;
    check(branchTape.load(path, error) && branchTape.inputAt(6, read) && read == branch,
          "v2 branch player four and eight round trip");
    QFile source(path); check(source.open(QIODevice::ReadOnly), "read v2 source");
    QByteArray oversized = source.readAll();
    qToLittleEndian<quint32>(0xffffffffu, oversized.data()+20);
    oversized.chop(32); seal(oversized);
    const QString oversizedPath = directory.filePath(QStringLiteral("oversized.krm"));
    writeFixture(oversizedPath, oversized);
    check(!branchTape.load(oversizedPath, error), "oversized v2 run count rejected");
}
}

int main() {
    QTemporaryDir directory;
    check(directory.isValid(), "temporary directory");
    checkLegacyCompatibility(directory);
    checkEightPlayerRoundTrip(directory);
    const QString path = directory.filePath(QStringLiteral("test.krm"));
    const QString badPath = directory.filePath(QStringLiteral("bad.krm"));
    QString error;
    const QByteArray snapshot("state\0bytes", 11);
    MovieTape tape;
    check(tape.create(QJsonObject{{QStringLiteral("game"), QStringLiteral("fixture")}}, snapshot, error),
          "create");
    const QByteArray originalId = tape.id();
    const MovieInput a = input(1), b = input(2), c = input(4, MovieTape::SoftReset);
    for (int i = 0; i < 100; ++i) check(tape.append(a, error), "append repeat");
    for (int i = 0; i < 3; ++i) check(tape.append(b, error), "append second run");
    check(tape.append(c, error) && tape.append(c, error), "append reset run");
    check(tape.size() == 105, "frame count");
    MovieInput read;
    check(tape.inputAt(0, read) && read == a, "first input");
    check(tape.inputAt(99, read) && read == a, "last first run");
    check(tape.inputAt(100, read) && read == b, "first second run");
    check(tape.inputAt(104, read) && read == c, "last input");
    check(!tape.inputAt(105, read), "out of range");
    const QByteArray resumeSnapshot("resume\0state", 12);
    check(tape.setResumeState(resumeSnapshot, 105, error) && tape.resumeFrame() == 105,
          "set resume snapshot");
    check(!tape.setResumeState(resumeSnapshot, 106, error) && tape.resumeFrame() == 105,
          "reject future resume frame without losing valid snapshot");
    const QByteArray checkpoint = tape.checkpoint(101, error);
    const QByteArray laterCheckpoint = tape.checkpoint(102, error);
    check(!checkpoint.isEmpty(), "checkpoint within run");
    check(tape.save(path, error), "initial save");
    const QFileInfo initialFile(path);
    check(initialFile.size() < 1000, "RLE serialized file size");
    check(tape.save(path, error) && !QFile::exists(path + QStringLiteral(".bak")),
          "unchanged save avoids backup");

    MovieTape loaded;
    check(loaded.load(path, error), "load");
    check(loaded.id() == originalId && loaded.initialState() == snapshot && loaded.size() == 105,
          "round trip header and snapshot");
    check(loaded.resumeState() == resumeSnapshot && loaded.resumeFrame() == 105,
          "resume snapshot round trip in same movie file");
    quint64 cursor = 999;
    check(loaded.restoreCheckpoint(checkpoint, cursor, true, error) && cursor == 101 &&
          loaded.size() == 105, "readonly checkpoint retains playback suffix");
    check(loaded.resumeState() == resumeSnapshot && loaded.resumeFrame() == 105,
          "readonly checkpoint retains resume snapshot");
    check(loaded.restoreCheckpoint(checkpoint, cursor, false, error) && loaded.size() == 101,
          "recording checkpoint truncates to saved prefix");
    check(loaded.resumeState().isEmpty(), "recording restore invalidates resume snapshot");
    check(loaded.inputAt(100, read) && read == b, "partial run restored");
    check(loaded.setResumeState(resumeSnapshot, 101, error), "reset resume snapshot on branch");
    check(loaded.append(c, error) && loaded.size() == 102, "branch appends new input");
    check(loaded.resumeState().isEmpty(), "append invalidates resume snapshot");
    check(!loaded.restoreCheckpoint(laterCheckpoint, cursor, true, error),
          "diverged prefix rejected in playback");
    check(loaded.save(path, error), "branch save");
    MovieTape backup;
    check(backup.load(path + QStringLiteral(".bak"), error) && backup.size() == 105 &&
          backup.resumeState() == resumeSnapshot,
          "previous version backup");
    MovieTape branch;
    check(branch.load(path, error) && branch.size() == 102, "new branch round trip");
    check(branch.resumeState().isEmpty(), "branch without resume remains empty on disk");
    check(branch.inputAt(101, read) && read == c, "new branch input");
    check(branch.setResumeState(resumeSnapshot, 102, error) && branch.truncate(101) &&
          branch.resumeState().isEmpty(), "truncate invalidates resume snapshot");
    check(branch.setResumeState(resumeSnapshot, 101, error), "set resume before takeover");
    check(branch.forkAt(101, error) && branch.id() != originalId && branch.size() == 101,
          "takeover forks identity and prefix");
    check(branch.resumeState().isEmpty(), "fork invalidates resume snapshot");
    check(!branch.save(path, error), "fork cannot replace source movie");
    check(!branch.restoreCheckpoint(checkpoint, cursor, false, error),
          "old movie checkpoint rejected after fork");

    MovieInput invalid = a;
    invalid.commands = 2;
    check(!branch.append(invalid, error), "unsupported command rejected");
    invalid = a; invalid.aim[0] = 1ULL << 40;
    check(!branch.append(invalid, error), "unsupported aim bits rejected");

    QFile source(path);
    check(source.open(QIODevice::ReadOnly), "read saved file");
    QByteArray damaged = source.readAll();
    damaged[damaged.size()-33] = char(damaged[damaged.size()-33] ^ 1);
    QFile target(badPath);
    check(target.open(QIODevice::WriteOnly) && target.write(damaged) == damaged.size(),
          "write damaged fixture");
    target.close();
    const quint64 unchangedSize = branch.size();
    check(!branch.load(badPath, error) && branch.size() == unchangedSize,
          "checksum failure preserves active tape");

    MovieTape empty;
    check(empty.create(QJsonObject{}, QByteArray("x", 1), error) && empty.save(badPath, error) == false,
          "invalid existing movie cannot be replaced");
    check(!empty.save(path, error), "unrelated valid movie cannot be replaced");
    check(empty.save(directory.filePath(QStringLiteral("empty.krm")), error), "save empty movie");
    MovieTape emptyLoaded;
    check(emptyLoaded.load(directory.filePath(QStringLiteral("empty.krm")), error) &&
          emptyLoaded.size() == 0, "load empty movie");
    return EXIT_SUCCESS;
}
