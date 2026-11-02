#include "core/MovieTape.h"
#include <QFile>
#include <QFileInfo>
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
}

int main() {
    QTemporaryDir directory;
    check(directory.isValid(), "temporary directory");
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
