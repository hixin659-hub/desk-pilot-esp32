/*
 * AVI（MJPEG + PCM）播放器。
 *
 * 画面：用 LVGL 自带的 TJPGD 解 MJPEG，RGB888 转 RGB565，双缓冲画到全屏覆盖层。
 * 声音：I2S 标准模式推给 NS4168（BCLK=IO46, LRCLK=IO9, SDATA=IO8）。
 *       功放 CTRL（XL9555 P0_0）平时关断，只有真要出声时才打开，避免底噪。
 *
 * 同步：有音频时音频写 I2S 本身会阻塞（DMA 满），由它给整个循环定速；
 *       落后太多就丢视频帧，保证声音不断。没有音频时按帧率 sleep。
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#include "custom.h"
#include "sdkconfig.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "libs/tjpgd/tjpgd.h"
#include "xl9555.h"
#include "usb_camera.h"
#include "jpeg_color.h"

#define VIDEO_SCREEN_W          320
#define VIDEO_SCREEN_H          240
#define VIDEO_PATH_MAX          512
#define VIDEO_JPEG_POOL         8192
#define VIDEO_MAX_DIM           2560    /* 输入分辨率上限，输出会缩到屏幕大小 */
#define VIDEO_AUDIO_BUF         2048
#define VIDEO_FILE_BUF          16384   /* stdio 缓冲：TJPGD 每次只读 512 字节 */
#define VIDEO_DEFAULT_PERIOD_US (1000000 / CONFIG_APP_VIDEO_FPS_LIMIT)  /* 默认帧率来自 menuconfig */
#define VIDEO_ERROR_MAX         96
#define VIDEO_CTRL_H            40

/* NS4168：BCLK / LRCLK / SDATA */
#define VIDEO_I2S_BCLK          GPIO_NUM_46
#define VIDEO_I2S_WS            GPIO_NUM_9
#define VIDEO_I2S_DOUT          GPIO_NUM_8

/* NS4168 的 CTRL 使能脚接在 XL9555 的 P0_0（和 main.c 的 SPK_EN_IO 是同一脚） */
#define VIDEO_SPK_EN            IO0_0

#define VIDEO_TAG "video"

static const char *TAG = VIDEO_TAG;

/* ---------- AVI 头信息 ---------- */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;               /* strh: dwRate */
    uint32_t fps_den;               /* strh: dwScale */
    uint32_t frame_period_us;
    uint32_t video_frames;          /* strh: dwLength，进度条用 */
    uint32_t avih_us_per_frame;
    uint32_t avih_width;
    uint32_t avih_height;
    uint32_t avih_total_frames;
    uint32_t compression;

    bool has_video;
    uint8_t video_stream;
    bool has_audio;
    uint8_t audio_stream;
    uint16_t audio_format;
    uint16_t audio_channels;
    uint16_t audio_bits;
    uint32_t audio_rate;
    uint32_t audio_bytes_per_sec;
    uint16_t audio_block_align;

    uint32_t movi_start;
    uint32_t movi_end;

    uint32_t idx1_pos;              /* idx1 索引数据起点，0 表示没有索引 */
    uint32_t idx1_size;
} avi_info_t;

/* ---------- JPEG 输入输出 ---------- */

typedef struct {
    FILE *f;
    size_t left;                    /* 当前 MJPEG chunk 还剩多少字节 */
    uint16_t *dst;
    int dst_w;
    int dst_h;
    uint64_t read_us;               /* fread 花掉的时间，用于区分解码和读卡 */
} jpeg_io_t;

/* ---------- 播放器状态 ---------- */

typedef struct {
    volatile bool playing;
    volatile bool stop;
    volatile bool paused;
    volatile bool restart;
    volatile bool seek_req;         /* 界面请求从 seek_frame 重新播放，视频任务负责清掉 */
    volatile bool seeking;          /* 正在拖进度条：不要用播放进度覆盖进度条 */
    volatile uint32_t seek_frame;
    char path[VIDEO_PATH_MAX];

    lv_obj_t *overlay;
    lv_obj_t *img;
    lv_obj_t *ctrl_bar;
    lv_obj_t *play_icon;
    lv_obj_t *bar;
    lv_obj_t *time_label;
    lv_obj_t *error_label;
    lv_obj_t *vol_label;            /* 调音量时显示 "80%" 的小标签 */
    lv_timer_t *vol_timer;          /* 1.2s 后把音量标签藏起来 */

    bool finished;
    bool failed;
    char error[VIDEO_ERROR_MAX];

    uint32_t total_frames;          /* 0 表示文件头里没写 */
    uint32_t frame_period_us;
    int64_t clock_offset_us;        /* 暂停累计时长，只有播放任务线程改 */
    uint32_t frames_decoded;

    uint32_t *seek_pos;             /* idx1 建出来的每帧文件位置（PSRAM），没有索引时为 NULL */
    uint32_t seek_count;
    uint32_t drag_frame;            /* 拖拽时预览到的帧号 */

    /* 解码耗时统计：排帧时提前判断这一帧解完是否还来得及显示 */
    uint32_t decode_avg_us;         /* 滑动平均，含读卡时间 */
    uint64_t decode_us_sum;
    uint64_t decode_io_us_sum;
    uint32_t decode_count;

    uint16_t *fb[2];                /* PSRAM 双缓冲，RGB565 */
    lv_image_dsc_t dsc[2];
    int front;
    uint32_t width;
    uint32_t height;

    uint8_t jpeg_pool[VIDEO_JPEG_POOL];
    uint8_t *file_buf;              /* stdio 大缓冲，减少 FatFS/SD 小块读取 */

    i2s_chan_handle_t tx;
    bool audio_ready;
    uint32_t audio_bytes;
    uint32_t audio_peak;            /* 写出的 PCM 峰值，0 说明音频是静音 */
    uint8_t *pcm_in;                /* 只有打开声音时才分配（约 2KB） */
    uint8_t *pcm_out;               /* 单声道展开成立体声（约 4KB） */
    lv_obj_t *audio_icon;           /* 声音开关按钮上的图标 */
} video_ctx_t;

static video_ctx_t video;

/* 声音开关：默认关闭。关闭时不初始化 I2S、不分配 PCM 缓冲，省内部 RAM */
static volatile bool video_audio_enabled = false;

/* NS4168 只有 CTRL 使能脚，没有音量控制接口；音量只能在软件里缩放 PCM，
   0-100 表示百分比，给所有视频共用 */
static uint8_t video_volume = 100;

bool album_video_is_playing(void)
{
    return video.playing;
}

/* NS4168 没有静音寄存器，只能拉 CTRL 脚开关功放。I2S 没起时钟时输入是悬空的，
   功放开着就会一直滋滋响，所以只在真要有声音的时候打开 */
static void video_spk_power(bool on)
{
    esp_err_t err = xl9555_pin_write(VIDEO_SPK_EN, on ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spk en=%d write failed: %s", on ? 1 : 0,
                 esp_err_to_name(err));
    }
}

/* ---------- 小工具 ---------- */

static uint16_t avi_rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t avi_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 把 BITMAPINFOHEADER 里的 biCompression 转成可读的四个字符 */
static void avi_fourcc(uint32_t code, char out[5])
{
    out[0] = (char)(code & 0xFF);
    out[1] = (char)((code >> 8) & 0xFF);
    out[2] = (char)((code >> 16) & 0xFF);
    out[3] = (char)((code >> 24) & 0xFF);
    out[4] = '\0';
    for (int i = 0; i < 4; i++) {
        if (out[i] < 0x20 || out[i] > 0x7E) {
            out[i] = '?';
        }
    }
}

/* 只解 JPEG；compression 为 0 的文件头没写，先按 JPEG 试 */
static bool avi_codec_is_jpeg(uint32_t code)
{
    char s[5];

    if (code == 0) {
        return true;
    }
    avi_fourcc(code, s);
    return strcasecmp(s, "MJPG") == 0 || strcasecmp(s, "JPEG") == 0 ||
           strcasecmp(s, "MJPA") == 0 || strcasecmp(s, "JPG") == 0;
}

/* chunk 太离谱（损坏文件）就停下，防止 pos 溢出后死循环 */
static bool avi_advance(uint32_t *pos, uint32_t content, uint32_t sz)
{
    uint64_t next = (uint64_t)content + sz + (sz & 1);
    if (next > 0x7FFFFFFFULL) {
        return false;
    }
    *pos = (uint32_t)next;
    return true;
}

