#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <time.h>
#include "sdkconfig.h"
#include "lvgl.h"
#include "image/lv_image_decoder_private.h"
#include "custom.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_camera.h"


#define ALBUM_PICTURE_DIR       "/sd/DCIM"
#define ALBUM_MAX_IMAGES        128
#define ALBUM_MAX_DEPTH         4       /* 子目录最多递归几层 */
#define ALBUM_IMG_PATH_MAX      512

#define ALBUM_THUMB_SIZE        70
#define ALBUM_THUMB_PIXELS      (ALBUM_THUMB_SIZE * ALBUM_THUMB_SIZE)
#define ALBUM_THUMB_BYTES       (ALBUM_THUMB_PIXELS * 2)
#define ALBUM_THUMBS_PER_PAGE   8

#define DOT_SIZE                6
#define ALBUM_SCREEN_W          320
#define ALBUM_SCREEN_H          240
#define ALBUM_VIEW_H            186     
#define ALBUM_CELL_GAP          4

#define CAM_PREVIEW_W   320
#define CAM_PREVIEW_H   240
#define CAM_PREVIEW_Y   (-28)

static void album_picture_scan_task(void *arg);
static bool album_is_image_file(const char *name);
static int album_path_cmp(const void *a, const void *b);
static void picture_page_create(gg_ui_t *ui, const lv_image_dsc_t *thumbs, uint32_t img_cnt);
static void album_grid_try_build(gg_ui_t *ui);
static void album_scan_dir(const char *dir_path, char (*paths)[ALBUM_IMG_PATH_MAX],
                           uint32_t *count, int depth);
static void album_camera_init(gg_ui_t *ui);
static void album_camera_frame_cb(int buf_index,int w,int h);
static void cam_img_deleted_cb(lv_event_t *e);
static bool album_camera_should_run(gg_ui_t *ui);

static const char *TAG = "album";

/* 相册的运行状态集中放这里：
   thumbs     扫描时一次性生成的 70×70 RGB565 缩略图（PSRAM），滑动只做内存拷贝
   paths      原图的 SD 卡路径，点开大图时要用，所以扫描完一直留着
   grid_screen 网格已经建在哪个 screen 上（GuiGuider 懒创建，防止重复建）
   viewer     全屏看图的遮罩，NULL 表示没打开
   dots       页码圆点所在的行 
   cam_img    显示摄像头画面的组件
   cam_dsc[2]   两个图像描述符，分别绑定两块缓冲内存，用来切换画面源
*/
typedef struct {
    lv_image_dsc_t *thumbs;
    char (*paths)[ALBUM_IMG_PATH_MAX];
    uint32_t count;
    lv_obj_t *grid_screen;
    lv_obj_t *viewer;
    lv_obj_t *dots;
    lv_obj_t *cam_img;
    lv_image_dsc_t cam_dsc[2];
} album_ctx_t;

static album_ctx_t album;

/* ---------------- 底部 tab ---------------- */

static void album_tab_select(gg_ui_t *ui, lv_obj_t *active_btn)
{
    lv_obj_t *containers[] = {
        ui->screen_album.container_picture,
        ui->screen_album.container_folder,
        ui->screen_album.container_video,
    };
    lv_obj_t *buttons[] = {
        ui->screen_album.container_bottom_imagebutton_picture,
        ui->screen_album.container_bottom_imagebutton_folder,
        ui->screen_album.container_bottom_imagebutton_video,
    };

    for (int i = 0; i < 3; i++) {
        if (buttons[i] == active_btn) {
            lv_obj_set_hidden(containers[i], false);
            lv_imagebutton_set_state(buttons[i], LV_IMAGEBUTTON_STATE_CHECKED_RELEASED);
        } else {
            lv_obj_set_hidden(containers[i], true);
            lv_imagebutton_set_state(buttons[i], LV_IMAGEBUTTON_STATE_RELEASED);
        }
    }
    usb_camera_set_active(active_btn == buttons[2]);
}

static void album_tab_clicked_cb(lv_event_t *e)
{
    gg_ui_t *ui = lv_event_get_user_data(e);
    album_tab_select(ui, lv_event_get_target(e));
}

