#include "button.h"
#include "esp_log.h"
#include "string.h"
#include "esp_timer.h"

#define TAG "button"

#define BUTTON_SCAN_PERIOD_MS   5   /* 定时器扫描周期 */
#define BUTTON_DEBOUNCE_MS      20  /* 按下后持续多久才算稳定 */

static button_info_t* button_head = NULL;

static esp_timer_handle_t button_timer_handle;

static bool timer_running = false;

static void button_handle(void* arg);

esp_err_t button_event_set(button_config_t* cfg)
{
    button_info_t* btn = (button_info_t*)malloc(sizeof(button_info_t));
    if(!btn)
    {
        return ESP_FAIL;
    }
    memset(btn, 0, sizeof(button_info_t));
    memcpy(&btn->button_config, cfg, sizeof(button_config_t));
    if(!button_head)
    {
        button_head = btn;
    }
    else
    {
        button_info_t* info = button_head;
        while(info->next)
        {
            info = info->next;
        }
        info->next = btn;
    }

    if(!timer_running)
    {
        esp_timer_create_args_t button_timer =
        {
        .callback = button_handle,
        .name = "button",
        .dispatch_method = ESP_TIMER_TASK,
        };
        esp_timer_create(&button_timer, &button_timer_handle);
        esp_timer_start_periodic(button_timer_handle, BUTTON_SCAN_PERIOD_MS * 1000);
        timer_running = true;
    }
    return ESP_OK;
}
static void button_handle(void* arg)
{
    (void)arg;
    button_info_t* btn_info = button_head;
    const int interval = BUTTON_SCAN_PERIOD_MS;
    for(;btn_info;btn_info = btn_info->next)
    {
        int gpio_num = btn_info->button_config.gpio_num;
        switch(btn_info->state)
        {
            case BUTTON_RELEASE:
                if(btn_info->button_config.get_level_callback(gpio_num) == btn_info->button_config.active_level)
                {
                    btn_info->state = BUTTON_PRESS;
                    btn_info->press_cnt += interval;
                }
                break;  
            case BUTTON_PRESS:
                if(btn_info->button_config.get_level_callback(gpio_num) == btn_info->button_config.active_level)
                {
                    btn_info->press_cnt += interval;
                    if(btn_info->press_cnt >= BUTTON_DEBOUNCE_MS)
                    {
                        btn_info->state = BUTTON_HOLD;
                    }
                }
                else
                {
                    btn_info->state = BUTTON_RELEASE;
                    btn_info->press_cnt = 0;
                }
                break;     
            case BUTTON_HOLD:
                if(btn_info->button_config.get_level_callback(gpio_num) == btn_info->button_config.active_level)
                {
                    btn_info->press_cnt += interval;
                    if(btn_info->press_cnt >= btn_info->button_config.long_press_time)
                    {
                        if(btn_info->button_config.long_press_callback)
                            btn_info->button_config.long_press_callback(gpio_num);
                        btn_info->state = BUTTON_WAIT_RELEASE;
                    }
                }
                else
                {
                    //在长按时间之前松开，视为短按
                    if(btn_info->button_config.short_press_callback)
                        btn_info->button_config.short_press_callback(gpio_num);
                    btn_info->state = BUTTON_RELEASE;
                    btn_info->press_cnt = 0;
                }
                break;
            case BUTTON_WAIT_RELEASE:
                if(btn_info->button_config.get_level_callback(gpio_num) != btn_info->button_config.active_level)
                {
                    btn_info->state = BUTTON_RELEASE;
                    btn_info->press_cnt = 0;
                }
                break;     
            default:
                break;
        }
    }
}
