#include <stdio.h>
#include "ota_ui.h"
#include "custom.h"
#include "esp_lvgl_port.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "onenet_ota.h"
#include "wifi_manager.h"

LV_FONT_DECLARE(lv_font_ziti15);

#define COL_DIALOG_BG   lv_color_hex(0x0a1f36)
#define COL_TEXT        lv_color_hex(0xfbfbfb)
#define COL_SUBTEXT     lv_color_hex(0xb7edef)
#define COL_BTN_BG      lv_color_hex(0x0051ae)
#define COL_ACCENT      lv_color_hex(0xffd32c)
#define COL_ACCENT_TXT  lv_color_hex(0x0a1f36)
#define COL_WARN        lv_color_hex(0xffb74d)
#define COL_BAR_BG      lv_color_hex(0x123b6b)

typedef enum {
    UI_IDLE = 0,
    UI_CHECKING,
    UI_NO_UPDATE,
    UI_FOUND,
    UI_DOWNLOADING,
    UI_SUCCESS,
    UI_FAILED,
} ota_ui_state_t;

static lv_obj_t *s_overlay;
static lv_obj_t *s_spinner;
static lv_obj_t *s_lbl_new;
static lv_obj_t *s_lbl_new_ver;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_warn;
static lv_obj_t *s_bar;
static lv_obj_t *s_btn_cancel;
static lv_obj_t *s_lbl_cancel;
static lv_obj_t *s_btn_primary;
static lv_obj_t *s_lbl_primary;

static ota_ui_state_t s_state = UI_IDLE;

static void create_dialog(void);

static void style_text(lv_obj_t *lbl, lv_color_t color)
{
    lv_obj_set_style_text_font(lbl, &lv_font_ziti15, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
}

static lv_obj_t* make_label(lv_obj_t *parent, int x, int y, const char *text, lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    style_text(lbl, color);
    lv_label_set_text(lbl, text);
    return lbl;
}

static void show_buttons(bool show_cancel, const char *cancel_text,
                         bool show_primary, const char *primary_text)
{
    if(show_cancel)
    {
        lv_obj_set_hidden(s_btn_cancel, false);
        lv_label_set_text(s_lbl_cancel, cancel_text);
    }
    else
    {
        lv_obj_set_hidden(s_btn_cancel, true);
    }

    if(show_primary)
    {
        lv_obj_set_hidden(s_btn_primary, false);
        lv_label_set_text(s_lbl_primary, primary_text);
    }
    else
    {
        lv_obj_set_hidden(s_btn_primary, true);
    }

    /* 只有一个按钮时居中 */
    if(show_cancel && !show_primary)
    {
        lv_obj_set_pos(s_btn_cancel, 90, 176);
    }
    else
    {
        lv_obj_set_pos(s_btn_cancel, 16, 176);
    }
}

static void set_progress(int pct)
{
    if(s_state != UI_DOWNLOADING)
    {
        return;
    }

    if(pct < 0)
    {
        lv_obj_set_hidden(s_bar, true);
        lv_label_set_text(s_lbl_status, "正在下载...");
        return;
    }

    lv_obj_set_hidden(s_bar, false);
    lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_lbl_status, "正在下载 %d%%", pct);
}

static void set_state(ota_ui_state_t st)
{
    s_state = st;
    lv_obj_set_hidden(s_overlay, false);

    /* 默认收起可选元素，下面各状态按需打开 */
    lv_obj_set_hidden(s_lbl_new, true);
    lv_obj_set_hidden(s_lbl_new_ver, true);
    lv_obj_set_hidden(s_bar, true);
    lv_obj_set_hidden(s_lbl_warn, true);
    lv_obj_set_hidden(s_spinner, true);

    switch(st)
    {
    case UI_IDLE:
        lv_label_set_text(s_lbl_status, "检查更新以获取新固件");
        show_buttons(true, "取消", true, "检查更新");
        break;
    case UI_CHECKING:
        lv_label_set_text(s_lbl_status, "正在检查更新...");
        lv_obj_set_hidden(s_spinner, false);
        show_buttons(false, NULL, false, NULL);
        break;
    case UI_NO_UPDATE:
        lv_label_set_text(s_lbl_status,
                          "固件版本上报成功，未发现升级任务\n"
                          "请到 OneNET 平台验证设备并创建升级任务后重试");
        show_buttons(true, "关闭", true, "重试");
        break;
    case UI_FOUND:
    {
        lv_label_set_text_fmt(s_lbl_new_ver, "v%s", onenet_ota_target_version());
        lv_obj_set_hidden(s_lbl_new, false);
        lv_obj_set_hidden(s_lbl_new_ver, false);
        lv_label_set_text(s_lbl_status, "发现新版本，是否立即升级？");
        show_buttons(true, "取消", true, "立即升级");
        break;
    }
    case UI_DOWNLOADING:
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_obj_set_hidden(s_lbl_warn, false);
        lv_label_set_text(s_lbl_status, "正在下载...");
        show_buttons(false, NULL, false, NULL);
        break;
    case UI_SUCCESS:
        lv_obj_set_hidden(s_bar, false);
        lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
        lv_label_set_text(s_lbl_status, "升级完成，正在重启...");
        show_buttons(false, NULL, false, NULL);
        break;
    case UI_FAILED:
        lv_label_set_text(s_lbl_status, "升级失败");
        show_buttons(true, "关闭", true, "重试");
        break;
    default:
        break;
    }
}

