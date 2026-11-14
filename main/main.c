#include <stdio.h>
#include "lv_port.h"
#include "xl9555.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "ft6336u_driver.h"
#include "gui_guider.h"
#include "gg_utils.h"
#include "custom.h"
#include "button.h"
#include "ap_wifi.h"
#include "time.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "weather.h"
#include "esp_spiffs.h"
#include "animotion.h"
#include "sd_card.h"
#include "usb_camera.h"

#define TAG "main"

#define XL9555_SDA      GPIO_NUM_10
#define XL9555_SCL      GPIO_NUM_11

#define LCD_RST_IO      IO1_3
#define LCD_BL_IO       IO1_2
#define SPK_EN_IO       IO0_0  

gg_ui_t guider_ui;

static volatile uint16_t xl9555_button_level = 0xFFFF;

static void spiffs_mount(const char *base_path, const char *partition)
{
    esp_vfs_spiffs_conf_t conf =
    {
        .base_path = base_path,
        .format_if_mount_failed = false,
        .max_files = 5,
        .partition_label = partition
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
}

static void img_spiffs_init(void)
{
    spiffs_mount("/img", "img");
}

static void bootani_spiffs_init(void) __attribute__((unused));
static void bootani_spiffs_init(void)
{
    spiffs_mount("/bootani", "bootani");
}

int get_button_level(int gpio)
{
    return (xl9555_button_level&gpio)?1:0;
}

void short_press(int gpio)
{
    ESP_LOGD(TAG,"Button %d short press",gpio);
}

void long_press(int gpio)
{
    ESP_LOGD(TAG,"Button %d long press",gpio);
}

void button_init(void)
{
    static const uint16_t button_pins[] = { IO0_1, IO0_2, IO0_3, IO0_4 };
    button_config_t button_cfg =
    {
        .active_level = 0,
        .get_level_callback = get_button_level,
        .long_press_callback = long_press,
        .long_press_time = 3000,
        .short_press_callback = short_press,
    };

    for (int i = 0; i < (int)(sizeof(button_pins) / sizeof(button_pins[0])); i++) {
        button_cfg.gpio_num = button_pins[i];
        button_event_set(&button_cfg);
    }
}

void xl9555_input_callback(uint16_t pin, int level)
{
      if(level)
    {
        xl9555_button_level |= pin;
    }
    else
    {
        xl9555_button_level &= ~pin;
    }
    if(pin == IO1_1)
    {
        ft6336u_int_info(level ? false : true);
    }
}

void sntp_finish_callback(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP synced, epoch = %lld", (long long)tv->tv_sec);
}

void my_sntp_init()
{
    if(!esp_sntp_enabled())
    {
        ESP_LOGI(TAG,"stnp_init");
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0,"ntp.aliyun.com");
        esp_sntp_setservername(1,"time.asia.apple.com");
        esp_sntp_set_time_sync_notification_cb(sntp_finish_callback);
        esp_sntp_init();
    }
}

void wifi_state_cb(WIFI_STATE state)
{
    if(state == WIFI_STATE_CONNECTED)
    {
        my_sntp_init();
        ESP_LOGI(TAG,"WIFI connected!");
    }
}

void app_main(void)
{
    nvs_flash_init();

    xl9555_init(XL9555_SCL,XL9555_SDA,GPIO_NUM_17,xl9555_input_callback);
    xl9555_pin_write(SPK_EN_IO, 0);
    xl9555_ioconfig((~(LCD_RST_IO | LCD_BL_IO | SPK_EN_IO))&0xFFFF);
    xl9555_pin_write(LCD_BL_IO,1);
    img_spiffs_init();
    usb_camera_init();
    bootani_spiffs_init();
    sd_card_init();
    lv_port_init();
    animotion_play();   
    lvgl_port_lock(0);
    setup_ui(&guider_ui);
    custom_init(&guider_ui);
    lvgl_port_unlock();
    button_init();
    ap_wifi_init(wifi_state_cb);
    setenv("TZ","CST-8",1);
    tzset();
    weather_start();
}
