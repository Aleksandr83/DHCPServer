#ifndef DHCP_STORAGE_FATFILESYSTEM_H
#define DHCP_STORAGE_FATFILESYSTEM_H

#include <string>

#include "IFileSystem.h"
#include "wear_levelling.h"

namespace dhcp {
namespace storage {

/**
 * @brief Internal FAT volume on the flash data partition (`/fat`).
 *
 * This is the mount that used to live inline in `main.cpp`: the `fat`
 * partition of the ESP32-P4 partition table (~21 MB) served through FATFS +
 * wear levelling. The partition is optional — on the classic ESP32 table it
 * does not exist and @ref mount simply fails with a clear message, which is
 * what `FileManager` reports to the web UI.
 *
 * `format_if_mount_failed` stays enabled (historic behaviour for this
 * partition: it holds only generated data such as `cache.dat`). Formatting is
 * NOT offered through the API — the flash partition is not a removable medium.
 */
class FatFileSystem : public IFileSystem {
public:
    /**
     * @param[in] id             API id (e.g. `fat`).
     * @param[in] partitionLabel Partition label from the partition table.
     * @param[in] mountPoint     Absolute VFS mount point (`/fat`).
     */
    FatFileSystem(std::string id, std::string partitionLabel,
                  std::string mountPoint);

    // IFileSystem
    const std::string& id() const override { return id_; }
    const std::string& mountPoint() const override { return mountPoint_; }
    bool mount() override;
    void unmount() override;
    bool isMounted() const override { return mounted_; }
    bool isPresent() const override { return partitionFound_; }
    const std::string& lastError() const override { return error_; }
    void verify() override;
    VolumeInfo info() override;
    bool format() override;

private:
    std::string id_;
    std::string partitionLabel_;
    std::string mountPoint_;
    std::string error_;
    wl_handle_t wlHandle_ = WL_INVALID_HANDLE;
    bool mounted_ = false;
    bool partitionFound_ = false;
};

} // namespace storage
} // namespace dhcp

#endif // DHCP_STORAGE_FATFILESYSTEM_H