static int avi_hex(uint8_t c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static int avi_chunk_stream(const uint8_t id[4])
{
    int hi = avi_hex(id[0]);
    int lo = avi_hex(id[1]);
    return (hi < 0 || lo < 0) ? -1 : hi * 16 + lo;
}

static bool avi_chunk_is_video(const uint8_t id[4], const avi_info_t *info)
{
    return info->has_video && id[2] == 'd' && (id[3] == 'c' || id[3] == 'b') &&
           avi_chunk_stream(id) == info->video_stream;
}

static bool avi_chunk_is_audio(const uint8_t id[4], const avi_info_t *info)
{
    return info->has_audio && id[2] == 'w' && id[3] == 'b' &&
           avi_chunk_stream(id) == info->audio_stream;
}

static void video_delay_us(int64_t us)
{
    while (us > 0 && !video.stop && !video.seek_req) {
        int64_t step = us > 20000 ? 20000 : us;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((step + 999) / 1000)));
        us -= step;
    }
}

/* 暂停时卡在这里，并把暂停时长记下来，恢复后时间轴往后顺延 */
static void video_pause_wait(void)
{
    int64_t begin;

    if (!video.paused || video.stop) {
        return;
    }
    if (video.audio_ready) {
        video_spk_power(false);     /* 暂停时关功放，喇叭彻底安静 */
    }
    begin = esp_timer_get_time();
    while (video.paused && !video.stop && !video.seek_req) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (video.audio_ready && !video.stop) {
        video_spk_power(true);
    }
    video.clock_offset_us += esp_timer_get_time() - begin;
}

/* ---------- AVI 解析 ---------- */

static void avi_parse_strl(FILE *f, uint32_t start, uint32_t len,
                           avi_info_t *info, int stream_index)
{
    uint32_t pos;
    uint64_t end64 = (uint64_t)start + len;
    if (end64 > 0x7FFFFFFFULL) {
        return;
    }
    uint32_t end = (uint32_t)end64;
    bool is_video = false;
    bool is_audio = false;
    uint32_t scale = 0;
    uint32_t rate = 0;
    uint32_t length = 0;

    /* 先看 strh，确定这条流是视频还是音频 */
    for (pos = start; pos + 8 <= end; ) {
        uint8_t ch[8];
        if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(ch, 1, 8, f) != 8) {
            return;
        }
        uint32_t sz = avi_rd32(ch + 4);
        if (memcmp(ch, "strh", 4) == 0 && sz >= 32) {
            uint8_t b[56];
            size_t n = sz < sizeof(b) ? sz : sizeof(b);
            if (fread(b, 1, n, f) == n) {
                if (memcmp(b, "vids", 4) == 0) {
                    is_video = true;
                } else if (memcmp(b, "auds", 4) == 0) {
                    is_audio = true;
                }
                scale = avi_rd32(b + 20);
                rate = avi_rd32(b + 24);
                if (n >= 36) {
                    length = avi_rd32(b + 32);
                }
            }
        }
        if (!avi_advance(&pos, pos + 8, sz)) {
            return;
        }
    }

    if (!is_video && !is_audio) {
        return;
    }

    /* 再看 strf，拿分辨率或 WAVEFORMATEX */
    for (pos = start; pos + 8 <= end; ) {
        uint8_t ch[8];
        if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(ch, 1, 8, f) != 8) {
            break;
        }
        uint32_t sz = avi_rd32(ch + 4);
        if (memcmp(ch, "strf", 4) == 0) {
            if (is_video && sz >= 40 && !info->has_video) {
                uint8_t b[40];
                if (fread(b, 1, 40, f) == 40) {
                    int32_t h = (int32_t)avi_rd32(b + 8);
                    if (h < 0) {
                        h = -h;
                    }
                    info->width = avi_rd32(b + 4);
                    info->height = (uint32_t)h;
                    info->compression = avi_rd32(b + 16);
                }
            } else if (is_audio && sz >= 16 && !info->has_audio) {
                uint8_t b[18];
                size_t n = sz < sizeof(b) ? sz : sizeof(b);
                if (fread(b, 1, n, f) == n) {
                    info->audio_format = avi_rd16(b + 0);
                    info->audio_channels = avi_rd16(b + 2);
                    info->audio_rate = avi_rd32(b + 4);
                    info->audio_bytes_per_sec = avi_rd32(b + 8);
                    info->audio_block_align = avi_rd16(b + 12);
                    info->audio_bits = avi_rd16(b + 14);
                    info->has_audio = true;
                    info->audio_stream = (uint8_t)stream_index;
                    if (info->audio_bytes_per_sec == 0 &&
                        info->audio_rate > 0 && info->audio_bits > 0) {
                        info->audio_bytes_per_sec =
                            info->audio_rate * info->audio_channels * (info->audio_bits / 8);
                    }
                }
            }
            break;
        }
        if (!avi_advance(&pos, pos + 8, sz)) {
            break;
        }
    }

    if (is_video && !info->has_video) {
        info->has_video = true;
        info->video_stream = (uint8_t)stream_index;
        info->video_frames = length;
        if (rate > 0 && scale > 0) {
            info->fps_num = rate;
            info->fps_den = scale;
        }
    }
}

static void avi_parse_hdrl(FILE *f, uint32_t start, uint32_t len, avi_info_t *info)
{
    uint32_t pos;
    uint64_t end64 = (uint64_t)start + len;
    if (end64 > 0x7FFFFFFFULL) {
        return;
    }
    uint32_t end = (uint32_t)end64;
    int stream_index = 0;

    for (pos = start; pos + 8 <= end; ) {
        uint8_t ch[8];
        if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(ch, 1, 8, f) != 8) {
            return;
        }
        uint32_t sz = avi_rd32(ch + 4);

        if (memcmp(ch, "avih", 4) == 0 && sz >= 40) {
            uint8_t b[56];
            size_t n = sz < sizeof(b) ? sz : sizeof(b);
            if (fread(b, 1, n, f) == n) {
                info->avih_us_per_frame = avi_rd32(b + 0);
                info->avih_total_frames = avi_rd32(b + 16);
                info->avih_width = avi_rd32(b + 32);
                info->avih_height = avi_rd32(b + 36);
            }
        } else if (memcmp(ch, "LIST", 4) == 0 && sz >= 4) {
            uint8_t type[4];
            if (fread(type, 1, 4, f) == 4 && memcmp(type, "strl", 4) == 0) {
                avi_parse_strl(f, pos + 12, sz - 4, info, stream_index);
                stream_index++;
            }
        }
        if (!avi_advance(&pos, pos + 8, sz)) {
            return;
        }
    }
}