static void album_tab_checked_icons_init(gg_ui_t *ui)
{
    lv_obj_t *buttons[] = {
        ui->screen_album.container_bottom_imagebutton_picture,
        ui->screen_album.container_bottom_imagebutton_folder,
        ui->screen_album.container_bottom_imagebutton_video,
    };

    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = buttons[i];
        const void *released = lv_imagebutton_get_src_middle(btn, LV_IMAGEBUTTON_STATE_RELEASED);
        const void *checked  = lv_imagebutton_get_src_middle(btn, LV_IMAGEBUTTON_STATE_CHECKED_RELEASED);
        const void *pressed  = lv_imagebutton_get_src_middle(btn, LV_IMAGEBUTTON_STATE_PRESSED);

        if (checked == released && pressed != NULL && pressed != released) {
            lv_imagebutton_set_src(btn, LV_IMAGEBUTTON_STATE_CHECKED_RELEASED, NULL, pressed, NULL);
            lv_imagebutton_set_src(btn, LV_IMAGEBUTTON_STATE_CHECKED_PRESSED, NULL, pressed, NULL);
        }
    }
}

/* 扫描结果就绪后，在 album 屏幕第一次创建出来时构建缩略图网格。 */
static void album_grid_try_build(gg_ui_t *ui)
{
    if (ui->screen_album.screen == NULL || album.thumbs == NULL) {
        return;
    }
    if (album.grid_screen == ui->screen_album.screen) {
        return;
    }

    ESP_LOGI(TAG,"build grid: %u media file(s)",(unsigned)album.count);
    picture_page_create(ui,album.thumbs,album.count);
    album.grid_screen = ui->screen_album.screen;
}

static void cam_img_deleted_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    album.cam_img = NULL;
}

/* 相册页在前台、视频 tab 可见、且没在播 AVI 时才需要解码摄像头 */
static bool album_camera_should_run(gg_ui_t *ui)
{
    return ui->screen_album.screen != NULL &&
           lv_screen_active() == ui->screen_album.screen &&
           !lv_obj_is_hidden(ui->screen_album.container_video) &&
           !album_video_is_playing();
}
static void album_camera_frame_cb(int buf_index,int w,int h)
{
    LV_UNUSED(w);
    LV_UNUSED(h);
    if(buf_index < 0 || buf_index > 1 || album.cam_img == NULL)
    {
        return;
    }
    lvgl_port_lock(0);
    if(album.cam_img != NULL)
    {
        lv_image_set_src(album.cam_img,&album.cam_dsc[buf_index]);
    }
    lvgl_port_unlock();
}

static void album_camera_init(gg_ui_t *ui)
{
    if(album.cam_img != NULL)
    {
        return;
    }
    if(usb_camera_get_frame_buf(0) == NULL || usb_camera_get_frame_buf(1) == NULL)
    {
        ESP_LOGW(TAG,"camera buffers not ready, preview diabled");
        return;
    }
    lv_obj_set_style_pad_all(ui->screen_album.container_video,0,0);
    for(int i=0;i<2;i++)
    {
        memset(&album.cam_dsc[i],0,sizeof(album.cam_dsc[i]));
        album.cam_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        album.cam_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        album.cam_dsc[i].header.w = CAM_PREVIEW_W;
        album.cam_dsc[i].header.h = CAM_PREVIEW_H;
        album.cam_dsc[i].header.stride = CAM_PREVIEW_W * 2;
        album.cam_dsc[i].data_size = CAM_PREVIEW_W * CAM_PREVIEW_H * 2;
        album.cam_dsc[i].data = (const uint8_t*)usb_camera_get_frame_buf(i);
    }
    album.cam_img = lv_image_create(ui->screen_album.container_video);
    lv_image_set_src(album.cam_img,&album.cam_dsc[0]);
    lv_obj_set_pos(album.cam_img,0,CAM_PREVIEW_Y);
    lv_obj_add_event_cb(album.cam_img,cam_img_deleted_cb,LV_EVENT_DELETE,NULL);
    usb_camera_set_frame_cb(album_camera_frame_cb);
}

