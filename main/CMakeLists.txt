set(INCLUDES "." "./iot" "./ui/generated" "./ui/custom" "./ui/generated/assets/fonts"
             "./ui/generated/assets/images" "./ui/generated/events")
file(GLOB_RECURSE SOURCES ./*.c)
# 旧工程的 MQTT / 设备模型代码暂不参与编译：
# IDF 6.1 不再内置 mqtt 组件，且 led_ws2812 驱动尚未移植；OTA 走 UI 触发，不依赖它们。
list(FILTER SOURCES EXCLUDE REGEX "[/\\\\]iot[/\\\\]onenet_(mqtt|dm)\\.c$")

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${INCLUDES}
                    PRIV_REQUIRES ap_wifi esp_lcd driver nvs_flash lvgl esp_lvgl_port lwip
                                  esp_http_client mbedtls espressif__cjson spiffs
                                  fatfs sdmmc sd_card esp_driver_i2s esp_timer
                                  app_update esp_https_ota )

spiffs_create_partition_image(html ../html FLASH_IN_PROJECT)
spiffs_create_partition_image(img ../img FLASH_IN_PROJECT)
spiffs_create_partition_image(bootani ../bootani FLASH_IN_PROJECT)