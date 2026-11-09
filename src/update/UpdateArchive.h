#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Qt-free shared code: the detached helper must not load DLLs it replaces.
namespace UpdateInternal {
namespace fs = std::filesystem;
enum class Platform { Windows, Linux };
constexpr std::uint64_t MaxArchiveBytes = 512ULL << 20;
constexpr std::uint64_t MaxExpandedBytes = 2ULL << 30;
constexpr std::uint64_t MaxFileBytes = 512ULL << 20;
constexpr std::size_t MaxEntries = 4096;

bool validRelativePath(const std::string& relative);
bool packagePath(const std::string& relative, Platform platform);
bool safeTreePath(const fs::path& root, const fs::path& relative, std::string& error);
bool extractPackage(const fs::path& archive, const fs::path& stage, Platform platform,
                    std::vector<std::string>& files, std::string& error);
bool preflight(const fs::path& root, const fs::path& stage,
               const std::vector<std::string>& files, Platform platform, std::string& error);
bool sameFileContents(const fs::path& first, const fs::path& second);

struct Plan {
    fs::path root;
    fs::path work;
    Platform platform = Platform::Linux;
    std::uint64_t parentPid = 0;
    std::vector<std::string> files;
    std::vector<std::string> retire;
    std::vector<std::string> restartArguments;
};
bool writePlan(const Plan& plan, std::string& error);
bool readPlan(const fs::path& work, Plan& plan, std::string& error);
// Restores every replaced/retired file on any operation failure. On a failed
// rollback, backups and the journal are retained for recovery.
bool apply(const Plan& plan, std::string& error, bool& rollbackComplete);

class DirectoryLock {
public:
    DirectoryLock();
    ~DirectoryLock();
    DirectoryLock(const DirectoryLock&) = delete;
    DirectoryLock& operator=(const DirectoryLock&) = delete;
    bool acquire(const fs::path& root, std::string& error);
    void release();
private:
    std::intptr_t handle_ = -1;
};

bool processAlive(std::uint64_t pid);
bool anotherInstance(const fs::path& root, Platform platform, std::uint64_t excludedPid);
bool launch(const fs::path& program, const std::vector<std::string>& arguments,
            const fs::path& workingDirectory, std::string& error);
void showFailure(const std::string& message);
bool validWorkDirectory(const fs::path& work);
}
