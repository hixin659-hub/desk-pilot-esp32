#include "usb_camera.h"
#include "jpeg_color.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "string.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "libs/tjpgd/tjpgd.h"

#define TAG "usb_camera"
#define CAM_W   320
#define CAM_H   240

/* 以下两项来自 menuconfig（main/Kconfig.projbuild） */
#define CAM_JPEG_POOL_SIZE  (CONFIG_APP_CAMERA_JPEG_POOL_KB * 1024)  //TJPEG解码器工作缓冲区大小
#define CAM_DECODE_FPS      CONFIG_APP_CAMERA_FPS                    //解码目标帧率
#define CAM_DECODE_PERIOD_US (1000000 / CAM_DECODE_FPS)

static uint8_t s_jpeg_pool[CAM_JPEG_POOL_SIZE]; //TJPEG做解码的内部RAM
static uint16_t *s_frame_buf[2];    //双缓冲区，指针指向PSRAM里的RGB565图像缓冲区（PSRAM：CAM_W*CAM_H*2）
static int s_front;                 //当前正在被LVGL显示的buffer编号
static usb_camera_frame_cb_t s_frame_cb;    //保存UI传进来的回调指针函数

static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx);
static void stream_callback(const uvc_host_stream_event_data_t *event,void *user_ctx);

static QueueHandle_t rx_frames_queue;
static volatile bool dev_connected = false;
static volatile bool s_active = false;
static volatile bool s_suspended = false;    //OTA 等场景临时关闭 UVC
static volatile bool s_stream_closed = true; //没有已打开的 UVC 流（URB 已释放）

static const uvc_host_stream_config_t stream_config = 
{
    .event_cb = stream_callback,
    .frame_cb = frame_callback,
    .user_ctx = &rx_frames_queue,
    .usb =
    {
        .vid =  UVC_HOST_ANY_VID,
        .pid =  UVC_HOST_ANY_PID,
        .uvc_stream_index = 0,
    },
    .vs_format =
    {
        .h_res = 640,
        .v_res = 480,
        .fps = CAM_DECODE_FPS,
        .format = UVC_VS_FORMAT_MJPEG,
    },
    .advanced = 
    {
        .frame_size = 256 * 1024,
#if CONFIG_SPIRAM
        .number_of_frame_buffers = 5, 
        .number_of_urbs = 3,          
        .urb_size = 10 * 1024,        
        .frame_heap_caps = MALLOC_CAP_SPIRAM,
#else
        .number_of_frame_buffers = 2, 
        .number_of_urbs = 2,          
        .urb_size = 2 * 1024,         
#endif
    },
};

static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    assert(frame);
    assert(user_ctx);
    QueueHandle_t frame_q = *((QueueHandle_t*)user_ctx);
    ESP_LOGD(TAG, "Frame callback! data len: %d", frame->data_len);
    BaseType_t result = xQueueSendToBack(frame_q, &frame, 0);
    if(pdPASS != result)
    {
        ESP_LOGW(TAG,"Queue full, losing frame");
        return true;
    }
    return false;
}

static void stream_callback(const uvc_host_stream_event_data_t *event,void *user_ctx)
{
    switch (event->type) 
    {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "USB error has occurred, err_no = %i", event->transfer_error.error);
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "Device suddenly disconnected");
        dev_connected = false;
        ESP_ERROR_CHECK(uvc_host_stream_close(event->device_disconnected.stream_hdl));
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        // The Frame was discarded because it exceeded the available frame buffer size.
        // To resolve this, increase the `frame_size` parameter in `uvc_host_stream_config_t.advanced` to allocate a larger buffer.
        ESP_LOGW(TAG, "Frame buffer overflow");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        // The Frame was discarded because no available buffer was free for storage.
        // To resolve this, either optimize your processing speed or increase the `number_of_frame_buffers` parameter in
        // `uvc_host_stream_config_t.advanced` to allocate additional buffers.
        ESP_LOGW(TAG, "Frame buffer underflow");
        break;
    default:
        ESP_LOGW(TAG, "Unsupported UVC event: %d (possibly suspend/resume)", event->type);
        break;
    }
}