void album_tabs_init(gg_ui_t *ui)
{
    static lv_obj_t *inited_screen = NULL;

    if (ui->screen_album.screen == NULL || inited_screen == ui->screen_album.screen) {
        return;
    }

    lv_obj_t *buttons[] = {
        ui->screen_album.container_bottom_imagebutton_picture,
        ui->screen_album.container_bottom_imagebutton_folder,
        ui->screen_album.container_bottom_imagebutton_video,
    };

    for (int i = 0; i < 3; i++) {
        lv_obj_add_event_cb(buttons[i], album_tab_clicked_cb, LV_EVENT_CLICKED, ui);
    }

    album_tab_checked_icons_init(ui);
    album_tab_select(ui, buttons[0]);

    /* GuiGuider 只关了滚动条，没关滚动本身；这些容器的子控件
       位置超出边界（负 y），拖动时内容会跟着动。全部禁掉滚动。 */
    lv_obj_t *fix_scroll[] = {
        ui->screen_album.container_top,
        ui->screen_album.container_bottom,
        ui->screen_album.container_picture,
        ui->screen_album.container_folder,
        ui->screen_album.container_video,
    };
    for (int i = 0; i < (int)(sizeof(fix_scroll) / sizeof(fix_scroll[0])); i++) {
        lv_obj_set_scrollable(fix_scroll[i], false);
    }

    album_grid_try_build(ui);

    album_camera_init(ui);
    usb_camera_set_active(album_camera_should_run(ui));

    inited_screen = ui->screen_album.screen;
}

static void album_timer_cb(lv_timer_t * timer)
{
    gg_ui_t * ui = lv_timer_get_user_data(timer);

    if (ui->screen_album.screen == NULL) {
        return;
    }

    album_tabs_init(ui);

    /* 每秒重新判断一次摄像头该不该工作：
       切去别的页面、开始播 AVI 时停流；播完/切回摄像头页后自动恢复预览。
       之前只在 tab 点击时判断，视频播完不切 tab 的话预览会一直停着。 */
    usb_camera_set_active(album_camera_should_run(ui));

    time_t now = time(NULL);
    if (now < 1700000000) {
        return;
    }
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    lv_label_set_text_fmt(ui->screen_album.container_top_label_time, "%02d:%02d",
                          tm_now.tm_hour, tm_now.tm_min);
}

/* ---------------- 页码圆点 ---------------- */

/* 父对象（container_picture）被删除时，album.dots 也会一起被删。
   用这个回调把指针清空，避免下一次重建时 delete 一个野指针。 */
static void dots_deleted_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    album.dots = NULL;
}