static bool avi_parse(FILE *f, avi_info_t *info)
{
    uint8_t hdr[12];

    memset(info, 0, sizeof(*info));
    info->frame_period_us = VIDEO_DEFAULT_PERIOD_US;

    if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, 12, f) != 12) {
        return false;
    }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "AVI ", 4) != 0) {
        ESP_LOGW(TAG, "not a RIFF/AVI file");
        return false;
    }

    uint64_t riff_end64 = 8ULL + avi_rd32(hdr + 4);
    if (riff_end64 < 12 || riff_end64 > 0x7FFFFFFFULL) {
        return false;
    }
    uint32_t riff_end = (uint32_t)riff_end64;

    for (uint32_t pos = 12; pos + 8 <= riff_end; ) {
        uint8_t ch[12];
        if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(ch, 1, 8, f) != 8) {
            break;
        }
        uint32_t sz = avi_rd32(ch + 4);

        if (memcmp(ch, "LIST", 4) == 0 && sz >= 4 && fread(ch + 8, 1, 4, f) == 4) {
            if (memcmp(ch + 8, "hdrl", 4) == 0) {
                avi_parse_hdrl(f, pos + 12, sz - 4, info);
            } else if (memcmp(ch + 8, "movi", 4) == 0) {
                uint64_t movi_end = (uint64_t)pos + 8 + sz;
                info->movi_start = pos + 12;
                info->movi_end = movi_end > 0x7FFFFFFFULL ? 0 : (uint32_t)movi_end;
            }
        } else if (memcmp(ch, "idx1", 4) == 0 && sz >= 16) {
            info->idx1_pos = pos + 8;
            info->idx1_size = sz;
        }
        if (!avi_advance(&pos, pos + 8, sz)) {
            break;
        }
    }

    if (!info->has_video) {
        ESP_LOGW(TAG, "no video stream");
        return false;
    }

    if (info->width == 0 || info->height == 0) {
        info->width = info->avih_width;
        info->height = info->avih_height;
    }
    if (info->width == 0 || info->height == 0) {
        ESP_LOGW(TAG, "unknown video size");
        return false;
    }

    if (info->video_frames == 0) {
        info->video_frames = info->avih_total_frames;
    }

    if (info->fps_num > 0 && info->fps_den > 0) {
        uint64_t period = 1000000ULL * info->fps_den / info->fps_num;
        if (period >= 1000 && period <= 1000000) {
            info->frame_period_us = (uint32_t)period;
        }
    } else if (info->avih_us_per_frame >= 1000 && info->avih_us_per_frame <= 1000000) {
        info->frame_period_us = info->avih_us_per_frame;
    }

    if (info->movi_start == 0) {
        ESP_LOGW(TAG, "no movi list");
        return false;
    }
    if (info->movi_end <= info->movi_start) {
        long file_size = -1;
        if (fseek(f, 0, SEEK_END) == 0) {
            file_size = ftell(f);
        }
        if (file_size > (long)info->movi_start) {
            info->movi_end = (uint32_t)file_size;
        }
    }

    char codec[5];
    avi_fourcc(info->compression, codec);
    ESP_LOGI(TAG, "AVI %ux%u %lufps codec=%s frames=%lu vid#%u aud#%u %luHz %uch %ubit",
             (unsigned)info->width, (unsigned)info->height,
             (unsigned long)(1000000UL / info->frame_period_us), codec,
             (unsigned long)info->video_frames,
             info->video_stream, info->audio_stream,
             (unsigned long)info->audio_rate, info->audio_channels, info->audio_bits);
    if (!avi_codec_is_jpeg(info->compression)) {
        ESP_LOGW(TAG, "codec %s is not MJPEG, this file will not play", codec);
    }
    return true;
}

/* ---------- JPEG 解码 ---------- */

static size_t jpeg_in_cb(JDEC *jd, uint8_t *buf, size_t len)
{
    jpeg_io_t *io = (jpeg_io_t *)jd->device;

    if (len > io->left) {
        len = io->left;
    }
    if (len == 0) {
        return 0;
    }

    if (buf != NULL) {
        int64_t t0 = esp_timer_get_time();
        size_t n = fread(buf, 1, len, io->f);
        io->read_us += (uint64_t)(esp_timer_get_time() - t0);
        io->left -= n;
        return n;
    }

    /* TJPGD 用 buf == NULL 来跳字节 */
    if (fseek(io->f, (long)len, SEEK_CUR) != 0) {
        return 0;
    }
    io->left -= len;
    return len;
}

static int jpeg_out_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_io_t *io = (jpeg_io_t *)jd->device;
    return jpeg_rgb888_to_rgb565((const uint8_t *)bitmap,
                                 (int)rect->right - (int)rect->left + 1,
                                 (int)rect->bottom - (int)rect->top + 1,
                                 (int)rect->left, (int)rect->top,
                                 io->dst, io->dst_w, io->dst_h) ? 1 : 0;
}

static void video_fail(const char *msg)
{
    video.failed = true;
    snprintf(video.error, sizeof(video.error), "%s",
             msg != NULL ? msg : "Playback failed");
    ESP_LOGE(TAG, "%s", video.error);
}

static bool video_alloc_frames(uint32_t w, uint32_t h)
{
    size_t bytes = (size_t)w * h * 2;

    for (int i = 0; i < 2; i++) {
        video.fb[i] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (video.fb[i] == NULL) {
            video.fb[i] = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        }
        if (video.fb[i] == NULL) {
            ESP_LOGE(TAG, "no memory for frame %d (%u bytes)", i, (unsigned)bytes);
            for (int j = 0; j < 2; j++) {
                if (video.fb[j] != NULL) {
                    heap_caps_free(video.fb[j]);
                    video.fb[j] = NULL;
                }
            }
            return false;
        }
    }

    video.width = w;
    video.height = h;
    video.front = 0;

    for (int i = 0; i < 2; i++) {
        memset(&video.dsc[i], 0, sizeof(video.dsc[i]));
        video.dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        video.dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        video.dsc[i].header.w = (uint16_t)w;
        video.dsc[i].header.h = (uint16_t)h;
        video.dsc[i].header.stride = (uint16_t)(w * 2);
        video.dsc[i].data_size = (uint32_t)bytes;
        video.dsc[i].data = (const uint8_t *)video.fb[i];
    }
    return true;
}

/* 刷新进度条和时间；调用者必须持有 LVGL 锁 */
static void video_progress_set(uint32_t frame_index)
{
    if (video.bar != NULL && video.total_frames > 0) {
        uint32_t value = (uint32_t)((uint64_t)frame_index * 1000 / video.total_frames);
        if (value > 1000) {
            value = 1000;
        }
        lv_bar_set_value(video.bar, (int32_t)value, LV_ANIM_OFF);
    }

    if (video.time_label != NULL) {
        uint64_t elapsed_us = (uint64_t)frame_index * video.frame_period_us;
        uint64_t total_us = (uint64_t)video.total_frames * video.frame_period_us;
        uint32_t elapsed_s = (uint32_t)(elapsed_us / 1000000ULL);
        uint32_t total_s = (uint32_t)(total_us / 1000000ULL);
        char text[32];

        snprintf(text, sizeof(text), "%02lu:%02lu / %02lu:%02lu",
                 (unsigned long)(elapsed_s / 60), (unsigned long)(elapsed_s % 60),
                 (unsigned long)(total_s / 60), (unsigned long)(total_s % 60));
        lv_label_set_text(video.time_label, text);
    }
}

/* 播放线程刷新进度：用户在拖进度条/已有定位请求时别把预览值冲掉 */
static void video_update_progress(uint32_t frame_index)
{
    if (video.seeking || video.seek_req) {
        return;
    }
    video_progress_set(frame_index);
}

static void video_show_frame(int idx, uint32_t frame_index)
{
    if (video.img == NULL || video.stop) {
        return;
    }
    lvgl_port_lock(0);
    if (video.img != NULL) {
        lv_image_set_src(video.img, &video.dsc[idx]);
        video_update_progress(frame_index);
    }
    lvgl_port_unlock();
    video.front = idx;
}

static bool video_decode_frame(FILE *f, uint32_t size, uint32_t frame_index)
{
    jpeg_io_t io = {
        .f = f,
        .left = size,
        .dst = NULL,
        .dst_w = 0,
        .dst_h = 0,
    };
    JDEC jd;
    int64_t t0 = esp_timer_get_time();
    JRESULT res = jd_prepare(&jd, jpeg_in_cb, video.jpeg_pool, sizeof(video.jpeg_pool), &io);
    if (res != JDR_OK) {
        ESP_LOGW(TAG, "frame %lu: jpeg header failed (%d)",
                 (unsigned long)frame_index, (int)res);
        return false;
    }

    if (jd.width == 0 || jd.height == 0 ||
        jd.width > VIDEO_MAX_DIM || jd.height > VIDEO_MAX_DIM) {
        ESP_LOGE(TAG, "bad frame size %ux%u", jd.width, jd.height);
        video_fail("Video too large (max 2560 px)");
        return false;
    }

    /* 按屏幕大小选 1/1、1/2、1/4、1/8 解码；分辨率越大，单帧开销越小 */
    uint8_t scale = 0;
    while (scale < 3 &&
           ((jd.width >> scale) > VIDEO_SCREEN_W ||
            (jd.height >> scale) > VIDEO_SCREEN_H)) {
        scale++;
    }
    uint32_t out_w = jd.width >> scale;
    uint32_t out_h = jd.height >> scale;
    if (out_w == 0 || out_h == 0) {
        video_fail("Bad video frame size");
        return false;
    }

    if (video.fb[0] == NULL) {
        if (!video_alloc_frames(out_w, out_h)) {
            video_fail("Out of memory for video frame");
            return false;
        }
    } else if (out_w != video.width || out_h != video.height) {
        ESP_LOGW(TAG, "frame size changed to %ux%u, skip", out_w, out_h);
        return false;
    }

    int back = 1 - video.front;
    io.dst = video.fb[back];
    io.dst_w = (int)video.width;
    io.dst_h = (int)video.height;

    res = jd_decomp(&jd, jpeg_out_cb, scale);

    int64_t elapsed = esp_timer_get_time() - t0;
    video.decode_us_sum += (uint64_t)elapsed;
    video.decode_io_us_sum += io.read_us;
    video.decode_count++;
    video.decode_avg_us = video.decode_avg_us == 0
                          ? (uint32_t)elapsed
                          : (uint32_t)(((uint64_t)video.decode_avg_us * 7 +
                                        (uint64_t)elapsed) / 8);

    if (res != JDR_OK) {
        ESP_LOGW(TAG, "frame %lu: jpeg decode failed (%d)",
                 (unsigned long)frame_index, (int)res);
        return false;
    }

    video.frames_decoded++;
    if (video.frames_decoded == 1) {
        ESP_LOGI(TAG, "first frame %ux%u -> %ux%u (scale 1/%u)",
                 jd.width, jd.height, out_w, out_h, 1u << scale);
    }
    video_show_frame(back, frame_index);
    return true;
}

