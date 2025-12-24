idf_component_register(
    SRCS "gd_edge_cam.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        freertos
        log
        esp_system
        esp_timer
        esp_http_server
        esp-mqtt
        esp32-camera
)
