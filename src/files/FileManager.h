#ifndef DHCP_FILES_FILEMANAGER_H
#define DHCP_FILES_FILEMANAGER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sdkconfig.h"
#include "IFileManager.h"

namespace dhcp {
namespace files {

/**
 * @brief Default @ref IFileManager implementation.
 *
 * Volume ownership, the retry policy and the file operations live here; the
 * actual filesystem work is delegated to the registered
 * `storage::IFileSystem` objects (internal FAT always, microSD on the
 * ESP32-P4). Volumes are registered by `main.cpp`, which keeps the
 * target-specific part (which volumes exist at all) in one place instead of
 * scattering `#if CONFIG_IDF_TARGET_*` across the module.
 */
class FileManager : public IFileManager {
public:
    /** @brief Minimum delay between mount attempts for a missing volume. */
    static constexpr uint32_t kRetryMs = 5000;

    /**
     * @brief Byte budget of one volume check (Kconfig `FILES_CHECK_MAX_MB`).
     *
     * A check reads whole files, so an unbounded walk over a large card would
     * keep the CPU and the SDMMC bus busy for a very long time; hitting the
     * budget ends the walk and is reported as @ref CheckReport::truncated
     * rather than hidden.
     */
    static constexpr uint64_t kCheckBudgetBytes =
        static_cast<uint64_t>(CONFIG_FILES_CHECK_MAX_MB) * 1024ULL * 1024ULL;

    FileManager();
    ~FileManager() override;

    /**
     * @brief Take ownership of a volume.
     *
     * The volume id must be unique; a duplicate registration is ignored with
     * a warning (it would make `?volume=` ambiguous).
     */
    void addVolume(std::unique_ptr<storage::IFileSystem> volume);

    // IFileManager
    bool supported() const override;
    void mountAll() override;
    void refresh() override;
    std::vector<storage::VolumeInfo> volumes() override;
    storage::IFileSystem* find(const std::string& volumeId) override;
    FileStatus list(const std::string& volumeId, const std::string& path,
                    std::vector<FileEntry>& out,
                    std::string* detail = nullptr) override;
    FileStatus mkdir(const std::string& volumeId, const std::string& path,
                     std::string* detail = nullptr) override;
    FileStatus rename(const std::string& volumeId, const std::string& from,
                      const std::string& to,
                      std::string* detail = nullptr) override;
    FileStatus remove(const std::string& volumeId, const std::string& path,
                      bool recursive = false,
                      std::string* detail = nullptr) override;
    FileStatus format(const std::string& volumeId,
                      std::string* detail = nullptr) override;
    FileStatus openRead(const std::string& volumeId, const std::string& path,
                        std::unique_ptr<IFileSource>& out,
                        std::string* detail = nullptr) override;
    FileStatus stat(const std::string& volumeId, const std::string& path,
                    FileEntry& out, std::string* detail = nullptr) override;
    FileStatus openWrite(const std::string& volumeId, const std::string& path,
                         uint64_t expectedLen, std::unique_ptr<IFileSink>& out,
                         std::string* detail = nullptr) override;
    uint64_t freeBytes(const std::string& volumeId) override;
    FileStatus checkStart(const std::string& volumeId,
                          std::string* detail = nullptr) override;
    void checkCancel() override;
    CheckReport checkReport() override;
    void applyAccessFilter() override;
    bool allowClient(uint32_t clientIp4Host) override;
    uint32_t foreignBlocked() const override { return foreignBlocked_; }
    bool filterActive() const override { return allowOwnSubnet_ && subnetValid_; }

private:
    /** @brief Milliseconds since boot (monotonic). */
    static uint32_t nowMs();

    /** @brief Resolve a volume that must be mounted. */
    FileStatus resolve(const std::string& volumeId, storage::IFileSystem*& vol);

    /** @brief Normalize @p path and build the absolute VFS path for @p vol. */
    static FileStatus absolute(storage::IFileSystem& vol, const std::string& path,
                               std::string& rel, std::string& full);

    /** @brief Task body of a volume check (owns the walk and the report). */
    static void checkTask(void* arg);

    /** @brief The walk itself; runs in the task created by @ref checkStart. */
    void checkWalk(const std::string& mountPoint);

    /** @brief True when a cancel was requested (locks the report mutex). */
    bool checkStopRequested();

    /** @brief Update the live counters of the report (locks the mutex). */
    void checkSetProgress(const std::string& current, uint32_t dirs, uint32_t files,
                          uint32_t badEntries, uint64_t bytes);

    /** @brief Publish the finished report (locks the mutex). */
    void checkFinish(const CheckReport& report);

    std::vector<std::unique_ptr<storage::IFileSystem>> volumes_;
    uint32_t lastRetryMs_ = 0;

    // Volume check job (one at a time, like the DNS cache persist job). The
    // handles are `void*` so this header stays free of FreeRTOS includes.
    void* checkMutex_ = nullptr;
    void* checkTask_ = nullptr;
    bool checkCancel_ = false;
    std::string checkMount_;
    CheckReport checkState_;

    // LAN-only access (see IFileManager::applyAccessFilter)
    bool allowOwnSubnet_ = true;
    bool subnetValid_ = false;
    uint32_t subnetNet_ = 0;
    uint32_t subnetMask_ = 0;
    uint32_t foreignBlocked_ = 0;
    uint32_t lastForeignLogMs_ = 0;
};

} // namespace files
} // namespace dhcp

#endif // DHCP_FILES_FILEMANAGER_H