/* ---------- 视频缩略图（相册网格用） ---------- */

/* 在 movi 里找第 *skip 个视频 chunk（skip 为 NULL 表示第一个），支持 LIST rec 嵌套。
   找到后返回 chunk 位置，out_size 非 NULL 时一并返回大小。 */
static bool avi_find_video_chunk(FILE *f, uint32_t start, uint32_t end,
                                 const avi_info_t *info, int depth,
                                 uint32_t *skip,
                                 uint32_t *out_pos, uint32_t *out_size)
{
    if (depth > 4) {
        return false;
    }

    for (uint32_t pos = start; pos + 8 <= end; ) {
        uint8_t ch[8];
        if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(ch, 1, 8, f) != 8) {
            return false;
        }
        uint32_t sz = avi_rd32(ch + 4);
        uint32_t content = pos + 8;

        if (memcmp(ch, "LIST", 4) == 0) {
            uint8_t type[4];
            if (sz >= 4 && fread(type, 1, 4, f) == 4 &&
                memcmp(type, "rec ", 4) == 0 &&
                (uint64_t)content + sz <= 0x7FFFFFFFULL) {
                if (avi_find_video_chunk(f, content + 4, content + sz,
                                         info, depth + 1, skip,
                                         out_pos, out_size)) {
                    return true;
                }
            }
        } else if (avi_chunk_is_video(ch, info)) {
            if (skip == NULL || *skip == 0) {
                *out_pos = pos;
                if (out_size != NULL) {
                    *out_size = sz;
                }
                return true;
            }
            (*skip)--;
        }

        if (!avi_advance(&pos, content, sz)) {
            return false;
        }
    }
    return false;
}

/* 解 AVI 第一帧，按比例缩到 dst_size×dst_size（RGB565，黑边填充）。
   path 是不带 LVGL 盘符的路径，如 "/sd/DCIM/a.avi"。 */
bool album_video_thumb(const char *path, uint16_t *dst, int dst_size)
{
    static uint8_t thumb_pool[VIDEO_JPEG_POOL];
    uint16_t *full = NULL;
    bool ok = false;

    if (path == NULL || dst == NULL || dst_size <= 0) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }

    avi_info_t info;
    uint32_t chunk_pos = 0;
    uint32_t chunk_size = 0;

    if (avi_parse(f, &info) &&
        avi_find_video_chunk(f, info.movi_start, info.movi_end, &info, 0,
                             NULL, &chunk_pos, &chunk_size) &&
        chunk_size > 0 &&
        fseek(f, (long)chunk_pos + 8, SEEK_SET) == 0) {

        jpeg_io_t io = {
            .f = f,
            .left = chunk_size,
            .dst = NULL,
            .dst_w = 0,
            .dst_h = 0,
        };
        JDEC jd;

        if (jd_prepare(&jd, jpeg_in_cb, thumb_pool, sizeof(thumb_pool), &io) == JDR_OK &&
            jd.width > 0 && jd.height > 0) {

            /* 缩略图不用全尺寸解：挑一个仍然不小于 dst_size 的 1/2、1/4、1/8 */
            uint8_t scale = 0;
            while (scale < 3 &&
                   (jd.width >> (scale + 1)) >= (uint32_t)dst_size &&
                   (jd.height >> (scale + 1)) >= (uint32_t)dst_size) {
                scale++;
            }
            uint32_t out_w = jd.width >> scale;
            uint32_t out_h = jd.height >> scale;

            if (out_w > 0 && out_h > 0 &&
                (uint64_t)out_w * out_h * 2 <= 1024 * 1024) {

                size_t bytes = (size_t)out_w * out_h * 2;
                full = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (full == NULL) {
                    full = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
                }

                if (full != NULL) {
                    io.dst = full;
                    io.dst_w = (int)out_w;
                    io.dst_h = (int)out_h;

                    if (jd_decomp(&jd, jpeg_out_cb, scale) == JDR_OK) {
                        memset(dst, 0, (size_t)dst_size * dst_size * 2);

                        int tw;
                        int th;
                        if (out_w >= out_h) {
                            tw = dst_size;
                            th = (int)(out_h * (uint32_t)dst_size / out_w);
                        } else {
                            th = dst_size;
                            tw = (int)(out_w * (uint32_t)dst_size / out_h);
                        }
                        if (tw < 1) tw = 1;
                        if (th < 1) th = 1;

                        int ox = (dst_size - tw) / 2;
                        int oy = (dst_size - th) / 2;

                        for (int y = 0; y < th; y++) {
                            int sy = y * (int)out_h / th;
                            const uint16_t *srow = full + (size_t)sy * out_w;
                            uint16_t *drow = dst + (size_t)(oy + y) * dst_size + ox;
                            for (int x = 0; x < tw; x++) {
                                int sx = x * (int)out_w / tw;
                                drow[x] = srow[sx];
                            }
                        }
                        ok = true;
                    }
                }
            }
        }
    }

    if (full != NULL) {
        heap_caps_free(full);
    }
    fclose(f);
    return ok;
}

/* ---------- 音频 ---------- */

