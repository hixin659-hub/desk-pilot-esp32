#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "xl9555.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ft6336u_driver.h"
#include "lvgl.h"
#include <stdio.h>

#define TAG "lvgl"

#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define LCD_RST_IO  IO1_3

static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t lcd_panel = NULL;
static lv_display_t* lv_disp = NULL;
static lv_indev_t *touch_indev = NULL;

void lv_disp_hard_init(void)
{
    ESP_LOGI(TAG, "Initialize Intel 8080 bus");
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = GPIO_NUM_1,
        .wr_gpio_num = GPIO_NUM_41,
        .data_gpio_nums = {
            GPIO_NUM_40,
            GPIO_NUM_38,
            GPIO_NUM_39,
            GPIO_NUM_48,
            GPIO_NUM_45,
            GPIO_NUM_21,
            GPIO_NUM_47,
            GPIO_NUM_14,
        },
        .bus_width = 8,
        .max_transfer_bytes = LCD_WIDTH * LCD_HEIGHT,
        .dma_burst_size = 64,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = GPIO_NUM_2,
        .pclk_hz = 20*1000*1000,
        .trans_queue_depth = 10,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags =
        {
            .swap_color_bytes = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install LCD driver of st7789");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &lcd_panel));

    esp_lcd_panel_reset(lcd_panel);
    esp_lcd_panel_init(lcd_panel);
    esp_lcd_panel_swap_xy(lcd_panel,true);
    esp_lcd_panel_mirror(lcd_panel, false, true);
    esp_lcd_panel_disp_on_off(lcd_panel, true);
}

void indev_read(lv_indev_t * indev, lv_indev_data_t * data)
{
    int16_t x = 0,y = 0;
    int state = 0;
    ft6336u_read(&x,&y,&state);
    data->point.x = LCD_WIDTH - y - 1;
    data->point.y = x;
    data->state = state;
}

/* LVGL 文件系统桥接：把已挂载到 VFS 的 /img(spiffs) 接给 LVGL，
   使 lv_img_set_src("S:/img/xxx.png") 能读取文件系统中的图片 */
static lv_fs_drv_t img_fs_drv;

static void * img_fs_open(lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode)
{
    const char * mode_str = (mode & LV_FS_MODE_WR) ? "wb" : "rb";
    return (void *)fopen(path, mode_str);
}

static lv_fs_res_t img_fs_close(lv_fs_drv_t * drv, void * file_p)
{
    fclose((FILE *)file_p);
    return LV_FS_RES_OK;
}

static lv_fs_res_t img_fs_read(lv_fs_drv_t * drv, void * file_p, void * buf, uint32_t btr, uint32_t * br)
{
    *br = fread(buf, 1, btr, (FILE *)file_p);
    return (*br > 0 || feof((FILE *)file_p)) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t img_fs_seek(lv_fs_drv_t * drv, void * file_p, uint32_t pos, lv_fs_whence_t whence)
{
    int w = (whence == LV_FS_SEEK_CUR) ? SEEK_CUR : (whence == LV_FS_SEEK_END) ? SEEK_END : SEEK_SET;
    fseek((FILE *)file_p, (long)pos, w);
    return LV_FS_RES_OK;
}

static lv_fs_res_t img_fs_tell(lv_fs_drv_t * drv, void * file_p, uint32_t * pos_p)
{
    *pos_p = (uint32_t)ftell((FILE *)file_p);
    return LV_FS_RES_OK;
}

static void img_fs_init(void)
{
    lv_fs_drv_init(&img_fs_drv);
    img_fs_drv.letter = 'S';
    img_fs_drv.open_cb = img_fs_open;
    img_fs_drv.close_cb = img_fs_close;
    img_fs_drv.read_cb = img_fs_read;
    img_fs_drv.seek_cb = img_fs_seek;
    img_fs_drv.tell_cb = img_fs_tell;
    lv_fs_drv_register(&img_fs_drv);
}

void lv_port_init(void)
{
    xl9555_pin_write(LCD_RST_IO,0);
    vTaskDelay(pdMS_TO_TICKS(100));
    xl9555_pin_write(LCD_RST_IO,1);

    lv_disp_hard_init();
    ft6336u_driver(GPIO_NUM_13,GPIO_NUM_12);

    /* Initialize LVGL */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,         /* LVGL task priority */
        .task_stack = 8192,         /* LVGL task stack size */
        .task_affinity = 1,        /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500,   /* Maximum sleep in LVGL task */
        .timer_period_ms = 5       /* LVGL timer tick period in ms */
    };
    lvgl_port_init(&lvgl_cfg);
    img_fs_init();

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = lcd_panel,
        /* 内部 RAM 紧张：320*32*2 ≈ 20KB，比原来的 320*80*2（≈50KB）省约 30KB，
           用于给 USB 摄像头 URB 和 SD 卡 DMA 留空间 */
        .buffer_size = LCD_WIDTH * 32,
        .double_buffer = 0,
        .hres = LCD_WIDTH,
        .vres = LCD_HEIGHT,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,

        }
    };
    lv_disp = lvgl_port_add_disp(&disp_cfg);
    lvgl_port_lock(0);
    touch_indev = lv_indev_create();
    lv_indev_set_type(touch_indev,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_indev,indev_read);

    /* LVGL 默认的“刷屏周期”和“触摸采样周期”都是 LV_DEF_REFR_PERIOD=33ms，
       一次点击最坏要等 30~70ms 才看到画面变化，点底部标签时会显得发顿。
       这里都降到 10ms，和 FreeRTOS 100Hz 的 tick 对齐，跟手性明显变好。 */
    lv_timer_t *refr_timer = lv_display_get_refr_timer(lv_disp);
    if (refr_timer) {
        lv_timer_set_period(refr_timer, 10);
    }
    lv_timer_set_period(lv_indev_get_read_timer(touch_indev), 10);

    lvgl_port_unlock();
}
