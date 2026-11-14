#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include "custom.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define FOLDER_ROOT             "/sd"           /* 返回按钮最多退到这里 */
#define FOLDER_START_DIR        "/sd/DCIM"      /* 打开文件夹 tab 时显示的目录 */
#define FOLDER_PATH_MAX         512
#define FOLDER_NAME_MAX         260             /* FAT 长文件名最长 255 */
#define FOLDER_MAX_ENTRIES      128
#define FOLDER_ROW_H            32
#define FOLDER_POLL_MS          100
#define FOLDER_FORMAT_MAX       8

typedef enum {
    FOLDER_ENTRY_DIR = 0,
    FOLDER_ENTRY_IMAGE,
    FOLDER_ENTRY_VIDEO,
    FOLDER_ENTRY_FILE,
} folder_entry_kind_t;

typedef struct {
    char name[FOLDER_NAME_MAX];
    uint8_t kind;
} folder_entry_t;

typedef struct {
    gg_ui_t *ui;
    lv_obj_t *list;
    lv_obj_t *path_label;
    lv_obj_t *back_btn;
    lv_obj_t *inited_screen;
    char cwd[FOLDER_PATH_MAX];
    folder_entry_t *entries;
    volatile bool pending;
    char request[FOLDER_PATH_MAX];
} folder_ctx_t;

static folder_ctx_t folder;

static const char *TAG = "folder";

static void folder_row_clicked_cb(lv_event_t *e);
static void folder_image_clicked_cb(lv_event_t *e);
static void folder_video_clicked_cb(lv_event_t *e);
static void folder_back_clicked_cb(lv_event_t *e);

static const char *folder_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0') {
        return "";
    }
    return dot + 1;
}

static bool folder_ext_match(const char *ext, const char *const *list)
{
    for (int i = 0; list[i] != NULL; i++) {
        if (strcasecmp(ext, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static uint8_t folder_file_kind(const char *name)
{
    static const char *const image_ext[] = {
        "png", "jpg", "jpeg", "bmp", "gif", "webp", "heic", NULL
    };
    static const char *const video_ext[] = {
        "mp4", "avi", "mov", "mkv", "m4v", "webm", "3gp", "flv", "ts", NULL
    };

    const char *ext = folder_ext(name);
    if (folder_ext_match(ext, image_ext)) {
        return FOLDER_ENTRY_IMAGE;
    }
    if (folder_ext_match(ext, video_ext)) {
        return FOLDER_ENTRY_VIDEO;
    }
    return FOLDER_ENTRY_FILE;
}


static void folder_format_text(const char *name, char *out, size_t len)
{
    const char *ext = folder_ext(name);
    size_t i = 0;

    while (ext[i] != '\0' && i + 1 < len && i < FOLDER_FORMAT_MAX - 1) {
        out[i] = (char)toupper((unsigned char)ext[i]);
        i++;
    }
    out[i] = '\0';
    if (out[0] == '\0') {
        snprintf(out, len, "FILE");
    }
}

static int folder_entry_cmp(const void *a, const void *b)
{
    const folder_entry_t *ea = (const folder_entry_t *)a;
    const folder_entry_t *eb = (const folder_entry_t *)b;
    bool dir_a = ea->kind == FOLDER_ENTRY_DIR;
    bool dir_b = eb->kind == FOLDER_ENTRY_DIR;

    if (dir_a != dir_b) {
        return dir_a ? -1 : 1;
    }
    return strcasecmp(ea->name, eb->name);
}

static uint32_t folder_scan_dir(const char *dir_path, folder_entry_t *entries)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        ESP_LOGW(TAG, "open %s failed", dir_path);
        return 0;
    }

    uint32_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (ent->d_type != DT_DIR && ent->d_type != DT_REG) {
            continue;
        }
        if (count >= FOLDER_MAX_ENTRIES) {
            ESP_LOGW(TAG, "more than %d entries, ignoring the rest", FOLDER_MAX_ENTRIES);
            break;
        }

        folder_entry_t *item = &entries[count];
        snprintf(item->name, FOLDER_NAME_MAX, "%s", ent->d_name);
        item->kind = (ent->d_type == DT_DIR) ? FOLDER_ENTRY_DIR
                                             : folder_file_kind(ent->d_name);
        count++;
    }
    closedir(dir);

    qsort(entries, count, sizeof(folder_entry_t), folder_entry_cmp);
    return count;
}

static bool folder_join_path(const char *name, char *out, size_t len)
{
    int n = snprintf(out, len, "%s/%s", folder.cwd, name);

    if (n < 0 || n >= (int)len) {
        ESP_LOGW(TAG, "path too long, skip: %s/%s", folder.cwd, name);
        return false;
    }
    return true;
}

static void folder_request(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return;
    }
    folder.pending = false;
    snprintf(folder.request, sizeof(folder.request), "%s", path);
    folder.pending = true;
}