static bool video_audio_start(const avi_info_t *info)
{
    if (!video_audio_enabled) {
        return false;
    }
    if (!info->has_audio) {
        return false;
    }
    if (info->audio_format != 1 || info->audio_bits != 16 ||
        (info->audio_channels != 1 && info->audio_channels != 2) ||
        info->audio_rate < 8000 || info->audio_rate > 96000) {
        ESP_LOGW(TAG, "unsupported audio: fmt=%u ch=%u bit=%u rate=%lu",
                 info->audio_format, info->audio_channels, info->audio_bits,
                 (unsigned long)info->audio_rate);
        return false;
    }

    /* PCM 缓冲按需分配：关闭声音时这部分内部 RAM 归零 */
    if (video.pcm_in == NULL) {
        video.pcm_in = heap_caps_malloc(VIDEO_AUDIO_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        video.pcm_out = heap_caps_malloc(VIDEO_AUDIO_BUF * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (video.pcm_in == NULL || video.pcm_out == NULL) {
            if (video.pcm_in != NULL) {
                heap_caps_free(video.pcm_in);
                video.pcm_in = NULL;
            }
            if (video.pcm_out != NULL) {
                heap_caps_free(video.pcm_out);
                video.pcm_out = NULL;
            }
            ESP_LOGE(TAG, "no mem for audio buffers");
            return false;
        }
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* DMA 缓冲全部在内部 RAM，且每个描述符单独分配。
     * 44.1kHz 立体声 16bit 下：Frames x 4 字节 = 单个描述符大小。
     * menuconfig 里可调，摄像头同时工作时通常需要调小以免分配失败。 */
    chan_cfg.dma_desc_num = CONFIG_APP_VIDEO_AUDIO_DMA_DESC;
    chan_cfg.dma_frame_num = CONFIG_APP_VIDEO_AUDIO_DMA_FRAMES;
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &video.tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        video.tx = NULL;
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(info->audio_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = VIDEO_I2S_BCLK,
            .ws = VIDEO_I2S_WS,
            .dout = VIDEO_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = 0,
                .bclk_inv = 0,
                .ws_inv = 0,
            },
        },
    };

    err = i2s_channel_init_std_mode(video.tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s init std: %s", esp_err_to_name(err));
        i2s_del_channel(video.tx);
        video.tx = NULL;
        return false;
    }

    err = i2s_channel_enable(video.tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s enable: %s", esp_err_to_name(err));
        i2s_del_channel(video.tx);
        video.tx = NULL;
        return false;
    }

    video.audio_ready = true;

    /* 播放中打开声音时，这里要负责拉功放使能；暂停中打开则等 resume 再开，
       避免 I2S 没数据时功放把底噪放出来 */
    if (!video.paused) {
        video_spk_power(true);
    }

    ESP_LOGI(TAG, "audio %luHz %uch -> BCLK=%d WS=%d DOUT=%d",
             (unsigned long)info->audio_rate, info->audio_channels,
             (int)VIDEO_I2S_BCLK, (int)VIDEO_I2S_WS, (int)VIDEO_I2S_DOUT);
    return true;
}

/* 关闭声音：停 I2S、释放 DMA 和 PCM 缓冲。只在视频任务里调用，避免和 I2S 写入并发 */
static void video_audio_stop(void)
{
    if (video.tx != NULL) {
        video_spk_power(false);     /* 先关功放再停 I2S，避免关时钟的咔哒声 */
        i2s_channel_disable(video.tx);
        i2s_del_channel(video.tx);
        video.tx = NULL;
    }
    video.audio_ready = false;

    if (video.pcm_in != NULL) {
        heap_caps_free(video.pcm_in);
        video.pcm_in = NULL;
    }
    if (video.pcm_out != NULL) {
        heap_caps_free(video.pcm_out);
        video.pcm_out = NULL;
    }
}

/* 用读丢弃代替 fseek：fseek 会让 stdio 丢掉整块预读缓冲，顺序播放时
   每一帧都重新读盘，静音播放会明显变慢 */
static void video_skip_bytes(FILE *f, uint32_t size)
{
    uint8_t tmp[256];

    while (size > 0) {
        size_t want = size < sizeof(tmp) ? size : sizeof(tmp);
        size_t got = fread(tmp, 1, want, f);
        if (got == 0) {
            break;
        }
        size -= (uint32_t)got;
    }
}

/* 按百分比缩放 16bit PCM 采样（就地），0 就是静音 */
static void video_pcm_volume(int16_t *pcm, size_t samples)
{
    uint32_t vol = video_volume;

    if (vol >= 100) {
        return;
    }
    for (size_t i = 0; i < samples; i++) {
        int32_t v = ((int32_t)pcm[i] * (int32_t)vol + 50) / 100;
        pcm[i] = (int16_t)v;
    }
}

/* 把一个音频 chunk 写进 I2S；单声道展开成左右相同的立体声 */
static void video_audio_chunk(FILE *f, uint32_t size, const avi_info_t *info)
{
    if (!video_audio_enabled) {
        if (video.audio_ready || video.tx != NULL) {
            video_audio_stop();
        }
        video_skip_bytes(f, size);
        return;
    }
    if (!video.audio_ready || video.tx == NULL) {
        if (!video_audio_start(info)) {
            video_skip_bytes(f, size);
            return;
        }
    }

    const size_t align = (info->audio_channels == 2) ? 4 : 2;

    while (size > 0 && !video.stop && !video.failed && !video.seek_req) {
        video_pause_wait();
        if (video.stop || video.failed || video.seek_req) {
            break;
        }

        size_t want = size < VIDEO_AUDIO_BUF ? size : VIDEO_AUDIO_BUF;
        want -= want % align;
        if (want == 0) {
            video_skip_bytes(f, size);
            break;
        }

        size_t got = fread(video.pcm_in, 1, want, f);
        if (got < want) {
            size -= got;
            break;
        }
        size -= got;

        size_t out_bytes;
        if (info->audio_channels == 1) {
            const int16_t *in = (const int16_t *)video.pcm_in;
            int16_t *out = (int16_t *)video.pcm_out;
            size_t samples = got / 2;
            for (size_t i = 0; i < samples; i++) {
                out[i * 2] = in[i];
                out[i * 2 + 1] = in[i];
            }
            out_bytes = samples * 4;
        } else {
            memcpy(video.pcm_out, video.pcm_in, got);
            out_bytes = got;
        }

        /* 音量：NS4168 没有音量接口，只能缩放 PCM 采样 */
        video_pcm_volume((int16_t *)video.pcm_out, out_bytes / 2);

        /* 顺便统计峰值：用来判断文件里到底有没有声音 */
        const int16_t *pcm = (const int16_t *)video.pcm_out;
        uint32_t peak = video.audio_peak;
        for (size_t i = 0; i < out_bytes / 2; i++) {
            int v = pcm[i];
            if (v < 0) {
                v = -v;
            }
            if ((uint32_t)v > peak) {
                peak = (uint32_t)v;
            }
        }
        video.audio_peak = peak;

        size_t written = 0;
        esp_err_t err = i2s_channel_write(video.tx, video.pcm_out, out_bytes,
                                          &written, 200);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write: %s", esp_err_to_name(err));
            video.audio_ready = false;
            break;
        }
        video.audio_bytes += written;
    }

    if (size > 0 && !video.seek_req) {
        video_skip_bytes(f, size);
    }
}

/* ---------- 拖拽定位 ---------- */

/* idx1 里的 dwChunkOffset 基准不统一：ffmpeg 是相对 "movi" 四个字符，
   也有相对 movi 数据开头或文件开头的。拿第一个视频项试出正确基准。 */
static bool video_idx1_base(FILE *f, const avi_info_t *info, uint32_t rel,
                            const uint8_t ckid[4], uint32_t *out_base)
{
    const uint32_t bases[3] = {
        info->movi_start - 4,
        info->movi_start,
        0,
    };

    for (int i = 0; i < 3; i++) {
        uint64_t at = (uint64_t)bases[i] + rel;
        uint8_t h[4];

        if (at > 0x7FFFFFFFULL) {
            continue;
        }
        if (fseek(f, (long)at, SEEK_SET) == 0 &&
            fread(h, 1, 4, f) == 4 && memcmp(h, ckid, 4) == 0) {
            *out_base = bases[i];
            return true;
        }
    }
    return false;
}