static const char* ota_err_text(int err)
{
    switch(err)
    {
    case ESP_ERR_OTA_VALIDATE_FAILED:
        return "固件校验失败";
    case ESP_ERR_NO_MEM:
        return "内存不足";
    case ESP_ERR_NOT_FOUND:
        return "分区配置错误";
    case ESP_ERR_INVALID_STATE:
        return "没有可升级的任务";
    default:
        break;
    }

    if(err >= ESP_ERR_HTTP_BASE && err < ESP_ERR_HTTP_BASE + 0x100)
    {
        return "网络连接失败";
    }
    if(err == ESP_FAIL)
    {
        return "网络连接失败";
    }
    return "升级失败";
}

/* 在 OTA 任务上下文调用，需要拿 LVGL 锁 */
static void ota_backend_event_cb(onenet_ota_event_t evt, int value)
{
    lvgl_port_lock(0);

    if(s_overlay == NULL)
    {
        create_dialog();
    }

    switch(evt)
    {
    case ONENET_OTA_EVT_CHECKING:
        set_state(UI_CHECKING);
        break;
    case ONENET_OTA_EVT_VERSION_UPLOADED:
        if(s_state == UI_CHECKING)
        {
            lv_label_set_text(s_lbl_status, "固件版本上报成功，正在检查升级任务...");
        }
        break;
    case ONENET_OTA_EVT_NO_UPDATE:
        set_state(UI_NO_UPDATE);
        break;
    case ONENET_OTA_EVT_UPDATE_FOUND:
        set_state(UI_FOUND);
        break;
    case ONENET_OTA_EVT_DOWNLOADING:
        if(s_state != UI_DOWNLOADING)
        {
            set_state(UI_DOWNLOADING);
        }
        set_progress(value);
        break;
    case ONENET_OTA_EVT_SUCCESS:
        set_state(UI_SUCCESS);
        break;
    case ONENET_OTA_EVT_PLATFORM_ERROR:
        set_state(UI_FAILED);
        lv_label_set_text_fmt(s_lbl_status,
                              "固件版本上报成功，平台返回错误：%d\n"
                              "请到 OneNET 平台检查设备验证/升级任务",
                              value);
        break;
    case ONENET_OTA_EVT_FAILED:
        set_state(UI_FAILED);
        lv_label_set_text_fmt(s_lbl_status, "升级失败：%s", ota_err_text(value));
        break;
    default:
        break;
    }

    lvgl_port_unlock();
}

void ota_ui_open(void)
{
    /* 首次打开弹窗：先释放相机占用的内部 RAM，再创建弹窗控件 */
    if(s_overlay == NULL)
    {
        onenet_ota_prepare_memory();
        create_dialog();
    }

    if(onenet_ota_is_busy())
    {
        set_state(UI_CHECKING);
        return;
    }
    set_state(UI_IDLE);
}

static void cancel_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    lv_obj_set_hidden(s_overlay, true);

    /* 没有 OTA 任务在跑时，把相机恢复回来 */
    if(!onenet_ota_is_busy())
    {
        onenet_ota_restore_camera();
    }
}

