#include "UpdateArchive.h"

#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace UpdateInternal {
namespace {
std::string lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
bool unsafeLink(const fs::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(path, ec));
}
std::string archiveFailure(archive* reader) {
    const char* message = archive_error_string(reader);
    return message ? message : "Invalid or damaged update archive.";
}
bool requiredFiles(const fs::path& stage, Platform platform) {
    if (platform == Platform::Windows)
        return fs::is_regular_file(stage / "KegaRefusion.exe") &&
               fs::is_regular_file(stage / "cores/genesis_plus_gx_libretro.dll");
    return fs::is_regular_file(stage / "KegaRefusion") && fs::is_regular_file(stage / "run.sh") &&
           fs::is_regular_file(stage / "cores/genesis_plus_gx_libretro.so");
}
bool retryRename(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    for (int i = 0; i < 20; ++i) {
        fs::rename(from, to, ec);
        if (!ec) return true;
        // Antivirus and recently exited Windows DLL users can release handles late.
        if (i != 19) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    error = "Cannot replace " + from.u8string() + ": " + ec.message();
    return false;
}
bool writeStrings(std::ostream& out, const std::vector<std::string>& strings) {
    out << strings.size() << '\n';
    for (const auto& value : strings) out << std::quoted(value) << '\n';
    return bool(out);
}
bool readStrings(std::istream& in, std::vector<std::string>& strings, std::size_t maximum) {
    std::size_t count = 0;
    if (!(in >> count) || count > maximum) return false;
    for (std::size_t i = 0; i < count; ++i) {
        std::string value;
        if (!(in >> std::quoted(value)) || value.size() > 8192) return false;
        strings.push_back(std::move(value));
    }
    return true;
}
}