/* 用 idx1 建“第 N 帧在文件哪个位置”的表，放 PSRAM；失败就退回顺序扫描 */
static bool video_build_index(FILE *f, const avi_info_t *info)
{
    const uint32_t max_entries = 262144;    /* 最多 1 MB 偏移表 */

    if (info->idx1_pos == 0 || info->idx1_size < 16) {
        return false;
    }

    uint32_t entries = info->idx1_size / 16;
    if (entries > max_entries) {
        entries = max_entries;
    }

    /* 第一遍：找第一个视频项，用它确定偏移基准 */
    if (fseek(f, (long)info->idx1_pos, SEEK_SET) != 0) {
        return false;
    }
    uint8_t e[16];
    uint8_t ckid[4] = {0};
    uint32_t rel = 0;
    bool found = false;
    for (uint32_t i = 0; i < entries; i++) {
        if (fread(e, 1, 16, f) != 16) {
            break;
        }
        if (avi_chunk_is_video(e, info)) {
            memcpy(ckid, e, 4);
            rel = avi_rd32(e + 8);
            found = true;
            break;
        }
    }
    if (!found) {
        ESP_LOGW(TAG, "idx1 has no video entries");
        return false;
    }

    uint32_t base = 0;
    if (!video_idx1_base(f, info, rel, ckid, &base)) {
        ESP_LOGW(TAG, "idx1 offsets do not match movi, seeking disabled");
        return false;
    }

    uint32_t *pos = heap_caps_malloc(entries * sizeof(uint32_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pos == NULL) {
        pos = heap_caps_malloc(entries * sizeof(uint32_t), MALLOC_CAP_8BIT);
    }
    if (pos == NULL) {
        ESP_LOGW(TAG, "no memory for seek index");
        return false;
    }

    /* 第二遍：收集每个视频帧的位置 */
    uint32_t count = 0;
    if (fseek(f, (long)info->idx1_pos, SEEK_SET) != 0) {
        heap_caps_free(pos);
        return false;
    }
    for (uint32_t i = 0; i < entries; i++) {
        if (fread(e, 1, 16, f) != 16) {
            break;
        }
        if (avi_chunk_is_video(e, info)) {
            pos[count++] = base + avi_rd32(e + 8);
        }
    }

    if (count == 0) {
        heap_caps_free(pos);
        return false;
    }

    video.seek_pos = pos;
    video.seek_count = count;
    ESP_LOGI(TAG, "seek index: %lu video frames from idx1", (unsigned long)count);
    return true;
}

/* 第 frame 帧（视频 chunk）在文件里的位置 */
static bool video_seek_target(FILE *f, const avi_info_t *info,
                              uint32_t frame, uint32_t *out_pos)
{
    if (video.seek_pos != NULL && video.seek_count > 0) {
        if (frame >= video.seek_count) {
            frame = video.seek_count - 1;
        }
        *out_pos = video.seek_pos[frame];
        return true;
    }
    return avi_find_video_chunk(f, info->movi_start, info->movi_end, info,
                                0, &frame, out_pos, NULL);
}

/* ---------- movi 遍历 ---------- */

typedef struct {
    avi_info_t *info;
    uint32_t frame_index;
    int64_t start_us;
    int depth;
    uint32_t fail_streak;
    uint32_t dropped;
    int64_t last_drop_log;
    int64_t budget_us;              /* 解码预算：按真实时间累积，解码时扣除 */
    int64_t last_budget_us;
    bool allow_first;               /* 暂停中定位：先显示目标帧，再卡在暂停里 */
    bool first_done;
} demux_ctx_t;

static void video_demux_range(FILE *f, uint32_t start, uint32_t end,
                              demux_ctx_t *ctx)
{
    for (uint32_t pos = start;
         pos + 8 <= end && !video.stop && !video.failed && !video.seek_req; ) {
        uint8_t ch[8];
        /* 顺序访问时不要 fseek，否则 stdio 会把预读的 16KB 全丢掉 */
        if (ftell(f) != (long)pos && fseek(f, (long)pos, SEEK_SET) != 0) {
            return;
        }
        if (fread(ch, 1, 8, f) != 8) {
            return;
        }
        uint32_t sz = avi_rd32(ch + 4);
        uint32_t content = pos + 8;

        /* 暂停中定位：跳过第一次暂停等待，先解出并显示目标帧 */
        bool first = ctx->allow_first && !ctx->first_done;
        if (!first) {
            video_pause_wait();
            if (video.stop || video.failed || video.seek_req) {
                return;
            }
        }

        if (memcmp(ch, "LIST", 4) == 0) {
            uint8_t type[4];
            if (sz >= 4 && ctx->depth < 4 && fread(type, 1, 4, f) == 4 &&
                memcmp(type, "rec ", 4) == 0) {
                video_demux_range(f, content + 4, content + sz, ctx);
                if (video.stop || video.failed) {
                    return;
                }
            }
        } else if (avi_chunk_is_video(ch, ctx->info)) {
            int64_t period = (int64_t)ctx->info->frame_period_us;
            int64_t target = ctx->start_us +
                             (int64_t)ctx->frame_index * period +
                             video.clock_offset_us;
            int64_t now = esp_timer_get_time();

            /* 没有音频时才需要自己等；有音频时 i2s 写会阻塞，天然定速 */
            if (!video.audio_ready && now < target) {
                video_delay_us(target - now);
                now = esp_timer_get_time();
            }

            /* 解码预算：按真实时间累积，解一帧扣掉它的实际耗时。
               解码比帧间隔慢时，预算不够就跳过这一帧——总解码时间
               不会超过真实时间，播放保持实时、和音频同步，不会变成
               慢放。不能用 now-target 判断：音频让墙上时钟一直前进，
               一旦落后就永远追不回来，画面会永久定格。 */
            int64_t estimate = video.decode_avg_us > 0
                               ? (int64_t)video.decode_avg_us : period;
            ctx->budget_us += now - ctx->last_budget_us;
            ctx->last_budget_us = now;

            int64_t cap = estimate * 2 + period;
            if (ctx->budget_us > cap) {
                ctx->budget_us = cap;   /* 卡顿后不要一次补解一大串 */
            }

            if (ctx->budget_us >= estimate) {
                int64_t started = esp_timer_get_time();
                ctx->budget_us -= estimate;

                if (video_decode_frame(f, sz, ctx->frame_index)) {
                    ctx->fail_streak = 0;
                } else {
                    ctx->fail_streak++;
                    if (video.failed) {
                        return;
                    }
                    /* 连续多帧连头都解不出来，说明不是 MJPEG，别干等 */
                    if (ctx->fail_streak >= 5 && video.frames_decoded == 0) {
                        video_fail("MJPEG decode failed, check the codec");
                        return;
                    }
                }

                /* 多退少补：按实测耗时修正预算 */
                ctx->budget_us += estimate - (esp_timer_get_time() - started);
            } else {
                ctx->dropped++;
                if (ctx->last_drop_log == 0 ||
                    now - ctx->last_drop_log > 1000000) {
                    ESP_LOGW(TAG, "decode budget short, dropped %lu frames",
                             (unsigned long)ctx->dropped);
                    ctx->last_drop_log = now;
                }
            }
            ctx->frame_index++;
            ctx->first_done = true;

        } else if (avi_chunk_is_audio(ch, ctx->info)) {
            video_audio_chunk(f, sz, ctx->info);
        }

        if (!avi_advance(&pos, content, sz)) {
            return;
        }
    }
}

/* ---------- 任务与 UI ---------- */

static void video_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    video.stop = true;
}

