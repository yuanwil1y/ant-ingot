idf_component_register(
    SRCS "app_main.c" "hw.c" "net.c" "store.c" "ble.c" "surf.c"
         "core/scene.c" "core/ingot.c" "core/market.c" "core/motion.c" "core/gfx.c" "core/fonts.c" "core/qrcodegen.c"
    INCLUDE_DIRS "." "core"
    EMBED_FILES "portal.html"
    REQUIRES esp_lcd esp_driver_spi esp_driver_i2c esp_driver_ledc esp_driver_gpio
             esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_http_client
             esp-tls mbedtls json bt dns_server esp_timer esp_partition spi_flash)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-misleading-indentation -Wno-format-truncation)
