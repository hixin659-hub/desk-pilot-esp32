#ifndef MONITOR_UI_H
#define MONITOR_UI_H
#ifdef __cplusplus
extern "C" {
#endif

#include "../generated/gui_guider.h"

/* monitor 页外观：深色卡片 + 顶栏（系统监控 / 版本 / 升级齿轮） */
void monitor_ui_init(gg_ui_t *ui);

#ifdef __cplusplus
}
#endif
#endif /* MONITOR_UI_H */