/* 点画面：播放中暂停/继续；出错时当关闭用 */
static void video_overlay_clicked_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if (video.failed) {
        video.stop = true;
        return;
    }
    if (video.finished || video.play_icon == NULL) {
        return;
    }

    video.paused = !video.paused;
    lv_label_set_text(video.play_icon,
                      video.paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

/* 播放/暂停按钮；放到头以后再点就是重播 */
static void video_play_clicked_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if (video.failed) {
        return;
    }

    if (video.finished) {
        video.finished = false;
        video.restart = true;
        if (video.play_icon != NULL) {
            lv_label_set_text(video.play_icon, LV_SYMBOL_PAUSE);
        }
        if (video.bar != NULL) {
            lv_bar_set_value(video.bar, 0, LV_ANIM_OFF);
        }
        return;
    }

    video.paused = !video.paused;
    if (video.play_icon != NULL) {
        lv_label_set_text(video.play_icon,
                          video.paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    }
}

/* ---------- 进度条拖拽 ---------- */

/* 触摸点 x 换算成帧号；返回 false 表示这个视频不能定位 */
static bool video_bar_point_to_frame(lv_obj_t *bar, const lv_point_t *p, uint32_t *frame)
{
    lv_area_t a;
    int32_t w;
    int32_t x;

    if (bar == NULL || video.total_frames == 0) {
        return false;
    }
    lv_obj_get_coords(bar, &a);

    w = lv_area_get_width(&a);
    if (w <= 0) {
        return false;
    }

    x = p->x - a.x1;
    if (x < 0) {
        x = 0;
    }
    if (x > w) {
        x = w;
    }

    uint32_t f = (uint32_t)((uint64_t)x * video.total_frames / (uint32_t)w);
    if (f >= video.total_frames) {
        f = video.total_frames - 1;
    }
    *frame = f;
    return true;
}

/* 按下/拖动时只改预览，不发定位请求；松手才真正跳 */
static void video_bar_drag_update(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;
    uint32_t frame;

    if (indev == NULL) {
        return;
    }
    lv_indev_get_point(indev, &p);
    if (p.x < 0 || !video_bar_point_to_frame(lv_event_get_target(e), &p, &frame)) {
        return;
    }
    video.drag_frame = frame;
    video_progress_set(frame);
}

static void video_bar_pressed_cb(lv_event_t *e)
{
    if (video.total_frames == 0) {
        return;
    }
    video.seeking = true;
    video_bar_drag_update(e);
}

static void video_bar_pressing_cb(lv_event_t *e)
{
    if (video.seeking) {
        video_bar_drag_update(e);
    }
}

static void video_bar_released_cb(lv_event_t *e)
{
    if (!video.seeking) {
        return;
    }
    video_bar_drag_update(e);
    /* 先挂请求再清 seeking，避免播放线程趁空档把进度条冲回去 */
    video.seek_frame = video.drag_frame;
    video.seek_req = true;
    video.seeking = false;
}

/* 解析/解码失败：把原因留在画面上，别让用户只看到黑屏 */
static void video_show_error(void)
{
    lvgl_port_lock(0);

    if (video.overlay != NULL && video.error_label == NULL) {
        video.error_label = lv_label_create(video.overlay);
        lv_obj_set_width(video.error_label, 280);
        lv_label_set_long_mode(video.error_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(video.error_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(video.error_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(video.error_label, &lv_font_montserrat_14, 0);
        lv_label_set_text(video.error_label, video.error);
        lv_obj_center(video.error_label);
    }
    if (video.ctrl_bar != NULL) {
        lv_obj_set_hidden(video.ctrl_bar, true);
    }
    if (video.time_label != NULL) {
        lv_obj_set_hidden(video.time_label, true);
    }

    lvgl_port_unlock();
}

static void video_task(void *arg)
{
    LV_UNUSED(arg);

    /* 插不插摄像头最大的变量就是内部 RAM；这行能直接看出 ring/I2S 是不是
       因为内存不够才没起来 */
    ESP_LOGD(TAG, "start: internal heap %u B, psram %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    FILE *f = fopen(video.path, "rb");
    avi_info_t info;

    if (f != NULL) {
        /* TJPGD 的输入回调每次只读 512 字节；用 16KB stdio 缓冲让 FatFS
           一次读多个扇区，否则 30fps 下光读卡就会拖垮播放 */
        video.file_buf = heap_caps_malloc(VIDEO_FILE_BUF,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (video.file_buf == NULL) {
            video.file_buf = heap_caps_malloc(VIDEO_FILE_BUF, MALLOC_CAP_8BIT);
        }
        if (video.file_buf != NULL) {
            setvbuf(f, (char *)video.file_buf, _IOFBF, VIDEO_FILE_BUF);
        } else {
            ESP_LOGW(TAG, "no memory for file buffer, using default stdio");
        }
    }

    if (f == NULL) {
        video_fail("Cannot open video file");
    } else if (!avi_parse(f, &info)) {
        video_fail("Not a supported AVI file");
    } else if (!avi_codec_is_jpeg(info.compression)) {
        char codec[5];
        char msg[VIDEO_ERROR_MAX];
        avi_fourcc(info.compression, codec);
        snprintf(msg, sizeof(msg), "Codec %.4s not supported\nRe-encode as MJPEG AVI",
                 codec);
        video_fail(msg);
    } else {
        video.frame_period_us = info.frame_period_us;
        video_audio_start(&info);   /* 音频不行就静音播，不影响画面 */
        video_build_index(f, &info);
        /* 索引里的帧数比头里写的可靠；没有索引时用文件头的值 */
        video.total_frames = video.seek_count > 0 ? video.seek_count : info.video_frames;

        uint32_t start_frame = 0;
        bool paused_start = false;

        while (!video.stop && !video.failed) {
            /* 拖了进度条（或播放结束后定位）：从目标帧重新开始 */
            if (video.seek_req) {
                start_frame = video.seek_frame;
                video.seek_req = false;
                paused_start = video.paused;
                if (video.total_frames > 0 && start_frame >= video.total_frames) {
                    start_frame = video.total_frames - 1;
                }
                if (video.tx != NULL) {
                    /* 丢掉 DMA 里还没播完的旧声音 */
                    i2s_channel_disable(video.tx);
                    i2s_channel_enable(video.tx);
                }
            }

            /* 这一段真要出声了才开功放 */
            if (video.audio_ready) {
                video_spk_power(true);
            }

            uint32_t start_pos = 0;
            if (!video_seek_target(f, &info, start_frame, &start_pos)) {
                video_fail("Cannot seek in this video");
                break;
            }

            int64_t now = esp_timer_get_time();
            demux_ctx_t ctx = {
                .info = &info,
                .frame_index = start_frame,
                /* 让目标帧的显示时间正好是“现在”，不能从现在重新算时间轴 */
                .start_us = now - (int64_t)start_frame * (int64_t)info.frame_period_us,
                .depth = 0,
                .fail_streak = 0,
                .allow_first = paused_start,
            };
            ctx.budget_us = info.frame_period_us;   /* 先解目标帧 */
            ctx.last_budget_us = now;
            video.clock_offset_us = 0;
            video.frames_decoded = 0;
            video.decode_avg_us = 0;
            video.decode_us_sum = 0;
            video.decode_io_us_sum = 0;
            video.decode_count = 0;
            video.audio_bytes = 0;
            video.audio_peak = 0;

            lvgl_port_lock(0);
            video_update_progress(start_frame);
            lvgl_port_unlock();

            video_demux_range(f, start_pos, info.movi_end, &ctx);
            if (video.stop || video.failed) {
                break;
            }
            if (video.seek_req) {
                /* 播放中拖了进度条，直接开下一轮 */
                paused_start = false;
                continue;
            }

            /* 等最后一点声音放完，然后关功放：停在最后一帧时喇叭也没有底噪 */
            if (video.audio_ready) {
                vTaskDelay(pdMS_TO_TICKS(80));
                video_spk_power(false);
            }

            lvgl_port_lock(0);
            video.finished = true;
            video_update_progress(video.total_frames);
            if (video.bar != NULL && video.total_frames == 0) {
                lv_bar_set_value(video.bar, 1000, LV_ANIM_OFF);
            }
            if (video.play_icon != NULL) {
                lv_label_set_text(video.play_icon, LV_SYMBOL_PLAY);
            }
            lvgl_port_unlock();
            ESP_LOGI(TAG, "playback finished (%lu frames, %lu dropped, decode avg %lu us, sd %lu us)",
                     (unsigned long)video.frames_decoded,
                     (unsigned long)ctx.dropped,
                     video.decode_count ? (unsigned long)(video.decode_us_sum / video.decode_count) : 0UL,
                     video.decode_count ? (unsigned long)(video.decode_io_us_sum / video.decode_count) : 0UL);
            ESP_LOGI(TAG, "audio written %lu bytes, peak %lu (0 = silent track)",
                     (unsigned long)video.audio_bytes,
                     (unsigned long)video.audio_peak);

            while (!video.stop && !video.restart && !video.seek_req) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (video.stop) {
                break;
            }
            if (video.seek_req) {
                /* 停在结尾时拖了进度条：从目标帧继续播 */
                video.finished = false;
                video.paused = false;
                paused_start = false;
                if (video.play_icon != NULL) {
                    lv_label_set_text(video.play_icon, LV_SYMBOL_PAUSE);
                }
                continue;
            }

            /* 重播：回到开头，时间轴清零 */
            video.restart = false;
            video.finished = false;
            video.paused = false;
            start_frame = 0;
            paused_start = false;
            if (video.play_icon != NULL) {
                lv_label_set_text(video.play_icon, LV_SYMBOL_PAUSE);
            }
        }
    }

    if (video.failed) {
        video_show_error();
    }

    /* 停在最后一帧或错误画面上，等用户点关闭 */
    while (!video.stop) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    video_audio_stop();
    if (f != NULL) {
        fclose(f);
    }
    if (video.file_buf != NULL) {
        heap_caps_free(video.file_buf);
        video.file_buf = NULL;
    }
    video.audio_ready = false;

    lvgl_port_lock(0);
    if (video.vol_timer != NULL) {
        lv_timer_delete(video.vol_timer);
        video.vol_timer = NULL;
    }
    if (video.overlay != NULL) {
        lv_obj_delete(video.overlay);
        video.overlay = NULL;
        video.img = NULL;
        video.ctrl_bar = NULL;
        video.play_icon = NULL;
        video.bar = NULL;
        video.time_label = NULL;
        video.error_label = NULL;
        video.vol_label = NULL;
    }
    lvgl_port_unlock();

    for (int i = 0; i < 2; i++) {
        if (video.fb[i] != NULL) {
            heap_caps_free(video.fb[i]);
            video.fb[i] = NULL;
        }
    }
    if (video.seek_pos != NULL) {
        heap_caps_free(video.seek_pos);
        video.seek_pos = NULL;
        video.seek_count = 0;
    }

    /* 视频资源已释放，恢复相机（相册切回摄像头页时会重新打开） */
    usb_camera_set_suspended(false);

    video.playing = false;
    vTaskDelete(NULL);
}

/* 统一给圆按钮上样式，省得默认主题加阴影/边框 */
static void video_round_button_style(lv_obj_t *btn)
{
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
}

/* 小圆按钮 + 白色符号图标 */
static lv_obj_t *video_icon_button(lv_obj_t *parent, int x, int y,
                                   const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 28, 28);
    lv_obj_set_pos(btn, x, y);
    video_round_button_style(btn);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_center(icon);
    return btn;
}

/* 音量标签显示 1.2s 后自己藏起来 */
static void video_vol_hide_cb(lv_timer_t *timer)
{
    if (video.vol_label != NULL) {
        lv_obj_set_hidden(video.vol_label, true);
    }
    lv_timer_pause(timer);
}

static void video_volume_step(int delta)
{
    int v = (int)video_volume + delta;

    if (v < 0) {
        v = 0;
    } else if (v > 100) {
        v = 100;
    }
    video_volume = (uint8_t)v;

    if (video.vol_label != NULL) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)video_volume);
        lv_label_set_text(video.vol_label, buf);
        lv_obj_set_hidden(video.vol_label, false);
    }
    if (video.vol_timer != NULL) {
        lv_timer_reset(video.vol_timer);
        lv_timer_resume(video.vol_timer);
    }
}

static void video_vol_down_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    video_volume_step(-10);
}

static void video_vol_up_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    video_volume_step(10);
}

