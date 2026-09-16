#ifndef DHCP_STORAGE_IFILESYSTEM_H
#define DHCP_STORAGE_IFILESYSTEM_H

#include <cstdint>
#include <string>

namespace dhcp {
namespace storage {

/**
 * @brief Runtime description of one file-explorer volume.
 *
 * Reported as-is by `GET /api/files/volumes`, so the web UI can show the
 * medium state without knowing which driver is behind it.
 */
struct VolumeInfo {
    /** @brief Volume id used in the API (`fat`, `sd`). */
    std::string id;
    /** @brief Absolute VFS mount point (`/fat`, `/sdcard`). */
    std::string mountPoint;
    /** @brief True when the filesystem is mounted and usable. */
    bool mounted = false;
    /** @brief True when a medium/partition exists (card inserted / partition found). */
    bool present = false;
    /** @brief Volume capacity in bytes (0 when unknown/not mounted). */
    uint64_t totalBytes = 0;
    /** @brief Free space in bytes (0 when unknown/not mounted). */
    uint64_t freeBytes = 0;
    /** @brief Last mount error text ("" while mounted) — shown in the UI. */
    std::string error;
};

/**
 * @brief Abstract filesystem volume of the file explorer (internal FAT / microSD).
 *
 * A volume is one FAT filesystem mounted at a fixed point in the VFS
 * (`/fat` — the flash data partition, `/sdcard` — the external card). All
 * file operations themselves go through POSIX (`fopen`/`opendir`/…), so a
 * volume only has to manage mounting, capacity reporting and (optionally)
 * formatting — see `FileManager` for the use of the interface.
 */
class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    /** @brief Short id used in the API (`fat`, `sd`). */
    virtual const std::string& id() const = 0;

    /** @brief Absolute VFS path of the mount point (no trailing slash). */
    virtual const std::string& mountPoint() const = 0;

    /**
     * @brief Mount the volume.
     *
     * Idempotent: returns true immediately when already mounted. Never
     * formats the medium on failure (formatting is an explicit operation),
     * except the internal FAT which keeps its historic
     * `format_if_mount_failed` behaviour.
     *
     * @return true when the volume is mounted after the call.
     */
    virtual bool mount() = 0;

    /** @brief Unmount the volume (no-op when not mounted). */
    virtual void unmount() = 0;

    /** @brief True while the filesystem is mounted and usable. */
    virtual bool isMounted() const = 0;

    /** @brief True when the medium is known to be present (partition/card). */
    virtual bool isPresent() const = 0;

    /** @brief Last mount error text ("" when mounted). */
    virtual const std::string& lastError() const = 0;

    /**
     * @brief Check that a mounted volume is still usable; unmount it when its
     * medium has gone away.
     *
     * A filesystem does not notice a removable medium disappearing behind it —
     * FatFS keeps serving the allocation data it cached at mount time — so a
     * mounted volume has to be asked. The microSD card is queried for its
     * status on the bus (`sdmmc_get_status`, CMD13): that reads no filesystem
     * data and therefore cannot damage it.
     *
     * Driven by `FileManager::refresh()`, i.e. from the REST polling path, so
     * it must be cheap. Volumes that cannot be unplugged (the internal
     * partitions) implement it as a no-op. Not mounted → nothing to check.
     */
    virtual void verify() = 0;

    /**
     * @brief Refresh and return capacity information.
     *
     * Reads total/free bytes from the FATFS layer, so it must only be called
     * while mounted (otherwise the byte counts stay 0).
     */
    virtual VolumeInfo info() = 0;

    /**
     * @brief Format the volume (erases everything on it).
     * @return false when the volume does not support formatting.
     */
    virtual bool format() = 0;
};

} // namespace storage
} // namespace dhcp

#endif // DHCP_STORAGE_IFILESYSTEM_H
