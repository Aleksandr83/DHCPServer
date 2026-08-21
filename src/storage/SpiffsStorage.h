#ifndef DHCP_STORAGE_SPIFFSSTORAGE_H
#define DHCP_STORAGE_SPIFFSSTORAGE_H

#include "IStorage.h"
#include <string>

namespace dhcp {
namespace storage {

/**
 * @brief SPIFFS implementation of IStorage.
 *
 * Mounts the "spiffs" partition at /spiffs.
 */
class SpiffsStorage : public IStorage {
public:
    /**
     * @param basePath  Mount point (default "/spiffs").
     * @param maxFiles  Max open files (default 5).
     */
    explicit SpiffsStorage(const std::string& basePath = "/spiffs",
                           size_t maxFiles = 5);

    ~SpiffsStorage() override;

    bool mount() override;
    void unmount() override;
    bool isMounted() const override;

    std::vector<uint8_t> readFile(const std::string& path) override;
    std::string readTextFile(const std::string& path) override;
    bool writeFile(const std::string& path, const std::vector<uint8_t>& data) override;
    bool writeTextFile(const std::string& path, const std::string& text) override;
    bool exists(const std::string& path) override;
    bool remove(const std::string& path) override;

    /**
     * @brief Get total and used bytes on SPIFFS.
     */
    bool stats(size_t& total, size_t& used) const;

private:
    std::string fullPath(const std::string& path) const;

    std::string basePath_;
    size_t maxFiles_;
    bool mounted_ = false;
};

} // namespace storage
} // namespace dhcp

#endif // DHCP_STORAGE_SPIFFSSTORAGE_H
