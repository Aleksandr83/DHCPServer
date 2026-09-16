# Directories shared by all targets.
#
# `idf_component_register(SRC_DIRS …)` collects the sources when the project is
# configured, so a *new* file (or a new directory added to this list) only enters
# the build after `idf.py reconfigure` — touching this file has the same effect,
# because CMake re-runs when a CMakeLists is newer than its cache.
set(DHCPSERVER_SRC_DIRS
    "."
    "core"
    "eth"
    "dhcp"
    "dns"
    "time"
    "web"
    "storage"
    "files"
    "led"
    "menu")

# Component dependencies used directly by the application sources. Native
# idf.py builds resolve headers strictly through this REQUIRES list, so every
# IDF component whose public headers are included must be listed here.
set(DHCPSERVER_REQUIRES
    esp_http_client
    esp_http_server
    esp_netif
    lwip
    esp_eth
    esp_timer
    nvs_flash
    spiffs
    app_update
    mbedtls)

# Classic ESP32 uses Wi-Fi coexistence over the ENC28J60 (SPI Ethernet).
# ESP32-P4 has no Wi-Fi — Ethernet uses the internal RMII EMAC, and the FAT
# data partition is served through the FATFS + wear-levelling drivers.
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND DHCPSERVER_REQUIRES esp_partition fatfs wear_levelling
                                    sdmmc esp_driver_sdmmc)
else()
    list(APPEND DHCPSERVER_SRC_DIRS "wifi")
    list(APPEND DHCPSERVER_REQUIRES enc28j60 esp_wifi)
endif()

idf_component_register(
    SRC_DIRS ${DHCPSERVER_SRC_DIRS}
    INCLUDE_DIRS "."
    REQUIRES ${DHCPSERVER_REQUIRES}
)