//TJPGD输入回调：读TJPGD数据
static size_t cam_jpeg_in_cb(JDEC *jd,uint8_t *buf,size_t len)
{
    cam_jpeg_io_t *io = (cam_jpeg_io_t*)jd->device;
    //还剩下多少字节没读完
    size_t left = io->len-io->pos;
    if(len>left)
    {
        len = left;
    }
    if(len == 0)
    {
        return 0;
    }
    if(buf != NULL)
    {
        memcpy(buf,io->data + io->pos,len);
    }
    io->pos += len;
    return len;
}
//输出回调（bitmap：TJPDG输出一块RGB888图像小块，rect：每个小块分别在图上的坐标）
static int cam_jpeg_out_cb(JDEC *jd,void *bitmap,JRECT *rect)
{
    cam_jpeg_io_t *io = (cam_jpeg_io_t*)jd->device;
    return jpeg_rgb888_to_rgb565((const uint8_t *)bitmap,
                                 (int)rect->right - (int)rect->left + 1,
                                 (int)rect->bottom - (int)rect->top + 1,
                                 (int)rect->left, (int)rect->top,
                                 io->dst, io->dst_w, io->dst_h) ? 1 : 0;
}

//主解码函数（frame：UVC传来的的一帧MJPEG）
static bool cam_decode_frame(const uvc_host_frame_t *frame,uint16_t *dst)
{
    cam_jpeg_io_t io =
    {
        .data = frame->data,
        .len = frame->data_len,
        .pos = 0,
        .dst = dst,
        .dst_w = CAM_W,
        .dst_h = CAM_H,
    };
    JDEC jd;
    //解析JPEG文件头
    JRESULT res = jd_prepare(&jd,cam_jpeg_in_cb,s_jpeg_pool,sizeof(s_jpeg_pool),&io); 
    if(res != JDR_OK)
    {
        ESP_LOGW(TAG,"JPEG header failed (%d)",(int)res);
        return false;
    }
    uint8_t scale = 0;
    //计算缩放倍数（ scale=0：320x240 用 1/1；scale=1：640x480 用 1/2；scale=2：1280x720 用 1/4）
    while(scale < 3 && ((jd.width >> scale) > CAM_W || (jd.height >> scale) > CAM_H))
    {
        scale ++;
    }
    //解码
    res = jd_decomp(&jd,cam_jpeg_out_cb,scale);
    if(res != JDR_OK)
    {
        ESP_LOGW(TAG,"JPEG decode failed (%d)", (int)res);
        return false;
    }
    return true;
}

static void usb_lib_task(void *param)
{
    while(1)
    {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY,&event_flags);
        if(event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
        {
            ESP_LOGI(TAG,"No clients connected, starting to free all devices");
            usb_host_device_free_all();
        }
        if(event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)
        {
            ESP_LOGI(TAG,"All devices freed, uninstalling USB Host Library");
        }
    }
}

