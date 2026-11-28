#include "update/UpdateArchive.h"
#include <archive.h>
#include <archive_entry.h>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace UpdateInternal;
namespace {
void put(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out << value; assert(out.good());
}
std::string get(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
struct Item { std::string path; std::string content; int type = AE_IFREG; std::string link = {}; int mode = 0644; };
void archiveFile(const fs::path& path, const std::vector<Item>& items) {
    auto* writer = archive_write_new(); assert(writer);
    assert(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK);
#ifdef _WIN32
    assert(archive_write_open_filename_w(writer, path.c_str()) == ARCHIVE_OK);
#else
    assert(archive_write_open_filename(writer, path.c_str()) == ARCHIVE_OK);
#endif
    for (const auto& item : items) {
        auto* entry = archive_entry_new();
        archive_entry_set_pathname(entry, item.path.c_str());
        archive_entry_set_filetype(entry, item.type);
        archive_entry_set_perm(entry, item.mode);
        archive_entry_set_size(entry, item.type == AE_IFREG ? item.content.size() : 0);
        if (!item.link.empty()) archive_entry_set_symlink(entry, item.link.c_str());
        assert(archive_write_header(writer, entry) == ARCHIVE_OK);
        if (item.type == AE_IFREG) assert(archive_write_data(writer, item.content.data(), item.content.size()) == la_ssize_t(item.content.size()));
        archive_entry_free(entry);
    }
    assert(archive_write_close(writer) == ARCHIVE_OK); archive_write_free(writer);
}
std::vector<Item> package() {
    return {{"release/KegaRefusion", "application", AE_IFREG, {}, 0755},
            {"release/run.sh", "#!/bin/sh\n", AE_IFREG, {}, 0755},
            {"release/cores/genesis_plus_gx_libretro.so", "core"},
            {"release/languages/ru.json", "{}"},
            {"release/scripts/01_Overlay.lua", "emu.frameadvance()"},
            {"release/scripts/games/sonic1-lab.lua", "-- example"},
            {"release/docs/lua-api.ru.md", "# Lua"}};
}
}
int main() {
    const auto identity = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto sandbox = fs::temp_directory_path() / ("kega-updater-tests-" + identity);
    fs::create_directories(sandbox);
    for (const std::string name : {"../data", "/tmp/data", "C:/data", "cores/../../data", "cores//bad", "cores/CON.dll", "x\\data", "a/./b"}) {
        assert(!validRelativePath(name));
    }
    assert(validRelativePath("cores/genesis_plus_gx_libretro.so"));
    assert(!packagePath("data/settings.ini", Platform::Linux));
    assert(!packagePath("saves/game.srm", Platform::Linux));
    assert(!packagePath("games/game.md", Platform::Windows));
    assert(!packagePath("bios/boot.bin", Platform::Windows));
    for (const auto platform : {Platform::Windows, Platform::Linux}) {
        assert(packagePath("scripts/games/example.lua", platform));
        assert(packagePath("docs/lua-api.ru.md", platform));
        assert(!packagePath("scripts/../data/settings.ini", platform));
    }
    std::string error;
    {
        auto items = package();
        archiveFile(sandbox / "good.tar", items);
        fs::create_directory(sandbox / "good");
        std::vector<std::string> files;
        assert(extractPackage(sandbox / "good.tar", sandbox / "good", Platform::Linux, files, error));
        assert(files.size() == 7 && get(sandbox / "good/KegaRefusion") == "application");
        assert((fs::status(sandbox / "good/run.sh").permissions() & fs::perms::owner_exec) != fs::perms::none);
    }
    int index = 0;
    for (const auto& bad : std::vector<Item>{{"../outside", "bad"}, {"release/data/settings.ini", "bad"},
              {"release/cores/linked.so", "", AE_IFLNK, "../../outside"},
              {"release/cores/setuid.so", "bad", AE_IFREG, {}, 04755},
              {"release/KegaRefusion", "duplicate"}}) {
        auto items = package(); items.push_back(bad);
        const auto archive = sandbox / ("bad" + std::to_string(index) + ".tar");
        const auto stage = sandbox / ("bad" + std::to_string(index++));
        archiveFile(archive, items); fs::create_directory(stage);
        std::vector<std::string> files; error.clear();
        assert(!extractPackage(archive, stage, Platform::Linux, files, error));
        assert(!error.empty());
    }
    {
        Plan plan; plan.root = sandbox / "install"; plan.work = plan.root / ".kega-update-0123456789abcdef0123456789abcdef";
        plan.files = {"KegaRefusion", "cores/new.so", "kega-package.json"};
        plan.retire = {"cores/obsolete.so", "cores/customized.so"};
        put(plan.root / "KegaRefusion", "old"); put(plan.root / "data/settings.ini", "settings");
        put(plan.root / "games/user.md", "rom"); put(plan.root / "bios/user.bin", "bios");
        put(plan.root / "cores/obsolete.so", "obsolete"); put(plan.root / "cores/customized.so", "user changed");
        put(plan.work / "retired-reference/cores/obsolete.so", "obsolete");
        put(plan.work / "retired-reference/cores/customized.so", "original shipped");
        put(plan.work / "package/KegaRefusion", "new"); put(plan.work / "package/cores/new.so", "new core");
        put(plan.work / "package/kega-package.json", "{}");
        bool restored = false; error.clear();
        assert(apply(plan, error, restored));
        assert(restored && get(plan.root / "KegaRefusion") == "new");
        assert(!fs::exists(plan.root / "cores/obsolete.so"));
        assert(get(plan.root / "cores/customized.so") == "user changed");
        assert(get(plan.root / "data/settings.ini") == "settings");
        assert(get(plan.root / "games/user.md") == "rom" && get(plan.root / "bios/user.bin") == "bios");
    }
    for (const auto platform : {Platform::Windows, Platform::Linux}) {
        Plan plan; plan.platform = platform; plan.parentPid = 1;
        plan.root = sandbox / (platform == Platform::Windows ? "editable-windows" : "editable-linux");
        plan.work = plan.root / ".kega-update-0123456789abcdef0123456789abcdef";
        plan.files = {"scripts/example.lua", "scripts/edited.lua", "scripts/new.lua",
                      "scripts/untracked.lua", "scripts/raced.lua", "docs/lua-api.md", "kega-package.json"};
        plan.retire = {"scripts/obsolete.lua", "docs/edited-old.md"};
        put(plan.root / "scripts/example.lua", "old example");
        put(plan.work / "editable-reference/scripts/example.lua", "old example");
        put(plan.root / "scripts/edited.lua", "my edits");
        put(plan.root / "scripts/untracked.lua", "my own script");
        put(plan.root / "scripts/personal.lua", "personal script");
        put(plan.root / "scripts/raced.lua", "changed after staging");
        put(plan.work / "editable-reference/scripts/raced.lua", "old example");
        put(plan.root / "docs/lua-api.md", "old manual");
        put(plan.work / "editable-reference/docs/lua-api.md", "old manual");
        put(plan.root / "scripts/obsolete.lua", "obsolete");
        put(plan.work / "retired-reference/scripts/obsolete.lua", "obsolete");
        put(plan.root / "docs/edited-old.md", "my notes");
        put(plan.work / "retired-reference/docs/edited-old.md", "old manual");
        put(plan.root / "data/settings.ini", "settings");
        for (const auto& name : plan.files) put(plan.work / "package" / name, "new release");
        error.clear(); assert(writePlan(plan, error));
        Plan loaded; assert(readPlan(plan.work, loaded, error));
        bool restored = false; assert(apply(loaded, error, restored));
        assert(restored && get(plan.root / "scripts/example.lua") == "new release");
        assert(get(plan.root / "docs/lua-api.md") == "new release");
        assert(get(plan.root / "scripts/new.lua") == "new release");
        assert(get(plan.root / "scripts/edited.lua") == "my edits");
        assert(get(plan.root / "scripts/untracked.lua") == "my own script");
        assert(get(plan.root / "scripts/personal.lua") == "personal script");
        assert(get(plan.root / "scripts/raced.lua") == "changed after staging");
        assert(get(plan.root / "docs/edited-old.md") == "my notes");
        assert(get(plan.root / "data/settings.ini") == "settings");
        assert(!fs::exists(plan.root / "scripts/obsolete.lua"));
    }
#ifndef _WIN32
    {
        const auto root = sandbox / "links"; fs::create_directories(root);
        fs::create_directory_symlink(sandbox / "good", root / "cores");
        error.clear(); assert(!safeTreePath(root, "cores/escape", error));
    }
    if (geteuid() != 0) {
        Plan plan; plan.root = sandbox / "rollback"; plan.work = plan.root / ".kega-update-fedcba9876543210fedcba9876543210";
        plan.files = {"KegaRefusion", "cores/new.so"};
        put(plan.root / "KegaRefusion", "old"); put(plan.root / "cores/existing.so", "old core");
        put(plan.work / "package/KegaRefusion", "new"); put(plan.work / "package/cores/new.so", "new core");
        fs::permissions(plan.root / "cores", fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
        bool restored = false; error.clear();
        assert(!apply(plan, error, restored));
        assert(restored && get(plan.root / "KegaRefusion") == "old");
        assert(get(plan.root / "cores/existing.so") == "old core");
        assert(!fs::exists(plan.root / "cores/new.so"));
        fs::permissions(plan.root / "cores", fs::perms::owner_all, fs::perm_options::replace);
    }
#endif
    {
        const auto root = sandbox / "lock"; fs::create_directory(root);
        DirectoryLock first, second; error.clear(); assert(first.acquire(root, error));
        assert(!second.acquire(root, error)); first.release(); assert(second.acquire(root, error));
    }
    fs::remove_all(sandbox);
    std::cout << "Update extraction, ownership, rollback and lock checks passed.\n";
}
