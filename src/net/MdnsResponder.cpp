#include "MdnsResponder.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "mdns.h"
#endif

#include "esp_log.h"

using namespace std;

static const char* TAG = "MdnsResponder";

namespace dhcp {
namespace net {

bool MdnsResponder::available()
{
#if CONFIG_IDF_TARGET_ESP32P4
    return true;
#else
    return false;
#endif
}

bool MdnsResponder::start(const string& hostName)
{
#if CONFIG_IDF_TARGET_ESP32P4
    if (!running_) {
        const esp_err_t err = mdns_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
            return false;
        }
        running_ = true;
    }

    const esp_err_t err = mdns_hostname_set(hostName.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set(%s) failed: %s",
                 hostName.c_str(), esp_err_to_name(err));
        stop();
        return false;
    }
    // What a browser shows next to the name when it lists the device.
    mdns_instance_name_set(hostName.c_str());
    ESP_LOGI(TAG, "Publishing \"%s.local\"", hostName.c_str());
    return true;
#else
    (void)hostName;
    ESP_LOGW(TAG, "mDNS is not part of this build (ESP32-P4 only)");
    return false;
#endif
}

void MdnsResponder::stop()
{
#if CONFIG_IDF_TARGET_ESP32P4
    if (!running_) return;
    mdns_free();
    running_ = false;
    ESP_LOGI(TAG, "Stopped publishing the local name");
#endif
}

bool MdnsResponder::setHostName(const string& hostName)
{
#if CONFIG_IDF_TARGET_ESP32P4
    if (!running_) return true;
    const esp_err_t err = mdns_hostname_set(hostName.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set(%s) failed: %s",
                 hostName.c_str(), esp_err_to_name(err));
        return false;
    }
    mdns_instance_name_set(hostName.c_str());
    ESP_LOGI(TAG, "Publishing \"%s.local\"", hostName.c_str());
    return true;
#else
    (void)hostName;
    return true;
#endif
}

MdnsResponder::~MdnsResponder()
{
    stop();
}

} // namespace net
} // namespace dhcp
