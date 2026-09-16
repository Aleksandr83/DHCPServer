#include "FatFileSystem.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"

namespace dhcp {
namespace storage {

namespace {
const char* TAG = "FatFileSystem";
}

FatFileSystem::FatFileSystem(std::string id, std::string partitionLabel,
                             std::string mountPoint)
    : id_(std::move(id))
    , partitionLabel_(std::move(partitionLabel))
    , mountPoint_(std::move(mountPoint))
{
}

bool FatFileSystem::mount()
{
    if (mounted_) return true;

    // The partition is optional: the legacy 4 MB ESP32 table has no `fat` row,
    // in which case this volume simply stays absent.
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
        partitionLabel_.c_str());
    if (part == nullptr) {
        partitionFound_ = false;
        error_ = "no FAT partition in the partition table";
        ESP_LOGW(TAG, "%s: no FAT partition '%s' in the partition table",
                 id_.c_str(), partitionLabel_.c_str());
        return false;
    }
    partitionFound_ = true;

    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 10,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    wlHandle_ = WL_INVALID_HANDLE;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        mountPoint_.c_str(), partitionLabel_.c_str(), &cfg, &wlHandle_);
    if (err != ESP_OK) {
        error_ = std::string("mount failed: ") + esp_err_to_name(err);
        ESP_LOGE(TAG, "%s: mount of '%s' failed (%s)", id_.c_str(),
                 partitionLabel_.c_str(), esp_err_to_name(err));
        return false;
    }

    mounted_ = true;
    error_.clear();
    ESP_LOGI(TAG, "%s mounted at %s (%u bytes)", id_.c_str(),
             mountPoint_.c_str(), (unsigned)part->size);
    return true;
}

void FatFileSystem::unmount()
{
    if (!mounted_) return;

    esp_err_t err =
        esp_vfs_fat_spiflash_unmount_rw_wl(mountPoint_.c_str(), wlHandle_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: unmount failed (%s)", id_.c_str(),
                 esp_err_to_name(err));
    }
    wlHandle_ = WL_INVALID_HANDLE;
    mounted_ = false;
}

void FatFileSystem::verify()
{
    // The volume lives in the flash chip of the board itself: it cannot be
    // unplugged while the firmware runs, so there is nothing to check.
    // (Also nothing to ask: there is no bus behind it.)
}

VolumeInfo FatFileSystem::info()
{
    VolumeInfo out;
    out.id = id_;
    out.mountPoint = mountPoint_;
    out.mounted = mounted_;
    out.present = partitionFound_;
    out.error = error_;

    if (mounted_) {
        uint64_t total = 0, free = 0;
        if (esp_vfs_fat_info(mountPoint_.c_str(), &total, &free) == ESP_OK) {
            out.totalBytes = total;
            out.freeBytes = free;
        } else {
            ESP_LOGW(TAG, "%s: esp_vfs_fat_info failed", id_.c_str());
        }
    }
    return out;
}

bool FatFileSystem::format()
{
    // Not offered: this is the internal flash partition, not a removable
    // medium. (Formatting is offered for the microSD volume only.)
    return false;
}

} // namespace storage
} // namespace dhcp