static lv_obj_t *folder_row_create(lv_obj_t *parent, const folder_entry_t *entry)
{
    bool is_dir = entry->kind == FOLDER_ENTRY_DIR;
    bool can_open = is_dir || entry->kind == FOLDER_ENTRY_IMAGE ||
                    entry->kind == FOLDER_ENTRY_VIDEO;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), FOLDER_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_set_style_pad_top(row, 0, 0);
    lv_obj_set_style_pad_bottom(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xe8e8e8), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_border_post(row, true, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, can_open);

    if (is_dir) {
        lv_obj_t *icon = lv_image_create(row);
        lv_image_set_src(icon, &folder_min_14x14_RGB565A8);
    } else {
        lv_obj_t *icon = lv_label_create(row);
        lv_obj_set_width(icon, 16);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
        if (entry->kind == FOLDER_ENTRY_IMAGE) {
            lv_label_set_text(icon, LV_SYMBOL_IMAGE);
            lv_obj_set_style_text_color(icon, lv_color_hex(0x2196f3), 0);
        } else if (entry->kind == FOLDER_ENTRY_VIDEO) {
            lv_label_set_text(icon, LV_SYMBOL_PLAY);
            lv_obj_set_style_text_color(icon, lv_color_hex(0xe91e63), 0);
        } else {
            lv_label_set_text(icon, LV_SYMBOL_FILE);
            lv_obj_set_style_text_color(icon, lv_color_hex(0x9e9e9e), 0);
        }
    }

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, entry->name);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x333333), 0);

    if (is_dir) {
        lv_obj_t *arrow = lv_label_create(row);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(0x999999), 0);
        lv_obj_add_event_cb(row, folder_row_clicked_cb, LV_EVENT_CLICKED, (void *)entry->name);
    } else {
        char fmt[FOLDER_FORMAT_MAX];
        folder_format_text(entry->name, fmt, sizeof(fmt));

        lv_obj_t *tag = lv_label_create(row);
        lv_label_set_text(tag, fmt);
        lv_obj_set_style_text_font(tag, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(tag, lv_color_hex(0x666666), 0);
        lv_obj_set_style_bg_color(tag, lv_color_hex(0xe8e8e8), 0);
        lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tag, 3, 0);
        lv_obj_set_style_pad_hor(tag, 4, 0);
        lv_obj_set_style_pad_ver(tag, 1, 0);
    }

    if (entry->kind == FOLDER_ENTRY_IMAGE) {
        lv_obj_add_event_cb(row, folder_image_clicked_cb, LV_EVENT_CLICKED, (void *)entry);
    } else if (entry->kind == FOLDER_ENTRY_VIDEO) {
        lv_obj_add_event_cb(row, folder_video_clicked_cb, LV_EVENT_CLICKED, (void *)entry);
    }

    return row;
}