static void primary_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    if(s_state == UI_IDLE || s_state == UI_FAILED || s_state == UI_NO_UPDATE)
    {
        if(!wifi_manager_is_connected())
        {
            set_state(UI_FAILED);
            lv_label_set_text(s_lbl_status, "网络未连接");
            return;
        }
        set_state(UI_CHECKING);
        onenet_ota_check_async();
    }
    else if(s_state == UI_FOUND)
    {
        set_state(UI_DOWNLOADING);
        set_progress(-1);
        onenet_ota_upgrade_async();
    }
}

static void overlay_event_cb(lv_event_t *e)
{
    /* 吞掉点击/手势，防止升级过程中滑动切页 */
    (void)e;
}

static void create_dialog(void)
{
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_clickable(s_overlay, true);
    lv_obj_add_event_cb(s_overlay, overlay_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_hidden(s_overlay, true);

    lv_obj_t *dialog = lv_obj_create(s_overlay);
    /* 状态文字最多约 3 行（每行 20px），按钮放在 176 以下避免挡住文字 */
    lv_obj_set_size(dialog, 280, 216);
    lv_obj_center(dialog);
    lv_obj_set_style_bg_color(dialog, COL_DIALOG_BG, 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dialog, 10, 0);
    lv_obj_set_style_border_width(dialog, 0, 0);
    lv_obj_set_style_pad_all(dialog, 0, 0);
    lv_obj_set_scrollable(dialog, false);

    make_label(dialog, 16, 8, "系统升级", COL_TEXT);

    make_label(dialog, 16, 34, "当前版本", COL_SUBTEXT);
    lv_obj_t *lbl_cur_ver = make_label(dialog, 196, 34, "", COL_TEXT);
    lv_label_set_text_fmt(lbl_cur_ver, "v%s", get_app_version());

    s_lbl_new = make_label(dialog, 16, 56, "最新版本", COL_SUBTEXT);
    s_lbl_new_ver = make_label(dialog, 196, 56, "", COL_TEXT);

    s_lbl_status = lv_label_create(dialog);
    lv_obj_set_pos(s_lbl_status, 16, 82);
    lv_obj_set_width(s_lbl_status, 248);
    lv_obj_set_height(s_lbl_status, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_MODE_WRAP);
    style_text(s_lbl_status, COL_TEXT);
    lv_label_set_text(s_lbl_status, "");

    s_bar = lv_bar_create(dialog);
    lv_obj_set_pos(s_bar, 16, 118);
    lv_obj_set_size(s_bar, 248, 10);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 5, LV_PART_INDICATOR);

    s_lbl_warn = make_label(dialog, 16, 138, "升级中，请勿断电", COL_WARN);

    s_spinner = lv_spinner_create(dialog);
    lv_obj_set_size(s_spinner, 18, 18);
    lv_obj_set_pos(s_spinner, 246, 8);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spinner, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spinner, COL_ACCENT, LV_PART_INDICATOR);

    s_btn_cancel = lv_button_create(dialog);
    lv_obj_set_size(s_btn_cancel, 100, 28);
    lv_obj_set_pos(s_btn_cancel, 16, 176);
    lv_obj_set_style_bg_color(s_btn_cancel, COL_BTN_BG, 0);
    lv_obj_set_style_radius(s_btn_cancel, 6, 0);
    lv_obj_set_style_shadow_width(s_btn_cancel, 0, 0);
    lv_obj_add_event_cb(s_btn_cancel, cancel_event_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_cancel = lv_label_create(s_btn_cancel);
    style_text(s_lbl_cancel, COL_TEXT);
    lv_obj_center(s_lbl_cancel);

    s_btn_primary = lv_button_create(dialog);
    lv_obj_set_size(s_btn_primary, 100, 28);
    lv_obj_set_pos(s_btn_primary, 164, 176);
    lv_obj_set_style_bg_color(s_btn_primary, COL_ACCENT, 0);
    lv_obj_set_style_radius(s_btn_primary, 6, 0);
    lv_obj_set_style_shadow_width(s_btn_primary, 0, 0);
    lv_obj_add_event_cb(s_btn_primary, primary_event_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_primary = lv_label_create(s_btn_primary);
    style_text(s_lbl_primary, COL_ACCENT_TXT);
    lv_obj_center(s_lbl_primary);
}

void ota_ui_init(gg_ui_t *ui)
{
    (void)ui;
    onenet_ota_set_event_cb(ota_backend_event_cb);
}