static void frame_handling_task(void *arg)
{
    (void)arg;
    QueueHandle_t frame_q = *((QueueHandle_t *)(stream_config.user_ctx));

    while(true)
    {
        if(s_suspended)
        {
            s_stream_closed = true;     /* 没有已打开的流 */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uvc_host_stream_hdl_t uvc_stream = NULL;
        s_stream_closed = false;        /* 正在打开，URB 可能已占用 */
        ESP_LOGI(TAG,"Opening UVC device 0x%04X:0x%04X %dx%d@%2.1FPS...",stream_config.usb.vid,
                stream_config.usb.pid,stream_config.vs_format.h_res,
                stream_config.vs_format.v_res,stream_config.vs_format.fps);
        esp_err_t err = uvc_host_stream_open(&stream_config,pdMS_TO_TICKS(5000),&uvc_stream);
        if(ESP_OK != err)
        {
            s_stream_closed = true;
            ESP_LOGI(TAG,"Failed to open device");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        dev_connected = true;
        ESP_LOGI(TAG,"UVC device opened!");
        vTaskDelay(pdMS_TO_TICKS(100));
        /* 相册不在摄像头页时先不推流，等真要预览了再 start */
        bool streaming = false;
        if(s_active)
        {
            if(uvc_host_stream_start(uvc_stream) != ESP_OK)
            {
                ESP_LOGW(TAG,"stream start failed");
                dev_connected = false;
            }
            else
            {
                streaming = true;
            }
        }
        int64_t next_decode_us = 0;
        while(dev_connected)
        {
            if(s_suspended)
            {
                if(streaming)
                {
                    uvc_host_stream_stop(uvc_stream);
                    streaming = false;
                    /* 停流前已排进队列的帧要还回去 */
                    uvc_host_frame_t *stale = NULL;
                    while(xQueueReceive(frame_q,&stale,0) == pdPASS)
                    {
                        uvc_host_frame_return(uvc_stream,stale);
                    }
                }
                dev_connected = false;
                break;
            }
            if(!s_active && streaming)
            {
                esp_err_t e = uvc_host_stream_stop(uvc_stream);
                streaming = false;
                /* 停流前已排进队列的帧要还回去，否则帧缓冲一直被占着 */
                uvc_host_frame_t *stale = NULL;
                while(xQueueReceive(frame_q,&stale,0) == pdPASS)
                {
                    uvc_host_frame_return(uvc_stream,stale);
                }
                ESP_LOGI(TAG,"stream stopped (%s)",esp_err_to_name(e));
            }
            else if(s_active && !streaming)
            {
                esp_err_t e = uvc_host_stream_start(uvc_stream);
                if(e == ESP_OK)
                {
                    streaming = true;
                    next_decode_us = 0;     /* 恢复后马上解一帧 */
                    ESP_LOGI(TAG,"stream resumed");
                }
                else
                {
                    /* 这颗摄像头对 alt 切换比较挑，起不来就整条流关掉重开 */
                    ESP_LOGW(TAG,"stream resume failed:%s",esp_err_to_name(e));
                    dev_connected = false;
                    break;
                }
            }
            if(!streaming)
            {
                /* 停流后收不到帧，短超时轮询 s_active，切回摄像头页能快速恢复 */
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            uvc_host_frame_t *frame = NULL;
            //从帧队列里取出一帧，超时一秒退出
            if(xQueueReceive(frame_q,&frame,pdMS_TO_TICKS(1000)) != pdPASS)
            {
                continue;
            }
            uvc_host_frame_t *newer = NULL;
            //非阻塞读取队列，如果还有排队帧
            while(xQueueReceive(frame_q,&newer,0) == pdPASS)
            {
                //把当前旧帧归还缓冲区
                uvc_host_frame_return(uvc_stream,frame);
                //保留队列里最新收到的帧
                frame = newer;
            }
            //软件限速解码
            int64_t now = esp_timer_get_time();
            if(s_active && now >= next_decode_us)
            {
                int back = s_front ^ 1;
                if(cam_decode_frame(frame,s_frame_buf[back]))
                {
                    s_front = back;
                    if(s_frame_cb != NULL)
                    {
                        s_frame_cb(back,CAM_W,CAM_H);
                    }
                }
                //4帧/s，限制CPU占用
                next_decode_us = now + 250000;
            }
            uvc_host_frame_return(uvc_stream,frame);
            vTaskDelay(1);
        }
        uvc_host_frame_t *frame = NULL;
        while(xQueueReceive(frame_q,&frame,0) == pdPASS)
        {
            uvc_host_frame_return(uvc_stream,frame);
        }
        err = uvc_host_stream_close(uvc_stream);
        s_stream_closed = (err == ESP_OK);
        if(err != ESP_OK)
        {
            ESP_LOGW(TAG,"stream close failed:%s",esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

esp_err_t usb_camera_init(void)
{
    rx_frames_queue = xQueueCreate(3, sizeof(uvc_host_frame_t *));
    assert(rx_frames_queue);

    usb_host_config_t usb_config = 
    {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&usb_config));

    BaseType_t task_created = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib_task", 
                            4096, NULL, 7, NULL, tskNO_AFFINITY);
    assert(task_created == pdTRUE);
    
    uvc_host_driver_config_t uvc_driver_config = 
    {
        .driver_task_stack_size = 4 * 1024,
        .driver_task_priority = 16,
        .xCoreID = 0,
        .create_background_task = true,
    };
    ESP_ERROR_CHECK(uvc_host_install(&uvc_driver_config));

    for(int i=0;i<2;i++)
    {
        //内存分配到外部PSRAM
        s_frame_buf[i] = heap_caps_calloc(1,(size_t)CAM_W * CAM_H * sizeof(uint16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        //释放前面已经成功分配的几块缓冲区，防止内存泄漏
        if(s_frame_buf[i] == NULL)
        {
            ESP_LOGE(TAG,"No memory for camera frame buffer %d",i);
            for(int j=0;j<i;j++)
            {
                heap_caps_free(s_frame_buf[j]);
                s_frame_buf[j] = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
    }

    task_created = xTaskCreatePinnedToCore(frame_handling_task, "frame_handling_task",
                            8196, NULL, 3, NULL, 0);
    assert(task_created == pdTRUE);
    

    return ESP_OK;
}

//注册画面回调函数
void usb_camera_set_frame_cb(usb_camera_frame_cb_t cb)
{
    s_frame_cb = cb;
}

//获取缓冲区编号，用于初始化lv_image_dst_t*
uint16_t *usb_camera_get_frame_buf(int buf_index)
{
    if(buf_index < 0 || buf_index > 1)
    {
        return NULL;
    }
    return s_frame_buf[buf_index];
}

void usb_camera_set_active(bool on)
{
    s_active = on;
}

void usb_camera_set_suspended(bool on)
{
    if(on)
    {
        /* 先清 closed，等相机任务真正关闭流以后再置 true */
        s_stream_closed = false;
    }
    s_suspended = on;
}

bool usb_camera_is_closed(void)
{
    return s_stream_closed;
}
