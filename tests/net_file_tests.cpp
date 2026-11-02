#include "net/NetFileTransfer.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdlib>
#include <cstdio>

namespace {
void check(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr, "net file test failed: %s\n", label); std::abort(); }
}
void put(const QString& path, const QByteArray& contents) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "create fixture");
    check(file.write(contents) == contents.size(), "write fixture");
}
QString transfer(NetFileTransfer::Prepared prepared, const QString& destination) {
    QString error;
    const auto parsed = NetFileTransfer::Manifest::fromBytes(prepared.manifest.toBytes(), &error);
    check(bool(parsed), "manifest roundtrip");
    NetFileTransfer::Sender sender(std::move(prepared));
    NetFileTransfer::Receiver receiver;
    check(receiver.begin(*parsed, destination, &error), "begin download");
    if (auto first = receiver.nextRequest()) {
        auto chunk = sender.readChunk(*first, &error);
        check(bool(chunk), "first chunk");
        auto altered = *chunk;
        altered.data[0] ^= 1;
        check(!receiver.writeChunk(altered, &error), "reject corrupt chunk");
        check(receiver.writeChunk(*chunk, &error), "accept first chunk");
        receiver.cancel();
        NetFileTransfer::Receiver resumed;
        check(resumed.begin(*parsed, destination, &error), "resume download");
        check(resumed.bytesReceived() == chunk->data.size(), "verified resume offset");
        while (auto request = resumed.nextRequest()) {
            auto next = sender.readChunk(*request, &error);
            check(bool(next) && resumed.writeChunk(*next, &error), "stream chunk");
        }
        check(resumed.complete(), "download complete");
        const QString path = resumed.completedPath();
        QFile changed(path);
        check(changed.open(QIODevice::WriteOnly | QIODevice::Append), "modify completed fixture");
        check(changed.write("x", 1) == 1, "modify completed contents");
        changed.close();
        NetFileTransfer::Receiver repeat;
        check(!repeat.begin(*parsed, destination, &error), "never overwrite existing media");
        return path;
    }
    check(false, "request exists");
    return {};
}
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    check(directory.isValid(), "temporary directory");
    QDir root(directory.path());
    check(root.mkpath("media") && root.mkpath("downloads"), "fixture directories");
    QString error;

    const QByteArray rom(150000, 'R');
    put(directory.filePath("media/game.md"), rom);
    auto cartridge = NetFileTransfer::prepare(directory.filePath("media/game.md"), {}, &error);
    check(bool(cartridge), "prepare cartridge");
    const QByteArray manifest = cartridge->manifest.toBytes();
    QByteArray altered = manifest;
    altered.replace("game.md", "bad_.md");
    check(!NetFileTransfer::Manifest::fromBytes(altered, &error), "reject changed manifest");
    transfer(std::move(*cartridge), directory.filePath("downloads"));

    put(directory.filePath("media/track.bin"), QByteArray(100000, 'D'));
    put(directory.filePath("media/disc.cue"),
        "FILE \"track.bin\" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n");
    auto cue = NetFileTransfer::prepare(directory.filePath("media/disc.cue"), {}, &error);
    check(bool(cue) && cue->manifest.files.size() == 2, "prepare CUE and referenced track");
    check(cue->sources.last().contents.contains("game_track01.bin"), "rewrite CUE reference");
    put(directory.filePath("media/list.m3u"), "disc.cue\n");
    auto playlist = NetFileTransfer::prepare(directory.filePath("media/list.m3u"), {}, &error);
    check(bool(playlist) && playlist->manifest.files.size() == 3, "prepare M3U media only");
    check(playlist->sources.last().contents.contains("disc0.cue"), "rewrite M3U reference");

    put(directory.filePath("media/escape.cue"),
        "FILE \"../outside.bin\" BINARY\nTRACK 01 MODE1/2352\n");
    check(!NetFileTransfer::prepare(directory.filePath("media/escape.cue"), {}, &error),
          "reject outside reference");
    put(directory.filePath("media/malformed.cue"),
        "FILE \"track.bin\"\nTRACK 01 MODE1/2352\n");
    check(!NetFileTransfer::prepare(directory.filePath("media/malformed.cue"), {}, &error),
          "reject unparsed FILE line");
    check(QFile::link(directory.filePath("media/track.bin"),
                      directory.filePath("media/link.bin")), "create symlink fixture");
    put(directory.filePath("media/symlink.cue"),
        "FILE \"link.bin\" BINARY\nTRACK 01 MODE1/2352\n");
    check(!NetFileTransfer::prepare(directory.filePath("media/symlink.cue"), {}, &error),
          "reject symlink media");
    std::puts("net file tests passed");
}
