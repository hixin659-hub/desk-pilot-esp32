set(INCLUDES "." "./ui/generated" "./ui/custom" "./ui/generated/assets/fonts" "./ui/generated/assets/images"
    "./ui/generated/events")
file(GLOB_RECURSE SOURCES ./*.c)

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${INCLUDES}
                    PRIV_REQUIRES ap_wifi esp_lcd driver nvs_flash lvgl esp_lvgl_port lwip
                                  esp_http_client mbedtls espressif__cjson spiffs
                                  fatfs sdmmc sd_card esp_driver_i2s esp_timer)

spiffs_create_partition_image(html ../html FLASH_IN_PROJECT)
spiffs_create_partition_image(img ../img FLASH_IN_PROJECT)
spiffs_create_partition_image(bootani ../bootani FLASH_IN_PROJECT)