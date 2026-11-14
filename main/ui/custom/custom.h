/*
* Copyright 2026 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/

#ifndef CUSTOM_H
#define CUSTOM_H
#ifdef __cplusplus
extern "C" {
#endif

#include "../generated/gui_guider.h"

void custom_init(gg_ui_t *ui);

/* 各功能模块的初始化*/
void home_weather_init(gg_ui_t *ui);

void album_picture_init(gg_ui_t *ui);

void album_folder_init(gg_ui_t *ui);

void album_memory_init(gg_ui_t *ui);

void set_today_img(gg_ui_t *ui,const char* img_path,int low,int high);

void set_tomorrow_img(gg_ui_t *ui,const char* img_path,int low,int high);

void set_after_img(gg_ui_t *ui,const char* img_path,int low,int high);

void set_home_city(gg_ui_t *ui,const char* city);

void set_monitor_param(gg_ui_t *ui,int cpu_rate,int cpu_temp,int mem_rate,int mem_use);

void set_monitor_connect_status(gg_ui_t *ui,bool flag);

void album_tabs_init(gg_ui_t *ui);

void album_image_open(const char *path);

void album_video_play(const char *path);

bool album_video_thumb(const char *path, uint16_t *dst, int dst_size);

bool album_video_is_playing(void);

#ifdef __cplusplus
}
#endif
#endif /* CUSTOM_H */
