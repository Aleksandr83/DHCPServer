# Directories shared by all targets.
set(DHCPSERVER_SRC_DIRS
    "."
    "core"
    "eth"
    "dhcp"
    "dns"
    "web"
    "storage"
    "led"
    "menu")

# Component dependencies used directly by the application sources. Native
# idf.py builds resolve headers strictly through this REQUIRES list, so every
# IDF component whose public headers are included must be listed here.
set(DHCPSERVER_REQUIRES
    esp_http_client
    esp_http_server
    esp_netif
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
    list(APPEND DHCPSERVER_REQUIRES esp_partition fatfs wear_levelling)
else()
    list(APPEND DHCPSERVER_SRC_DIRS "wifi")
    list(APPEND DHCPSERVER_REQUIRES enc28j60 esp_wifi)
endif()

idf_component_register(
    SRC_DIRS ${DHCPSERVER_SRC_DIRS}
    INCLUDE_DIRS "."
    REQUIRES ${DHCPSERVER_REQUIRES}
)