/* 声音开关：默认关闭。只翻转标志和图标，真正的 I2S 起停在视频任务里做，
   避免 UI 线程和 I2S 写入并发 */
static void video_audio_toggle_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    video_audio_enabled = !video_audio_enabled;

    if (video.audio_icon != NULL) {
        lv_label_set_text(video.audio_icon,
                          video_audio_enabled ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    }
    if (video.vol_label != NULL) {
        lv_label_set_text(video.vol_label, video_audio_enabled ? "ON" : "OFF");
        lv_obj_set_hidden(video.vol_label, false);
        if (video.vol_timer != NULL) {
            lv_timer_reset(video.vol_timer);
            lv_timer_resume(video.vol_timer);
        }
    }
}

void album_video_play(const char *path)
{
    if (path == NULL || path[0] == '\0' || video.playing) {
        return;
    }

    memset(&video, 0, sizeof(video));
    snprintf(video.path, sizeof(video.path), "%s", path);

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    if (overlay == NULL) {
        return;
    }
    video.overlay = overlay;
    lv_obj_set_size(overlay, VIDEO_SCREEN_W, VIDEO_SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_scrollable(overlay, false);
    lv_obj_set_clickable(overlay, true);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(overlay, video_overlay_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *img = lv_image_create(overlay);
    video.img = img;
    lv_obj_set_size(img, VIDEO_SCREEN_W, VIDEO_SCREEN_H);
    lv_obj_center(img);
    lv_obj_set_clickable(img, false);
    lv_obj_set_scrollable(img, false);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);

    /* 左上角时间 */
    lv_obj_t *time_label = lv_label_create(overlay);
    video.time_label = time_label;
    lv_label_set_text(time_label, "00:00 / 00:00");
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(time_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(time_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(time_label, 6, 0);
    lv_obj_set_style_pad_ver(time_label, 2, 0);
    lv_obj_set_style_radius(time_label, 6, 0);
    lv_obj_set_pos(time_label, 8, 8);
    lv_obj_set_clickable(time_label, false);

    /* 底部控制条：播放/暂停 + 可拖拽进度条 */
    lv_obj_t *ctrl = lv_obj_create(overlay);
    video.ctrl_bar = ctrl;
    lv_obj_set_size(ctrl, VIDEO_SCREEN_W, VIDEO_CTRL_H);
    lv_obj_set_pos(ctrl, 0, VIDEO_SCREEN_H - VIDEO_CTRL_H);
    lv_obj_set_scrollable(ctrl, false);
    lv_obj_set_clickable(ctrl, true);
    lv_obj_set_style_pad_all(ctrl, 0, 0);
    lv_obj_set_style_border_width(ctrl, 0, 0);
    lv_obj_set_style_radius(ctrl, 0, 0);
    lv_obj_set_style_shadow_width(ctrl, 0, 0);
    lv_obj_set_style_bg_color(ctrl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_60, 0);

    lv_obj_t *play_btn = lv_button_create(ctrl);
    lv_obj_set_size(play_btn, 30, 30);
    lv_obj_set_pos(play_btn, 6, 5);
    video_round_button_style(play_btn);
    lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x2196f3), 0);
    lv_obj_add_event_cb(play_btn, video_play_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *play_icon = lv_label_create(play_btn);
    video.play_icon = play_icon;
    lv_label_set_text(play_icon, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(play_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(play_icon, lv_color_white(), 0);
    lv_obj_center(play_icon);

    /* 进度条本身只有 8 像素高，点击区上下各扩 8 像素，好按也好拖 */
    lv_obj_t *bar = lv_bar_create(ctrl);
    video.bar = bar;
    lv_obj_set_size(bar, 268, 8);
    lv_obj_set_pos(bar, 44, 16);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_clickable(bar, true);
    lv_obj_set_ext_click_area(bar, 8);
    lv_obj_add_event_cb(bar, video_bar_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(bar, video_bar_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(bar, video_bar_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(bar, video_bar_released_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x2196f3), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    /* 右上角关闭 */
    lv_obj_t *close_btn = lv_button_create(overlay);
    lv_obj_set_size(close_btn, 30, 30);
    lv_obj_set_pos(close_btn, VIDEO_SCREEN_W - 38, 6);
    video_round_button_style(close_btn);
    lv_obj_add_event_cb(close_btn, video_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_icon = lv_label_create(close_btn);
    lv_label_set_text(close_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(close_icon, lv_color_white(), 0);
    lv_obj_center(close_icon);

    /* 声音开关：默认关闭；关闭时不初始化 I2S、不分配 PCM 缓冲 */
    lv_obj_t *audio_btn = video_icon_button(overlay, 180, 6,
                                            video_audio_enabled ? LV_SYMBOL_VOLUME_MAX
                                                                : LV_SYMBOL_MUTE,
                                            video_audio_toggle_cb);
    if (audio_btn != NULL) {
        video.audio_icon = lv_obj_get_child(audio_btn, 0);
    }

    /* 音量：NS4168 没有音量脚，- / + 直接缩放 PCM，一次 10% */
    video_icon_button(overlay, 214, 6, LV_SYMBOL_MINUS, video_vol_down_cb);
    video_icon_button(overlay, 248, 6, LV_SYMBOL_PLUS, video_vol_up_cb);

    lv_obj_t *vol_label = lv_label_create(overlay);
    video.vol_label = vol_label;
    lv_obj_set_size(vol_label, 64, 22);
    lv_obj_align(vol_label, LV_ALIGN_BOTTOM_MID, 0, -(VIDEO_CTRL_H + 8));
    lv_obj_set_style_text_font(vol_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(vol_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(vol_label, 3, 0);
    lv_obj_set_style_text_color(vol_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(vol_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(vol_label, LV_OPA_50, 0);
    lv_obj_set_style_radius(vol_label, 6, 0);
    lv_obj_set_hidden(vol_label, true);
    lv_obj_set_clickable(vol_label, false);

    /* 一个复用的定时器：每次调音量重新计时，1.2s 没再按就把百分比藏起来 */
    video.vol_timer = lv_timer_create(video_vol_hide_cb, 1200, NULL);
    if (video.vol_timer != NULL) {
        lv_timer_pause(video.vol_timer);
    }
    usb_camera_set_active(false);
    /* 视频解码、I2S 和 SD 卡也需要内部 RAM，先把 UVC 设备关掉释放 URB */
    usb_camera_set_suspended(true);
    for (int i = 0; i < 10 && !usb_camera_is_closed(); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    video.playing = true;
    if (xTaskCreatePinnedToCore(video_task, "avi_play", 8192, NULL, 6, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no task for video");
        usb_camera_set_suspended(false);
        lvgl_port_lock(0);
        if (video.vol_timer != NULL) {
            lv_timer_delete(video.vol_timer);
            video.vol_timer = NULL;
        }
        lv_obj_delete(overlay);
        lvgl_port_unlock();
        memset(&video, 0, sizeof(video));
    }
}
