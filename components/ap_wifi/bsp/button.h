#ifndef _BUTTON_H_
#define _BUTTON_H_
#include "esp_err.h"

//按键回调函数
typedef void(*button_press_callback_t)(int gpio);

//获取GPIO电平的操作回调函数
typedef int(*button_get_gpio_level_callback_t)(int gpio);

typedef struct 
{
    int gpio_num;       //按键对应的GPIO编号
    int active_level;   //按下的电平
    int long_press_time; //长按时间，单位ms
    button_press_callback_t short_press_callback; //短按回调函数
    button_press_callback_t long_press_callback;  //长按回调函数
    button_get_gpio_level_callback_t get_level_callback; //获取GPIO电平的回调函数
}button_config_t;

typedef enum
{
    BUTTON_RELEASE,    //按键松开
    BUTTON_PRESS,      //消抖状态(按键按下)
    BUTTON_HOLD,       //按键按住状态
    BUTTON_WAIT_RELEASE, //等待按键松开状态
} BUTTON_STATE;

typedef struct Button_info
{
    button_config_t button_config;    //按键配置
    BUTTON_STATE state;        //按键状态
    int press_cnt;            //按键按下计数
    struct Button_info *next;    //下一个按键参数
} button_info_t;

esp_err_t button_event_set(button_config_t* cfg);

#endif
