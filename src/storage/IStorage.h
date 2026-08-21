#ifndef DHCP_STORAGE_ISTORAGE_H
#define DHCP_STORAGE_ISTORAGE_H

#include <string>
#include <vector>
#include <cstdint>

namespace dhcp {
namespace storage {

/**
 * @brief Abstract interface for file-based storage (SPIFFS / LittleFS).
 */
class IStorage {
public:
    virtual ~IStorage() = default;

    /**
     * @brief Mount the filesystem.
     * @return true on success.
     */
    virtual bool mount() = 0;

    /**
     * @brief Unmount the filesystem.
     */
    virtual void unmount() = 0;

    /**
     * @brief Check if filesystem is mounted.
     */
    virtual bool isMounted() const = 0;

    /**
     * @brief Read entire file content.
     * @param path  Absolute path within the filesystem.
     * @return File content as byte vector, empty on error.
     */
    virtual std::vector<uint8_t> readFile(const std::string& path) = 0;

    /**
     * @brief Read file as text string.
     */
    virtual std::string readTextFile(const std::string& path) = 0;

    /**
     * @brief Write data to file (overwrites if exists).
     * @return true on success.
     */
    virtual bool writeFile(const std::string& path, const std::vector<uint8_t>& data) = 0;

    /**
     * @brief Write text to file.
     * @return true on success.
     */
    virtual bool writeTextFile(const std::string& path, const std::string& text) = 0;

    /**
     * @brief Check if file exists.
     */
    virtual bool exists(const std::string& path) = 0;

    /**
     * @brief Delete a file.
     * @return true on success or if file did not exist.
     */
    virtual bool remove(const std::string& path) = 0;
};

} // namespace storage
} // namespace dhcp

#endif // DHCP_STORAGE_ISTORAGE_H
