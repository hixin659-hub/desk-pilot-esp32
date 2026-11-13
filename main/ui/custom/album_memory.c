/*
 * 相册顶栏的 SD 容量显示。
 *
 * Free 值来自 FatFS 的剩余簇缓存（FATFS.free_clst）。卡上的 FSInfo
 * 如果没被正常更新（比如拷完文件直接拔卡），f_getfree 会直接相信
 * FSInfo，屏幕上看就是 Free == Total。开机后强制扫一遍 FAT 纠正缓存，
 * 之后每秒读一次缓存值刷新（应用自己不写卡，值基本不变）。
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "custom.h"
#include "esp_lvgl_port.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ALBUM_MEM_REFRESH_MS    1000

static const char *TAG = "album_mem";

static bool s_rescan_done = false;

/* 把字节数格式化成 GB/MB/KB（两位小数，和 GuiGuider 原始设计文本一致） */
static void format_bytes(uint64_t b, char *out, size_t len)
{
    if (b >= 1024ULL * 1024 * 1024) {
        snprintf(out, len, "%.2fGB", (double)b / (1024ULL * 1024 * 1024));
    } else if (b >= 1024ULL * 1024) {
        snprintf(out, len, "%.1fMB", (double)b / (1024ULL * 1024));
    } else {
        snprintf(out, len, "%.1fKB", (double)b / 1024ULL);
    }
}

/* 清掉 FatFS 的剩余簇缓存，逼它扫一遍 FAT 得到真实空闲空间（每次开机一次） */
static void fat_rescan_once(void)
{
    if (s_rescan_done) {
        return;
    }

    for (char drv = '0'; drv <= '9'; drv++) {
        char path[4];
        snprintf(path, sizeof(path), "%c:", drv);

        FATFS *fs = NULL;
        DWORD nclst = 0;
        if (f_getfree(path, &nclst, &fs) != FR_OK || fs == NULL) {
            continue;   /* 这个盘符没挂载 */
        }

        s_rescan_done = true;
        /* free_clst >= n_fatent-2 时 f_getfree 才会真的扫 FAT */
        fs->free_clst = 0xFFFFFFFF;
        FRESULT fr = f_getfree(path, &nclst, &fs);
        ESP_LOGI(TAG, "FAT rescan on %s %s, free clusters %lu",
                 path, fr == FR_OK ? "ok" : "failed", (unsigned long)nclst);
        break;
    }
}

static void set_album_memory(gg_ui_t *ui)
{
    if (ui->screen_album.screen == NULL ||
        ui->screen_album.container_top_label_memory == NULL) {
        return;
    }

    static uint64_t last_total = 0, last_free = 0;  /* 上次读到的总容量、剩余容量 */
    static bool last_ok = false;                    /* 上次读取成功了吗（SD 挂载正常） */
    static bool have_last = false;                  /* 之前到底执行过没有 */
    static lv_obj_t *last_label = NULL;             /* 上次写的是哪个标签 */

    /* 界面重建后标签指针会变，缓存作废，否则新标签永远拿不到文字 */
    if (ui->screen_album.container_top_label_memory != last_label) {
        last_label = ui->screen_album.container_top_label_memory;
        have_last = false;
    }

    fat_rescan_once();

    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info("/sd", &total_bytes, &free_bytes);

    if (err == ESP_OK) {
        if (have_last && last_ok && total_bytes == last_total && free_bytes == last_free) {
            return;
        }

        static bool logged = false;
        if (!logged) {
            ESP_LOGI(TAG, "SD total %llu bytes, free %llu bytes",
                     (unsigned long long)total_bytes,
                     (unsigned long long)free_bytes);
            logged = true;
        }

        lvgl_port_lock(0);
        char buf[16];
        format_bytes(total_bytes, buf, sizeof(buf));
        lv_label_set_text(ui->screen_album.container_top_label_memory, buf);
        format_bytes(free_bytes, buf, sizeof(buf));
        lv_label_set_text(ui->screen_album.container_top_label_used_memory, buf);
        lvgl_port_unlock();
        last_total = total_bytes;
        last_free = free_bytes;
        last_ok = true;
        have_last = true;
    } else {
        if (have_last && !last_ok) {
            return;
        }
        lvgl_port_lock(0);
        lv_label_set_text(ui->screen_album.container_top_label_memory, "SD err");
        lv_label_set_text(ui->screen_album.container_top_label_used_memory, "--");
        lvgl_port_unlock();
        last_ok = false;
        have_last = true;
    }
}

static void album_memory_task(void *arg)
{
    gg_ui_t *ui = (gg_ui_t *)arg;

    for (;;) {
        set_album_memory(ui);
        vTaskDelay(pdMS_TO_TICKS(ALBUM_MEM_REFRESH_MS));
    }
}

void album_memory_init(gg_ui_t *ui)
{
    xTaskCreatePinnedToCore(album_memory_task, "album_mem", 4096, ui, 1, NULL, 1);
}
