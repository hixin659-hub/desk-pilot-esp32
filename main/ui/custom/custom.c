/*********************
 *      INCLUDES
 *********************/
#include "custom.h"
#include "ota_ui.h"
#include "monitor_ui.h"

void custom_init(gg_ui_t *ui)
{
    home_weather_init(ui);   /* 首页时钟 + 字体 */
    album_picture_init(ui);  /* 相册 tab、圆点、扫描、缩略图 */
    album_folder_init(ui);   /* 文件夹 tab、路径栏、子文件夹浏览 */
    album_memory_init(ui);   /* 相册顶部 SD 容量显示 */
    ota_ui_init(ui);         /* 升级弹窗 */
    monitor_ui_init(ui);     /* monitor 页深色卡片 + 顶栏齿轮 */
}
