#ifndef _USB_CAMERA_H_
#define _USB_CAMERA_H_

#include "esp_err.h"
#include "stdbool.h"

typedef struct 
{
    const uint8_t *data;
    size_t len;             //JPEG总字节长度
    size_t pos;             //字节偏移量
    uint16_t *dst;          //目标RGB565缓冲区（s_frame_buf）
    int dst_w;              
    int dst_h;
}cam_jpeg_io_t;


esp_err_t usb_camera_init(void);

//帧解码完成后存入buf_index（缓冲区编号），调用刷新UI
typedef void (*usb_camera_frame_cb_t)(int buf_index,int w,int h);

//注册画面回调函数
void usb_camera_set_frame_cb(usb_camera_frame_cb_t cb);

//获取缓冲区编号，用于初始化lv_image_dst_t*
uint16_t *usb_camera_get_frame_buf(int buf_index);

void usb_camera_set_active(bool on);

#endif 
