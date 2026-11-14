idf_component_register(
    SRCS "app_main.c" "app_state.c" "app_cmd.c" "app_ui.c" "app_fetch.c" "app_muse.c" "app_led.c" "app_knob.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES campus_net dash_model dash_ui lvgl cjson
                  esp_driver_rmt esp_driver_pcnt esp_driver_gpio esp_driver_usb_serial_jtag esp_event esp_http_client esp_netif esp_timer
                  nvs_flash mbedtls espressif__mqtt
    EMBED_TXTFILES "certs/hub.pem"
                   "../fixtures/1_ok.json" "../fixtures/2_gh_failed.json"
                   "../fixtures/3_gh_running.json" "../fixtures/4_server_down.json"
)
