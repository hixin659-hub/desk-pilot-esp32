#ifndef OTA_UI_H
#define OTA_UI_H
#ifdef __cplusplus
extern "C" {
#endif

#include "../generated/gui_guider.h"

/* OTA 入口按钮 + 升级弹窗（运行期创建，不修改 GuiGuider 生成文件） */
void ota_ui_init(gg_ui_t *ui);

/* 打开升级弹窗（供 monitor 页右上角齿轮调用） */
void ota_ui_open(void);

#ifdef __cplusplus
}
#endif
#endif /* OTA_UI_H */
