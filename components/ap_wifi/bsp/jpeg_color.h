#ifndef _JPEG_COLOR_H_
#define _JPEG_COLOR_H_

#include <stdbool.h>
#include <stdint.h>

/* TJPGD 在 JD_FORMAT=0 下按 B,G,R 字节序输出 RGB888。
   把一块 rw×rh 的像素转换成 RGB565，写到 dst 里以 (left,top) 为左上角的位置。
   越界或参数非法返回 false（TJPGD 输出回调返回 0 会中止解码）。 */
bool jpeg_rgb888_to_rgb565(const uint8_t *bgr, int rw, int rh,
                           int left, int top,
                           uint16_t *dst, int dst_w, int dst_h);

#endif
