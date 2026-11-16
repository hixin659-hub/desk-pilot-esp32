#include <stdio.h>
#include "monitor_ui.h"
#include "custom.h"
#include "ota_ui.h"
#include "onenet_ota.h"

LV_FONT_DECLARE(lv_font_ziti15);

#define COL_BG          lv_color_hex(0x0a1f36)
#define COL_CARD        lv_color_hex(0x123b6b)
#define COL_LINE        lv_color_hex(0x1e3a5f)
#define COL_TEXT        lv_color_hex(0xffffff)
#define COL_SUBTEXT     lv_color_hex(0xb7edef)
#define COL_ACCENT      lv_color_hex(0xffd32c)
#define COL_ACCENT_TXT  lv_color_hex(0x0a1f36)
#define COL_WARN        lv_color_hex(0xffb74d)

static gg_ui_t *s_ui = NULL;

static void gear_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        ota_ui_open();
    }
}

static void style_bar_label(lv_obj_t *lbl, lv_color_t color)
{
    lv_obj_set_style_text_font(lbl, &lv_font_ziti15, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
}

static void style_card(lv_obj_t *card)
{
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
}

static void monitor_ui_apply(gg_ui_t *ui)
{
    lv_obj_t *screen = ui->screen_monitor.screen;

    /* 深色背景，和升级弹窗统一 */
    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* CPU / 内存卡片：下移给顶栏让位，颜色统一 */
    lv_obj_set_pos(ui->screen_monitor.container_1, 8, 40);
    lv_obj_set_pos(ui->screen_monitor.container_2, 164, 40);
    lv_obj_set_size(ui->screen_monitor.container_1, 150, 100);
    lv_obj_set_size(ui->screen_monitor.container_2, 150, 100);
    style_card(ui->screen_monitor.container_1);
    style_card(ui->screen_monitor.container_2);

    lv_obj_set_style_text_color(ui->screen_monitor.container_1_label_cpu, COL_ACCENT, 0);
    lv_obj_set_style_text_color(ui->screen_monitor.container_1_label_used_1, COL_SUBTEXT, 0);
    lv_obj_set_style_text_color(ui->screen_monitor.container_1_label_tmp, COL_SUBTEXT, 0);
    lv_obj_set_style_text_color(ui->screen_monitor.container_1_label_cpu_used, COL_TEXT, 0);
    lv_obj_set_style_text_color(ui->screen_monitor.container_1_label_tmp_num, COL_TEXT, 0);

    lv_obj_set_style_text_color(ui->screen_monitor.container_2_label_mem, COL_ACCENT, 0);
    lv_obj_set_style_text_color(ui->screen_monitor.container_2_label_used_2, COL_SUBTEXT, 0);
    lv_obj_set_style_text_color(ui->screen_monitor.container_2_label_unused, COL_SUBTEXT, 0);
    lv_obj_set_style_text_color(ui->screen_monitor.container_2_label_mem_used, COL_TEXT, 0);
    lv_obj_set_style_text_color(ui->screen_monitor.container_2_label_mem_num, COL_TEXT, 0);
    lv_obj_set_width(ui->screen_monitor.container_2_label_mem_num, 60);

    /* 主机连接行 */
    lv_obj_set_pos(ui->screen_monitor.label_host, 14, 146);
    lv_obj_set_style_text_color(ui->screen_monitor.label_host, COL_SUBTEXT, 0);

    lv_obj_set_pos(ui->screen_monitor.textarea_ip, 59, 146);
    lv_obj_set_style_bg_color(ui->screen_monitor.textarea_ip, COL_CARD, 0);
    lv_obj_set_style_bg_opa(ui->screen_monitor.textarea_ip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ui->screen_monitor.textarea_ip, COL_LINE, 0);
    lv_obj_set_style_border_width(ui->screen_monitor.textarea_ip, 1, 0);
    lv_obj_set_style_radius(ui->screen_monitor.textarea_ip, 6, 0);
    lv_obj_set_style_text_color(ui->screen_monitor.textarea_ip, COL_TEXT, 0);
    lv_obj_set_style_bg_color(ui->screen_monitor.textarea_ip, COL_ACCENT, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ui->screen_monitor.textarea_ip, LV_OPA_COVER, LV_PART_CURSOR);

    lv_obj_set_pos(ui->screen_monitor.button_connect, 246, 147);
    lv_obj_set_size(ui->screen_monitor.button_connect, 56, 24);
    lv_obj_set_style_bg_color(ui->screen_monitor.button_connect, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(ui->screen_monitor.button_connect, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui->screen_monitor.button_connect, 6, 0);
    lv_obj_set_style_shadow_width(ui->screen_monitor.button_connect, 0, 0);
    lv_obj_set_style_text_color(ui->screen_monitor.button_connect_button_1_label, COL_ACCENT_TXT, 0);

    lv_obj_set_pos(ui->screen_monitor.label_status, 67, 171);
    lv_obj_set_style_text_color(ui->screen_monitor.label_status, COL_WARN, 0);

    /* 顶栏：系统监控 + 版本 + 升级齿轮 */
    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_pos(title, 10, 4);
    style_bar_label(title, COL_TEXT);
    lv_label_set_text(title, "系统监控");

    lv_obj_t *ver = lv_label_create(screen);
    lv_obj_set_pos(ver, 214, 4);
    style_bar_label(ver, COL_SUBTEXT);
    lv_label_set_text_fmt(ver, "v%s", get_app_version());

    lv_obj_t *sep = lv_obj_create(screen);
    lv_obj_set_size(sep, 320, 1);
    lv_obj_set_pos(sep, 0, 27);
    lv_obj_set_style_bg_color(sep, COL_LINE, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_set_scrollable(sep, false);
    lv_obj_set_clickable(sep, false);

    lv_obj_t *gear = lv_button_create(screen);
    lv_obj_set_size(gear, 30, 30);
    lv_obj_set_pos(gear, 284, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gear, 0, 0);
    lv_obj_set_style_shadow_width(gear, 0, 0);
    lv_obj_add_event_cb(gear, gear_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_label_create(gear);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(icon, COL_ACCENT, 0);
    lv_label_set_text(icon, LV_SYMBOL_SETTINGS);
    lv_obj_center(icon);
}

static void monitor_ui_timer_cb(lv_timer_t *t)
{
    if(s_ui != NULL && s_ui->screen_monitor.screen != NULL)
    {
        monitor_ui_apply(s_ui);
        lv_timer_delete(t);
    }
}

void monitor_ui_init(gg_ui_t *ui)
{
    s_ui = ui;
    lv_timer_create(monitor_ui_timer_cb, 100, NULL);
}