bool validRelativePath(const std::string& value) {
    if (value.empty() || value.size() > 1024 || value.front() == '/' || value.back() == '/') return false;
    if (value.find_first_of("\\:\r\n\t\0", 0, 6) != std::string::npos) return false;
    std::size_t start = 0;
    while (start < value.size()) {
        const auto end = value.find('/', start);
        const auto component = value.substr(start, end == std::string::npos ? end : end - start);
        if (component.empty() || component == "." || component == ".." || component.size() > 240 ||
            component.back() == '.' || component.back() == ' ') return false;
        for (unsigned char c : component) if (c < 32 || c == 127 || c == '<' || c == '>' || c == '"' ||
                                                c == '|' || c == '?' || c == '*') return false;
        const auto base = lower(component.substr(0, component.find('.')));
        if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
            (base.size() == 4 && (base.substr(0, 3) == "com" || base.substr(0, 3) == "lpt") &&
             base[3] >= '0' && base[3] <= '9')) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool packagePath(const std::string& value, Platform platform) {
    if (!validRelativePath(value)) return false;
    const std::string name = lower(value);
    const auto slash = name.find('/');
    if (slash != std::string::npos) {
        const std::string directory = name.substr(0, slash);
        return directory == "cores" || directory == "plugins" || directory == "languages" ||
               directory == "shaders" || directory == "licenses" ||
               (platform == Platform::Linux && directory == "lib");
    }
    if (name == "kega-package.json" || name == "readme.md" || name == "license" ||
        name == "license-exception" || name == "licensing.md" || name == "third_party.md") return true;
    if (platform == Platform::Windows)
        return name == "kegarefusion.exe" || name == "kegarefusionupdater.exe" ||
               name == "qt.conf" || (name.size() > 4 && name.substr(name.size() - 4) == ".dll");
    return name == "kegarefusion" || name == "kegarefusionupdater" || name == "run.sh" ||
           name == u8"запуск-linux.txt" || name == u8"ЗАПУСК-linux.txt" || name == u8"ЗАПУСК-LINUX.txt";
}

bool safeTreePath(const fs::path& root, const fs::path& relative, std::string& error) {
    if (!validRelativePath(relative.generic_u8string())) { error = "Unsafe update path."; return false; }
    fs::path path;
    for (const auto& part : fs::absolute(root)) {
        path /= part;
        if (unsafeLink(path)) { error = "Update path contains a symbolic link: " + path.u8string(); return false; }
    }
    for (const auto& part : relative) {
        path /= part;
        if (unsafeLink(path)) { error = "Update target contains a symbolic link: " + path.u8string(); return false; }
    }
    return true;
}

bool extractPackage(const fs::path& archivePath, const fs::path& stage, Platform platform,
                    std::vector<std::string>& files, std::string& error) {
    try {
        if (!fs::is_regular_file(archivePath) || fs::file_size(archivePath) > MaxArchiveBytes) {
            error = "Update archive is absent or exceeds 512 MiB."; return false;
        }
        if (!fs::is_directory(stage) || !fs::is_empty(stage) || unsafeLink(stage)) {
            error = "Update staging directory is unsafe or not empty."; return false;
        }
        std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
        if (!reader) { error = "Cannot allocate archive reader."; return false; }
        archive_read_support_filter_none(reader.get());
        archive_read_support_filter_gzip(reader.get());
        archive_read_support_format_zip_seekable(reader.get());
        archive_read_support_format_tar(reader.get());
        archive_read_set_format_option(reader.get(), "zip", "mac-ext", "0");
#ifdef _WIN32
        const int opened = archive_read_open_filename_w(reader.get(), archivePath.c_str(), 64 * 1024);
#else
        const int opened = archive_read_open_filename(reader.get(), archivePath.c_str(), 64 * 1024);
#endif
        if (opened != ARCHIVE_OK) { error = archiveFailure(reader.get()); return false; }
        archive_entry* entry = nullptr;
        std::set<std::string> names;
        std::vector<std::string> extracted;
        std::uint64_t total = 0;
        std::size_t count = 0;
        std::array<char, 64 * 1024> buffer{};
        for (;;) {
            const int status = archive_read_next_header(reader.get(), &entry);
            if (status == ARCHIVE_EOF) break;
            if (status != ARCHIVE_OK) { error = archiveFailure(reader.get()); return false; }
            if (++count > MaxEntries) { error = "Update contains too many files."; return false; }
            const char* raw = archive_entry_pathname_utf8(entry);
            if (!raw) raw = archive_entry_pathname(entry);
            std::string name = raw ? raw : "";
            const bool directory = archive_entry_filetype(entry) == AE_IFDIR;
            if (directory && !name.empty() && name.back() == '/') name.pop_back();
            if (!validRelativePath(name) || !names.insert(lower(name)).second ||
                archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
                (!directory && archive_entry_filetype(entry) != AE_IFREG) ||
                archive_entry_is_encrypted(entry) > 0 || (archive_entry_perm(entry) & 07000)) {
                error = "Update contains an unsafe, duplicate, linked or special file: " + name; return false;
            }
            if (!safeTreePath(stage, fs::u8path(name), error)) return false;
            const fs::path target = stage / fs::u8path(name);
            if (directory) {
                if (archive_entry_size_is_set(entry) && archive_entry_size(entry) != 0) {
                    error = "Update directory unexpectedly contains file data."; return false;
                }
                fs::create_directories(target); continue;
            }
            const la_int64_t expected = archive_entry_size_is_set(entry) ? archive_entry_size(entry) : -1;
            if (expected < 0 || std::uint64_t(expected) > MaxFileBytes ||
                std::uint64_t(expected) > MaxExpandedBytes - total) {
                error = "Update unpacked size exceeds its limit."; return false;
            }
            fs::create_directories(target.parent_path());
            std::ofstream output(target, std::ios::binary | std::ios::trunc);
            if (!output) { error = "Cannot create staged update file."; return false; }
            std::uint64_t decoded = 0;
            for (;;) {
                const auto n = archive_read_data(reader.get(), buffer.data(), buffer.size());
                if (n < 0) { error = archiveFailure(reader.get()); return false; }
                if (n == 0) break;
                if (std::uint64_t(n) > std::uint64_t(expected) - decoded) {
                    error = "Archive file size does not match its header."; return false;
                }
                output.write(buffer.data(), n);
                if (!output) { error = "Cannot write staged update (disk full or permissions)."; return false; }
                decoded += std::uint64_t(n);
            }
            output.close();
            if (decoded != std::uint64_t(expected) || !output) { error = "Truncated update file."; return false; }
            total += decoded;
            // Never restore owner, ACLs, xattrs or privilege bits from a package.
            const bool executable = platform == Platform::Linux && (archive_entry_perm(entry) & 0111);
            fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write |
                fs::perms::group_read | fs::perms::others_read |
                (executable ? fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec : fs::perms::none));
            extracted.push_back(name);
        }
        if (extracted.empty()) { error = "Update archive is empty."; return false; }
        // Release artifacts can be flat or have exactly one common wrapper.
        std::string wrapper;
        if (!requiredFiles(stage, platform)) {
            const auto slash = extracted.front().find('/');
            if (slash == std::string::npos) { error = "Update has no application executable."; return false; }
            wrapper = extracted.front().substr(0, slash) + "/";
            for (const auto& name : extracted) if (name.compare(0, wrapper.size(), wrapper) != 0) {
                error = "Update has mixed package roots."; return false;
            }
            if (!requiredFiles(stage / fs::u8path(wrapper), platform)) {
                error = "Update is missing its application, launcher or core."; return false;
            }
        }
        for (const auto& original : extracted) {
            const std::string relative = original.substr(wrapper.size());
            if (!packagePath(relative, platform)) {
                error = "Update attempts to modify an unmanaged/user path: " + relative; return false;
            }
            files.push_back(relative);
        }
        if (!wrapper.empty()) {
            const fs::path source = stage / fs::u8path(wrapper);
            std::vector<fs::path> children;
            for (const auto& child : fs::directory_iterator(source)) children.push_back(child.path());
            for (const auto& child : children) fs::rename(child, stage / child.filename());
            fs::remove(source);
        }
        std::sort(files.begin(), files.end());
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool preflight(const fs::path& root, const fs::path& stage,
               const std::vector<std::string>& files, Platform platform, std::string& error) {
    try {
        std::set<std::string> seen;
        std::uint64_t bytes = 0;
        for (const auto& name : files) {
            if (!packagePath(name, platform) || !seen.insert(lower(name)).second ||
                !safeTreePath(root, fs::u8path(name), error) || !safeTreePath(stage, fs::u8path(name), error)) {
                if (error.empty()) error = "Unsafe or duplicate install target.";
                return false;
            }
            const auto target = root / fs::u8path(name);
            if (fs::exists(target) && !fs::is_regular_file(target)) {
                error = "A package file conflicts with a directory: " + target.u8string(); return false;
            }
            for (auto parent = target.parent_path(); parent != root && !parent.empty(); parent = parent.parent_path()) {
                if (fs::exists(parent) && !fs::is_directory(parent)) {
                    error = "A package directory conflicts with a file: " + parent.u8string(); return false;
                }
            }
            if (!fs::is_regular_file(stage / fs::u8path(name))) { error = "Staged file is missing."; return false; }
            bytes += fs::file_size(stage / fs::u8path(name));
        }
        if (bytes > MaxExpandedBytes || fs::space(root).available < (16ULL << 20)) {
            error = "Not enough free disk space to finish the update."; return false;
        }
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool sameFileContents(const fs::path& first, const fs::path& second) {
    try {
        if (!fs::is_regular_file(first) || !fs::is_regular_file(second) || unsafeLink(first) || unsafeLink(second) ||
            fs::file_size(first) != fs::file_size(second)) return false;
        std::ifstream a(first, std::ios::binary), b(second, std::ios::binary);
        std::array<char, 64 * 1024> left{}, right{};
        while (a && b) {
            a.read(left.data(), left.size()); b.read(right.data(), right.size());
            if (a.gcount() != b.gcount() || !std::equal(left.begin(), left.begin() + a.gcount(), right.begin())) return false;
        }
        return a.eof() && b.eof();
    } catch (...) { return false; }
}

bool validWorkDirectory(const fs::path& work) {
    const auto name = work.filename().u8string();
    return work.is_absolute() && name.size() == 45 && name.compare(0, 13, ".kega-update-") == 0 &&
        std::all_of(name.begin() + 13, name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) &&
        !unsafeLink(work) && fs::is_regular_file(work / "job.txt");
}

bool writePlan(const Plan& plan, std::string& error) {
    std::ofstream out(plan.work / "job.txt", std::ios::binary | std::ios::trunc);
    out << "KEGA-UPDATE-1\n" << (plan.platform == Platform::Windows ? "windows" : "linux") << '\n'
        << plan.parentPid << '\n';
    writeStrings(out, plan.files); writeStrings(out, plan.retire); writeStrings(out, plan.restartArguments);
    out.close();
    if (!out) { error = "Cannot save update installation plan."; return false; }
    return true;
}

bool readPlan(const fs::path& work, Plan& plan, std::string& error) {
    try {
        if (!validWorkDirectory(work) || fs::file_size(work / "job.txt") > (4ULL << 20)) {
            error = "Unsafe update work directory or plan."; return false;
        }
        plan.work = work;
        plan.root = work.parent_path();
        std::ifstream in(work / "job.txt", std::ios::binary);
        std::string magic, platform;
        if (!(in >> magic >> platform >> plan.parentPid) || magic != "KEGA-UPDATE-1" ||
            (platform != "windows" && platform != "linux") || plan.parentPid == 0 ||
            !readStrings(in, plan.files, MaxEntries) || !readStrings(in, plan.retire, MaxEntries) ||
            !readStrings(in, plan.restartArguments, 32)) { error = "Invalid update installation plan."; return false; }
        plan.platform = platform == "windows" ? Platform::Windows : Platform::Linux;
        for (const auto& name : plan.retire) if (!packagePath(name, plan.platform) ||
            !safeTreePath(plan.root, fs::u8path(name), error) ||
            !safeTreePath(work / "retired-reference", fs::u8path(name), error)) return false;
        return preflight(plan.root, work / "package", plan.files, plan.platform, error);
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool apply(const Plan& plan, std::string& error, bool& rollbackComplete) {
    struct Changed { fs::path relative; bool backedUp = false; bool installed = false; };
    std::vector<Changed> changed;
    std::vector<fs::path> createdDirectories;
    rollbackComplete = true;
    try {
        if (!preflight(plan.root, plan.work / "package", plan.files, plan.platform, error)) return false;
        fs::create_directories(plan.work / "backup");
        std::ofstream journal(plan.work / "journal.txt", std::ios::binary | std::ios::trunc);
        if (!journal) { error = "Cannot create rollback journal."; return false; }
        auto replace = [&](const std::string& name, bool installing) {
            const fs::path relative = fs::u8path(name), target = plan.root / relative;
            if (!safeTreePath(plan.root, relative, error)) throw std::runtime_error(error);
            std::vector<fs::path> missing;
            for (auto parent = target.parent_path(); parent != plan.root && !fs::exists(parent); parent = parent.parent_path())
                missing.push_back(parent);
            for (auto i = missing.rbegin(); i != missing.rend(); ++i) {
                fs::create_directory(*i); createdDirectories.push_back(*i);
            }
            changed.push_back({relative});
            auto& record = changed.back();
            if (fs::exists(target)) {
                if (!fs::is_regular_file(target)) throw std::runtime_error("Install target is no longer a regular file.");
                fs::create_directories((plan.work / "backup" / relative).parent_path());
                journal << "backup " << std::quoted(name) << '\n'; journal.flush();
                if (!journal || !retryRename(target, plan.work / "backup" / relative, error)) throw std::runtime_error(error);
                record.backedUp = true;
            }
            if (installing) {
                journal << "install " << std::quoted(name) << '\n'; journal.flush();
                if (!journal || !retryRename(plan.work / "package" / relative, target, error)) throw std::runtime_error(error);
                record.installed = true;
            }
        };
        // Manifest commits last so interrupted transactions remain recognizable.
        for (const auto& name : plan.files) if (name != "kega-package.json") replace(name, true);
        for (const auto& name : plan.retire) {
            if (!packagePath(name, plan.platform)) throw std::runtime_error("Unsafe retirement path.");
            if (std::find(plan.files.begin(), plan.files.end(), name) != plan.files.end()) continue;
            if (sameFileContents(plan.root / fs::u8path(name), plan.work / "retired-reference" / fs::u8path(name)))
                replace(name, false);
        }
        if (std::find(plan.files.begin(), plan.files.end(), "kega-package.json") != plan.files.end()) replace("kega-package.json", true);
        journal << "complete\n"; journal.flush();
        if (!journal) throw std::runtime_error("Cannot commit update journal.");
        return true;
    } catch (const std::exception& e) {
        if (error.empty()) error = e.what();
        for (auto i = changed.rbegin(); i != changed.rend(); ++i) {
            std::error_code ec;
            if (i->installed) {
                fs::create_directories((plan.work / "package" / i->relative).parent_path(), ec);
                std::string detail;
                if (!retryRename(plan.root / i->relative, plan.work / "package" / i->relative, detail)) rollbackComplete = false;
            }
            if (i->backedUp) {
                std::string detail;
                if (!retryRename(plan.work / "backup" / i->relative, plan.root / i->relative, detail)) rollbackComplete = false;
            }
        }
        for (auto i = createdDirectories.rbegin(); i != createdDirectories.rend(); ++i) {
            std::error_code ec; fs::remove(*i, ec);
        }
        if (rollbackComplete) {
            std::ofstream journal(plan.work / "journal.txt", std::ios::binary | std::ios::app);
            journal << "rolled-back\n";
        }
        if (!rollbackComplete) error += "\nRollback could not restore every file. Recovery files: " + plan.work.u8string();
        return false;
    }
}

DirectoryLock::DirectoryLock() = default;
DirectoryLock::~DirectoryLock() { release(); }
bool DirectoryLock::acquire(const fs::path& root, std::string& error) {
    const fs::path path = root / ".kega-update.lock";
    if (unsafeLink(path)) { error = "Unsafe update lock path."; return false; }
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) { error = "Another update is running or the application directory is not writable."; return false; }
    handle_ = reinterpret_cast<std::intptr_t>(h);
#else
    int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0) close(fd);
        error = "Another update is running or the application directory is not writable."; return false;
    }
    handle_ = fd;
#endif
    return true;
}
void DirectoryLock::release() {
    if (handle_ == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    close(int(handle_));
#endif
    handle_ = -1;
}

bool processAlive(std::uint64_t pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process); return alive;
#else
    return kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

bool anotherInstance(const fs::path& root, Platform platform, std::uint64_t excludedPid) {
    const fs::path executable = root / (platform == Platform::Windows ? "KegaRefusion.exe" : "KegaRefusion");
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL next = Process32FirstW(snapshot, &entry); next; next = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == excludedPid || _wcsicmp(entry.szExeFile, L"KegaRefusion.exe") != 0) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) { found = true; break; }
        std::array<wchar_t, 32768> buffer{}; DWORD size = DWORD(buffer.size());
        if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size) &&
            _wcsicmp(buffer.data(), executable.c_str()) == 0) found = true;
        CloseHandle(process);
        if (found) break;
    }
    CloseHandle(snapshot); return found;
