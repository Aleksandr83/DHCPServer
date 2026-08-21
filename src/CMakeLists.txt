idf_component_register(
    SRC_DIRS "."
             "core"
             "wifi"
             "eth"
             "dhcp"
             "dns"
             "web"
             "storage"
             "led"
             "menu"
    INCLUDE_DIRS "."
    REQUIRES enc28j60 esp_http_client
)