/* 创建圆点行（pages:总页数） */
static void page_dots_create(lv_obj_t *parent,uint32_t pages)
{
    if(album.dots) {
        lv_obj_delete(album.dots);
        album.dots = NULL;
    }
    /* 只有一页就不显示圆点 */
    if(pages <= 1)
        return;
    album.dots = lv_obj_create(parent);
    lv_obj_add_event_cb(album.dots, dots_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(album.dots,LV_SIZE_CONTENT,DOT_SIZE);
    lv_obj_align(album.dots,LV_ALIGN_BOTTOM_MID,0,-4);
    lv_obj_set_flex_flow(album.dots,LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(album.dots,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(album.dots,false);
    lv_obj_set_style_pad_all(album.dots,0,0);
    lv_obj_set_style_bg_opa(album.dots,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(album.dots,0,0);
    for(uint32_t i = 0;i < pages;i ++)
    {
        lv_obj_t *dot = lv_obj_create(album.dots);
        lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_30, 0);    
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_set_style_margin_hor(dot, 3, 0);
        lv_obj_set_scrollable(dot, false);
    }
}

static void page_dots_set(lv_obj_t *row,uint32_t page)
{
    if(row == NULL)
        return;
    uint32_t i = 0;
    lv_obj_t *dot = lv_obj_get_child(row, 0);
    while(dot)
    {
        lv_obj_set_style_bg_opa(dot,(i == page ? LV_OPA_100 : LV_OPA_30), 0);
        dot = lv_obj_get_child(row, ++i);
    }
}

static void pager_changed_cb(lv_event_t *e)
{
    gg_ui_t *ui = lv_event_get_user_data(e);
    lv_obj_t *tv = ui->screen_album.container_picture_tileview_picture;
    if(tv == NULL)
        return;
    lv_obj_t *tile = lv_tileview_get_tile_active(tv);
    if(tile == NULL)
        return;
    int32_t page = lv_obj_get_index(tile);
    if(page < 0)
        return;
    page_dots_set(album.dots,(uint32_t)page);
}

/* ---------------- 扫描 + 缩略图 ---------------- */

static bool album_is_image_file(const char *name)
{
    const char *dot = strrchr(name,'.');
    if(dot == NULL)
        return false;

    if(strcasecmp(dot,".png") == 0)
        return true;
#ifdef CONFIG_LV_USE_TJPGD
    if(strcasecmp(dot,".jpg") == 0 || strcasecmp(dot,".jpeg") == 0)
        return true;
#endif
#ifdef CONFIG_LV_USE_BMP
    if(strcasecmp(dot,".bmp") == 0)
        return true;
#endif
    return false;
}

/* 目前只有 AVI（MJPEG+PCM）能播，网格里也只把 .avi 当视频收进来 */
static bool album_is_video_file(const char *name)
{
    const char *dot = strrchr(name,'.');
    return dot != NULL && strcasecmp(dot,".avi") == 0;
}

static bool album_is_media_file(const char *name)
{
    return album_is_image_file(name) || album_is_video_file(name);
}

static int album_path_cmp(const void *a,const void *b)
{
    return strcmp((const char *)a,(const char *)b);
}

static void album_thumb_accumulate(uint32_t *sum_r,uint32_t *sum_g,uint32_t *sum_b,
                                   uint32_t *sum_a,uint32_t *cnt,
                                   const uint8_t *data,uint32_t stride,
                                   uint32_t bw,uint32_t bh,
                                   uint32_t ox,uint32_t oy,
                                   uint32_t sw,uint32_t sh,
                                   lv_color_format_t cf)
{
    for(uint32_t y = 0;y < bh;y ++)
    {
        uint32_t sy = oy + y;
        if(sy >= sh)
            break;
        uint32_t dy = sy * ALBUM_THUMB_SIZE / sh;
        const uint8_t *row = data + y * stride;

        for(uint32_t x = 0;x < bw;x ++)
        {
            uint32_t sx = ox + x;
            if(sx >= sw)
                break;
            uint32_t dx = sx * ALBUM_THUMB_SIZE / sw;
            uint32_t idx = dy * ALBUM_THUMB_SIZE + dx;
            uint32_t r,g,b,a;

            if(cf == LV_COLOR_FORMAT_RGB565)
            {
                lv_color16_t c = ((const lv_color16_t *)row)[x];
                r = (uint32_t)c.red << 3;
                g = (uint32_t)c.green << 2;
                b = (uint32_t)c.blue << 3;
                a = 255;
            }
            else if(cf == LV_COLOR_FORMAT_RGB888)
            {
                b = row[x * 3 + 0];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
                a = 255;
            }
            else if(cf == LV_COLOR_FORMAT_XRGB8888)
            {
                b = row[x * 4 + 0];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = 255;
            }
            else /* LV_COLOR_FORMAT_ARGB8888 */
            {
                b = row[x * 4 + 0];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = row[x * 4 + 3];
            }

            sum_r[idx] += r * a;
            sum_g[idx] += g * a;
            sum_b[idx] += b * a;
            sum_a[idx] += a;
            cnt[idx] ++;
        }
    }
}

static uint16_t album_rgb565(uint32_t r,uint32_t g,uint32_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void album_thumb_finalize(const uint32_t *sum_r,const uint32_t *sum_g,
                                 const uint32_t *sum_b,const uint32_t *sum_a,
                                 const uint32_t *cnt,uint16_t *dst)
{
    const uint32_t bg = 0x44;

    for(uint32_t i = 0;i < ALBUM_THUMB_PIXELS;i ++)
    {
        if(cnt[i] == 0)
        {
            dst[i] = album_rgb565(bg,bg,bg);
            continue;
        }
        uint32_t a = sum_a[i] / cnt[i];
        uint32_t r = sum_a[i] ? sum_r[i] / sum_a[i] : 0;
        uint32_t g = sum_a[i] ? sum_g[i] / sum_a[i] : 0;
        uint32_t b = sum_a[i] ? sum_b[i] / sum_a[i] : 0;

        r = (r * a + bg * (255 - a)) / 255;
        g = (g * a + bg * (255 - a)) / 255;
        b = (b * a + bg * (255 - a)) / 255;

        dst[i] = album_rgb565(r,g,b);
    }
}

static bool album_thumb_generate(const char *path,
                                 uint32_t *sum_r,uint32_t *sum_g,uint32_t *sum_b,
                                 uint32_t *sum_a,uint32_t *cnt,uint16_t *dst)
{
    lv_image_decoder_args_t args = {0};
    args.no_cache = true;

    lv_image_decoder_dsc_t dsc;
    if(lv_image_decoder_open(&dsc,path,&args) != LV_RESULT_OK)
    {
        ESP_LOGW(TAG,"decode failed: %s",path);
        return false;
    }
    uint32_t sw = dsc.header.w;
    uint32_t sh = dsc.header.h;
    lv_color_format_t cf = dsc.header.cf;

    if(sw == 0 || sh == 0 ||
       (cf != LV_COLOR_FORMAT_RGB565 && cf != LV_COLOR_FORMAT_RGB888 &&
        cf != LV_COLOR_FORMAT_XRGB8888 && cf != LV_COLOR_FORMAT_ARGB8888))
    {
        ESP_LOGW(TAG,"unsupported image: %s (cf=%d, %ux%u)",path,(int)cf,
                 (unsigned)sw,(unsigned)sh);
        lv_image_decoder_close(&dsc);
        return false;
    }
    memset(sum_r,0,ALBUM_THUMB_PIXELS * sizeof(uint32_t));
    memset(sum_g,0,ALBUM_THUMB_PIXELS * sizeof(uint32_t));
    memset(sum_b,0,ALBUM_THUMB_PIXELS * sizeof(uint32_t));
    memset(sum_a,0,ALBUM_THUMB_PIXELS * sizeof(uint32_t));
    memset(cnt,0,ALBUM_THUMB_PIXELS * sizeof(uint32_t));

    if(dsc.decoded != NULL)
    {
        /* PNG 等一次性解码完的格式 */
        album_thumb_accumulate(sum_r,sum_g,sum_b,sum_a,cnt,
                               dsc.decoded->data,dsc.decoded->header.stride,
                               dsc.decoded->header.w,dsc.decoded->header.h,
                               0,0,sw,sh,cf);
    }
    else
    {
        /* JPEG/BMP 分块解码：每次拿一块，累加后继续 */
        lv_area_t full = {0,0,(int32_t)sw - 1,(int32_t)sh - 1};
        lv_area_t area = {LV_COORD_MIN,LV_COORD_MIN,LV_COORD_MIN,LV_COORD_MIN};
        while(lv_image_decoder_get_area(&dsc,&full,&area) == LV_RESULT_OK)
        {
            const lv_draw_buf_t *block = dsc.decoded;
            if(block == NULL)
                break;

            album_thumb_accumulate(sum_r,sum_g,sum_b,sum_a,cnt,
                                   block->data,block->header.stride,
                                   block->header.w,block->header.h,
                                   (uint32_t)area.x1,(uint32_t)area.y1,
                                   sw,sh,cf);
        }
    }
    lv_image_decoder_close(&dsc);
    album_thumb_finalize(sum_r,sum_g,sum_b,sum_a,cnt,dst);
    return true;
}

static void album_thumb_dsc_init(lv_image_dsc_t *dsc,const uint16_t *pixels)
{
    memset(dsc,0,sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = ALBUM_THUMB_SIZE;
    dsc->header.h = ALBUM_THUMB_SIZE;
    dsc->header.stride = ALBUM_THUMB_SIZE * 2;
    dsc->data_size = ALBUM_THUMB_BYTES;
    dsc->data = (const uint8_t *)pixels;
}

/* ---------------- 点击缩略图全屏查看 ---------------- */
/* 遮罩是 top layer 上的一个对象，点它任意位置就关掉 */
static void viewer_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if(album.viewer)
    {
        lv_obj_delete(album.viewer);
        album.viewer = NULL;
    }
}

/* 打开全屏查看器。path 要带 LVGL 的盘符前缀，如 "S:/sd/DCIM/1.jpg"。
   相册缩略图和文件夹列表都调用这个函数，必须在 LVGL 任务里调。 */
void album_image_open(const char *path)
{
    if(path == NULL || album.viewer != NULL)
        return;

    lv_image_header_t info;
    if(lv_image_decoder_get_info(path,&info) != LV_RESULT_OK ||
       info.w == 0 || info.h == 0)
    {
        ESP_LOGW(TAG,"viewer: failed to read %s",path);
        return;
    }
    if((uint64_t)info.w * info.h * 4 > 4 * 1024 * 1024)
    {
        ESP_LOGW(TAG,"viewer: %s too large (%ux%u)",path,
                 (unsigned)info.w,(unsigned)info.h);
        return;
    }
    lv_obj_t *viewer = lv_obj_create(lv_layer_top());
    album.viewer = viewer;
    lv_obj_set_size(viewer,ALBUM_SCREEN_W,ALBUM_SCREEN_H);
    lv_obj_set_pos(viewer,0,0);
    lv_obj_set_scrollable(viewer,false);
    lv_obj_set_clickable(viewer,true);
    lv_obj_set_style_pad_all(viewer,0,0);
    lv_obj_set_style_border_width(viewer,0,0);
    lv_obj_set_style_radius(viewer,0,0);
    lv_obj_set_style_bg_color(viewer,lv_color_black(),0);
    lv_obj_set_style_bg_opa(viewer,LV_OPA_COVER,0);
    lv_obj_add_event_cb(viewer,viewer_close_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t *img = lv_image_create(viewer);
    lv_obj_set_size(img,ALBUM_SCREEN_W,ALBUM_SCREEN_H);
    lv_obj_set_pos(img,0,0);
    lv_image_set_src(img,path);
    lv_image_set_inner_align(img,LV_IMAGE_ALIGN_CONTAIN);
}

static void album_thumb_clicked_cb(lv_event_t *e)
{
    const char *path = (const char *)lv_event_get_user_data(e);

    if(path == NULL || path[0] != 'S' || path[1] != ':')
        return;

    if(album_is_video_file(path))
    {
        /* "S:/sd/..." -> "/sd/..."，播放器用文件系统路径 */
        album_video_play(path + 2);
    }
    else
    {
        album_image_open(path);
    }
}

/* ---------------- 缩略图网格 ---------------- */

static void picture_page_create(gg_ui_t *ui,const lv_image_dsc_t *thumbs,uint32_t img_cnt)
{
    const uint32_t per_page = ALBUM_THUMBS_PER_PAGE;
    uint32_t pages = (img_cnt + per_page - 1) / per_page;

    lv_obj_t *parent = ui->screen_album.container_picture;
    lv_obj_t *tv = ui->screen_album.container_picture_tileview_picture;

    if(tv == NULL || (img_cnt > 0 && thumbs == NULL))
        return;
    lv_obj_set_style_pad_all(parent,0,0);
    lv_obj_set_style_border_width(parent,0,0);
    lv_obj_set_scrollable(parent,false);
    lv_obj_clean(tv);
    lv_obj_remove_event_cb(tv,pager_changed_cb);
    lv_obj_add_event_cb(tv,pager_changed_cb,LV_EVENT_VALUE_CHANGED,ui);
    lv_obj_set_size(tv,ALBUM_SCREEN_W,ALBUM_VIEW_H);
    lv_obj_set_pos(tv,0,0);
    lv_obj_set_style_pad_all(tv,0,0);
    lv_obj_set_style_border_width(tv,0,0);
    lv_obj_set_style_radius(tv,0,0);
    lv_obj_set_scroll_dir(tv,LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(tv,LV_SCROLLBAR_MODE_OFF);

    for(uint32_t p = 0;p < pages;p ++)
    {
        lv_obj_t *tile = lv_tileview_add_tile(tv,p,0,LV_DIR_HOR);
        lv_obj_set_style_pad_all(tile,0,0);
        lv_obj_set_style_radius(tile,0,0);
        lv_obj_set_style_bg_opa(tile,LV_OPA_TRANSP,0);
        lv_obj_set_style_border_width(tile,0,0);
        lv_obj_set_scrollable(tile,false);
        lv_obj_set_flex_flow(tile,LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(tile,LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);

        for(uint32_t i = 0;i < per_page;i ++)
        {
            uint32_t index = p * per_page + i;
            if(index >= img_cnt)
                break;

            lv_obj_t *cell = lv_obj_create(tile);
            lv_obj_set_size(cell,ALBUM_THUMB_SIZE,ALBUM_THUMB_SIZE);
            lv_obj_set_style_radius(cell,6,0);
            lv_obj_set_style_clip_corner(cell,true,0);
            lv_obj_set_style_bg_color(cell,lv_color_hex(0x444444),0);
            lv_obj_set_style_bg_opa(cell,LV_OPA_COVER,0);
            lv_obj_set_style_border_width(cell,0,0);
            lv_obj_set_style_pad_all(cell,0,0);
            lv_obj_set_style_margin_all(cell,ALBUM_CELL_GAP,0);
            lv_obj_set_scrollable(cell,false);
            lv_obj_set_clickable(cell,true);
            if(album.paths)
            {
                lv_obj_add_event_cb(cell,album_thumb_clicked_cb,LV_EVENT_CLICKED,
                                    (void *)album.paths[index]);
            }

            /* 缩略图解码失败时 data 为 NULL，留深灰占位，别把空指针交给 LVGL */
            if(thumbs[index].data != NULL)
            {
                lv_obj_t *img = lv_image_create(cell);
                lv_obj_set_size(img,ALBUM_THUMB_SIZE,ALBUM_THUMB_SIZE);
                lv_image_set_src(img,&thumbs[index]);
                lv_obj_center(img);
            }

            /* 视频加一个圆形播放角标，和照片区分开 */
            if(album.paths && album_is_video_file(album.paths[index]))
            {
                lv_obj_t *badge = lv_obj_create(cell);
                lv_obj_set_size(badge,26,26);
                lv_obj_center(badge);
                lv_obj_set_style_radius(badge,LV_RADIUS_CIRCLE,0);
                lv_obj_set_style_bg_color(badge,lv_color_black(),0);
                lv_obj_set_style_bg_opa(badge,LV_OPA_50,0);
                lv_obj_set_style_border_width(badge,0,0);
                lv_obj_set_style_pad_all(badge,0,0);
                lv_obj_set_scrollable(badge,false);
                lv_obj_set_clickable(badge,false);

                lv_obj_t *play = lv_label_create(badge);
                lv_label_set_text(play,LV_SYMBOL_PLAY);
                lv_obj_set_style_text_font(play,&lv_font_montserrat_14,0);
                lv_obj_set_style_text_color(play,lv_color_white(),0);
                lv_obj_center(play);
            }
        }
    }

    page_dots_create(parent,pages);
    if(album.dots)
        page_dots_set(album.dots,0);
    if(pages > 0)
        lv_tileview_set_tile_by_index(tv,0,0,LV_ANIM_OFF);
}

/* ---------------- 目录扫描 ---------------- */

/* 递归扫描 dir_path 下的图片。子目录最多钻 ALBUM_MAX_DEPTH 层，
   找到的图片按 LVGL 的 "S:/..." 全路径存进 paths[]。 */
static void album_scan_dir(const char *dir_path,char (*paths)[ALBUM_IMG_PATH_MAX],
                           uint32_t *count,int depth)
{
    if(depth > ALBUM_MAX_DEPTH)
        return;

    DIR *dir = opendir(dir_path);
    if(dir == NULL)
    {
        ESP_LOGW(TAG,"open %s failed",dir_path);
        return;
    }

    struct dirent *ent;
    while((ent = readdir(dir)) != NULL)
    {
        /* 跳过 "."、".." 和隐藏文件，递归也不会死循环 */
        if(ent->d_name[0] == '.')
            continue;

        char fs_path[ALBUM_IMG_PATH_MAX];
        int n = snprintf(fs_path,sizeof(fs_path),"%s/%s",dir_path,ent->d_name);
        if(n < 0 || n >= (int)sizeof(fs_path))
        {
            ESP_LOGW(TAG,"path too long, skip: %s/%s",dir_path,ent->d_name);
            continue;
        }

        if(ent->d_type == DT_DIR)
        {
            album_scan_dir(fs_path,paths,count,depth + 1);
            continue;
        }

        if(!album_is_media_file(ent->d_name))
            continue;

        if(*count >= ALBUM_MAX_IMAGES)
        {
            ESP_LOGW(TAG,"more than %d images, ignoring the rest",ALBUM_MAX_IMAGES);
            break;
        }
        /* 存成 LVGL 能识别的路径，如 "S:/sd/DCIM/test1/a.png" */
        n = snprintf(paths[*count],ALBUM_IMG_PATH_MAX,"S:%s",fs_path);
        if(n < 0 || n >= ALBUM_IMG_PATH_MAX)
        {
            ESP_LOGW(TAG,"path too long, skip: %s",fs_path);
            continue;
        }
        (*count) ++;
    }

    closedir(dir);
}

static void album_picture_scan_task(void *arg)
{
    gg_ui_t *ui = (gg_ui_t *)arg;

    char (*paths)[ALBUM_IMG_PATH_MAX] = heap_caps_malloc(
        ALBUM_MAX_IMAGES * ALBUM_IMG_PATH_MAX,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(paths == NULL)
    {
        paths = heap_caps_malloc(
            ALBUM_MAX_IMAGES * ALBUM_IMG_PATH_MAX,
            MALLOC_CAP_8BIT);
    }
    if(paths == NULL)
    {
        ESP_LOGE(TAG,"no memory for image list");
        vTaskDelete(NULL);
        return;
    }

    uint32_t count = 0;
    album_scan_dir(ALBUM_PICTURE_DIR,paths,&count,0);
    qsort(paths,count,ALBUM_IMG_PATH_MAX,album_path_cmp);

    ESP_LOGI(TAG,"found %u media file(s) in %s",(unsigned)count,ALBUM_PICTURE_DIR);
    if(count == 0)
    {
        ESP_LOGW(TAG,"no supported media in %s",ALBUM_PICTURE_DIR);
        free(paths);
        vTaskDelete(NULL);
        return;
    }

    /* 缩略图：每张 70×70 RGB565，再加一份复用的大求和表 */
    lv_image_dsc_t *thumbs = heap_caps_calloc(count,sizeof(lv_image_dsc_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *pixels = heap_caps_malloc(count * ALBUM_THUMB_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t *acc = heap_caps_malloc(5 * ALBUM_THUMB_PIXELS * sizeof(uint32_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(thumbs == NULL || pixels == NULL || acc == NULL)
    {
        ESP_LOGE(TAG,"no memory for %u thumbnails",(unsigned)count);
        heap_caps_free(thumbs);
        heap_caps_free(pixels);
        heap_caps_free(acc);
        free(paths);
        vTaskDelete(NULL);
        return;
    }

    uint32_t *sum_r = acc;
    uint32_t *sum_g = sum_r + ALBUM_THUMB_PIXELS;
    uint32_t *sum_b = sum_g + ALBUM_THUMB_PIXELS;
    uint32_t *sum_a = sum_b + ALBUM_THUMB_PIXELS;
    uint32_t *cnt   = sum_a + ALBUM_THUMB_PIXELS;

    /* 逐张解码。每张单独加锁，LVGL 任务可以在两张之间刷屏。 */
    uint32_t ready = 0;
    for(uint32_t i = 0;i < count;i ++)
    {
        uint16_t *dst = pixels + i * ALBUM_THUMB_PIXELS;
        bool ok;

        if(album_is_video_file(paths[i]))
        {
            /* 视频取第一帧当缩略图；路径去掉 LVGL 的 "S:" 前缀 */
            ok = album_video_thumb(paths[i] + 2,dst,ALBUM_THUMB_SIZE);
        }
        else
        {
            lvgl_port_lock(0);
            ok = album_thumb_generate(paths[i],sum_r,sum_g,sum_b,sum_a,cnt,dst);
            lvgl_port_unlock();
        }

        if(ok)
        {
            album_thumb_dsc_init(&thumbs[i],dst);
            ready ++;
        }
    }

    ESP_LOGI(TAG,"thumbnails ready: %u/%u",(unsigned)ready,(unsigned)count);

    lvgl_port_lock(0);

    album.thumbs = thumbs;
    album.count = count;
    album.grid_screen = NULL;
    album.paths = paths;

    /* 屏幕可能还没被 GuiGuider 创建出来（它是懒创建的）。
       如果已经存在就直接建；否则等 album_tabs_init 里再建。 */
    if(ui->screen_album.container_picture_tileview_picture != NULL)
    {
        picture_page_create(ui,album.thumbs,album.count);
        album.grid_screen = ui->screen_album.screen;
    }

    lvgl_port_unlock();

    heap_caps_free(acc);

    /* paths 不能释放：album.paths 还指向它，点缩略图看大图时要用 */
    vTaskDelete(NULL);
}

void album_picture_init(gg_ui_t *ui)
{
    lv_timer_create(album_timer_cb, 1000, ui);
    
    picture_page_create(ui, NULL, 0);

    xTaskCreatePinnedToCore(album_picture_scan_task, "album_scan", 8192, ui, 2, NULL, 0);
}