#else
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/proc", ec)) {
        const auto name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        if (name == std::to_string(excludedPid)) continue;
        const auto target = fs::read_symlink(entry.path() / "exe", ec);
        if (!ec && target == executable) return true;
        ec.clear();
    }
    return false;
#endif
}

bool launch(const fs::path& program, const std::vector<std::string>& arguments,
            const fs::path& workingDirectory, std::string& error) {
#ifdef _WIN32
    auto quote = [](const std::wstring& value) {
        std::wstring result = L"\""; std::size_t slashes = 0;
        for (wchar_t c : value) {
            if (c == L'\\') { ++slashes; continue; }
            if (c == L'"') result.append(slashes * 2 + 1, L'\\'); else result.append(slashes, L'\\');
            slashes = 0; result.push_back(c);
        }
        result.append(slashes * 2, L'\\'); result.push_back(L'"'); return result;
    };
    std::wstring command = quote(program.wstring());
    for (const auto& argument : arguments) command += L" " + quote(fs::u8path(argument).wstring());
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, workingDirectory.c_str(), &startup, &process)) {
        error = "Cannot launch application/helper (Windows error " + std::to_string(GetLastError()) + ")."; return false;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return true;
#else
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC) != 0) { error = "Cannot create launch channel."; return false; }
    const pid_t child = fork();
    if (child < 0) { close(descriptors[0]); close(descriptors[1]); error = "Cannot start helper."; return false; }
    if (child == 0) {
        close(descriptors[0]);
        const pid_t grandchild = fork();
        if (grandchild < 0) { const int saved = errno; (void)!write(descriptors[1], &saved, sizeof(saved)); _exit(127); }
        if (grandchild > 0) _exit(0);
        setsid();
        if (chdir(workingDirectory.c_str()) != 0) { const int saved = errno; (void)!write(descriptors[1], &saved, sizeof(saved)); _exit(127); }
        std::vector<std::string> strings{program.string()};
        strings.insert(strings.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& value : strings) argv.push_back(value.data());
        argv.push_back(nullptr);
        execv(program.c_str(), argv.data());
        const int saved = errno; (void)!write(descriptors[1], &saved, sizeof(saved)); _exit(127);
    }
    close(descriptors[1]);
    int status = 0; while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    int saved = 0; const auto n = read(descriptors[0], &saved, sizeof(saved)); close(descriptors[0]);
    if (n > 0) { error = "Cannot launch application/helper: " + std::string(std::strerror(saved)); return false; }
    return true;
#endif
}

void showFailure(const std::string& message) {
#ifdef _WIN32
    const auto text = fs::u8path(message).wstring();
    MessageBoxW(nullptr, text.c_str(), L"Kega Refusion update", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#else
    // The restarted GUI reads the persisted report. Also retain useful diagnostics
    // for terminal launches and failures before a GUI can be started.
    std::fprintf(stderr, "Kega Refusion update: %s\n", message.c_str());
#endif
}
}
