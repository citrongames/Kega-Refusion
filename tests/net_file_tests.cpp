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

    // Two-file CUE, two-disc playlist and Paprium soundtrack all round-trip
    // through the same manifest, chunk and final-file checks as cartridges.
    put(directory.filePath("media/music.mp3"), QByteArray(90000, 'M'));
    put(directory.filePath("media/disc.cue"),
        "FILE \"track.bin\" BINARY\nTRACK 01 MODE1/2352\n"
        "INDEX 01 00:00:00\nFILE \"music.mp3\" MP3\n"
        "TRACK 02 AUDIO\nINDEX 01 00:00:00\n");
    auto multiCue = NetFileTransfer::prepare(directory.filePath("media/disc.cue"), {}, &error);
    check(bool(multiCue) && multiCue->manifest.files.size() == 3, "multi-file CUE manifest");
    check(multiCue->manifest.entry == "game.cue", "preserve CUE basename");
    {
        auto area = NetFileTransfer::TemporaryArea::create(
            directory.path(), NetFileTransfer::TemporaryArea::Kind::Downloads, &error);
        check(bool(area), "create temporary download area");
        NetFileTransfer::Sender sender(std::move(*multiCue));
        NetFileTransfer::Receiver receiver;
        check(receiver.begin(sender.manifest(), directory.path(), &error, area->directory()),
              "begin isolated CUE download");
        while (auto request = receiver.nextRequest()) {
            const auto chunk = sender.readChunk(*request, &error);
            check(bool(chunk) && receiver.writeChunk(*chunk, &error), "stream CUE and MP3");
        }
        check(receiver.complete(), "multi-file CUE complete");
        QFile cueFile(receiver.completedPath());
        check(cueFile.open(QIODevice::ReadOnly) &&
              cueFile.readAll().contains("game_track02.mp3"), "downloaded CUE references MP3");
        check(QFileInfo::exists(QFileInfo(receiver.completedPath()).absolutePath() +
                                "/game_track02.mp3"), "downloaded MP3 present");
        check(NetFileTransfer::TemporaryArea::cleanupOwned(area->directory(), &error),
              "clean owned download area");
        check(!QFileInfo::exists(area->directory()), "temporary download removed");
        check(QFileInfo::exists(directory.filePath("media/music.mp3")),
              "source MP3 preserved");
    }
    put(directory.filePath("media/track2.bin"), QByteArray(20000, '2'));
    put(directory.filePath("media/disc2.cue"),
        "FILE \"track2.bin\" BINARY\nTRACK 01 MODE1/2352\n");
    put(directory.filePath("media/list.m3u"), "disc.cue\ndisc2.cue\n");
    auto multiPlaylist = NetFileTransfer::prepare(directory.filePath("media/list.m3u"), {}, &error);
    check(bool(multiPlaylist) && multiPlaylist->manifest.files.size() == 6,
          "playlist includes both CUEs and all tracks");
    check(multiPlaylist->manifest.entry == "game.m3u", "preserve playlist basename");
    check(root.mkpath("media/paprium"), "Paprium directory");
    QByteArray papriumRom(0x800000, 'P');
    papriumRom.replace(0x182, 12, " T-574120-00");
    put(directory.filePath("media/Paprium.bin"), papriumRom);
    put(directory.filePath("media/paprium/01 Theme of Paprium.mp3"), QByteArray(10000, 'A'));
    put(directory.filePath("media/02 90's Acid Dub Character Select.mp3"), QByteArray(11000, 'B'));
    put(directory.filePath("media/paprium/unrelated.mp3"), QByteArray(1000, 'X'));
    auto paprium = NetFileTransfer::prepare(directory.filePath("media/Paprium.bin"), {}, &error);
    check(bool(paprium) && paprium->manifest.files.size() == 3, "Paprium ROM and music");
    check(paprium->manifest.entry == "game.bin", "preserve Paprium basename");
    bool theme = false, select = false;
    for (const auto& file : paprium->manifest.files) {
        theme |= file.name == "01 Theme of Paprium.mp3";
        select |= file.name == "02 90's Acid Dub Character Select.mp3";
        check(file.name != "unrelated.mp3", "exclude unrelated music");
    }
    check(theme && select, "Paprium MP3 names preserved");

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
    auto locked = NetFileTransfer::TemporaryArea::create(
        directory.path(), NetFileTransfer::TemporaryArea::Kind::Downloads, &error);
    check(bool(locked), "locked download area");
    auto stale = NetFileTransfer::TemporaryArea::create(
        directory.path(), NetFileTransfer::TemporaryArea::Kind::Downloads, &error);
    check(bool(stale), "stale download area");
    const QString stalePath = stale->directory();
    stale.reset();
    NetFileTransfer::TemporaryArea::cleanupStale(directory.path());
    check(QFileInfo::exists(locked->directory()), "active instance download retained");
    check(!QFileInfo::exists(stalePath), "stale download removed");
    check(QFileInfo::exists(directory.filePath("media/game.md")), "user ROM retained");

    const QString scope = "0123456789abcdef0123456789abcdef";
    check(root.mkpath("netplay/sessions/" + scope + "/saves") &&
          root.mkpath("netplay/sessions/" + scope + "/states") &&
          root.mkpath("saves") && root.mkpath("states"), "legacy scope fixture");
    put(directory.filePath("netplay/sessions/" + scope + "/saves/game.srm"), "old-save");
    put(directory.filePath("saves/game.srm"), "ordinary-save");
    put(directory.filePath("netplay/sessions/" + scope + "/states/game.slot0.krs"), "old-state");
    NetFileTransfer::TemporaryArea::migrateLegacySessions(directory.path());
    QFile ordinary(directory.filePath("saves/game.srm"));
    check(ordinary.open(QIODevice::ReadOnly) && ordinary.readAll() == "ordinary-save",
          "ordinary save not overwritten");
    QFile moved(directory.filePath("saves/game.netplay-" + scope + ".srm"));
    check(moved.open(QIODevice::ReadOnly) && moved.readAll() == "old-save",
          "legacy collision retained under stable name");
    check(QFileInfo::exists(directory.filePath("states/game.slot0.krs")),
          "legacy state copied");
    NetFileTransfer::TemporaryArea::migrateLegacySessions(directory.path());
    check(!QFileInfo::exists(directory.filePath(
          "saves/game.netplay-" + scope + "-1.srm")), "legacy migration idempotent");
    check(QFileInfo::exists(directory.filePath(
          "netplay/sessions/" + scope + "/saves/game.srm")), "legacy source untouched");
    std::puts("net file tests passed");
}
