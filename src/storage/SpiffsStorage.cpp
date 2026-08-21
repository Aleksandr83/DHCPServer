#include "SpiffsStorage.h"
#include <algorithm>
#include <cstring>

#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_err.h"

static const char* TAG = "SpiffsStorage";

namespace dhcp {
namespace storage {

SpiffsStorage::SpiffsStorage(const std::string& basePath, size_t maxFiles)
    : basePath_(basePath)
    , maxFiles_(maxFiles)
{
}

SpiffsStorage::~SpiffsStorage()
{
    if (mounted_) {
        unmount();
    }
}

bool SpiffsStorage::mount()
{
    if (mounted_) {
        ESP_LOGW(TAG, "Already mounted");
        return true;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = basePath_.c_str(),
        .partition_label = "spiffs",
        .max_files = static_cast<size_t>(maxFiles_),
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed (%s)", esp_err_to_name(ret));
        return false;
    }

    // Check consistency
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS info failed (%s)", esp_err_to_name(ret));
        esp_vfs_spiffs_unregister(conf.partition_label);
        return false;
    }

    ESP_LOGI(TAG, "SPIFFS mounted: %zu total, %zu used", total, used);
    mounted_ = true;
    return true;
}

void SpiffsStorage::unmount()
{
    if (!mounted_) return;
    esp_vfs_spiffs_unregister("spiffs");
    mounted_ = false;
    ESP_LOGI(TAG, "SPIFFS unmounted");
}

bool SpiffsStorage::isMounted() const
{
    return mounted_;
}

std::string SpiffsStorage::fullPath(const std::string& path) const
{
    // If path already starts with basePath, use as-is
    if (path.rfind(basePath_, 0) == 0) {
        return path;
    }
    return basePath_ + path;
}

std::vector<uint8_t> SpiffsStorage::readFile(const std::string& path)
{
    std::vector<uint8_t> result;
    if (!mounted_) return result;

    std::string fpath = fullPath(path);
    FILE* f = fopen(fpath.c_str(), "rb");
    if (!f) return result;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size > 0) {
        result.resize(static_cast<size_t>(size));
        fread(result.data(), 1, static_cast<size_t>(size), f);
    }
    fclose(f);
    return result;
}

std::string SpiffsStorage::readTextFile(const std::string& path)
{
    auto data = readFile(path);
    return std::string(data.begin(), data.end());
}

bool SpiffsStorage::writeFile(const std::string& path, const std::vector<uint8_t>& data)
{
    if (!mounted_) return false;

    std::string fpath = fullPath(path);
    FILE* f = fopen(fpath.c_str(), "wb");
    if (!f) return false;

    if (!data.empty()) {
        fwrite(data.data(), 1, data.size(), f);
    }
    fclose(f);
    return true;
}

bool SpiffsStorage::writeTextFile(const std::string& path, const std::string& text)
{
    std::vector<uint8_t> data(text.begin(), text.end());
    return writeFile(path, data);
}

bool SpiffsStorage::exists(const std::string& path)
{
    if (!mounted_) return false;
    std::string fpath = fullPath(path);
    FILE* f = fopen(fpath.c_str(), "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

bool SpiffsStorage::remove(const std::string& path)
{
    if (!mounted_) return false;
    std::string fpath = fullPath(path);
    return (::remove(fpath.c_str()) == 0);
}

bool SpiffsStorage::stats(size_t& total, size_t& used) const
{
    if (!mounted_) return false;
    return (esp_spiffs_info("spiffs", &total, &used) == ESP_OK);
}

} // namespace storage
} // namespace dhcp
