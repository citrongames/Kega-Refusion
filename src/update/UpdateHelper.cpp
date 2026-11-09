#include "UpdateArchive.h"
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace UpdateInternal;
namespace {
void report(const fs::path& file, const std::string& message) {
    std::string error;
    if (!safeTreePath(file.parent_path(), file.filename(), error)) return;
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << message << '\n';
}
std::uint64_t ownPid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return std::uint64_t(getpid());
#endif
}
fs::path installedHelper(const Plan& plan) {
    return plan.root / (plan.platform == Platform::Windows ? "KegaRefusionUpdater.exe" : "KegaRefusionUpdater");
}
void cleanupAfterExit(const Plan& plan) {
    std::string ignored;
    if (plan.platform == Platform::Linux) {
        // Linux permits removal of the executable currently mapped by this helper.
        std::error_code ec; fs::remove_all(plan.work, ec);
    } else {
        // The installed static helper does not use any replaceable runtime DLLs.
        // A second short-lived process waits for this executable to be released.
        launch(installedHelper(plan), {"--cleanup", plan.work.u8string(), std::to_string(ownPid())}, plan.root, ignored);
    }
}
bool restart(const Plan& plan, std::string& error) {
    const auto executable = plan.root / (plan.platform == Platform::Windows ? "KegaRefusion.exe" : "run.sh");
    return launch(executable, plan.restartArguments, plan.root, error);
}
int run(const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) return 2;
    const fs::path work = fs::absolute(fs::u8path(arguments[1])).lexically_normal();
    if (arguments[0] == "--cleanup") {
        if (arguments.size() != 3 || !validWorkDirectory(work)) return 2;
        std::uint64_t pid = 0;
        try { pid = std::stoull(arguments[2]); } catch (...) { return 2; }
        for (int i = 0; i < 200 && processAlive(pid); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (processAlive(pid)) return 1;
        for (int i = 0; i < 100; ++i) {
            std::error_code ec; fs::remove_all(work, ec);
            if (!ec) return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return 1;
    }
    if (arguments[0] != "--apply" || arguments.size() != 2) return 2;
    Plan plan;
    std::string error;
    if (!readPlan(work, plan, error)) { report(work / "prepare-error.txt", error); return 1; }
    DirectoryLock lock;
    if (!lock.acquire(plan.root, error) || anotherInstance(plan.root, plan.platform, plan.parentPid)) {
        if (error.empty()) error = "Close the other Kega Refusion instance before installing an update.";
        report(work / "prepare-error.txt", error); return 1;
    }
    if (fs::exists(work / "cancel")) { lock.release(); cleanupAfterExit(plan); return 1; }
    report(work / "ready", "ready");
    for (int i = 0; i < 1200 && processAlive(plan.parentPid); ++i) {
        if (fs::exists(work / "cancel")) { lock.release(); cleanupAfterExit(plan); return 1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (processAlive(plan.parentPid)) {
        report(plan.root / ".kega-update-error.txt", "Update cancelled: the application did not exit within 60 seconds. No files were changed.");
        lock.release(); cleanupAfterExit(plan); return 1;
    }
    bool rollbackComplete = true;
    bool success = false;
    if (anotherInstance(plan.root, plan.platform, 0)) error = "Update cancelled because another Kega Refusion instance is running. No files were changed.";
    else success = apply(plan, error, rollbackComplete);
    if (!success) report(plan.root / ".kega-update-error.txt", "Update failed: " + error +
        (rollbackComplete ? "\nThe previous version was retained." : "\nDo not remove the recovery directory."));
    else {
        std::error_code ec; fs::remove(plan.root / ".kega-update-error.txt", ec);
    }
    lock.release();
    std::string restartError;
    if (rollbackComplete && !restart(plan, restartError)) {
        report(plan.root / ".kega-update-error.txt", (success ? "Update installed, but restart failed: " : "Update failed: " + error + "\n") + restartError);
        showFailure(restartError);
#ifdef _WIN32
        // A native dialog has delivered this failure; avoid showing it again at
        // the next launch unless recovery files need the user's attention.
        if (rollbackComplete) { std::error_code ec; fs::remove(plan.root / ".kega-update-error.txt", ec); }
#endif
    } else if (!rollbackComplete) {
        report(plan.work / "recovery-error.txt", error);
        showFailure(error);
#ifdef _WIN32
        std::error_code ec; fs::remove(plan.root / ".kega-update-error.txt", ec);
#endif
    }
    if (success || rollbackComplete) cleanupAfterExit(plan);
    return success ? 0 : 1;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) arguments.push_back(fs::path(argv[i]).u8string());
#else
int main(int argc, char** argv) {
    std::vector<std::string> arguments(argv + 1, argv + argc);
#endif
    try { return run(arguments); }
    catch (const std::exception& e) { showFailure(e.what()); return 1; }
}
