#include <stdio.h>
#include "custom.h"
#include "esp_lvgl_port.h"

LV_FONT_DECLARE(lv_font_ziti15);

void set_monitor_param(gg_ui_t *ui,int cpu_rate,int cpu_temp,int mem_rate,int mem_use)
{
    char value[16];

    lvgl_port_lock(0);
    snprintf(value,sizeof(value),"%d%%",cpu_rate);
    lv_label_set_text(ui->screen_monitor.container_1_label_cpu_used,value);

    snprintf(value,sizeof(value),"%d℃",cpu_temp);
    lv_label_set_text(ui->screen_monitor.container_1_label_tmp_num,value);

    snprintf(value,sizeof(value),"%d%%",mem_rate);
    lv_label_set_text(ui->screen_monitor.container_2_label_mem_used,value);

    snprintf(value,sizeof(value),"%dMB",mem_use);
    lv_label_set_text(ui->screen_monitor.container_2_label_mem_num,value);

    lvgl_port_unlock();
}

void set_monitor_connect_status(gg_ui_t *ui,bool flag)
{
    static bool fonts_set = false;

    lvgl_port_lock(0);

    /* 字体只需要设一次，别每次连接状态变化都重复设置 */
    if(!fonts_set)
    {
        lv_obj_set_style_text_font(ui->screen_monitor.label_status, &lv_font_ziti15,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(ui->screen_monitor.button_connect_button_1_label, &lv_font_ziti15,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        fonts_set = true;
    }
    if(!flag)
    {
        lv_label_set_text(ui->screen_monitor.label_status,"未连接");
        lv_label_set_text(ui->screen_monitor.button_connect_button_1_label,"连接");
        lv_obj_set_style_text_color(ui->screen_monitor.label_status,
                                    lv_color_hex(0xffb74d), 0);
    }
    else
    {
        lv_label_set_text(ui->screen_monitor.label_status,"已连接");
        lv_label_set_text(ui->screen_monitor.button_connect_button_1_label,"断开");
        lv_obj_set_style_text_color(ui->screen_monitor.label_status,
                                    lv_color_hex(0x7bd88f), 0);
    }
    lvgl_port_unlock();
}
