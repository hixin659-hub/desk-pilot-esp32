#include <stdio.h>
#include <time.h>
#include "custom.h"
#include "esp_lvgl_port.h"

LV_FONT_DECLARE(lv_font_ziti15);

static const char *week_cn[] = {"日", "一", "二", "三", "四", "五", "六"};

static void clock_timer_cb(lv_timer_t * timer)
{
    gg_ui_t * ui = lv_timer_get_user_data(timer);

    if (ui->screen_home.screen == NULL) {
        return;
    }
    time_t now = time(NULL);
    if (now < 1700000000) {
        return;
    }
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    lv_label_set_text_fmt(ui->screen_home.image_6_label_time, "%02d:%02d:%02d",
                          tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);

    lv_label_set_text_fmt(ui->screen_home.image_6_label_date, "%d年%02d月%02d日",
                          tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);

    lv_label_set_text_fmt(ui->screen_home.image_6_label_week, "星期%s",
                          week_cn[tm_now.tm_wday]);
}

static void set_forecast(gg_ui_t *ui,lv_obj_t *img,lv_obj_t *label_tmp,
                         const char *img_path,int low,int high)
{
    char text[32];

    lvgl_port_lock(0);
    lv_image_set_src(img,img_path);
    snprintf(text,sizeof(text),"%d-%d℃",low,high);
    lv_label_set_text(label_tmp,text);
    lvgl_port_unlock();
}

void set_today_img(gg_ui_t *ui,const char* img_path,int low,int high)
{
    set_forecast(ui,ui->screen_home.image_6_image_today,
                 ui->screen_home.image_6_label_tmp_1,img_path,low,high);
}

void set_tomorrow_img(gg_ui_t *ui,const char* img_path,int low,int high)
{
    set_forecast(ui,ui->screen_home.image_6_image_tomorrow,
                 ui->screen_home.image_6_label_tmp_2,img_path,low,high);
}

void set_after_img(gg_ui_t *ui,const char* img_path,int low,int high)
{
    set_forecast(ui,ui->screen_home.image_6_image_after,
                 ui->screen_home.image_6_label_tmp_3,img_path,low,high);
}

void set_home_city(gg_ui_t *ui,const char* city)
{
    char city_text[32];

    lvgl_port_lock(0);
    snprintf(city_text,sizeof(city_text),"%s",city);
    lv_label_set_text(ui->screen_home.image_6_label_ip,city_text);
    lvgl_port_unlock();
}

void home_weather_init(gg_ui_t *ui)
{
    lv_obj_set_style_text_font(ui->screen_home.image_6_label_week, &lv_font_ziti15,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_home.image_6_label_ip, &lv_font_ziti15,
                               LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_timer_create(clock_timer_cb, 1000, ui);
}
