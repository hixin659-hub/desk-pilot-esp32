#ifndef _ANIMOTION_H_
#define _ANIMOTION_H_

#include <stdbool.h>

//开始播放开机动画：非阻塞，图像建在系统最顶层，播完自动销毁
void animotion_play(void);

//动画是否已播完
bool animotion_is_done(void);

#endif
