#include "weather.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <string.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "custom.h"

#define TAG     "weather"

/* 心知天气凭据，来自 menuconfig（main/Kconfig.projbuild） */
#define PRIVATE_KEY     CONFIG_APP_WEATHER_API_KEY
#define LOCATION        CONFIG_APP_WEATHER_LOCATION

#define WEATHER_BUFF_LEN    1024

extern gg_ui_t guider_ui;

static uint8_t weather_data_buff[WEATHER_BUFF_LEN];

static int weather_data_size = 0;

esp_err_t _weather_http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGW(TAG, "HTTP error");
            break;
        case HTTP_EVENT_ON_DATA: {
            int copy_len = 0;
            if(evt->data_len > WEATHER_BUFF_LEN - weather_data_size)
            {
                copy_len = WEATHER_BUFF_LEN - weather_data_size;
            }
            else
            {
                copy_len = evt->data_len;
            }
            if(copy_len > 0)
            {
                memcpy(&weather_data_buff[weather_data_size],evt->data,(size_t)copy_len);
                weather_data_size += copy_len;
            }
            break;
        }
        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t pasre_weather(char* weather_data)
{
   cJSON* wt_js = cJSON_Parse(weather_data);
   if(!wt_js)
   {
        ESP_LOGW(TAG,"Invalid weather json");
        return ESP_FAIL;
   }
   cJSON* result_js = cJSON_GetObjectItem(wt_js,"results");
   if(!result_js)
   {
        ESP_LOGW(TAG,"Invalid weather result");
        return ESP_FAIL;
   }
   cJSON* result_child_js = result_js->child;
   if(!result_child_js)
   {
        ESP_LOGW(TAG,"Invalid weather result item");
        cJSON_Delete(wt_js);
        return ESP_FAIL;
   }
   char city[48];
   cJSON* location_js = cJSON_GetObjectItem(result_child_js,"location");
   cJSON* name_js = cJSON_GetObjectItem(location_js,"name");
   if(name_js)
   {
        snprintf(city,sizeof(city),"%s",cJSON_GetStringValue(name_js));
   }
   cJSON* daily_js = cJSON_GetObjectItem(result_child_js,"daily");
   weather_data_pkt_t data[3];
   int index = 0;
   if(daily_js)
   {
        cJSON* daily_child_js = daily_js->child;
        while(daily_child_js)
        {
            cJSON* high_js = cJSON_GetObjectItem(daily_child_js,"high");
            cJSON* low_js = cJSON_GetObjectItem(daily_child_js,"low");
            cJSON* code_js = cJSON_GetObjectItem(daily_child_js,"code_day");
            if(index < 3)
            {
                sscanf(cJSON_GetStringValue(high_js),"%d",&data[index].high_temp);
                sscanf(cJSON_GetStringValue(low_js),"%d",&data[index].low_temp);
                snprintf(data[index].code,sizeof(data[index].code),"%s",cJSON_GetStringValue(code_js));
            }
            index++;
            daily_child_js = daily_child_js->next;
        }
        set_home_city(&guider_ui,city);
        char img_path[32];
        snprintf(img_path,sizeof(img_path),"S:/img/%s@1x.png",data[0].code);
        set_today_img(&guider_ui,img_path,data[0].low_temp,data[0].high_temp);
        snprintf(img_path,sizeof(img_path),"S:/img/%s@1x.png",data[1].code);
        set_tomorrow_img(&guider_ui,img_path,data[1].low_temp,data[1].high_temp);
        snprintf(img_path,sizeof(img_path),"S:/img/%s@1x.png",data[2].code);
        set_after_img(&guider_ui,img_path,data[2].low_temp,data[2].high_temp);
   }
   cJSON_Delete(wt_js);
   return ESP_OK;
}

static esp_err_t weather_http_connect(void)
{
    static char url[256];
    snprintf(url,sizeof(url),
    "http://api.seniverse.com/v3/weather/daily.json?key=%s&location=%s&language=zh-Hans&unit=c&start=0&days=3",
    PRIVATE_KEY,LOCATION);
    esp_http_client_config_t config =
    {
        .url = url,
        .event_handler = _weather_http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    memset(weather_data_buff,0,WEATHER_BUFF_LEN);
    weather_data_size = 0;
    esp_err_t err = esp_http_client_perform(client);
    pasre_weather((char*)weather_data_buff);
    esp_http_client_cleanup(client);
    return err;
}

static void weather_task(void* param)
{
    while(1)
    {
        if(!wifi_manager_is_connected())
        {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        else
        {
            weather_http_connect();
            vTaskDelay(pdMS_TO_TICKS(1000 * CONFIG_APP_WEATHER_INTERVAL_S));
        }
    }
}

void weather_start(void)
{
    xTaskCreatePinnedToCore(weather_task,"weather",4096,NULL,2,NULL,1);
}
