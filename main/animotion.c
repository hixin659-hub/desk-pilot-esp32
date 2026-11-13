#include "animotion.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>

#define TAG "animotion"

#define ANIM_FRAME_COUNT     41     //总帧数（bootani 目录下共 41 张）
#define ANIM_FRAME_PERIOD_MS 100    //每帧停留 100ms = 10fps，全程约 5.2 秒
#define ANIM_FRAME_W         320
#define ANIM_FRAME_H         240
#define ANIM_FRAME_PATH      "S:/bootani/frame_%03d.png"

static lv_obj_t   *s_anim_img   = NULL;
static lv_timer_t *s_anim_timer = NULL;
static uint32_t    s_frame_idx  = 0;
static bool        s_done       = true;
static bool        s_started    = false;

static void anim_finish(void)
{
    if(s_anim_img) {
        lv_obj_delete(s_anim_img);
        s_anim_img = NULL;
    }
    s_done = true;
}

//LVGL 定时器回调
static void anim_timer_cb(lv_timer_t * timer)
{
    char path[64];

    s_frame_idx++;
    if(s_frame_idx > ANIM_FRAME_COUNT) {
        //最后一帧播完,图像和定时器一起销毁
        anim_finish();
        s_anim_timer = NULL;
        lv_timer_delete(timer);
        return;
    }
    snprintf(path, sizeof(path), ANIM_FRAME_PATH, (int)s_frame_idx);
    lv_image_set_src(s_anim_img, path);
}

void animotion_play(void)
{
    char path[64];
    lv_image_header_t header;

    if(s_started) {
        return;
    }
    s_started = true;

    snprintf(path, sizeof(path), ANIM_FRAME_PATH, 1);

    lvgl_port_lock(0);

    //预检第 1 帧能否解码
    if(lv_image_decoder_get_info(path, &header) != LV_RESULT_OK) {
        lvgl_port_unlock();
        ESP_LOGW(TAG, "can't read %s, skip boot animation", path);
        return;
    }

    s_done = false;
    s_frame_idx = 1;

    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

    s_anim_img = lv_image_create(lv_layer_sys());
    lv_obj_set_size(s_anim_img, ANIM_FRAME_W, ANIM_FRAME_H);
    lv_obj_set_pos(s_anim_img, 0,
                   (lv_display_get_vertical_resolution(lv_display_get_default()) - ANIM_FRAME_H) / 2);
    lv_image_set_src(s_anim_img, path);

    s_anim_timer = lv_timer_create(anim_timer_cb, ANIM_FRAME_PERIOD_MS, NULL);

    lvgl_port_unlock();
}

bool animotion_is_done(void)
{
    return s_done;
}
