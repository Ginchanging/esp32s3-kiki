idf_component_register(
    SRCS "campus_net.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_event esp_http_client esp_netif
                  esp_timer esp_wifi mbedtls nvs_flash wpa_supplicant
)
