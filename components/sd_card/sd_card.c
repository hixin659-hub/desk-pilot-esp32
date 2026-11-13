#include <stdio.h>
#include <string.h>
#include "sd_card.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"

#define TAG     "sd_card"
#define MOUNT_POINT  "/sd"     //挂载点

#define SD_CS    GPIO_NUM_4
#define SD_MOSI  GPIO_NUM_5   // 接 TF 卡 CMD
#define SD_CLK   GPIO_NUM_6   // 接 TF 卡 CLK
#define SD_MISO  GPIO_NUM_7   // 接 TF 卡 DATA0

static sdmmc_card_t *s_card = NULL;   

esp_err_t sd_card_init(void)
{
    if (s_card != NULL) {
        return ESP_OK;  
    }
    esp_err_t ret;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdmmc_card_t* card;
    // SPI 总线配置
    spi_bus_config_t bus_config =
    {
        .mosi_io_num = SD_MOSI,  
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_CLK,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
        .max_transfer_sz = 4000,      // 单次 DMA 传输的最大字节数
    };
    //SPI总线初始化
    ret = spi_bus_initialize(host.slot,&bus_config,SPI_DMA_CH_AUTO);
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "spi init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS;
    slot_config.host_id = host.slot;   // 设备挂在SPI控制器SPI2_HOST
    // FAT 文件系统的挂载参数
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,  // 挂载失败时不自动格式化，避免误删卡里的数据
        .max_files = 5,
        .allocation_unit_size = 16*1024,  // 簇大小 16KB，仅在需要格式化时生效；
                                          // 簇越大读写越快，但存小文件浪费空间
    };

    //SD卡挂载成FAT文件系统，并注册到VFS
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(ret));
        spi_bus_free(host.slot);
        return ret;
    }

    s_card = card;
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    return ESP_OK;
}

esp_err_t sd_card_deinit(void)
{
    if (s_card == NULL) {
        return ESP_OK;  
    }
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "unmount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_card = NULL;  
    ret = spi_bus_free(SDSPI_DEFAULT_HOST);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi bus free failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}

