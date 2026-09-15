# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

set(app_sources
    DEMO_LVGL.c
    battery_gauge.c
    pour_archive.c
    ota_manager.c
    wifi_manager.c
    esp_bsp.c
    esp_lcd_axs15231b.c
    esp_lcd_touch.c
    lv_port.c
    recipes.c
    splash_logo.c
    font_weight_72.c)

idf_component_register(SRCS ${app_sources}
                       REQUIRES nvs_flash esp_wifi esp_netif esp_event lwip fatfs sdmmc driver
                                esp_https_ota app_update mbedtls)
