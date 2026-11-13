#include <stddef.h>
#include "jpeg_color.h"

bool jpeg_rgb888_to_rgb565(const uint8_t *bgr, int rw, int rh,
                           int left, int top,
                           uint16_t *dst, int dst_w, int dst_h)
{
    if (dst == NULL || rw <= 0 || rh <= 0 ||
        left < 0 || top < 0 ||
        left + rw > dst_w || top + rh > dst_h) {
        return false;
    }

    for (int y = 0; y < rh; y++) {
        uint16_t *d = dst + (size_t)(top + y) * dst_w + left;
        for (int x = 0; x < rw; x++) {
            uint8_t b = bgr[0];
            uint8_t g = bgr[1];
            uint8_t r = bgr[2];
            bgr += 3;
            *d++ = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
    return true;
}