static void folder_render(const char *path, folder_entry_t *entries, uint32_t count)
{
    lvgl_port_lock(0);

    lv_obj_clean(folder.list);         
    if (folder.entries != NULL) {
        heap_caps_free(folder.entries);
    }
    folder.entries = entries;

    snprintf(folder.cwd, sizeof(folder.cwd), "%s", path);

    if (strncmp(path, "/sd", 3) == 0) {
        lv_label_set_text_fmt(folder.path_label, "SD:%s", path + 3);
    } else {
        lv_label_set_text(folder.path_label, path);
    }

    lv_obj_set_style_opa(folder.back_btn,
                         strcmp(path, FOLDER_ROOT) == 0 ? LV_OPA_30 : LV_OPA_COVER, 0);

    if (count == 0) {
        lv_obj_set_flex_align(folder.list, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *empty = lv_label_create(folder.list);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(empty, &lv_font_ziti15, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
        lv_label_set_text(empty, "空文件夹");
    } else {
        lv_obj_set_flex_align(folder.list, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        for (uint32_t i = 0; i < count; i++) {
            folder_row_create(folder.list, &folder.entries[i]);
        }
    }

    lv_obj_scroll_to_y(folder.list, 0, LV_ANIM_OFF);
    lvgl_port_unlock();
}

static void folder_ui_init(gg_ui_t *ui)
{
    lvgl_port_lock(0);

    folder.ui = ui;
    folder.list = ui->screen_album.container_folder_list_folder;
    folder.path_label = ui->screen_album.container_6_label_path;
    folder.back_btn = ui->screen_album.container_6_imagebutton_return;

    lv_obj_set_scrollable(ui->screen_album.container_folder_container_6, false);

    lv_obj_add_event_cb(folder.back_btn, folder_back_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_set_style_text_font(folder.path_label, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(folder.path_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(folder.path_label, 250);

    lv_obj_set_style_pad_all(folder.list, 0, 0);
    lv_obj_set_style_pad_row(folder.list, 0, 0);
    lv_obj_set_style_border_width(folder.list, 0, 0);
    lv_obj_set_style_bg_color(folder.list, lv_color_white(), 0);
    lv_obj_clean(folder.list);

    lvgl_port_unlock();

    folder.inited_screen = ui->screen_album.screen;
}

static void folder_row_clicked_cb(lv_event_t *e)
{
    const char *name = lv_event_get_user_data(e);
    char next[FOLDER_PATH_MAX];

    if (name != NULL && folder_join_path(name, next, sizeof(next))) {
        folder_request(next);
    }
}

static void folder_image_clicked_cb(lv_event_t *e)
{
    const folder_entry_t *entry = (const folder_entry_t *)lv_event_get_user_data(e);
    char fs_path[FOLDER_PATH_MAX];
    char lv_path[FOLDER_PATH_MAX + 2];

    if (entry == NULL || !folder_join_path(entry->name, fs_path, sizeof(fs_path))) {
        return;
    }
    snprintf(lv_path, sizeof(lv_path), "S:%s", fs_path);
    album_image_open(lv_path);
}

static void folder_video_clicked_cb(lv_event_t *e)
{
    const folder_entry_t *entry = (const folder_entry_t *)lv_event_get_user_data(e);
    char fs_path[FOLDER_PATH_MAX];

    if (entry != NULL && folder_join_path(entry->name, fs_path, sizeof(fs_path))) {
        album_video_play(fs_path);
    }
}

static void folder_back_clicked_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    char parent[FOLDER_PATH_MAX];
    char *slash;

    if (strcmp(folder.cwd, FOLDER_ROOT) == 0) {
        return;
    }

    snprintf(parent, sizeof(parent), "%s", folder.cwd);
    slash = strrchr(parent, '/');
    if (slash == NULL || slash == parent) {
        return;
    }
    *slash = '\0';

    if (strlen(parent) < strlen(FOLDER_ROOT)) {
        snprintf(parent, sizeof(parent), "%s", FOLDER_ROOT);
    }
    folder_request(parent);
}

static void album_folder_task(void *arg)
{
    gg_ui_t *ui = (gg_ui_t *)arg;

    for (;;) {
        if (ui->screen_album.screen != NULL && folder.inited_screen != ui->screen_album.screen) {
            folder_ui_init(ui);
            folder_request(FOLDER_START_DIR);
        }

        if (folder.pending) {
            char path[FOLDER_PATH_MAX];
            folder_entry_t *entries = NULL;
            folder.pending = false;    
            snprintf(path, sizeof(path), "%s", folder.request);
            entries = heap_caps_malloc((size_t)FOLDER_MAX_ENTRIES * sizeof(folder_entry_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (entries == NULL) {
                entries = heap_caps_malloc((size_t)FOLDER_MAX_ENTRIES * sizeof(folder_entry_t),
                                           MALLOC_CAP_8BIT);
            }
            uint32_t count = 0;
            if (entries != NULL) {
                count = folder_scan_dir(path, entries);
            } else {
                ESP_LOGE(TAG, "no memory for folder list");
            }
            folder_render(path, entries, count);
        }
        vTaskDelay(pdMS_TO_TICKS(FOLDER_POLL_MS));
    }
}

void album_folder_init(gg_ui_t *ui)
{
    xTaskCreatePinnedToCore(album_folder_task, "album_dir", 4096, ui, 2, NULL, 1);
}
